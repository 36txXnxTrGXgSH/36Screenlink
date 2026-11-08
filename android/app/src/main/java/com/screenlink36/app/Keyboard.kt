package com.screenlink36.app

import android.content.Context
import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import java.nio.ByteBuffer

/** Traduit le clavier de la tablette vers les codes attendus par Windows. */
object Keyboard {
    private const val EXTENDED = 0x100
    const val BACKSPACE = 0x0E

    // Codes Linux étendus vers scancodes set 1 à préfixe E0. De 1 à 88, les deux tables coïncident.
    private val extended = mapOf(
        96 to 0x1C, 97 to 0x1D, 98 to 0x35, 99 to 0x37, 100 to 0x38, 102 to 0x47, 103 to 0x48, 104 to 0x49,
        105 to 0x4B, 106 to 0x4D, 107 to 0x4F, 108 to 0x50, 109 to 0x51, 110 to 0x52, 111 to 0x53,
        125 to 0x5B, 126 to 0x5C, 127 to 0x5D,
    )

    private const val SCAN_GRAVE = 41
    private const val SCAN_TAB = 0x0F

    // Touches sans code physique (clavier à l'écran, claviers logiciels de développeur, adb) :
    // envoyées en codes virtuels Windows, qui suivent la disposition du clavier du PC.
    private val virtual = buildMap {
        put(KeyEvent.KEYCODE_ENTER, 0x0D); put(KeyEvent.KEYCODE_NUMPAD_ENTER, 0x0D)
        put(KeyEvent.KEYCODE_DEL, 0x08); put(KeyEvent.KEYCODE_TAB, 0x09); put(KeyEvent.KEYCODE_ESCAPE, 0x1B)
        put(KeyEvent.KEYCODE_FORWARD_DEL, 0x2E); put(KeyEvent.KEYCODE_INSERT, 0x2D)
        put(KeyEvent.KEYCODE_MOVE_HOME, 0x24); put(KeyEvent.KEYCODE_MOVE_END, 0x23)
        put(KeyEvent.KEYCODE_PAGE_UP, 0x21); put(KeyEvent.KEYCODE_PAGE_DOWN, 0x22)
        put(KeyEvent.KEYCODE_DPAD_UP, 0x26); put(KeyEvent.KEYCODE_DPAD_DOWN, 0x28)
        put(KeyEvent.KEYCODE_DPAD_LEFT, 0x25); put(KeyEvent.KEYCODE_DPAD_RIGHT, 0x27)
        put(KeyEvent.KEYCODE_CTRL_LEFT, 0xA2); put(KeyEvent.KEYCODE_CTRL_RIGHT, 0xA3)
        put(KeyEvent.KEYCODE_ALT_LEFT, 0xA4); put(KeyEvent.KEYCODE_ALT_RIGHT, 0xA5)
        put(KeyEvent.KEYCODE_SHIFT_LEFT, 0xA0); put(KeyEvent.KEYCODE_SHIFT_RIGHT, 0xA1)
        put(KeyEvent.KEYCODE_META_LEFT, 0x5B); put(KeyEvent.KEYCODE_META_RIGHT, 0x5C)
        put(KeyEvent.KEYCODE_CAPS_LOCK, 0x14); put(KeyEvent.KEYCODE_SYSRQ, 0x2C)
        for (i in 0..11) put(KeyEvent.KEYCODE_F1 + i, 0x70 + i)
    }

    private val modifierKeys = setOf(0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C)

    // Lettres et chiffres : seulement avec Ctrl, Alt ou Windows, sinon ils partent en texte.
    private fun shortcutKey(keyCode: Int): Int? = when (keyCode) {
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 0x41 + keyCode - KeyEvent.KEYCODE_A
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> 0x30 + keyCode - KeyEvent.KEYCODE_0
        KeyEvent.KEYCODE_SPACE -> 0x20
        else -> null
    }

    private val heldVirtual = mutableSetOf<Int>()
    private var altTabDown = false

    /**
     * Messages à envoyer pour une touche, ou null pour la laisser au système (volume, marche)
     * ou au texte. Android capte Alt+Tab : Alt+² (touche sous Échap) le remplace.
     */
    fun toWindows(event: KeyEvent): List<ByteArray>? {
        val down = event.action == KeyEvent.ACTION_DOWN
        val scan = event.scanCode
        if (scan == SCAN_GRAVE && (event.isAltPressed || (!down && altTabDown))) {
            altTabDown = down
            return listOf(keyMessage(down, SCAN_TAB))
        }
        if (scan in 1..88) return listOf(keyMessage(down, scan))
        if (scan != 0) return extended[scan]?.let { listOf(keyMessage(down, it or EXTENDED)) }

        val vk = virtual[event.keyCode]
            ?: shortcutKey(event.keyCode)?.takeIf { event.isCtrlPressed || event.isAltPressed || event.isMetaPressed }
            ?: return null
        if (vk in modifierKeys) {
            if (down) heldVirtual += vk else heldVirtual -= vk
            return listOf(virtualMessage(down, vk))
        }
        // Clavier qui annonce Ctrl ou Alt sans envoyer la touche : on l'encadre nous-mêmes.
        val wrap = buildList {
            if (event.isCtrlPressed && heldVirtual.none { it == 0xA2 || it == 0xA3 }) add(0xA2)
            if (event.isAltPressed && heldVirtual.none { it == 0xA4 || it == 0xA5 }) add(0xA4)
            if (event.isShiftPressed && heldVirtual.none { it == 0xA0 || it == 0xA1 } && vk != 0x20) add(0xA0)
            if (event.isMetaPressed && heldVirtual.none { it == 0x5B || it == 0x5C }) add(0x5B)
        }
        if (!down) return listOf(virtualMessage(false, vk))
        return wrap.map { virtualMessage(true, it) } + virtualMessage(true, vk) +
            if (wrap.isEmpty()) emptyList() else listOf(virtualMessage(false, vk)) + wrap.reversed().map { virtualMessage(false, it) }
    }

    fun virtualMessage(down: Boolean, vk: Int) = keyMessage(down, Protocol.KEY_VIRTUAL or vk)

    fun keyMessage(down: Boolean, code: Int): ByteArray =
        ByteBuffer.allocate(4).put(Protocol.MSG_KEY.toByte()).put(if (down) 1 else 0).putShort(code.toShort()).array()

    fun textMessage(text: String): ByteArray = byteArrayOf(Protocol.MSG_TEXT.toByte()) + text.toByteArray()
}

/** Vue invisible qui reçoit le clavier à l'écran. Le texte en cours de saisie part aussitôt vers Windows. */
class SoftKeyboardTarget(context: Context, private val sink: (ByteArray) -> Unit) : View(context) {

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        // Pas de texte en mode mot de passe visible : les suggestions réécriraient ce qui est déjà envoyé.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN
        return object : BaseInputConnection(this, false) {
            private var composing = 0

            private fun replace(text: CharSequence) {
                backspace(composing)
                if (text.isNotEmpty()) sink(Keyboard.textMessage(text.toString()))
            }

            private fun backspace(count: Int) = repeat(count) {
                sink(Keyboard.keyMessage(true, Keyboard.BACKSPACE))
                sink(Keyboard.keyMessage(false, Keyboard.BACKSPACE))
            }

            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                replace(text)
                composing = 0
                return true
            }

            override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
                replace(text)
                composing = text.length
                return true
            }

            override fun finishComposingText(): Boolean {
                composing = 0
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                backspace(beforeLength)
                return true
            }
        }
    }
}
