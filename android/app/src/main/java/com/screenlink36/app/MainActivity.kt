package com.screenlink36.app

import android.app.Activity
import android.app.AlertDialog
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Rect
import android.os.Bundle
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import java.net.NetworkInterface
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.cos
import kotlin.math.sin

class MainActivity : Activity(), SurfaceHolder.Callback, StreamServer.Ui {

    private lateinit var root: FrameLayout
    private lateinit var video: SurfaceView
    private lateinit var cursor: ImageView
    private lateinit var status: TextView
    private lateinit var softTarget: SoftKeyboardTarget
    private lateinit var pairButton: Button
    private lateinit var menu: SideMenu
    private var audioOnTablet = false
    private var displayMode = SideMenu.MODE_EXTENDED
    private var frameRate = 120
    private var rotationLocked = false
    private var touchEnabled = true
    private var edgeSwipeStart = -1f
    private var edgeSwipeActive = false
    private var softKeyboardOpen = false
    private var server: StreamServer? = null
    private var streamWidth = 0
    private var streamHeight = 0
    private var hotX = 0
    private var hotY = 0
    private var cursorX = 0
    private var cursorY = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.setDecorFitsSystemWindows(false)
        window.attributes.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        video = SurfaceView(this)
        video.holder.addCallback(this)
        video.setOnTouchListener { _, event ->
            if (isStylus(event)) sendPen(event) else sendTouch(event)
            true
        }
        video.setOnHoverListener { _, event ->
            if (isStylus(event)) sendPen(event)
            true
        }
        cursor = ImageView(this).apply { visibility = View.GONE }
        status = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 20f
            gravity = Gravity.CENTER
        }
        root.addView(video, FrameLayout.LayoutParams(MATCH, MATCH, Gravity.CENTER))
        root.addView(cursor, FrameLayout.LayoutParams(WRAP, WRAP))
        root.addView(status, FrameLayout.LayoutParams(MATCH, WRAP, Gravity.CENTER))
        softTarget = SoftKeyboardTarget(this) { server?.sendInput(it) }
        root.addView(softTarget, FrameLayout.LayoutParams(1, 1))
        val buttonSize = (56 * resources.displayMetrics.density).toInt()
        menu = SideMenu(this, menuActions)
        refreshMenu()
        root.addView(menu.view, FrameLayout.LayoutParams(MATCH, MATCH))
        pairButton = Button(this).apply {
            text = "Nouvel appairage"
            setOnClickListener {
                server?.armPairing()
                status.text = "Appairage autorisé : lancez 36Screenlink sur le PC"
            }
        }
        root.addView(pairButton, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = buttonSize
        })
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING)
        setContentView(root)
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            applyAspect()
            reserveLeftEdge()
        }
        hideSystemBars()
    }

    // Sans cette réservation, Android traite le glissement depuis le bord comme un retour.
    private fun reserveLeftEdge() {
        val width = (EDGE_DP * resources.displayMetrics.density).toInt()
        val height = (GESTURE_LIMIT_DP * resources.displayMetrics.density).toInt().coerceAtMost(root.height)
        val top = (root.height - height) / 2
        root.systemGestureExclusionRects = listOf(Rect(0, top, width, top + height))
    }

    private val menuActions = object : SideMenu.Actions {
        override fun onAudioToggle() {
            server?.requestAudioRoute(!audioOnTablet)
        }

        override fun onKeyboard() = toggleSoftKeyboard()

        override fun onDisplayMode(mode: Int) {
            server?.sendControl(Protocol.CONTROL_DISPLAY, mode)
        }

        override fun onFrameRateToggle() {
            server?.sendControl(Protocol.CONTROL_FPS, if (frameRate == 120) 60 else 120)
        }

        override fun onRotationLockToggle() {
            rotationLocked = !rotationLocked
            requestedOrientation = if (rotationLocked) ActivityInfo.SCREEN_ORIENTATION_LOCKED else ActivityInfo.SCREEN_ORIENTATION_FULL_USER
            refreshMenu()
        }

        override fun onTouchToggle() {
            touchEnabled = !touchEnabled
            refreshMenu()
        }

        override fun onWindowsKey() = pressKeys(KEY_WINDOWS)

        override fun onShowDesktop() = pressKeys(KEY_WINDOWS, KEY_D)

        override fun onDisconnect() {
            menu.close()
            server?.disconnect()
            finishAffinity()
        }
    }

    private fun pressKeys(vararg codes: Int) {
        val s = server ?: return
        codes.forEach { s.sendInput(Keyboard.keyMessage(true, it)) }
        codes.reversed().forEach { s.sendInput(Keyboard.keyMessage(false, it)) }
    }

    private fun refreshMenu() = menu.update(audioOnTablet, displayMode, frameRate, rotationLocked, touchEnabled)

    // Le geste commence au bord : rien ne part vers Windows tant qu'il n'est pas tranché.
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        val density = resources.displayMetrics.density
        if (!menu.isOpen) when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                edgeSwipeActive = event.x < EDGE_DP * density
                edgeSwipeStart = event.x
                if (edgeSwipeActive) return true
            }
            MotionEvent.ACTION_MOVE -> if (edgeSwipeActive) {
                if (event.x - edgeSwipeStart > SWIPE_DP * density) {
                    edgeSwipeActive = false
                    menu.open()
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (edgeSwipeActive) {
                edgeSwipeActive = false
                return true
            }
        }
        return super.dispatchTouchEvent(event)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        window.insetsController?.apply {
            hide(WindowInsets.Type.systemBars())
            systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    // Dimensions dans l'orientation actuelle, qui décide de celle de l'écran Windows.
    private fun screenSize(): Pair<Int, Int> {
        val mode = display.mode
        val long = maxOf(mode.physicalWidth, mode.physicalHeight)
        val short = minOf(mode.physicalWidth, mode.physicalHeight)
        val portrait = resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        return if (portrait) short to long else long to short
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val (width, height) = screenSize()
        server?.onScreenChanged(width, height)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val (width, height) = screenSize()
        val fps = display.supportedModes.maxOf { it.refreshRate }.toInt()
        onWaiting("En attente du PC")
        server = StreamServer(this, holder.surface, width, height, fps, this).also { it.start() }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        server?.stop()
        server = null
    }

    override fun onWaiting(message: String) = runOnUiThread {
        streamWidth = 0
        video.visibility = View.VISIBLE
        cursor.visibility = View.GONE
        status.visibility = View.VISIBLE
        pairButton.visibility = View.VISIBLE
        status.text = "$message\n${localAddress()}:${Protocol.PORT}"
    }

    override fun onStream(width: Int, height: Int, fps: Int) = runOnUiThread {
        streamWidth = width
        streamHeight = height
        status.visibility = View.GONE
        pairButton.visibility = View.GONE
        applyAspect()
    }

    // Conserve les proportions du flux : marges noires si elles diffèrent de l'écran.
    private fun applyAspect() {
        if (streamWidth == 0 || root.width == 0) return
        val scale = minOf(root.width.toFloat() / streamWidth, root.height.toFloat() / streamHeight)
        val w = (streamWidth * scale).toInt()
        val h = (streamHeight * scale).toInt()
        val params = video.layoutParams as FrameLayout.LayoutParams
        if (params.width != w || params.height != h) {
            params.width = w
            params.height = h
            video.layoutParams = params
        }
    }

    override fun onAudioRoute(onTablet: Boolean) = runOnUiThread {
        audioOnTablet = onTablet
        refreshMenu()
    }

    override fun onControlState(displayMode: Int, fps: Int) = runOnUiThread {
        this.displayMode = displayMode
        frameRate = fps
        refreshMenu()
    }

    override fun onStats(fps: Int, latencyMs: Int) = runOnUiThread {
        menu.updateStats(fps, latencyMs)
    }

    override fun confirmPairing(code: Int): Boolean {
        val answered = CountDownLatch(1)
        var accepted = false
        runOnUiThread {
            AlertDialog.Builder(this)
                .setTitle("Appairer ce PC ?")
                .setMessage("Code : %06d\n\nConfirmez seulement si le même code s'affiche sur le PC.".format(code))
                .setPositiveButton("Confirmer") { _, _ -> accepted = true }
                .setNegativeButton("Refuser", null)
                .setOnDismissListener { answered.countDown() }
                .show()
        }
        answered.await(PAIRING_WAIT_SECONDS, TimeUnit.SECONDS)
        return accepted
    }

    override fun onCursorPosition(visible: Boolean, x: Int, y: Int) = runOnUiThread {
        cursorX = x
        cursorY = y
        cursor.visibility = if (visible && streamWidth > 0) View.VISIBLE else View.GONE
        placeCursor()
    }

    override fun onCursorShape(width: Int, height: Int, hotX: Int, hotY: Int, bgra: ByteArray, offset: Int) {
        if (width == 0 || height == 0) return
        val pixels = IntArray(width * height)
        ByteBuffer.wrap(bgra, offset, pixels.size * 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(pixels)
        val bitmap = Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
        runOnUiThread {
            this.hotX = hotX
            this.hotY = hotY
            cursor.setImageBitmap(bitmap)
            placeCursor()
        }
    }

    private fun placeCursor() {
        if (streamWidth == 0) return
        val scale = video.width.toFloat() / streamWidth
        cursor.scaleType = ImageView.ScaleType.FIT_XY
        val drawable = cursor.drawable ?: return
        val params = cursor.layoutParams
        val w = (drawable.intrinsicWidth * scale).toInt().coerceAtLeast(1)
        val h = (drawable.intrinsicHeight * scale).toInt().coerceAtLeast(1)
        if (params.width != w || params.height != h) {
            params.width = w
            params.height = h
            cursor.layoutParams = params
        }
        cursor.x = video.x + (cursorX - hotX) * scale
        cursor.y = video.y + (cursorY - hotY) * scale
    }

    private fun sendTouch(event: MotionEvent) {
        val s = server ?: return
        if (video.width == 0 || !touchEnabled) return
        val action = event.actionMasked
        val lifted = when (action) {
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> event.actionIndex
            else -> -1
        }
        val cancelled = action == MotionEvent.ACTION_CANCEL
        val n = event.pointerCount
        val b = ByteBuffer.allocate(2 + n * 6).put(Protocol.MSG_TOUCH.toByte()).put(n.toByte())
        for (i in 0 until n) {
            val x = (event.getX(i) / video.width * MAX_COORD).toInt().coerceIn(0, MAX_COORD)
            val y = (event.getY(i) / video.height * MAX_COORD).toInt().coerceIn(0, MAX_COORD)
            b.put(event.getPointerId(i).toByte())
            b.put(if (cancelled || i == lifted) 0 else 1)
            b.putShort(x.toShort())
            b.putShort(y.toShort())
        }
        s.sendInput(b.array())
    }

    private fun isStylus(event: MotionEvent): Boolean {
        val tool = event.getToolType(0)
        return tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER
    }

    private fun sendPen(event: MotionEvent) {
        val s = server ?: return
        if (video.width == 0) return
        var state = 0
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> state = Protocol.PEN_IN_RANGE or Protocol.PEN_CONTACT
            MotionEvent.ACTION_UP, MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> state = Protocol.PEN_IN_RANGE
        }
        if (event.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0) state = state or Protocol.PEN_BARREL
        if (event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER) state = state or Protocol.PEN_ERASER

        val x = (event.x / video.width * MAX_COORD).toInt().coerceIn(0, MAX_COORD)
        val y = (event.y / video.height * MAX_COORD).toInt().coerceIn(0, MAX_COORD)
        val pressure = (event.pressure.coerceIn(0f, 1f) * MAX_PEN_PRESSURE).toInt()
        // Android donne inclinaison et azimut ; Windows attend deux angles en degrés.
        val tilt = event.getAxisValue(MotionEvent.AXIS_TILT)
        val orientation = event.orientation
        val tiltX = Math.toDegrees((tilt * sin(orientation)).toDouble()).toInt().coerceIn(-90, 90)
        val tiltY = Math.toDegrees((-tilt * cos(orientation)).toDouble()).toInt().coerceIn(-90, 90)
        val rotation = ((Math.toDegrees(orientation.toDouble()).toInt() + 360) % 360)

        s.sendInput(ByteBuffer.allocate(12).put(Protocol.MSG_PEN.toByte()).put(state.toByte())
            .putShort(x.toShort()).putShort(y.toShort()).putShort(pressure.toShort())
            .put(tiltX.toByte()).put(tiltY.toByte()).putShort(rotation.toShort()).array())
    }

    // Toute touche connue part vers le PC ; volume et marche restent à Android.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val s = server
        val code = Keyboard.toWindows(event)
        if (s != null && streamWidth != 0 && event.action != KeyEvent.ACTION_MULTIPLE) {
            if (code != null) {
                s.sendInput(Keyboard.keyMessage(event.action == KeyEvent.ACTION_DOWN, code))
                return true
            }
            val char = event.unicodeChar
            if (event.scanCode == 0 && char != 0) {
                if (event.action == KeyEvent.ACTION_DOWN) s.sendInput(Keyboard.textMessage(String(Character.toChars(char))))
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    private fun toggleSoftKeyboard() {
        val ime = getSystemService(InputMethodManager::class.java)
        softKeyboardOpen = !softKeyboardOpen
        if (softKeyboardOpen) {
            softTarget.requestFocus()
            ime.showSoftInput(softTarget, 0)
        } else {
            ime.hideSoftInputFromWindow(softTarget.windowToken, 0)
        }
    }

    private fun localAddress(): String = NetworkInterface.getNetworkInterfaces().asSequence()
        .filter { it.isUp && !it.isLoopback }
        .flatMap { it.inetAddresses.asSequence() }
        .firstOrNull { it is java.net.Inet4Address }?.hostAddress ?: "pas de réseau"

    companion object {
        private const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
        private const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT
        private const val MAX_COORD = 65535
        private const val MAX_PEN_PRESSURE = 1024
        private const val PAIRING_WAIT_SECONDS = 110L
        private const val EDGE_DP = 24
        private const val SWIPE_DP = 48
        private const val GESTURE_LIMIT_DP = 200
        private const val KEY_WINDOWS = 0x15B
        private const val KEY_D = 0x20
    }
}
