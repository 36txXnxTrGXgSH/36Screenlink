package com.screenlink36.app

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Typeface
import android.view.View

/**
 * Pendant qu'une icône de la barre des tâches est glissée vers le haut : un rectangle par écran
 * du PC, disposés comme dans les paramètres d'affichage de Windows. Celui sous le doigt s'allume.
 */
class ScreenPicker(context: Context) : View(context) {

    private val density = resources.displayMetrics.density
    private var screens: List<Rect> = emptyList()
    private var device = 0
    private var deviceLabel = ""
    private val boxes = mutableListOf<RectF>()
    private var hover = -1
    private var fingerX = -1f
    private var fingerY = -1f
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2 * density
    }
    private val number = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Typeface.DEFAULT_BOLD
    }
    private val caption = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        textSize = 14 * density
    }
    private val title = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        textSize = 18 * density
        color = Color.WHITE
        typeface = Typeface.DEFAULT_BOLD
    }

    init {
        visibility = GONE
    }

    /** Index de l'écran sous le doigt, ou -1. */
    val picked get() = hover

    fun show(screens: List<Rect>, device: Int, deviceLabel: String) {
        this.screens = screens
        this.device = device
        this.deviceLabel = deviceLabel
        hover = -1
        visibility = VISIBLE
        bringToFront()
        layoutBoxes()
        invalidate()
    }

    fun track(x: Float, y: Float) {
        fingerX = x
        fingerY = y
        hover = boxes.indexOfFirst { it.contains(x, y) }
        invalidate()
    }

    fun hide() {
        visibility = GONE
        hover = -1
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = layoutBoxes()

    // Les écrans gardent leurs proportions et leur place relative, dans le haut de l'écran :
    // le doigt part de la barre des tâches, en bas.
    private fun layoutBoxes() {
        boxes.clear()
        if (screens.isEmpty() || width == 0) return
        val left = screens.minOf { it.left }
        val top = screens.minOf { it.top }
        val right = screens.maxOf { it.right }
        val bottom = screens.maxOf { it.bottom }
        val areaW = width * AREA_WIDTH
        val areaH = height * AREA_HEIGHT
        val scale = minOf(areaW / (right - left), areaH / (bottom - top))
        val offsetX = (width - (right - left) * scale) / 2
        val offsetY = height * AREA_TOP + (areaH - (bottom - top) * scale) / 2
        val gap = GAP_DP * density / 2
        screens.forEach {
            boxes += RectF(offsetX + (it.left - left) * scale + gap, offsetY + (it.top - top) * scale + gap,
                offsetX + (it.right - left) * scale - gap, offsetY + (it.bottom - top) * scale - gap)
        }
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(DIM)
        canvas.drawText("Lâchez l'icône sur l'écran voulu", width / 2f, height * AREA_TOP - 16 * density, title)
        val radius = 14 * density
        boxes.forEachIndexed { i, box ->
            val on = i == hover
            fill.color = if (on) ACCENT else IDLE_FILL
            stroke.color = if (on) Color.WHITE else IDLE_LINE
            canvas.drawRoundRect(box, radius, radius, fill)
            canvas.drawRoundRect(box, radius, radius, stroke)
            val text = if (on) SELECTED_TEXT else Color.WHITE
            number.color = text
            number.textSize = minOf(box.height() * 0.4f, 56 * density)
            caption.color = text
            val cy = box.centerY() + number.textSize * 0.3f
            canvas.drawText("${i + 1}", box.centerX(), cy, number)
            if (box.height() > number.textSize + caption.textSize * 2) {
                canvas.drawText(if (i == device) deviceLabel else "Écran du PC", box.centerX(), cy + caption.textSize * 1.6f, caption)
            }
        }
        if (fingerX >= 0) {
            fill.color = FINGER
            canvas.drawCircle(fingerX, fingerY, 22 * density, fill)
        }
    }

    companion object {
        private const val AREA_WIDTH = 0.8f
        private const val AREA_HEIGHT = 0.5f
        private const val AREA_TOP = 0.15f
        private const val GAP_DP = 16
        private const val DIM = 0xCC0B0F15.toInt()
        private const val IDLE_FILL = 0x40FFFFFF
        private const val IDLE_LINE = 0x99FFFFFF.toInt()
        private const val ACCENT = 0xEB67E8F9.toInt()
        private const val SELECTED_TEXT = 0xFF0B2530.toInt()
        private const val FINGER = 0x66FFFFFF
    }
}
