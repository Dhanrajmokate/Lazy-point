package com.lazypoint.remote

import android.app.Activity
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Bundle
import android.os.Vibrator
import android.view.MotionEvent
import android.view.View
import android.widget.*
import kotlin.math.abs

class MainActivity : Activity(), SensorEventListener, RemotePointerClient.Listener {

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

    // Quick keys
    private lateinit var btnKeyEnter: Button
    private lateinit var btnKeyBackspace: Button
    private lateinit var btnKeyEsc: Button
    private lateinit var btnKeySpace: Button

    private var client: RemotePointerClient? = null
    private var sensorManager: SensorManager? = null
    private var gyroSensor: Sensor? = null
    private var vibrator: Vibrator? = null

    private var isAirMouseEnabled = false
    private var isMotionPaused = false
    private var sensitivity = 16.0f
    private val deadZone = 0.04f

    private var fracX = 0.0f
    private var fracY = 0.0f

    // Touchpad state
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var touchStartTime = 0L
    private var touchMoved = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        sensorManager = getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        gyroSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        vibrator = getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator

        bindViews()
        setupListeners()
    }

    private fun bindViews() {
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

    private fun vibrate(ms: Long = 15) {
        try { vibrator?.vibrate(ms) } catch (_: Exception) {}
    }

    private fun setupListeners() {
        btnConnect.setOnClickListener {
            if (client == null) {
                val ip = etIp.text.toString().trim()
                val pin = etPin.text.toString().trim()
                if (ip.isEmpty() || pin.length != 8) {
                    Toast.makeText(this, "Enter valid IP and 8-digit PIN", Toast.LENGTH_SHORT).show()
                    return@setOnClickListener
                }
                tvStatus.text = "Connecting..."
                client = RemotePointerClient(ip, 47621, pin, this)
                client?.connect()
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

        // Mouse buttons: DOWN on press, UP on release
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

        // Touchpad fallback listener
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

        // Text input and special keys
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
            val rawPitch = event.values[0] // X-axis angular speed
            val rawYaw = event.values[1]   // Y-axis angular speed
            val rawRoll = event.values[2]  // Z-axis angular speed

            // Filter dead zone
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

    override fun onAuthenticated() {
        runOnUiThread {
            vibrate(40)
            tvStatus.text = "● Connected & Paired"
            btnConnect.text = "Disconnect"
        }
    }

    override fun onAuthFailed() {
        runOnUiThread {
            Toast.makeText(this, "Authentication failed. Check PIN.", Toast.LENGTH_LONG).show()
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
        enableAirMouse(false)
        client?.disconnect()
    }
}
