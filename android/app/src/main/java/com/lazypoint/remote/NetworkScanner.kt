package com.lazypoint.remote

import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

data class DiscoveredPc(
    val ip: String,
    val name: String,
    val port: Int = 47621,
    var isPaired: Boolean = false
)

class NetworkScanner(
    private val onDeviceFound: (DiscoveredPc) -> Unit,
    private val onScanComplete: (foundCount: Int) -> Unit
) {
    companion object {
        private const val TAG = "NetworkScanner"
        private const val TCP_PORT = 47621
        private const val THREAD_COUNT = 6
        private const val CONNECT_TIMEOUT_MS = 250
    }

    private val isScanning = AtomicBoolean(false)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val foundIps = mutableSetOf<String>()

    /**
     * Discovers all candidate local subnet prefixes (especially Wi-Fi & Hotspot interfaces).
     * Filters out cellular interfaces (rmnet, ccmni, etc.) so hotspot subnets are prioritized.
     */
    fun getCandidateSubnetPrefixes(): List<String> {
        val prefixes = LinkedHashSet<String>()
        val excludedNames = listOf("rmnet", "ccmni", "pdp", "ppp", "dummy", "lo", "sit", "tun")
        val hotspotNames = listOf("ap", "wlan", "swlan", "softap", "rndis", "tether")

        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            if (interfaces != null) {
                val nonCellularIps = mutableListOf<String>()

                for (nif in interfaces) {
                    if (!nif.isUp || nif.isLoopback) continue
                    val name = nif.name.lowercase()
                    if (excludedNames.any { name.startsWith(it) }) continue

                    for (addr in nif.inetAddresses) {
                        if (!addr.isLoopbackAddress && addr is Inet4Address) {
                            val ip = addr.hostAddress ?: continue
                            if (ip.startsWith("127.")) continue
                            // Prioritize interfaces that match hotspot naming
                            if (hotspotNames.any { name.startsWith(it) }) {
                                nonCellularIps.add(0, ip)
                            } else {
                                nonCellularIps.add(ip)
                            }
                        }
                    }
                }

                for (ip in nonCellularIps) {
                    val lastDot = ip.lastIndexOf('.')
                    if (lastDot > 0) {
                        val prefix = ip.substring(0, lastDot + 1)
                        prefixes.add(prefix)
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get local subnets", e)
        }

        // Standard Android mobile hotspot defaults if not already detected
        if (!prefixes.any { it.startsWith("192.168.43.") }) {
            prefixes.add("192.168.43.")
        }

        return prefixes.toList()
    }

    fun startScan() {
        if (isScanning.getAndSet(true)) return
        foundIps.clear()

        Thread {
            val candidatePrefixes = getCandidateSubnetPrefixes()
            if (candidatePrefixes.isEmpty()) {
                isScanning.set(false)
                mainHandler.post { onScanComplete(0) }
                return@Thread
            }

            Log.d(TAG, "Scanning candidate subnets: $candidatePrefixes")
            val executor = Executors.newFixedThreadPool(THREAD_COUNT)
            val totalTargets = candidatePrefixes.size * 254
            val remainingTasks = java.util.concurrent.atomic.AtomicInteger(totalTargets)

            for (prefix in candidatePrefixes) {
                for (i in 1..254) {
                    val targetIp = "$prefix$i"
                    executor.submit {
                        try {
                            val sock = Socket()
                            sock.connect(InetSocketAddress(targetIp, TCP_PORT), CONNECT_TIMEOUT_MS)
                            sock.soTimeout = 400
                            val reader = BufferedReader(InputStreamReader(sock.getInputStream(), StandardCharsets.US_ASCII))
                            val greeting = reader.readLine()
                            sock.close()

                            if (greeting != null && greeting.startsWith("PAIRING_REQUIRED")) {
                                var pcName = "Lazy Point PC"
                                val parts = greeting.trim().split(" ")
                                if (parts.size >= 2 && parts[1].isNotEmpty()) {
                                    pcName = parts[1]
                                }

                                synchronized(foundIps) {
                                    if (foundIps.add(targetIp)) {
                                        val dev = DiscoveredPc(targetIp, pcName, TCP_PORT)
                                        mainHandler.post { onDeviceFound(dev) }
                                    }
                                }
                            }
                        } catch (_: Exception) {
                            // Target not active on this port
                        } finally {
                            if (remainingTasks.decrementAndGet() == 0) {
                                isScanning.set(false)
                                executor.shutdown()
                                mainHandler.post { onScanComplete(foundIps.size) }
                            }
                        }
                    }
                }
            }
        }.start()
    }
}
