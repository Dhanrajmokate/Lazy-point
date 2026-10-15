package com.lazypoint.remote

import android.util.Base64
import android.util.Log
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

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
    private var writer: BufferedWriter? = null
    private var reader: BufferedReader? = null

    private val isRunning = AtomicBoolean(false)
    private val isAuthenticated = AtomicBoolean(false)
    private var heldLeftButton = false
    private var heldRightButton = false

    private val commandQueue = ConcurrentLinkedQueue<String>()
    private var senderThread: Thread? = null
    private var readerThread: Thread? = null
    private var pingExecutor: ScheduledExecutorService? = null

    companion object {
        private const val TAG = "RemotePointerClient"
        private const val MAX_MOTION_QUEUE = 3
    }

    fun connect() {
        if (isRunning.get()) return
        isRunning.set(true)

        Thread {
            try {
                val sock = Socket()
                sock.tcpNoDelay = true
                sock.connect(InetSocketAddress(host, port), 4000)

                socket = sock
                val outStream = BufferedWriter(OutputStreamWriter(sock.getOutputStream(), StandardCharsets.US_ASCII))
                val inStream = BufferedReader(InputStreamReader(sock.getInputStream(), StandardCharsets.US_ASCII))
                writer = outStream
                reader = inStream

                listener.onConnected()

                val greeting = inStream.readLine()
                if (greeting != "PAIRING_REQUIRED") {
                    listener.onError("Unexpected greeting: $greeting")
                    disconnect("Bad greeting")
                    return@Thread
                }

                // Authenticate: Use saved token if available, otherwise use PIN
                if (!savedToken.isNullOrEmpty()) {
                    outStream.write("AUTH_TOKEN $deviceId $savedToken\n")
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
                    outStream.write("AUTH $pin $deviceId\n")
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
                        if (line == "ERR auth" || line == "ERR timeout" || line == "ERR line_too_long" || line == "ERR input") {
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

        senderThread = Thread {
            try {
                val outStream = writer ?: return@Thread
                while (isRunning.get()) {
                    val cmd = commandQueue.poll()
                    if (cmd != null) {
                        outStream.write(cmd)
                        outStream.flush()
                    } else {
                        Thread.sleep(2)
                    }
                }
            } catch (e: Exception) {
                if (isRunning.get()) {
                    disconnect("Write error")
                }
            }
        }.apply { start() }

        pingExecutor = Executors.newSingleThreadScheduledExecutor().apply {
            scheduleAtFixedRate({
                if (isAuthenticated.get()) {
                    sendCommand("PING\n", isMovement = false)
                }
            }, 5, 5, TimeUnit.SECONDS)
        }
    }

    private fun sendCommand(line: String, isMovement: Boolean = false) {
        if (!isAuthenticated.get()) return

        if (isMovement) {
            if (commandQueue.size > MAX_MOTION_QUEUE) {
                commandQueue.poll()
            }
        }
        commandQueue.offer(line)
    }

    fun move(dx: Int, dy: Int) {
        val clampedX = dx.coerceIn(-500, 500)
        val clampedY = dy.coerceIn(-500, 500)
        sendCommand("MOVE $clampedX $clampedY\n", isMovement = true)
    }

    fun buttonLeft(down: Boolean) {
        heldLeftButton = down
        val action = if (down) "DOWN" else "UP"
        sendCommand("BUTTON LEFT $action\n")
    }

    fun buttonRight(down: Boolean) {
        heldRightButton = down
        val action = if (down) "DOWN" else "UP"
        sendCommand("BUTTON RIGHT $action\n")
    }

    fun key(name: String) {
        sendCommand("KEY ${name.uppercase()}\n")
    }

    fun sendText(text: String) {
        if (text.isEmpty()) return
        val utf8Bytes = text.toByteArray(StandardCharsets.UTF_8)
        val b64 = Base64.encodeToString(utf8Bytes, Base64.NO_WRAP)
        if (b64.length <= 6000) {
            sendCommand("TEXT $b64\n")
        } else {
            var offset = 0
            while (offset < text.length) {
                val chunk = text.substring(offset, (offset + 1000).coerceAtMost(text.length))
                val chunkBytes = chunk.toByteArray(StandardCharsets.UTF_8)
                val chunkB64 = Base64.encodeToString(chunkBytes, Base64.NO_WRAP)
                sendCommand("TEXT $chunkB64\n")
                offset += 1000
            }
        }
    }

    fun disconnect(reason: String = "User requested") {
        if (!isRunning.getAndSet(false)) return
        isAuthenticated.set(false)

        try {
            if (heldLeftButton) {
                writer?.write("BUTTON LEFT UP\n")
                heldLeftButton = false
            }
            if (heldRightButton) {
                writer?.write("BUTTON RIGHT UP\n")
                heldRightButton = false
            }
            writer?.flush()
        } catch (_: Exception) {}

        try { pingExecutor?.shutdownNow() } catch (_: Exception) {}
        try { socket?.close() } catch (_: Exception) {}

        listener.onDisconnected(reason)
    }
}
