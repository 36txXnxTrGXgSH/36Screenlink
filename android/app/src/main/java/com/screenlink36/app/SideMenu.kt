package com.screenlink36.app

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Menu vertical transparent, à gauche. Il s'ouvre en glissant du bord de l'écran vers le centre. */
class SideMenu(context: Context, private val actions: Actions) {

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

    private val density = context.resources.displayMetrics.density
    private val list = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(10), dp(16), dp(10), dp(16))
    }
    private val rows = HashMap<String, TextView>()
    private val stats = TextView(context).apply {
        setTextColor(Color.WHITE)
        textSize = 12f
        alpha = 0.8f
        setPadding(dp(12), dp(8), dp(12), dp(4))
    }

    /** Couche plein écran : toucher hors du menu le referme. */
    val view: FrameLayout = FrameLayout(context).apply {
        visibility = View.GONE
        setOnClickListener { close() }
        addView(ScrollView(context).apply {
            setBackgroundColor(PANEL_COLOR)
            addView(list)
            setOnClickListener { }
        }, FrameLayout.LayoutParams(dp(WIDTH_DP), ViewGroup.LayoutParams.MATCH_PARENT, Gravity.START))
    }

    val isOpen get() = view.visibility == View.VISIBLE

    init {
        header("Son")
        row("audio") { actions.onAudioToggle() }
        row("keyboard") {
            close()
            actions.onKeyboard()
        }
        header("Affichage")
        row("mode0") { actions.onDisplayMode(MODE_EXTENDED) }
        row("mode1") { actions.onDisplayMode(MODE_DUPLICATE) }
        row("mode2") { actions.onDisplayMode(MODE_TABLET_ONLY) }
        row("fps") { actions.onFrameRateToggle() }
        row("rotation") { actions.onRotationLockToggle() }
        header("Saisie")
        row("touch") { actions.onTouchToggle() }
        row("windows") { actions.onWindowsKey() }
        row("desktop") { actions.onShowDesktop() }
        header("Connexion")
        list.addView(stats)
        row("disconnect") { actions.onDisconnect() }
    }

    private fun dp(value: Int) = (value * density).toInt()

    private fun header(title: String) {
        list.addView(TextView(view.context).apply {
            text = title
            textSize = 12f
            setTextColor(Color.WHITE)
            alpha = 0.6f
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(12), dp(14), dp(12), dp(4))
        })
    }

    private fun row(id: String, onClick: () -> Unit) {
        val row = TextView(view.context).apply {
            textSize = 16f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER_VERTICAL
            minHeight = dp(46)
            setPadding(dp(12), 0, dp(12), 0)
            setOnClickListener { onClick() }
        }
        rows[id] = row
        list.addView(row)
    }

    fun update(audioOnTablet: Boolean, displayMode: Int, fps: Int, rotationLocked: Boolean, touchEnabled: Boolean) {
        rows.getValue("audio").text = "🔊  Sortie : " + if (audioOnTablet) "tablette" else "PC"
        rows.getValue("keyboard").text = "⌨  Clavier"
        rows.getValue("mode0").text = mark(displayMode == MODE_EXTENDED) + "Étendu"
        rows.getValue("mode1").text = mark(displayMode == MODE_DUPLICATE) + "Dupliquer"
        rows.getValue("mode2").text = mark(displayMode == MODE_TABLET_ONLY) + "Tablette seule"
        rows.getValue("fps").text = "⏱  Fréquence : $fps Hz"
        rows.getValue("rotation").text = "🔄  Rotation : " + if (rotationLocked) "verrouillée" else "automatique"
        rows.getValue("touch").text = "✋  Tactile : " + if (touchEnabled) "activé" else "coupé"
        rows.getValue("windows").text = "⊞  Touche Windows"
        rows.getValue("desktop").text = "▭  Afficher le bureau"
        rows.getValue("disconnect").text = "⏻  Déconnecter"
    }

    private fun mark(selected: Boolean) = if (selected) "●  " else "○  "

    fun updateStats(fps: Int, latencyMs: Int) {
        stats.text = "$fps images/s · $latencyMs ms"
    }

    fun open() {
        view.visibility = View.VISIBLE
        view.bringToFront()
    }

    fun close() {
        view.visibility = View.GONE
    }

    companion object {
        const val MODE_EXTENDED = 0
        const val MODE_DUPLICATE = 1
        const val MODE_TABLET_ONLY = 2
        private const val WIDTH_DP = 250
        private const val PANEL_COLOR = 0x99000000.toInt()
    }
}
