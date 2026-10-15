package com.lazypoint.remote

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Bundle
import android.os.Vibrator
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.*
import org.json.JSONObject
import java.util.Locale
import java.util.UUID
import kotlin.math.abs

class MainActivity : Activity(), RemotePointerClient.Listener {

    private lateinit var tvDiscoveryStatus: TextView
    private lateinit var btnDevicesList: Button
    private lateinit var btnForget: Button
    private lateinit var etIp: EditText
    private lateinit var etPin: EditText
    private lateinit var btnScanQr: Button
    private lateinit var btnConnect: Button
    private lateinit var tvStatus: TextView
    private lateinit var tvSensitivityLabel: TextView
    private lateinit var sbSensitivity: SeekBar
    private var sensitivityMultiplier = 1.0f
    private lateinit var viewTouchpad: View
    private lateinit var btnLeftClick: Button
    private lateinit var btnRightClick: Button

    private lateinit var btnKeyVolDown: Button
    private lateinit var btnKeyVolUp: Button
    private lateinit var btnKeyPlayPause: Button
    private lateinit var btnKeyMute: Button
    private lateinit var btnKeyTab: Button
    private lateinit var btnKeyEsc: Button

    private lateinit var btnKeySpace: Button
    private lateinit var btnKeyBackspace: Button
    private lateinit var btnKeyEnter: Button
    private lateinit var btnKeyLeft: Button
    private lateinit var btnKeyUp: Button
    private lateinit var btnKeyDown: Button
    private lateinit var btnKeyRight: Button

    private lateinit var btnShowKeyboard: Button
    private lateinit var etTextInput: EditText
    private lateinit var btnSendText: Button

    private var client: RemotePointerClient? = null
    private var discoveryManager: DiscoveryManager? = null
    private var networkScanner: NetworkScanner? = null
    private var vibrator: Vibrator? = null
    private lateinit var prefs: SharedPreferences

    // Sub-pixel fractional accumulators for buttery smooth touchpad motion
    private var touchFracX = 0.0f
    private var touchFracY = 0.0f
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var touchStartTime = 0L
    private var touchMoved = false

    private var deviceId: String = ""
    private var hasAutoConnected = false
    private val discoveredDevices = mutableMapOf<String, DiscoveredPc>()

    companion object {
        private const val REQUEST_CODE_QR = 1001
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Prevent silent closes by persisting any uncaught exception
        val prevHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            Log.e("LazyPoint", "Uncaught exception on ${thread.name}", throwable)
            try {
                val sp = getSharedPreferences("lazy_point_prefs", Context.MODE_PRIVATE)
                sp.edit().putString("last_crash", "${throwable.javaClass.simpleName}: ${throwable.message}\n${throwable.stackTraceToString()}").apply()
            } catch (_: Exception) {}
            prevHandler?.uncaughtException(thread, throwable)
        }

