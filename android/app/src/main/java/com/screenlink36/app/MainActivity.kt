package com.screenlink36.app

import android.app.Activity
import android.app.AlertDialog
import android.app.KeyguardManager
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.CancellationSignal
import android.view.InputDevice
import android.view.PointerIcon
import android.widget.LinearLayout
import android.widget.Toast
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
    private lateinit var waitingButtons: LinearLayout
    private lateinit var menu: SideMenu
    private var audioOnTablet = false
    private var displayMode = SideMenu.MODE_EXTENDED
    private var frameRate = 120
    private var orientationChoice = SideMenu.ORIENTATION_AUTO
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
    private var mouseButtons = 0
    private var lastBackPress = 0L
    private var secureScreen = false
    private var secureArmed = false
    private lateinit var secureNotice: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setDecorFitsSystemWindows(false)
        window.attributes.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        video = SurfaceView(this)
        video.holder.addCallback(this)
        video.setOnTouchListener { _, event ->
            when {
                isMouse(event) -> sendMouse(event)
                isStylus(event) -> {
                    // Stylet : chaque point au plus tôt, sans attendre la synchronisation de l'écran.
                    if (event.actionMasked == MotionEvent.ACTION_DOWN) video.requestUnbufferedDispatch(event)
                    sendPen(event)
                }
                else -> sendTouch(event)
            }
            true
        }
        video.setOnHoverListener { _, event ->
            if (isMouse(event)) sendMouse(event) else if (isStylus(event)) sendPen(event)
            true
        }
        video.setOnGenericMotionListener { _, event ->
            if (isMouse(event)) {
                sendMouse(event)
                true
            } else {
                false
            }
        }
        // Le curseur affiché est celui de Windows : la flèche d'Android serait en double.
        video.pointerIcon = PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL)
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
        val maxRate = display.supportedModes.maxOf { it.refreshRate }.toInt()
        menu = SideMenu(this, video, menuActions, listOf(60, 120, 144).filter { it <= maxRate + 1 }, deviceName())
        orientationChoice = getSharedPreferences("app", MODE_PRIVATE).getInt("orientation",
            if (isPhone()) SideMenu.ORIENTATION_LANDSCAPE else SideMenu.ORIENTATION_AUTO)
        applyOrientation()
        refreshMenu()
        root.addView(menu.view, FrameLayout.LayoutParams(MATCH, MATCH))
        pairButton = Button(this).apply {
            text = "Nouvel appairage"
            setOnClickListener {
                server?.armPairing()
                status.text = "Appairage autorisé : lancez 36Screenlink sur le PC"
            }
        }
        val forgetButton = Button(this).apply {
            text = "Oublier ce PC"
            setOnClickListener { confirmForget() }
        }
        waitingButtons = LinearLayout(this).apply {
            gravity = Gravity.CENTER
            addView(pairButton)
            addView(forgetButton, LinearLayout.LayoutParams(WRAP, WRAP).apply { marginStart = buttonSize / 4 })
        }
        root.addView(waitingButtons, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = buttonSize
        })
        secureNotice = TextView(this).apply {
            visibility = View.GONE
            setTextColor(Color.WHITE)
            setBackgroundColor(0xE6141A2A.toInt())
            textSize = 18f
            gravity = Gravity.CENTER
            val pad = buttonSize / 2
            setPadding(pad, pad, pad, pad)
            text = "Le PC affiche une invite UAC ou son écran de verrouillage.\n\nTouchez ici et confirmez votre identité pour agir depuis cet appareil."
            setOnClickListener { confirmIdentity() }
        }
        root.addView(secureNotice, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER))
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING)
        setContentView(root)
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            applyAspect()
            reserveLeftEdge()
        }
        hideSystemBars()
        showPrivacyNoticeOnce()
        // Android 13 et plus, retour prédictif : onBackPressed n'est plus appelé.
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT) { handleBack() }
        }
    }

    // Information du premier lancement : ce qui est diffusé, et où.
    private fun showPrivacyNoticeOnce() {
        val prefs = getSharedPreferences("app", MODE_PRIVATE)
        if (prefs.getBoolean("privacy_notice", false)) return
        AlertDialog.Builder(this)
            .setTitle("Écran partagé")
            .setMessage("Cet appareil affichera l'écran de votre PC, avec tout ce qu'il contient : messages, documents, " +
                "mots de passe visibles. Le flux reste entre vos deux appareils, chiffré, sans serveur ni compte, " +
                "et rien n'est enregistré.\n\nPensez à qui peut voir cet écran.")
            .setPositiveButton("Compris") { _, _ -> prefs.edit().putBoolean("privacy_notice", true).apply() }
            .setCancelable(false)
            .show()
    }

    private fun confirmForget() {
        AlertDialog.Builder(this)
            .setTitle("Oublier ce PC ?")
            .setMessage("La clé d'appairage est effacée de cet appareil. Il faudra un nouvel appairage pour se reconnecter.")
            .setPositiveButton("Oublier") { _, _ ->
                server?.forgetPairing()
                status.text = "PC oublié. Touchez Nouvel appairage pour en ajouter un."
            }
            .setNegativeButton("Annuler", null)
            .show()
    }

    // Retour : referme le menu, sinon il faut appuyer deux fois pour quitter.
    @Deprecated("Gestion classique du retour, toujours appelée sur Android 11 à 16")
    override fun onBackPressed() = handleBack()

    private fun handleBack() {
        if (menu.isOpen) {
            menu.close()
            return
        }
        val now = android.os.SystemClock.uptimeMillis()
        if (now - lastBackPress < BACK_CONFIRM_MS) {
            finish()
            return
        }
        lastBackPress = now
        Toast.makeText(this, "Appuyez encore sur Retour pour fermer 36Screenlink", Toast.LENGTH_SHORT).show()
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

        override fun onFrameRate(fps: Int) {
            if (fps != frameRate) server?.sendControl(Protocol.CONTROL_FPS, fps)
        }

        override fun onOrientation(choice: Int) {
            if (choice == orientationChoice) return
            orientationChoice = choice
            getSharedPreferences("app", MODE_PRIVATE).edit().putInt("orientation", choice).apply()
            applyOrientation()
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

    private fun refreshMenu() = menu.update(audioOnTablet, displayMode, frameRate, orientationChoice, touchEnabled)

    // Un téléphone sert d'écran posé à l'horizontale : paysage par défaut, même si la rotation
    // automatique d'Android est coupée. « Auto » suit, lui, le réglage du téléphone.
    private fun applyOrientation() {
        requestedOrientation = when (orientationChoice) {
            SideMenu.ORIENTATION_LANDSCAPE -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            SideMenu.ORIENTATION_PORTRAIT -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            else -> ActivityInfo.SCREEN_ORIENTATION_FULL_USER
        }
    }

    private fun isPhone() = resources.configuration.smallestScreenWidthDp < 600

    private fun deviceName() = if (isPhone()) "Téléphone" else "Tablette"

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
        // Sans PC, l'écran de la tablette peut s'éteindre normalement.
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        video.visibility = View.VISIBLE
        cursor.visibility = View.GONE
        status.visibility = View.VISIBLE
        waitingButtons.visibility = View.VISIBLE
        status.text = "$message\n${localAddress()}:${Protocol.PORT}"
    }

    override fun onStream(width: Int, height: Int, fps: Int) = runOnUiThread {
        streamWidth = width
        streamHeight = height
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        status.visibility = View.GONE
        waitingButtons.visibility = View.GONE
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

    override fun onLink(usb: Boolean) = runOnUiThread {
        menu.updateLink(usb)
    }

    override fun onControlState(displayMode: Int, fps: Int) = runOnUiThread {
        this.displayMode = displayMode
        frameRate = fps
        refreshMenu()
    }

    override fun onStats(fps: Int, latencyMs: Int) = runOnUiThread {
        menu.updateStats(fps, latencyMs)
    }

    override fun onSecureScreen(active: Boolean) = runOnUiThread {
        secureScreen = active
        secureArmed = false
        secureNotice.visibility = if (active) View.VISIBLE else View.GONE
    }

    /** Validation UAC ou déverrouillage : jamais sans le code, l'empreinte ou le visage de la tablette. */
    private fun confirmIdentity() {
        val keyguard = getSystemService(KeyguardManager::class.java)
        if (!keyguard.isDeviceSecure) {
            AlertDialog.Builder(this)
                .setTitle("Agir sur l'écran sécurisé du PC ?")
                .setMessage("Cet appareil n'a pas de code de verrouillage : n'importe qui pourrait valider une invite UAC " +
                    "ou saisir sur le PC verrouillé. Ajoutez un code dans les paramètres d'Android.")
                .setPositiveButton("Continuer") { _, _ -> armSecure() }
                .setNegativeButton("Annuler", null)
                .show()
            return
        }
        val prompt = BiometricPrompt.Builder(this)
            .setTitle("Écran sécurisé du PC")
            .setSubtitle("Confirmez votre identité pour valider l'UAC ou déverrouiller le PC")
            .setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_WEAK or BiometricManager.Authenticators.DEVICE_CREDENTIAL)
            .build()
        prompt.authenticate(CancellationSignal(), mainExecutor, object : BiometricPrompt.AuthenticationCallback() {
            override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) = armSecure()
        })
    }

    private fun armSecure() {
        if (!secureScreen) return
        secureArmed = true
        secureNotice.visibility = View.GONE
        server?.armSecureInput()
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
        if (width !in 1..256 || height !in 1..256 || bgra.size - offset < width * height * 4) return
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
        // Windows n'accepte que 10 contacts : au-delà, toute la trame serait refusée.
        val n = minOf(event.pointerCount, MAX_CONTACTS)
        // Points intermédiaires regroupés par Android : envoyés aussi, pour des traits fidèles.
        if (action == MotionEvent.ACTION_MOVE) for (h in 0 until event.historySize) {
            s.sendInput(touchMessage(n) { i -> Triple(event.getPointerId(i), event.getHistoricalX(i, h), event.getHistoricalY(i, h)) to true })
        }
        s.sendInput(touchMessage(n) { i -> Triple(event.getPointerId(i), event.getX(i), event.getY(i)) to !(cancelled || i == lifted) })
    }

    private inline fun touchMessage(n: Int, point: (Int) -> Pair<Triple<Int, Float, Float>, Boolean>): ByteArray {
        val b = ByteBuffer.allocate(2 + n * 6).put(Protocol.MSG_TOUCH.toByte()).put(n.toByte())
        for (i in 0 until n) {
            val (p, down) = point(i)
            b.put(p.first.toByte())
            b.put(if (down) 1 else 0)
            b.putShort((p.second / video.width * MAX_COORD).toInt().coerceIn(0, MAX_COORD).toShort())
            b.putShort((p.third / video.height * MAX_COORD).toInt().coerceIn(0, MAX_COORD).toShort())
        }
        return b.array()
    }

    private fun isMouse(event: MotionEvent) =
        event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE || event.isFromSource(InputDevice.SOURCE_MOUSE)

    /** Souris branchée à la tablette : position, boutons et molettes, comme une vraie souris. */
    private fun sendMouse(event: MotionEvent) {
        val s = server ?: return
        if (video.width == 0) return
        // Un appui sans bouton déclaré (pavé tactile, souris simulée) vaut un clic gauche.
        mouseButtons = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> (event.buttonState and 7).takeIf { it != 0 } ?: 1
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> 0
            else -> event.buttonState and 7
        }
        if (event.actionMasked == MotionEvent.ACTION_HOVER_EXIT) return
        var wheelV = 0
        var wheelH = 0
        if (event.actionMasked == MotionEvent.ACTION_SCROLL) {
            wheelV = (event.getAxisValue(MotionEvent.AXIS_VSCROLL) * WHEEL_DELTA).toInt()
            wheelH = (event.getAxisValue(MotionEvent.AXIS_HSCROLL) * WHEEL_DELTA).toInt()
        }
        val x = (event.x / video.width * MAX_COORD).toInt().coerceIn(0, MAX_COORD)
        val y = (event.y / video.height * MAX_COORD).toInt().coerceIn(0, MAX_COORD)
        s.sendInput(ByteBuffer.allocate(10).put(Protocol.MSG_MOUSE.toByte()).put(mouseButtons.toByte())
            .putShort(x.toShort()).putShort(y.toShort()).putShort(wheelV.toShort()).putShort(wheelH.toShort()).array())
    }

    // Molette : Android la confie parfois à l'activité plutôt qu'à la vue sous le pointeur.
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (isMouse(event) && event.actionMasked == MotionEvent.ACTION_SCROLL && !menu.isOpen && video.width > 0) {
            val local = MotionEvent.obtain(event).apply { offsetLocation(-video.x, -video.y) }
            sendMouse(local)
            local.recycle()
            return true
        }
        return super.dispatchGenericMotionEvent(event)
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
        if (s != null && streamWidth != 0 && event.action != KeyEvent.ACTION_MULTIPLE) {
            val messages = Keyboard.toWindows(event)
            if (messages != null) {
                messages.forEach(s::sendInput)
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
        private const val MAX_CONTACTS = 10
        private const val WHEEL_DELTA = 120
        private const val BACK_CONFIRM_MS = 2000L
        private const val KEY_D = 0x20
    }
}
