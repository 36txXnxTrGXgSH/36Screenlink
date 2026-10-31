package com.screenlink36.app

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.ByteBuffer
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/** La tablette écoute et le PC se connecte vers elle, en USB comme en Wi-Fi. */
class StreamServer(
    context: Context,
    private val surface: Surface,
    private var screenWidth: Int,
    private var screenHeight: Int,
    private val maxFps: Int,
    private val ui: Ui,
) {
    interface Ui {
        fun onWaiting(message: String)
        fun onStream(width: Int, height: Int, fps: Int)
        fun onCursorPosition(visible: Boolean, x: Int, y: Int)
        fun onCursorShape(width: Int, height: Int, hotX: Int, hotY: Int, bgra: ByteArray, offset: Int)
        /** Montre le code et bloque jusqu'à la réponse de l'utilisateur. */
        fun confirmPairing(code: Int): Boolean
        fun onAudioRoute(onTablet: Boolean)
    }

    private val pairing = PairingStore(File(context.noBackupFilesDir, "pairing.v2"))
    private val nsd = context.getSystemService(NsdManager::class.java)
    private val touchSender = Executors.newSingleThreadExecutor()
    private val audio = AudioPlayer()
    @Volatile private var running = false
    // Une tablette déjà appairée n'accepte un nouveau PC qu'à la demande de l'utilisateur.
    @Volatile private var pairingArmed = false
    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null
    @Volatile private var client: Socket? = null
    @Volatile private var tx: Gcm? = null
    private var output: OutputStream? = null
    private val decoder = VideoDecoder(surface, ::sendStats) { send(byteArrayOf(Protocol.MSG_REQUEST_IDR.toByte())) }

    private val nsdListener = object : NsdManager.RegistrationListener {
        override fun onRegistrationFailed(info: NsdServiceInfo, code: Int) {
            Log.w(TAG, "mDNS refusé : $code")
        }
        override fun onUnregistrationFailed(info: NsdServiceInfo, code: Int) = Unit
        override fun onServiceRegistered(info: NsdServiceInfo) = Unit
        override fun onServiceUnregistered(info: NsdServiceInfo) = Unit
    }

    fun start() {
        running = true
        val socket = ServerSocket(Protocol.PORT).also { it.reuseAddress = true }
        serverSocket = socket
        nsd.registerService(NsdServiceInfo().apply {
            serviceName = "36Screenlink"
            serviceType = "_screenlink._tcp"
            port = Protocol.PORT
        }, NsdManager.PROTOCOL_DNS_SD, nsdListener)

        acceptThread = thread(name = "accept") {
            while (running) {
                val s = try {
                    socket.accept()
                } catch (_: IOException) {
                    break
                }
                try {
                    handle(s)
                } catch (e: IOException) {
                    Log.i(TAG, "Session terminée : ${e.message}")
                } catch (e: RuntimeException) {
                    Log.e(TAG, "Session interrompue", e)
                } finally {
                    val stopStart = SystemClock.uptimeMillis()
                    decoder.stop()
                    audio.stop()
                    Log.i(TAG, "Décodeur arrêté en ${SystemClock.uptimeMillis() - stopStart} ms")
                    tx = null
                    client = null
                    runCatching { s.close() }
                    if (running) ui.onWaiting("En attente du PC")
                }
            }
        }
    }

    fun stop() {
        running = false
        runCatching { nsd.unregisterService(nsdListener) }
        runCatching { serverSocket?.close() }
        runCatching { client?.close() }
        acceptThread?.join(1000)
        touchSender.shutdownNow()
    }

    fun armPairing() {
        pairingArmed = true
    }

    fun requestAudioRoute(onTablet: Boolean) {
        sendInput(byteArrayOf(Protocol.MSG_AUDIO_ROUTE.toByte(), if (onTablet) 1 else 0))
    }

    /** Coupe la session : le PC se reconnecte et relit la nouvelle orientation dans HELLO. */
    fun onScreenChanged(width: Int, height: Int) {
        Log.i(TAG, "Écran : ${width}x$height, session ${client != null}")
        screenWidth = width
        screenHeight = height
        val current = client ?: return
        touchSender.execute { runCatching { current.close() } }
    }

    private fun handle(socket: Socket) {
        client = socket
        socket.tcpNoDelay = true
        // Délai court avant authentification : un inconnu du réseau ne bloque pas longtemps l'attente.
        socket.soTimeout = HANDSHAKE_TIMEOUT_MS
        val input = DataInputStream(BufferedInputStream(socket.getInputStream(), 256 * 1024))
        val out = socket.getOutputStream()

        val hello = ByteArray(6)
        input.readFully(hello)
        if (!hello.copyOfRange(0, 4).contentEquals(Protocol.MAGIC)) return
        if (hello[4].toInt() != Protocol.VERSION) {
            out.write(reply(Protocol.STATUS_BAD_VERSION))
            return
        }
        val nonceC = ByteArray(16).also(input::readFully)

        if (hello[5].toInt() == Protocol.MODE_PAIR) {
            val peer = ByteArray(64).also(input::readFully)
            // Possible en Wi-Fi : le code comparé des deux côtés écarte un intrus du réseau.
            if (pairing.exists() && !pairingArmed) {
                out.write(reply(Protocol.STATUS_PAIR_REFUSED))
                return
            }
            pair(nonceC, peer, input, out)
            return
        }

        val psk = pairing.load()
        if (psk == null) {
            out.write(reply(Protocol.STATUS_NOT_PAIRED))
            return
        }
        val nonceS = Crypto.randomBytes(16)
        out.write(reply(Protocol.STATUS_OK) + nonceS + Crypto.derive(psk, "srv", nonceC, nonceS))
        val clientMac = ByteArray(32).also(input::readFully)
        if (!Crypto.equal(clientMac, Crypto.derive(psk, "cli", nonceC, nonceS))) return

        val rx = Gcm(Crypto.derive(psk, "a2t", nonceC, nonceS))
        output = out
        tx = Gcm(Crypto.derive(psk, "t2a", nonceC, nonceS))
        socket.soTimeout = READ_TIMEOUT_MS

        Log.i(TAG, "Session ouverte, écran ${screenWidth}x$screenHeight")
        send(ByteBuffer.allocate(8).put(Protocol.MSG_HELLO.toByte()).putShort(screenWidth.toShort())
            .putShort(screenHeight.toShort()).putShort(maxFps.toShort()).put(3).array())

        var buffer = ByteArray(1024 * 1024)
        while (running) {
            val length = input.readInt()
            if (length < 16 || length > Protocol.MAX_RECORD) throw IOException("taille invalide $length")
            if (buffer.size < length) buffer = ByteArray(length)
            input.readFully(buffer, 0, length)
            dispatch(rx.open(buffer, length))
        }
    }

    private fun reply(status: Int) = Protocol.MAGIC + status.toByte()

    /** ECDH : une clé n'est enregistrée qu'une fois le même code confirmé des deux côtés. */
    private fun pair(nonceC: ByteArray, peer: ByteArray, input: DataInputStream, out: OutputStream) {
        val key = PairingKey()
        val secret = key.agree(peer)
        client?.soTimeout = PAIRING_TIMEOUT_MS
        out.write(reply(Protocol.STATUS_OK) + key.publicBytes)

        val transcript = nonceC + peer + key.publicBytes
        val sas = ByteBuffer.wrap(Crypto.hmac(secret, "sas".toByteArray(), transcript)).int.toLong() and 0xFFFFFFFFL
        if (!ui.confirmPairing((sas % 1_000_000).toInt())) {
            out.write(reply(Protocol.STATUS_PAIR_REFUSED))
            return
        }
        val psk = Crypto.hmac(secret, "psk".toByteArray(), transcript)
        val mac = ByteArray(32).also(input::readFully)
        if (!Crypto.equal(mac, Crypto.hmac(psk, "cli".toByteArray(), transcript))) {
            out.write(reply(Protocol.STATUS_PAIR_REFUSED))
            return
        }
        pairing.save(psk)
        pairingArmed = false
        out.write(reply(Protocol.STATUS_OK) + Crypto.hmac(psk, "srv".toByteArray(), transcript))
        Log.i(TAG, "Appairage enregistré")
    }

    private fun dispatch(msg: ByteArray) {
        val b = ByteBuffer.wrap(msg)
        when (b.get().toInt()) {
            Protocol.MSG_VIDEO -> {
                val flags = b.get().toInt()
                val pts = b.long
                decoder.submit(msg, b.position(), b.remaining(), pts, flags)
            }
            Protocol.MSG_PING -> {
                val t0 = b.long
                send(ByteBuffer.allocate(17).put(Protocol.MSG_PONG.toByte()).putLong(t0)
                    .putLong(SystemClock.elapsedRealtimeNanos() / 1000).array())
            }
            Protocol.MSG_CLOCK -> decoder.clockOffsetUs = b.long
            Protocol.MSG_AUDIO -> {
                val rate = b.int
                audio.write(msg, b.position(), b.remaining(), rate)
            }
            Protocol.MSG_AUDIO_STATE -> {
                val onTablet = b.get().toInt() != 0
                if (!onTablet) audio.stop()
                ui.onAudioRoute(onTablet)
            }
            Protocol.MSG_STREAM_CONFIG -> {
                val codec = b.get().toInt()
                val w = b.short.toInt() and 0xFFFF
                val h = b.short.toInt() and 0xFFFF
                val fps = b.short.toInt() and 0xFFFF
                b.int // débit, informatif
                val rotation = b.short.toInt() and 0xFFFF
                val mime = if (codec == Protocol.CODEC_HEVC) "video/hevc" else "video/avc"
                decoder.start(mime, w, h, fps, rotation)
                surface.setFrameRate(fps.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                // Avec une rotation de 90 ou 270, l'image affichée a ses côtés inversés.
                if (rotation % 180 == 0) ui.onStream(w, h, fps) else ui.onStream(h, w, fps)
            }
            Protocol.MSG_CURSOR_POS -> {
                val visible = b.get().toInt() != 0
                val x = b.short.toInt()
                val y = b.short.toInt()
                ui.onCursorPosition(visible, x, y)
            }
            Protocol.MSG_CURSOR_SHAPE -> {
                val w = b.short.toInt() and 0xFFFF
                val h = b.short.toInt() and 0xFFFF
                val hx = b.short.toInt() and 0xFFFF
                val hy = b.short.toInt() and 0xFFFF
                ui.onCursorShape(w, h, hx, hy, msg, b.position())
            }
        }
    }

    private fun sendStats(s: VideoDecoder.Stats) {
        Log.i(TAG, "stats $s")
        send(ByteBuffer.allocate(21).put(Protocol.MSG_STATS.toByte()).putInt(s.frames).putInt(s.avgLatUs)
            .putInt(s.p95LatUs).putInt(s.avgDecUs).putInt(s.drops).array())
    }

    /** Appelé hors du thread UI : l'écriture socket y est interdite. */
    fun sendInput(message: ByteArray) {
        if (touchSender.isShutdown) return
        touchSender.execute { send(message) }
    }

    @Synchronized
    private fun send(plain: ByteArray) {
        val gcm = tx ?: return
        val out = output ?: return
        try {
            val sealed = gcm.seal(plain)
            out.write(ByteBuffer.allocate(4 + sealed.size).putInt(sealed.size).put(sealed).array())
        } catch (e: SocketException) {
            Log.i(TAG, "Envoi interrompu : ${e.message}")
        } catch (e: IOException) {
            Log.i(TAG, "Envoi interrompu : ${e.message}")
        }
    }

    companion object {
        private const val TAG = "S36X"
        private const val READ_TIMEOUT_MS = 6000
        private const val HANDSHAKE_TIMEOUT_MS = 3000
        private const val PAIRING_TIMEOUT_MS = 120_000
    }
}
