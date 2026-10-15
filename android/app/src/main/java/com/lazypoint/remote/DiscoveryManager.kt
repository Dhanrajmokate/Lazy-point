package com.lazypoint.remote

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean

class DiscoveryManager(
    private val context: Context,
    private val onDiscovered: (ip: String, name: String, port: Int) -> Unit
) {
    companion object {
        private const val TAG = "DiscoveryManager"
        private const val UDP_PORT = 47622
    }

    private val isSearching = AtomicBoolean(false)
    private var socket: DatagramSocket? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    fun startDiscovery() {
        if (isSearching.getAndSet(true)) return

        Thread {
            try {
                val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
                multicastLock = wifi?.createMulticastLock("LazyPointLock")?.apply {
                    setReferenceCounted(true)
                    acquire()
                }

                val sock = DatagramSocket(null).apply {
                    reuseAddress = true
                    broadcast = true
                    bind(java.net.InetSocketAddress(0))
                }
                socket = sock

                // Send discovery probe broadcast
                val probeMsg = "LAZY_POINT_DISCOVER".toByteArray()
                val bcastAddr = InetAddress.getByName("255.255.255.255")
                val probePacket = DatagramPacket(probeMsg, probeMsg.size, bcastAddr, UDP_PORT)

                sock.send(probePacket)

                val buf = ByteArray(512)
                val recvPacket = DatagramPacket(buf, buf.size)

                while (isSearching.get()) {
                    sock.soTimeout = 2000
                    try {
                        sock.receive(recvPacket)
                        val text = String(recvPacket.data, 0, recvPacket.length).trim()
                        val senderIp = recvPacket.address.hostAddress ?: ""

                        if (text.startsWith("LAZY_POINT_OFFER") || text.startsWith("LAZY_POINT_BEACON")) {
                            val parts = text.split(" ")
                            val pcName = if (parts.size >= 2) parts[1] else "PC"
                            val port = if (parts.size >= 3) parts[2].toIntOrNull() ?: 47621 else 47621

                            mainHandler.post {
                                onDiscovered(senderIp, pcName, port)
                            }
                            // Keep listening in case user wants continuous discovery
                        }
                    } catch (_: java.net.SocketTimeoutException) {
                        // Periodic re-probe
                        if (isSearching.get()) {
                            try { sock.send(probePacket) } catch (_: Exception) {}
                        }
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Discovery stopped or error: ${e.message}")
            } finally {
                stopDiscovery()
            }
        }.start()
    }

    fun stopDiscovery() {
        isSearching.set(false)
        try { socket?.close() } catch (_: Exception) {}
        socket = null
        try {
            if (multicastLock?.isHeld == true) {
                multicastLock?.release()
            }
        } catch (_: Exception) {}
        multicastLock = null
    }
}
