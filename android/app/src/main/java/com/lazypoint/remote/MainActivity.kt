package com.lazypoint.remote

import android.app.Activity
import android.content.Context
import android.content.SharedPreferences
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Bundle
import android.os.Vibrator
import android.view.MotionEvent
import android.view.View
import android.widget.*
import java.util.UUID
import kotlin.math.abs

class MainActivity : Activity(), SensorEventListener, RemotePointerClient.Listener {

    private lateinit var tvDiscoveryStatus: TextView
    private lateinit var btnForget: Button
    private lateinit var etIp: EditText
    private lateinit var etPin: EditText
    private lateinit var btnConnect: Button
    private lateinit var tvStatus: TextView
    private lateinit var rgMode: RadioGroup
    private lateinit var viewTouchpad: View
    private lateinit var btnLeftClick: Button
    private lateinit var btnRightClick: Button
    private lateinit var btnRecenter: Button
    private lateinit var etTextInput: EditText
    private lateinit var btnSendText: Button

    private lateinit var btnKeyEnter: Button
    private lateinit var btnKeyBackspace: Button
    private lateinit var btnKeyEsc: Button
    private lateinit var btnKeySpace: Button

    private var client: RemotePointerClient? = null
    private var discoveryManager: DiscoveryManager? = null
    private var sensorManager: SensorManager? = null
    private var gyroSensor: Sensor? = null
    private var vibrator: Vibrator? = null
    private lateinit var prefs: SharedPreferences

    private var isAirMouseEnabled = false
    private var isMotionPaused = false
    private var sensitivity = 16.0f
    private val deadZone = 0.04f

    private var fracX = 0.0f
    private var fracY = 0.0f

    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var touchStartTime = 0L
    private var touchMoved = false

    private var deviceId: String = ""
    private var savedToken: String? = null
    private var hasAutoConnected = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        prefs = getSharedPreferences("lazy_point_prefs", Context.MODE_PRIVATE)
        deviceId = prefs.getString("device_id", null) ?: run {
            val newId = UUID.randomUUID().toString()
            prefs.edit().putString("device_id", newId).apply()
            newId
        }
        savedToken = prefs.getString("auth_token", null)

        sensorManager = getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        gyroSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        vibrator = getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator

