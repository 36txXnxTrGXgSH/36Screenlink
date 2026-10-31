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

    // Touches sans code physique, envoyées par le clavier à l'écran.
    private val virtual = mapOf(
        KeyEvent.KEYCODE_ENTER to 0x1C, KeyEvent.KEYCODE_DEL to BACKSPACE, KeyEvent.KEYCODE_TAB to 0x0F,
        KeyEvent.KEYCODE_ESCAPE to 0x01, KeyEvent.KEYCODE_FORWARD_DEL to (0x53 or EXTENDED),
        KeyEvent.KEYCODE_DPAD_UP to (0x48 or EXTENDED), KeyEvent.KEYCODE_DPAD_DOWN to (0x50 or EXTENDED),
        KeyEvent.KEYCODE_DPAD_LEFT to (0x4B or EXTENDED), KeyEvent.KEYCODE_DPAD_RIGHT to (0x4D or EXTENDED),
    )

    /** Renvoie le code Windows, ou null pour une touche à laisser au système (volume, marche). */
    fun toWindows(event: KeyEvent): Int? {
        val scan = event.scanCode
        return when {
            scan in 1..88 -> scan
            scan != 0 -> extended[scan]?.or(EXTENDED)
            else -> virtual[event.keyCode]
        }
    }

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
