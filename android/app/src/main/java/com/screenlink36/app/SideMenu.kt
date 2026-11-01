package com.screenlink36.app

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Outline
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.RenderEffect
import android.graphics.Shader
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.PixelCopy
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.ViewOutlineProvider
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * Menu en verre dépoli, à gauche. Il s'ouvre en glissant du bord de l'écran vers le centre.
 * Beaucoup d'appareils n'ont pas le flou de fenêtre d'Android : le verre floute lui-même une
 * copie réduite de l'image reçue, ce qui fonctionne partout.
 */
class SideMenu(private val appContext: Context, private val video: SurfaceView, private val actions: Actions) {

    interface Actions {
        fun onAudioToggle()
        fun onKeyboard()
        fun onDisplayMode(mode: Int)
        fun onFrameRateToggle()
        fun onRotationLockToggle()
        fun onTouchToggle()
        fun onWindowsKey()
        fun onShowDesktop()
        fun onDisconnect()
    }

    private val density = appContext.resources.displayMetrics.density
    private val handler = Handler(Looper.getMainLooper())
    private val glass = GlassView(appContext)
    private val list = LinearLayout(appContext).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(16), dp(8), dp(8), dp(16))
    }
    private val panel = FrameLayout(appContext).apply {
        outlineProvider = object : ViewOutlineProvider() {
            override fun getOutline(view: View, outline: Outline) =
                outline.setRoundRect(0, 0, view.width, view.height, dp(CORNER_DP).toFloat())
        }
        clipToOutline = true
        elevation = dp(8).toFloat()
        foreground = GradientDrawable().apply {
            cornerRadius = dp(CORNER_DP).toFloat()
            setStroke(dp(1), BORDER_COLOR)
        }
        isClickable = true
        addView(glass, FrameLayout.LayoutParams(MATCH, MATCH))
        addView(ScrollView(appContext).apply {
            isScrollbarFadingEnabled = true
            addView(list)
        }, FrameLayout.LayoutParams(MATCH, WRAP))
    }
    private val link = icon(R.drawable.ms_usb, 24)
    private val pin = icon(R.drawable.ms_keep, 24)
    private val audio = Segments(listOf("PC" to R.drawable.ms_computer, "Tablette" to R.drawable.ms_tablet_android)) {
        if ((it == 1) != audioOnTablet) actions.onAudioToggle()
    }
    private val mode = Segments(listOf("Étendu" to null, "Dupliquer" to null, "Tablette seule" to null)) {
        actions.onDisplayMode(it)
    }
    private val rate = Segments(listOf("60 Hz" to null, "120 Hz" to null)) {
        if (FRAME_RATES[it] != frameRate) actions.onFrameRateToggle()
    }
    private val rotationRow = Setting(appContext, R.drawable.ms_screen_rotation, "Rotation") { actions.onRotationLockToggle() }
    private val touchRow = Setting(appContext, R.drawable.ms_touch_app, "Tactile") { actions.onTouchToggle() }
    private val stats = TextView(appContext).apply {
        setTextColor(SECONDARY_COLOR)
        textSize = 12f
    }
    private var audioOnTablet = false
    private var frameRate = 120
    private var pinned = false
    private var open = false

    /** Couche plein écran : toucher hors du menu le referme, sauf s'il est épinglé. */
    val view: FrameLayout = FrameLayout(appContext).apply {
        visibility = View.GONE
        setOnClickListener { close() }
        addView(panel, FrameLayout.LayoutParams(dp(WIDTH_DP), WRAP, Gravity.START or Gravity.CENTER_VERTICAL).apply {
            setMargins(dp(MARGIN_DP), dp(MARGIN_DP), dp(MARGIN_DP), dp(MARGIN_DP))
        })
    }

    val isOpen get() = open

    init {
        list.addView(header())
        section("Sortie son", audio.view)
        section("Affichage", mode.view)
        section("Fréquence", rate.view)
        list.addView(LinearLayout(appContext).apply {
            orientation = LinearLayout.VERTICAL
            addView(rotationRow.view)
            addView(touchRow.view)
        }, block(GAP_DP))
        list.addView(LinearLayout(appContext).apply {
            addView(tile(R.drawable.ms_keyboard, "Clavier") {
                close()
                actions.onKeyboard()
            }, tileParams(0))
            addView(tile(R.drawable.ms_grid_view, "Windows") { actions.onWindowsKey() }, tileParams(8))
            addView(tile(R.drawable.ms_desktop_windows, "Bureau") { actions.onShowDesktop() }, tileParams(8))
        }, block(GAP_DP))
        list.addView(footer(), block(GAP_DP))
    }

    private fun dp(value: Int) = (value * density).toInt()

    private fun block(top: Int) = LinearLayout.LayoutParams(MATCH, WRAP).apply {
        topMargin = dp(top)
        marginEnd = dp(8)
    }

    private fun tileParams(start: Int) = LinearLayout.LayoutParams(0, dp(64), 1f).apply { marginStart = dp(start) }

    private fun rounded(color: Int, radiusDp: Int, stroke: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radiusDp).toFloat()
        if (stroke != 0) setStroke(dp(1), stroke)
    }

    private fun icon(resource: Int, sizeDp: Int) = ImageView(appContext).apply {
        setImageResource(resource)
        imageTintList = ColorStateList.valueOf(Color.WHITE)
        layoutParams = LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp))
    }

    private fun header() = LinearLayout(appContext).apply {
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dp(56)
        addView(TextView(context).apply {
            text = "36Screenlink"
            textSize = 20f
            setTextColor(Color.WHITE)
            setTypeface(typeface, Typeface.BOLD)
        }, LinearLayout.LayoutParams(0, WRAP, 1f))
        addView(headerButton(link, null))
        addView(headerButton(pin) { togglePin() })
        addView(headerButton(icon(R.drawable.ms_close, 24)) { close() })
    }

    private fun headerButton(image: ImageView, onClick: (() -> Unit)?) = FrameLayout(appContext).apply {
        addView(image, FrameLayout.LayoutParams(dp(24), dp(24), Gravity.CENTER))
        layoutParams = LinearLayout.LayoutParams(dp(40), dp(40))
        if (onClick != null) setOnClickListener { onClick() }
    }

    private fun section(title: String, content: View) {
        list.addView(TextView(appContext).apply {
            text = title
            textSize = 12f
            setTextColor(SECONDARY_COLOR)
            setTypeface(typeface, Typeface.BOLD)
        }, block(if (list.childCount == 1) 8 else GAP_DP))
        list.addView(content, block(8).apply { height = dp(40) })
    }

    private fun tile(iconResource: Int, label: String, onClick: () -> Unit) = LinearLayout(appContext).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        background = rounded(GLASS_FILL, 16, GLASS_LINE)
        setOnClickListener { onClick() }
        addView(icon(iconResource, 22))
        addView(TextView(context).apply {
            text = label
            textSize = 12f
            gravity = Gravity.CENTER
            setTextColor(Color.WHITE)
            setPadding(0, dp(4), 0, 0)
        })
    }

    private fun footer() = LinearLayout(appContext).apply {
        gravity = Gravity.CENTER_VERTICAL
        addView(stats, LinearLayout.LayoutParams(0, WRAP, 1f))
        addView(LinearLayout(context).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(12), 0, dp(16), 0)
            background = rounded(GLASS_FILL, 20, GLASS_LINE)
            setOnClickListener { actions.onDisconnect() }
            addView(icon(R.drawable.ms_power_settings_new, 18))
            addView(TextView(context).apply {
                text = "Déconnecter"
                textSize = 13f
                setTextColor(Color.WHITE)
                setTypeface(typeface, Typeface.BOLD)
                setPadding(dp(4), 0, 0, 0)
            })
        }, LinearLayout.LayoutParams(WRAP, dp(40)))
    }

    fun update(audioOnTablet: Boolean, displayMode: Int, fps: Int, rotationLocked: Boolean, touchEnabled: Boolean) {
        this.audioOnTablet = audioOnTablet
        frameRate = fps
        audio.select(if (audioOnTablet) 1 else 0)
        mode.select(displayMode)
        rate.select(FRAME_RATES.indexOf(fps))
        rotationRow.set(!rotationLocked, if (rotationLocked) "Verrouillée" else "Automatique")
        touchRow.set(touchEnabled, if (touchEnabled) "Activé" else "Coupé")
    }

    fun updateLink(usb: Boolean) {
        link.setImageResource(if (usb) R.drawable.ms_usb else R.drawable.ms_wifi)
    }

    fun updateStats(fps: Int, latencyMs: Int) {
        stats.text = "$fps images/s · $latencyMs ms"
    }

    private fun togglePin() {
        pinned = !pinned
        pin.imageTintList = ColorStateList.valueOf(if (pinned) ACCENT_COLOR else Color.WHITE)
        // Épinglé, le menu reste ouvert et les touches à côté repartent vers Windows.
        view.isClickable = !pinned
    }

    fun open() {
        open = true
        view.visibility = View.VISIBLE
        view.bringToFront()
        // Fondu seul : un panneau déplacé fausserait la zone qu'Android réserve au-dessus de la vidéo.
        panel.animate().cancel()
        panel.alpha = 0f
        panel.animate().alpha(1f).setDuration(ANIMATION_MS).start()
        handler.removeCallbacks(refresh)
        refresh.run()
    }

    fun close() {
        if (!open) return
        open = false
        handler.removeCallbacks(refresh)
        panel.animate().alpha(0f).setDuration(ANIMATION_MS).withEndAction { view.visibility = View.GONE }.start()
    }

    private val refresh = object : Runnable {
        override fun run() {
            glass.capture()
            handler.postDelayed(this, GLASS_REFRESH_MS)
        }
    }

    /** Boutons segmentés : un seul choix actif. */
    private inner class Segments(labels: List<Pair<String, Int?>>, onPick: (Int) -> Unit) {
        val view = LinearLayout(appContext).apply {
            setPadding(dp(4), dp(4), dp(4), dp(4))
            background = rounded(GLASS_FILL, 20)
        }
        private val items = labels.mapIndexed { index, (label, iconResource) ->
            val text = TextView(appContext).apply {
                this.text = label
                textSize = 13f
                maxLines = 1
            }
            val image = iconResource?.let { icon(it, 16) }
            val item = LinearLayout(appContext).apply {
                gravity = Gravity.CENTER
                setPadding(dp(8), 0, dp(8), 0)
                setOnClickListener { onPick(index) }
                if (image != null) {
                    addView(image)
                    text.setPadding(dp(4), 0, 0, 0)
                }
                addView(text)
            }
            view.addView(item, LinearLayout.LayoutParams(WRAP, MATCH, 1f))
            Triple(item, text, image)
        }

        fun select(selected: Int) = items.forEachIndexed { index, (item, text, image) ->
            val on = index == selected
            val color = if (on) SELECTED_TEXT else Color.WHITE
            item.background = if (on) rounded(SELECTED_FILL, 16) else null
            text.setTextColor(color)
            text.setTypeface(null, if (on) Typeface.BOLD else Typeface.NORMAL)
            image?.imageTintList = ColorStateList.valueOf(color)
        }
    }

    /** Ligne avec icône, valeur et interrupteur. */
    private inner class Setting(context: Context, iconResource: Int, label: String, onToggle: () -> Unit) {
        private val value = TextView(context).apply {
            textSize = 12f
            setTextColor(SECONDARY_COLOR)
            setPadding(0, 0, dp(8), 0)
        }
        private val toggle = Toggle(context)
        val view = LinearLayout(context).apply {
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(MATCH, dp(40))
            setOnClickListener { onToggle() }
            addView(icon(iconResource, 20))
            addView(TextView(context).apply {
                text = label
                textSize = 14f
                setTextColor(Color.WHITE)
                setPadding(dp(8), 0, 0, 0)
            }, LinearLayout.LayoutParams(0, WRAP, 1f))
            addView(value)
            addView(toggle, LinearLayout.LayoutParams(dp(40), dp(24)))
        }

        fun set(on: Boolean, text: String) {
            value.text = text
            toggle.on = on
            toggle.invalidate()
        }
    }

    private class Toggle(context: Context) : View(context) {
        var on = false
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

        override fun onDraw(canvas: Canvas) {
            val h = height.toFloat()
            paint.color = if (on) SELECTED_FILL else TOGGLE_OFF
            canvas.drawRoundRect(RectF(0f, 0f, width.toFloat(), h), h / 2, h / 2, paint)
            paint.color = if (on) SELECTED_TEXT else Color.WHITE
            canvas.drawCircle(if (on) width - h / 2 else h / 2, h / 2, h / 3, paint)
        }
    }

    /** Image reçue, réduite puis floutée, recadrée sur le panneau et teintée pour le texte blanc. */
    private inner class GlassView(context: Context) : View(context) {
        private var shown: Bitmap? = null
        private var spare: Bitmap? = null
        private var copying = false
        private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        private val placement = Matrix()

        init {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val radius = BLUR_DP * density
                setRenderEffect(RenderEffect.createBlurEffect(radius, radius, Shader.TileMode.CLAMP))
            }
        }

        fun capture() {
            val w = video.width / GLASS_SCALE
            val h = video.height / GLASS_SCALE
            if (copying || w == 0 || h == 0 || !video.holder.surface.isValid) return
            val target = spare?.takeIf { it.width == w && it.height == h } ?: Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
            copying = true
            PixelCopy.request(video, target, { result ->
                copying = false
                if (result == PixelCopy.SUCCESS) {
                    spare = shown
                    shown = target
                    invalidate()
                } else {
                    spare = target
                }
            }, handler)
        }

        // Le verre prend la taille du contenu, sans la dicter au panneau.
        override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) = setMeasuredDimension(
            MeasureSpec.getMode(widthMeasureSpec).let { if (it == MeasureSpec.EXACTLY) MeasureSpec.getSize(widthMeasureSpec) else 0 },
            MeasureSpec.getMode(heightMeasureSpec).let { if (it == MeasureSpec.EXACTLY) MeasureSpec.getSize(heightMeasureSpec) else 0 })

        override fun onDraw(canvas: Canvas) {
            canvas.drawColor(EMPTY_GLASS)
            val bitmap = shown ?: return
            val scale = video.width.toFloat() / bitmap.width
            placement.setScale(scale, scale)
            placement.postTranslate(video.x - panel.left, video.y - panel.top)
            canvas.drawBitmap(bitmap, placement, paint)
            canvas.drawColor(GLASS_TINT)
        }
    }

    companion object {
        const val MODE_EXTENDED = 0
        const val MODE_DUPLICATE = 1
        const val MODE_TABLET_ONLY = 2
        private const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
        private const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT
        private const val WIDTH_DP = 296
        private const val MARGIN_DP = 16
        private const val CORNER_DP = 28
        private const val GAP_DP = 16
        private const val BLUR_DP = 24
        private const val GLASS_SCALE = 8
        private const val GLASS_REFRESH_MS = 100L
        private const val ANIMATION_MS = 160L
        private val FRAME_RATES = listOf(60, 120)
        private const val GLASS_TINT = 0x66141A2A
        private const val EMPTY_GLASS = 0xE6141A2A.toInt()
        private const val GLASS_FILL = 0x24FFFFFF
        private const val GLASS_LINE = 0x33FFFFFF
        private const val TOGGLE_OFF = 0x38FFFFFF
        private const val BORDER_COLOR = 0x57FFFFFF
        private const val SECONDARY_COLOR = 0xD1FFFFFF.toInt()
        private const val SELECTED_FILL = 0xEBFFFFFF.toInt()
        private const val SELECTED_TEXT = 0xFF21325E.toInt()
        private const val ACCENT_COLOR = 0xFF67E8F9.toInt()
    }
}