        bindViews()
        setupListeners()
        updatePairingUI()
        startAutoDiscovery()
    }

    private fun bindViews() {
        tvDiscoveryStatus = findViewById(R.id.tvDiscoveryStatus)
        btnForget = findViewById(R.id.btnForget)
        etIp = findViewById(R.id.etIp)
        etPin = findViewById(R.id.etPin)
        btnConnect = findViewById(R.id.btnConnect)
        tvStatus = findViewById(R.id.tvStatus)
        rgMode = findViewById(R.id.rgMode)
        viewTouchpad = findViewById(R.id.viewTouchpad)
        btnLeftClick = findViewById(R.id.btnLeftClick)
        btnRightClick = findViewById(R.id.btnRightClick)
        btnRecenter = findViewById(R.id.btnRecenter)
        etTextInput = findViewById(R.id.etTextInput)
        btnSendText = findViewById(R.id.btnSendText)

        btnKeyEnter = findViewById(R.id.btnKeyEnter)
        btnKeyBackspace = findViewById(R.id.btnKeyBackspace)
        btnKeyEsc = findViewById(R.id.btnKeyEsc)
        btnKeySpace = findViewById(R.id.btnKeySpace)
    }

    private fun updatePairingUI() {
        val lastIp = prefs.getString("last_ip", "")
        if (!lastIp.isNullOrEmpty()) {
            etIp.setText(lastIp)
        }

        if (!savedToken.isNullOrEmpty()) {
            etPin.visibility = View.GONE
            btnForget.visibility = View.VISIBLE
            tvDiscoveryStatus.text = "⚡ Remembered PC (No PIN needed)"
        } else {
            etPin.visibility = View.VISIBLE
            btnForget.visibility = View.GONE
        }
    }

    private fun startAutoDiscovery() {
        discoveryManager = DiscoveryManager(this) { ip, name, port ->
            runOnUiThread {
                tvDiscoveryStatus.text = "🖥️ Found: $name ($ip)"
                etIp.setText(ip)
                prefs.edit().putString("last_ip", ip).apply()

                // Auto-connect if we have a saved token for this PC
                if (!savedToken.isNullOrEmpty() && client == null && !hasAutoConnected) {
                    hasAutoConnected = true
                    tvStatus.text = "Auto-connecting to $name..."
                    doConnect(ip, port, token = savedToken, pin = null)
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
            prefs.edit().remove("auth_token").apply()
            savedToken = null
            updatePairingUI()
            Toast.makeText(this, "Device forgotten. Enter PIN to pair again.", Toast.LENGTH_SHORT).show()
        }

        btnConnect.setOnClickListener {
            if (client == null) {
                val ip = etIp.text.toString().trim()
                if (ip.isEmpty()) {
                    Toast.makeText(this, "Enter or discover PC IP first", Toast.LENGTH_SHORT).show()
                    return@setOnClickListener
                }

                if (!savedToken.isNullOrEmpty()) {
                    doConnect(ip, 47621, token = savedToken, pin = null)
                } else {
                    val pin = etPin.text.toString().trim()
                    if (pin.length != 8) {
                        Toast.makeText(this, "Enter the 8-digit PIN shown on PC", Toast.LENGTH_SHORT).show()
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

        rgMode.setOnCheckedChangeListener { _, checkedId ->
            if (checkedId == R.id.rbAirMouse) {
                enableAirMouse(true)
                viewTouchpad.visibility = View.GONE
                btnRecenter.visibility = View.VISIBLE
            } else {
                enableAirMouse(false)
                viewTouchpad.visibility = View.VISIBLE
                btnRecenter.visibility = View.GONE
            }
        }

        btnRecenter.setOnClickListener {
            vibrate(20)
            isMotionPaused = !isMotionPaused
            btnRecenter.text = if (isMotionPaused) "Resume Motion" else "Pause Motion"
        }

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

        viewTouchpad.setOnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    lastTouchX = event.x
                    lastTouchY = event.y
                    touchStartTime = System.currentTimeMillis()
                    touchMoved = false
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (event.x - lastTouchX) * 1.5f
                    val dy = (event.y - lastTouchY) * 1.5f
                    if (abs(dx) > 1 || abs(dy) > 1) {
                        touchMoved = true
                        client?.move(dx.toInt(), dy.toInt())
                        lastTouchX = event.x
                        lastTouchY = event.y
                    }
                    true
                }
                MotionEvent.ACTION_UP -> {
                    val duration = System.currentTimeMillis() - touchStartTime
                    if (!touchMoved && duration < 250) {
                        vibrate(15)
                        client?.buttonLeft(true)
                        client?.buttonLeft(false)
                    }
                    true
                }
                MotionEvent.ACTION_POINTER_UP -> {
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

        btnSendText.setOnClickListener {
            val text = etTextInput.text.toString()
            if (text.isNotEmpty()) {
                vibrate(20)
                client?.sendText(text)
                etTextInput.setText("")
            }
        }

        btnKeyEnter.setOnClickListener { vibrate(15); client?.key("ENTER") }
        btnKeyBackspace.setOnClickListener { vibrate(15); client?.key("BACKSPACE") }
        btnKeyEsc.setOnClickListener { vibrate(15); client?.key("ESC") }
        btnKeySpace.setOnClickListener { vibrate(15); client?.key("SPACE") }
    }

    private fun doConnect(ip: String, port: Int, token: String?, pin: String?) {
        tvStatus.text = "Connecting..."
        client = RemotePointerClient(ip, port, deviceId, pin, token, this)
        client?.connect()
    }

    private fun enableAirMouse(enable: Boolean) {
        isAirMouseEnabled = enable
        if (enable) {
            gyroSensor?.let {
                sensorManager?.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME)
            }
        } else {
            sensorManager?.unregisterListener(this)
        }
    }

    override fun onSensorChanged(event: SensorEvent?) {
        if (!isAirMouseEnabled || isMotionPaused || event == null) return
        if (event.sensor.type == Sensor.TYPE_GYROSCOPE) {
            val rawPitch = event.values[0]
            val rawRoll = event.values[2]

            val gyroX = if (abs(rawRoll) > deadZone) -rawRoll else 0f
            val gyroY = if (abs(rawPitch) > deadZone) -rawPitch else 0f

            if (gyroX != 0f || gyroY != 0f) {
                val totalX = (gyroX * sensitivity) + fracX
                val totalY = (gyroY * sensitivity) + fracY

                val stepX = totalX.toInt()
                val stepY = totalY.toInt()

                fracX = totalX - stepX
                fracY = totalY - stepY

                if (stepX != 0 || stepY != 0) {
                    client?.move(stepX, stepY)
                }
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    // Protocol Listener Callbacks
    override fun onConnected() {
        runOnUiThread { tvStatus.text = "Connected (Authenticating...)" }
    }

    override fun onAuthenticated(token: String?) {
        runOnUiThread {
            vibrate(40)
            tvStatus.text = "● Connected & Paired"
            btnConnect.text = "Disconnect"

            if (!token.isNullOrEmpty()) {
                savedToken = token
                prefs.edit().putString("auth_token", token).apply()
                updatePairingUI()
            }
        }
    }

    override fun onAuthFailed(reason: String) {
        runOnUiThread {
            Toast.makeText(this, "Authentication failed: $reason", Toast.LENGTH_LONG).show()
            tvStatus.text = "Auth Failed"
            btnConnect.text = "Connect"
            client = null

            // If token failed, clear it so user can enter PIN
            if (!savedToken.isNullOrEmpty()) {
                prefs.edit().remove("auth_token").apply()
                savedToken = null
                updatePairingUI()
            }
        }
    }

    override fun onDisconnected(reason: String) {
        runOnUiThread {
            tvStatus.text = "Disconnected ($reason)"
            btnConnect.text = "Connect"
            client = null
            enableAirMouse(false)
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
        enableAirMouse(false)
        client?.disconnect()
    }
}