        try {
            setContentView(R.layout.activity_main)

            prefs = getSharedPreferences("lazy_point_prefs", Context.MODE_PRIVATE)
            deviceId = prefs.getString("device_id", null) ?: run {
                val newId = UUID.randomUUID().toString()
                prefs.edit().putString("device_id", newId).apply()
                newId
            }

            vibrator = getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator

            bindViews()
            setupListeners()
            updatePairingUI()

            // Check if there was a previous crash to show the user
            val lastCrash = prefs.getString("last_crash", null)
            if (!lastCrash.isNullOrEmpty()) {
                prefs.edit().remove("last_crash").apply()
                AlertDialog.Builder(this)
                    .setTitle("Startup Diagnosis")
                    .setMessage("App recovered from unexpected error:\n\n$lastCrash")
                    .setPositiveButton("OK", null)
                    .show()
            }

            // Start background UDP discovery only (lightweight, instant, zero thread-storm)
            startAutoDiscovery()
        } catch (t: Throwable) {
            Log.e("LazyPoint", "Fatal error in onCreate", t)
            Toast.makeText(this, "Startup error: ${t.javaClass.simpleName} - ${t.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun bindViews() {
        tvDiscoveryStatus = findViewById(R.id.tvDiscoveryStatus)
        btnDevicesList = findViewById(R.id.btnDevicesList)
        btnForget = findViewById(R.id.btnForget)
        etIp = findViewById(R.id.etIp)
        etPin = findViewById(R.id.etPin)
        btnScanQr = findViewById(R.id.btnScanQr)
        btnConnect = findViewById(R.id.btnConnect)
        tvStatus = findViewById(R.id.tvStatus)
        tvSensitivityLabel = findViewById(R.id.tvSensitivityLabel)
        sbSensitivity = findViewById(R.id.sbSensitivity)
        viewTouchpad = findViewById(R.id.viewTouchpad)
        btnLeftClick = findViewById(R.id.btnLeftClick)
        btnRightClick = findViewById(R.id.btnRightClick)
        btnKeyVolDown = findViewById(R.id.btnKeyVolDown)
        btnKeyVolUp = findViewById(R.id.btnKeyVolUp)
        btnKeyPlayPause = findViewById(R.id.btnKeyPlayPause)
        btnKeyMute = findViewById(R.id.btnKeyMute)
        btnKeyTab = findViewById(R.id.btnKeyTab)
        btnKeyEsc = findViewById(R.id.btnKeyEsc)

        btnKeySpace = findViewById(R.id.btnKeySpace)
        btnKeyBackspace = findViewById(R.id.btnKeyBackspace)
        btnKeyEnter = findViewById(R.id.btnKeyEnter)
        btnKeyLeft = findViewById(R.id.btnKeyLeft)
        btnKeyUp = findViewById(R.id.btnKeyUp)
        btnKeyDown = findViewById(R.id.btnKeyDown)
        btnKeyRight = findViewById(R.id.btnKeyRight)

        btnShowKeyboard = findViewById(R.id.btnShowKeyboard)
        etTextInput = findViewById(R.id.etTextInput)
        btnSendText = findViewById(R.id.btnSendText)
    }

    // Persistent token and PC memory helpers
    private fun getSavedToken(ip: String): String? {
        val t = prefs.getString("token_$ip", null)
        if (!t.isNullOrEmpty()) return t
        return prefs.getString("auth_token", null)
    }

    private fun getSavedName(ip: String): String? {
        return prefs.getString("name_$ip", null)
    }

    private fun saveDevicePairing(ip: String, name: String?, token: String) {
        val editor = prefs.edit()
        editor.putString("token_$ip", token)
        editor.putString("auth_token", token)
        editor.putString("last_ip", ip)
        if (!name.isNullOrEmpty()) {
            editor.putString("name_$ip", name)
        }
        val ips = prefs.getStringSet("remembered_ips", mutableSetOf())?.toMutableSet() ?: mutableSetOf()
        ips.add(ip)
        editor.putStringSet("remembered_ips", ips)
        editor.apply()
    }

    private fun removeDevicePairing(ip: String) {
        val editor = prefs.edit()
        editor.remove("token_$ip")
        editor.remove("name_$ip")
        if (prefs.getString("last_ip", "") == ip) {
            editor.remove("auth_token")
        }
        val ips = prefs.getStringSet("remembered_ips", mutableSetOf())?.toMutableSet() ?: mutableSetOf()
        ips.remove(ip)
        editor.putStringSet("remembered_ips", ips)
        editor.apply()
    }

    private fun getRememberedIps(): Set<String> {
        return prefs.getStringSet("remembered_ips", emptySet()) ?: emptySet()
    }

    private fun updatePairingUI() {
        val currentIp = etIp.text.toString().trim().ifEmpty { prefs.getString("last_ip", "") ?: "" }
        if (currentIp.isNotEmpty() && etIp.text.toString().isEmpty()) {
            etIp.setText(currentIp)
        }

        val token = if (currentIp.isNotEmpty()) getSavedToken(currentIp) else null
        val name = if (currentIp.isNotEmpty()) getSavedName(currentIp) else null

        if (!token.isNullOrEmpty()) {
            etPin.visibility = View.GONE
            btnForget.visibility = View.VISIBLE
            val label = if (!name.isNullOrEmpty()) name else currentIp
            tvDiscoveryStatus.text = "⚡ Remembered: $label (No PIN needed)"
        } else {
            etPin.visibility = View.VISIBLE
            btnForget.visibility = View.GONE
        }
    }

    private fun launchQrScanner() {
        vibrate(20)
        try {
            val intent = Intent("com.google.zxing.client.android.SCAN")
            intent.putExtra("SCAN_MODE", "QR_CODE_MODE")
            startActivityForResult(intent, REQUEST_CODE_QR)
        } catch (_: Exception) {
            AlertDialog.Builder(this)
                .setTitle("📷 Scan PC Screen QR")
                .setMessage("Point your phone's normal Camera or Google Lens at the QR code on your PC screen.\n\nOr choose 'Select PC' to connect automatically!")
                .setPositiveButton("📋 Select PC") { _, _ -> showDevicePicker() }
                .setNegativeButton("OK", null)
                .show()
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_CODE_QR && resultCode == RESULT_OK) {
            val contents = data?.getStringExtra("SCAN_RESULT")
            if (!contents.isNullOrEmpty()) {
                handleScannedQr(contents)
            }
        }
    }

    private fun handleScannedQr(content: String) {
        vibrate(30)
        var ip: String? = null
        var port = 47621
        var pin: String? = null
        var name: String? = null

        try {
            if (content.startsWith("{") && content.endsWith("}")) {
                val json = JSONObject(content)
                ip = json.optString("ip")
                port = json.optInt("port", 47621)
                pin = json.optString("pin")
                name = json.optString("name")
            } else if (content.startsWith("LAZYPOINT://", ignoreCase = true) || content.startsWith("http://", ignoreCase = true)) {
                val uri = Uri.parse(content)
                ip = uri.host
                if (uri.port > 0) port = uri.port
                pin = uri.getQueryParameter("pin")
                name = uri.getQueryParameter("name")
            } else if (content.startsWith("LAZYPOINT:", ignoreCase = true)) {
                val parts = content.split(":")
                if (parts.size >= 2) ip = parts[1].replace("/", "")
                if (parts.size >= 3) port = parts[2].toIntOrNull() ?: 47621
                if (parts.size >= 4) pin = parts[3]
            } else if (content.contains(":")) {
                val parts = content.split(":")
                ip = parts[0].trim()
                if (parts.size >= 2) {
                    if (parts[1].length == 8 && parts[1].all { it.isDigit() }) {
                        pin = parts[1].trim()
                    } else {
                        port = parts[1].toIntOrNull() ?: 47621
                        if (parts.size >= 3) pin = parts[2].trim()
                    }
                }
            }
        } catch (e: Exception) {
            Log.e("MainActivity", "Failed to parse QR: $content", e)
        }

        if (ip.isNullOrEmpty()) {
            Toast.makeText(this, "Could not extract PC IP from QR code", Toast.LENGTH_LONG).show()
            return
        }

        etIp.setText(ip)
        if (!pin.isNullOrEmpty()) {
            etPin.setText(pin)
        }
        if (!name.isNullOrEmpty()) {
            val editor = prefs.edit()
            editor.putString("name_$ip", name)
            editor.apply()
            tvDiscoveryStatus.text = "🖥️ $name ($ip)"
        }

        Toast.makeText(this, "QR Scanned! Connecting to $ip...", Toast.LENGTH_SHORT).show()
        val token = getSavedToken(ip)
        doConnect(ip, port, token = token, pin = pin)
    }

    private fun showDevicePicker() {
        vibrate(15)
        val list = mutableListOf<DiscoveredPc>()

        // 1. Add all devices discovered from network scan / UDP
        list.addAll(discoveredDevices.values)

        // 2. Add all previously remembered PCs from prefs
        val rememberedIps = getRememberedIps()
        for (ip in rememberedIps) {
            if (list.none { it.ip == ip }) {
                val name = getSavedName(ip) ?: "Remembered PC"
                list.add(DiscoveredPc(ip, name, 47621, isPaired = true))
            }
        }

        // Tag paired status
        for (dev in list) {
            dev.isPaired = !getSavedToken(dev.ip).isNullOrEmpty()
        }

        val items = mutableListOf<String>()
        for (d in list) {
            val status = if (d.isPaired) "⚡ Paired" else "Needs PIN"
            items.add("🖥️ ${d.name} (${d.ip}) • $status")
        }
        items.add("🔄 Rescan Local Network")
        items.add("📷 Scan PC QR Code")

        AlertDialog.Builder(this)
            .setTitle("Select Lazy Point PC")
            .setItems(items.toTypedArray()) { _, which ->
                if (which < list.size) {
                    val chosen = list[which]
                    etIp.setText(chosen.ip)
                    val token = getSavedToken(chosen.ip)
                    if (!token.isNullOrEmpty()) {
                        tvDiscoveryStatus.text = "⚡ Remembered: ${chosen.name}"
                        doConnect(chosen.ip, chosen.port, token = token, pin = null)
                    } else {
                        tvDiscoveryStatus.text = "🖥️ ${chosen.name} (${chosen.ip})"
                        updatePairingUI()
                        Toast.makeText(this, "Selected ${chosen.name}. Scan QR or enter PIN to pair.", Toast.LENGTH_SHORT).show()
                    }
                } else if (which == list.size) {
                    startNetworkScan(showToast = true)
                } else {
                    launchQrScanner()
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun startNetworkScan(showToast: Boolean = false) {
        val dummy = NetworkScanner({}, {})
        val prefixes = dummy.getCandidateSubnetPrefixes()
        val summary = prefixes.joinToString(", ")
        if (showToast) Toast.makeText(this, "Scanning subnets: $summary", Toast.LENGTH_SHORT).show()
        tvDiscoveryStatus.text = "🔍 Scanning: $summary"

        networkScanner = NetworkScanner(
            onDeviceFound = { pc ->
                runOnUiThread {
                    discoveredDevices[pc.ip] = pc
                    tvDiscoveryStatus.text = "🖥️ Found: ${pc.name} (${pc.ip})"
                    if (etIp.text.toString().isEmpty()) {
                        etIp.setText(pc.ip)
                        updatePairingUI()
                    }
                }
            },
            onScanComplete = { count ->
                runOnUiThread {
                    if (count > 0) {
                        tvDiscoveryStatus.text = "✓ Found $count PC(s). Tap Devices to choose."
                    } else if (discoveredDevices.isEmpty()) {
                        tvDiscoveryStatus.text = "No PCs found on $summary. Tap QR to scan."
                    }
                }
            }
        )
        networkScanner?.startScan()
    }

    private fun startAutoDiscovery() {
        discoveryManager = DiscoveryManager(this) { ip, name, port ->
            runOnUiThread {
                discoveredDevices[ip] = DiscoveredPc(ip, name, port)
                tvDiscoveryStatus.text = "🖥️ Found: $name ($ip)"
                if (etIp.text.toString().isEmpty()) {
                    etIp.setText(ip)
                }

                // Auto-connect if we have a saved token for this PC
                val token = getSavedToken(ip)
                if (!token.isNullOrEmpty() && client == null && !hasAutoConnected) {
                    hasAutoConnected = true
                    tvStatus.text = "Auto-connecting to $name..."
                    doConnect(ip, port, token = token, pin = null)
                }
            }
        }
        discoveryManager?.startDiscovery()
    }

    private fun vibrate(ms: Long = 15) {
        try { vibrator?.vibrate(ms) } catch (_: Exception) {}
    }

    private fun setupListeners() {
        btnForget.setOnClickListener {
            vibrate(20)
            val currentIp = etIp.text.toString().trim()
            if (currentIp.isNotEmpty()) {
                removeDevicePairing(currentIp)
            }
            prefs.edit().remove("auth_token").apply()
            updatePairingUI()
            Toast.makeText(this, "Device forgotten. Scan QR or enter PIN to pair again.", Toast.LENGTH_SHORT).show()
        }

        btnScanQr.setOnClickListener {
            launchQrScanner()
        }

        btnDevicesList.setOnClickListener {
            showDevicePicker()
        }

        btnConnect.setOnClickListener {
            if (client == null) {
                val ip = etIp.text.toString().trim()
                if (ip.isEmpty()) {
                    Toast.makeText(this, "Enter IP, scan QR, or tap Devices", Toast.LENGTH_SHORT).show()
                    return@setOnClickListener
                }

                val token = getSavedToken(ip)
                if (!token.isNullOrEmpty()) {
                    doConnect(ip, 47621, token = token, pin = null)
                } else {
                    val pin = etPin.text.toString().trim()
                    if (pin.length != 8) {
                        Toast.makeText(this, "Enter 8-digit PIN or tap Scan QR", Toast.LENGTH_SHORT).show()
                        return@setOnClickListener
                    }
                    doConnect(ip, 47621, token = null, pin = pin)
                }
            } else {
                client?.disconnect("User disconnected")
                client = null
                onDisconnected("Disconnected")
            }
        }

        // Initialize and handle real-time Sensitivity Slider
        val savedSensitivityProgress = prefs.getInt("sensitivity_progress", 80)
        sbSensitivity.progress = savedSensitivityProgress
        sensitivityMultiplier = (savedSensitivityProgress + 20) / 100f
        tvSensitivityLabel.text = String.format(Locale.US, "⚡ Cursor Sensitivity: %.1fx", sensitivityMultiplier)

        sbSensitivity.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                sensitivityMultiplier = (progress + 20) / 100f
                tvSensitivityLabel.text = String.format(Locale.US, "⚡ Cursor Sensitivity: %.1fx", sensitivityMultiplier)
                if (fromUser && progress % 15 == 0) {
                    vibrate(5)
                }
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {
                val progress = seekBar?.progress ?: 80
                prefs.edit().putInt("sensitivity_progress", progress).apply()
            }
        })

        btnLeftClick.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    vibrate(15)
                    client?.buttonLeft(true)
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    client?.buttonLeft(false)
                    true
                }
                else -> false
            }
        }

