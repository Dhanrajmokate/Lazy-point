package com.lazypoint.remote

import android.util.Base64
import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class RemotePointerClient(
    private val host: String,
    private val port: Int,
    private val deviceId: String,
    private val pin: String?,
    private val savedToken: String?,
    private val listener: Listener
) {
    interface Listener {
        fun onConnected()
        fun onAuthenticated(token: String?)
        fun onAuthFailed(reason: String)
        fun onDisconnected(reason: String)
        fun onError(message: String)
    }

    private var socket: Socket? = null
    private var rawOut: OutputStream? = null
    private var reader: BufferedReader? = null

    private val isRunning = AtomicBoolean(false)
    private val isAuthenticated = AtomicBoolean(false)
    private var heldLeftButton = false
    private var heldRightButton = false

    // Motion coalescence: eliminates all queue lag and buffer bloat
    private val pendingMoveX = AtomicInteger(0)
    private val pendingMoveY = AtomicInteger(0)
    private val motionLock = Object()

    // Control commands queue (clicks, keys, text)
    private val controlQueue = ConcurrentLinkedQueue<String>()

    private var senderThread: Thread? = null
    private var readerThread: Thread? = null
    private var pingExecutor: ScheduledExecutorService? = null

    companion object {
        private const val TAG = "RemotePointerClient"
    }

    fun connect() {
        if (isRunning.get()) return
        isRunning.set(true)

        Thread {
            try {
                val sock = Socket()
                sock.tcpNoDelay = true
                sock.connect(InetSocketAddress(host, port), 4000)
                sock.tcpNoDelay = true // enforce after connect

                socket = sock
                val outStream = sock.getOutputStream()
                val inStream = BufferedReader(InputStreamReader(sock.getInputStream(), StandardCharsets.US_ASCII))
                rawOut = outStream
                reader = inStream

                listener.onConnected()

                val greeting = inStream.readLine()
                if (greeting == null || !greeting.startsWith("PAIRING_REQUIRED")) {
                    listener.onError("Unexpected greeting: $greeting")
                    disconnect("Bad greeting")
                    return@Thread
                }

                // Authenticate: Use saved token if available, otherwise use PIN
                if (!savedToken.isNullOrEmpty()) {
                    val authLine = "AUTH_TOKEN $deviceId $savedToken\n"
                    outStream.write(authLine.toByteArray(StandardCharsets.US_ASCII))
                    outStream.flush()

                    val authReply = inStream.readLine()
                    if (authReply == "OK") {
                        isAuthenticated.set(true)
                        listener.onAuthenticated(savedToken)
                        startWorkerThreads()
                    } else {
                        listener.onAuthFailed("Token expired or rejected")
                        disconnect("Token rejected")
                    }
                } else if (!pin.isNullOrEmpty()) {
                    val authLine = "AUTH $pin $deviceId\n"
                    outStream.write(authLine.toByteArray(StandardCharsets.US_ASCII))
                    outStream.flush()

                    val authReply = inStream.readLine()
                    if (authReply != null && authReply.startsWith("OK")) {
                        isAuthenticated.set(true)
                        var receivedToken: String? = null
                        if (authReply.contains("TOKEN ")) {
                            val parts = authReply.split(" ")
                            if (parts.size >= 3) {
                                receivedToken = parts[2]
                            }
                        }
                        listener.onAuthenticated(receivedToken)
                        startWorkerThreads()
                    } else {
                        listener.onAuthFailed("PIN rejected")
                        disconnect("PIN rejected")
                    }
                } else {
                    listener.onAuthFailed("No credentials provided")
                    disconnect("No credentials")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Connection error", e)
                listener.onDisconnected(e.message ?: "Failed to connect")
                disconnect(e.message ?: "Error")
            }
        }.start()
    }

    private fun startWorkerThreads() {
        readerThread = Thread {
            try {
                val inStream = reader ?: return@Thread
                while (isRunning.get()) {
                    val line = inStream.readLine() ?: break
                    if (line.startsWith("ERR")) {
                        Log.w(TAG, "Server replied error: $line")
                        if (line == "ERR auth" || line == "ERR timeout" || line == "ERR line_too_long") {
                            listener.onError("Fatal server error: $line")
                            disconnect("Server error: $line")
                            break
                        }
                    }
                }
            } catch (e: Exception) {
                if (isRunning.get()) {
                    disconnect("Socket closed")
                }
            }
        }.apply { start() }

        // Ultra-low-latency real-time sender thread
        senderThread = Thread {
            try {
                val stream = rawOut ?: return@Thread
                while (isRunning.get()) {
                    // Sleep efficiently until motion or command arrives
                    synchronized(motionLock) {
                        while (pendingMoveX.get() == 0 && pendingMoveY.get() == 0 && controlQueue.isEmpty() && isRunning.get()) {
                            motionLock.wait(100)
                        }
                    }
                    if (!isRunning.get()) break

                    var hadData = false

                    // 1. Drain high-priority control commands immediately
                    var ctrl = controlQueue.poll()
                    while (ctrl != null) {
                        stream.write(ctrl.toByteArray(StandardCharsets.US_ASCII))
                        hadData = true
                        ctrl = controlQueue.poll()
                    }

                    // 2. Coalesce and transmit latest accumulated delta motion (0ms backlog)
                    val dx = pendingMoveX.getAndSet(0)
                    val dy = pendingMoveY.getAndSet(0)
                    if (dx != 0 || dy != 0) {
                        val clampedX = dx.coerceIn(-1000, 1000)
                        val clampedY = dy.coerceIn(-1000, 1000)
                        val moveCmd = "MOVE $clampedX $clampedY\n"
                        stream.write(moveCmd.toByteArray(StandardCharsets.US_ASCII))
                        hadData = true
                    }

                    if (hadData) {
                        stream.flush()
                    }

                    // Rate limit motion loop to ~120-150 FPS (5ms) for instant zero-latency cursor response
                    if (dx != 0 || dy != 0) {
                        try {
                            Thread.sleep(5)
                        } catch (_: InterruptedException) {}
                    }
                }
            } catch (e: Exception) {
                if (isRunning.get()) {
                    disconnect("Write error")
                }
            }
        }.apply {
            priority = Thread.MAX_PRIORITY // High-priority thread for instant responsive pointer tracking
            start()
        }

        pingExecutor = Executors.newSingleThreadScheduledExecutor().apply {
            scheduleAtFixedRate({
                if (isAuthenticated.get()) {
                    sendControl("PING\n")
                }
            }, 5, 5, TimeUnit.SECONDS)
        }
    }

    private fun sendControl(line: String) {
        if (!isAuthenticated.get()) return
        controlQueue.offer(line)
        synchronized(motionLock) {
            motionLock.notify()
        }
    }

    fun move(dx: Int, dy: Int) {
        if (!isAuthenticated.get()) return
        pendingMoveX.addAndGet(dx)
        pendingMoveY.addAndGet(dy)
        synchronized(motionLock) {
            motionLock.notify()
        }
    }

    fun buttonLeft(down: Boolean) {
        heldLeftButton = down
        val action = if (down) "DOWN" else "UP"
        sendControl("BUTTON LEFT $action\n")
    }

    fun buttonRight(down: Boolean) {
        heldRightButton = down
        val action = if (down) "DOWN" else "UP"
        sendControl("BUTTON RIGHT $action\n")
    }

    fun key(name: String) {
        sendControl("KEY ${name.uppercase()}\n")
    }

    fun sendText(text: String) {
        if (text.isEmpty()) return
        val utf8Bytes = text.toByteArray(StandardCharsets.UTF_8)
        val b64 = Base64.encodeToString(utf8Bytes, Base64.NO_WRAP)
        if (b64.length <= 6000) {
            sendControl("TEXT $b64\n")
        } else {
            var offset = 0
            while (offset < text.length) {
                val chunk = text.substring(offset, (offset + 1000).coerceAtMost(text.length))
                val chunkBytes = chunk.toByteArray(StandardCharsets.UTF_8)
                val chunkB64 = Base64.encodeToString(chunkBytes, Base64.NO_WRAP)
                sendControl("TEXT $chunkB64\n")
                offset += 1000
            }
        }
    }

    fun disconnect(reason: String = "User requested") {
        if (!isRunning.getAndSet(false)) return
        isAuthenticated.set(false)

        try {
            if (heldLeftButton) {
                rawOut?.write("BUTTON LEFT UP\n".toByteArray(StandardCharsets.US_ASCII))
                heldLeftButton = false
            }
            if (heldRightButton) {
                rawOut?.write("BUTTON RIGHT UP\n".toByteArray(StandardCharsets.US_ASCII))
                heldRightButton = false
            }
            rawOut?.flush()
        } catch (_: Exception) {}

        synchronized(motionLock) {
            motionLock.notifyAll()
        }

        try { pingExecutor?.shutdownNow() } catch (_: Exception) {}
        try { socket?.close() } catch (_: Exception) {}

        listener.onDisconnected(reason)
    }

    fun isConnected(): Boolean = isAuthenticated.get()
}