        btnRightClick.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    vibrate(20)
                    client?.buttonRight(true)
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    client?.buttonRight(false)
                    true
                }
                else -> false
            }
        }

        // Primary Touchpad: Smooth gesture motion and taps
        viewTouchpad.setOnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    lastTouchX = event.x
                    lastTouchY = event.y
                    touchStartTime = System.currentTimeMillis()
                    touchMoved = false
                    touchFracX = 0f
                    touchFracY = 0f
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val rawDx = event.x - lastTouchX
                    val rawDy = event.y - lastTouchY
                    lastTouchX = event.x
                    lastTouchY = event.y

                    // Natural velocity acceleration: pinpoint precision for micro-adjustments, effortless glide for fast swipes
                    val dist = kotlin.math.hypot(rawDx, rawDy)
                    val accel = when {
                        dist < 2.5f -> 1.0f
                        dist < 12.0f -> 1.0f + ((dist - 2.5f) / 9.5f) * 0.6f // 1.0x to 1.6x
                        else -> (1.6f + ((dist - 12.0f) / 18.0f) * 0.9f).coerceAtMost(3.0f) // up to 3.0x
                    }

                    val speed = 1.4f * sensitivityMultiplier * accel
                    val totalDx = (rawDx * speed) + touchFracX
                    val totalDy = (rawDy * speed) + touchFracY
                    val stepX = totalDx.toInt()
                    val stepY = totalDy.toInt()
                    touchFracX = totalDx - stepX
                    touchFracY = totalDy - stepY

                    if (stepX != 0 || stepY != 0) {
                        touchMoved = true
                        client?.move(stepX, stepY)
                    }
                    true
                }
                MotionEvent.ACTION_UP -> {
                    val duration = System.currentTimeMillis() - touchStartTime
                    // Quick tap = Left Click
                    if (!touchMoved && duration < 250) {
                        vibrate(15)
                        client?.buttonLeft(true)
                        client?.buttonLeft(false)
                    }
                    true
                }
                MotionEvent.ACTION_POINTER_UP -> {
                    // Two-finger tap = Right Click
                    if (event.pointerCount == 2) {
                        vibrate(25)
                        client?.buttonRight(true)
                        client?.buttonRight(false)
                    }
                    true
                }
                else -> false
            }
        }

        btnShowKeyboard.setOnClickListener {
            vibrate(15)
            etTextInput.requestFocus()
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
            imm?.showSoftInput(etTextInput, InputMethodManager.SHOW_IMPLICIT)
        }

        etTextInput.setOnEditorActionListener { _, actionId, event ->
            if (actionId == EditorInfo.IME_ACTION_SEND ||
                actionId == EditorInfo.IME_ACTION_DONE ||
                (event != null && event.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)) {
                val text = etTextInput.text.toString()
                if (text.isNotEmpty()) {
                    vibrate(20)
                    client?.sendText(text)
                    etTextInput.setText("")
                } else {
                    vibrate(15)
                    client?.key("ENTER")
                }
                true
            } else {
                false
            }
        }

        btnSendText.setOnClickListener {
            val text = etTextInput.text.toString()
            if (text.isNotEmpty()) {
                vibrate(20)
                client?.sendText(text)
                etTextInput.setText("")
            } else {
                vibrate(15)
                client?.key("ENTER")
            }
        }

        // Media Controls
        btnKeyVolDown.setOnClickListener { vibrate(15); client?.key("VOLDOWN") }
        btnKeyVolUp.setOnClickListener { vibrate(15); client?.key("VOLUP") }
        btnKeyPlayPause.setOnClickListener { vibrate(15); client?.key("PLAYPAUSE") }
        btnKeyMute.setOnClickListener { vibrate(15); client?.key("MUTE") }
        btnKeyTab.setOnClickListener { vibrate(15); client?.key("TAB") }
        btnKeyEsc.setOnClickListener { vibrate(15); client?.key("ESC") }

        // Navigation & Arrow Controls
        btnKeySpace.setOnClickListener { vibrate(15); client?.key("SPACE") }
        btnKeyBackspace.setOnClickListener { vibrate(15); client?.key("BACKSPACE") }
        btnKeyEnter.setOnClickListener { vibrate(15); client?.key("ENTER") }
        btnKeyLeft.setOnClickListener { vibrate(15); client?.key("LEFT") }
        btnKeyUp.setOnClickListener { vibrate(15); client?.key("UP") }
        btnKeyDown.setOnClickListener { vibrate(15); client?.key("DOWN") }
        btnKeyRight.setOnClickListener { vibrate(15); client?.key("RIGHT") }
    }

    private fun doConnect(ip: String, port: Int, token: String?, pin: String?) {
        client?.disconnect("Reconnecting")
        client = null
        tvStatus.text = "Connecting to $ip..."
        client = RemotePointerClient(ip, port, deviceId, pin, token, this)
        client?.connect()
    }

    // Protocol Listener Callbacks
    override fun onConnected() {
        runOnUiThread { tvStatus.text = "Connected (Authenticating...)" }
    }

    override fun onAuthenticated(token: String?) {
        runOnUiThread {
            vibrate(40)
            tvStatus.text = "● Connected & Paired"
            btnConnect.text = "Disconnect"

            val targetIp = etIp.text.toString().trim()
            val pcName = getSavedName(targetIp) ?: "PC"
            if (!token.isNullOrEmpty()) {
                saveDevicePairing(targetIp, pcName, token)
            }
            updatePairingUI()
            Toast.makeText(this, "✓ Paired & Remembered!", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onAuthFailed(reason: String) {
        runOnUiThread {
            val targetIp = etIp.text.toString().trim()
            removeDevicePairing(targetIp)
            updatePairingUI()
            Toast.makeText(this, "Authentication failed ($reason). Scan QR or enter PIN.", Toast.LENGTH_LONG).show()
            tvStatus.text = "Auth Failed"
            btnConnect.text = "Connect"
            client = null
        }
    }

    override fun onDisconnected(reason: String) {
        runOnUiThread {
            tvStatus.text = "Disconnected ($reason)"
            btnConnect.text = "Connect"
            client = null
        }
    }

    override fun onError(message: String) {
        runOnUiThread {
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        discoveryManager?.stopDiscovery()
        client?.disconnect()
    }
}
