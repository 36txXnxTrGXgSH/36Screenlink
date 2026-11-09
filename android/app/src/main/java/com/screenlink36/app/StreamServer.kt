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
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.ByteBuffer
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

/**
 * La tablette écoute et le PC se connecte vers elle, en USB comme en Wi-Fi.
 *
 * Chaque connexion s'authentifie sur son propre thread : un inconnu du réseau qui ouvre des
 * connexions muettes ne retarde plus le vrai PC. Une seule session diffuse à la fois.
 */
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
        fun onControlState(displayMode: Int, fps: Int)
        fun onLink(usb: Boolean)
        fun onStats(fps: Int, latencyMs: Int)
        /** Le PC montre l'UAC ou l'écran de verrouillage : la saisie attend une confirmation. */
        fun onSecureScreen(active: Boolean)
    }

    private val pairing = PairingStore(File(context.noBackupFilesDir, "pairing.v2"))
    private val nsd = context.getSystemService(NsdManager::class.java)
    private val touchSender = Executors.newSingleThreadExecutor()
    private val handshakes = Executors.newCachedThreadPool()
    private val pendingByAddress = ConcurrentHashMap<InetAddress, AtomicInteger>()
    private val pendingTotal = AtomicInteger()
    private val sessionLock = Object()
    private val pairLock = Object()
    private val audio = AudioPlayer()
    @Volatile private var running = false
    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null
    @Volatile private var client: Socket? = null
    @Volatile private var tx: Gcm? = null
    private var output: OutputStream? = null
    private val decoder = VideoDecoder(surface, ::sendStats) { send(byteArrayOf(Protocol.MSG_REQUEST_IDR.toByte())) }

    // Le décodeur peut attendre un tampon libre : il a son propre thread, pour que la lecture
    // du réseau, le son et les pings ne restent jamais bloqués derrière lui.
    private sealed class VideoItem {
        class Frame(val data: ByteArray, val offset: Int, val length: Int, val pts: Long, val flags: Int) : VideoItem()
        class Config(val mime: String, val width: Int, val height: Int, val fps: Int, val rotation: Int) : VideoItem()
    }
    private val videoQueue = ArrayBlockingQueue<VideoItem>(VIDEO_QUEUE)
    @Volatile private var feeding = false
    private var feeder: Thread? = null

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
                // Plafond de connexions en cours d'authentification, par adresse et en tout.
                val address = s.inetAddress
                val perAddress = pendingByAddress.computeIfAbsent(address) { AtomicInteger() }
                val limit = if (address.isLoopbackAddress) MAX_PENDING_LOOPBACK else MAX_PENDING_PER_ADDRESS
                if (pendingTotal.get() >= MAX_PENDING || perAddress.get() >= limit) {
                    runCatching { s.close() }
                    continue
                }
                perAddress.incrementAndGet()
                pendingTotal.incrementAndGet()
                try {
                    handshakes.execute { serve(s, perAddress) }
                } catch (_: RuntimeException) {
                    perAddress.decrementAndGet()
                    pendingTotal.decrementAndGet()
                    runCatching { s.close() }
                }
            }
        }
    }

    fun stop() {
        running = false
        runCatching { nsd.unregisterService(nsdListener) }
        runCatching { serverSocket?.close() }
        runCatching { client?.close() }
        runCatching { acceptThread?.join(1000) }
        // Sans interruption : la session en cours se termine d'elle-même, socket fermée.
        handshakes.shutdown()
        touchSender.shutdownNow()
    }

    fun armPairing() {
        pairingArmed = true
    }

    /** Oublie le PC appairé : il faudra un nouvel appairage. */
    fun forgetPairing() {
        pairing.delete()
        runCatching { client?.close() }
    }

    fun sendControl(id: Int, value: Int) {
        sendInput(byteArrayOf(Protocol.MSG_CONTROL.toByte(), id.toByte(), value.toByte()))
    }

    fun disconnect() {
        client?.let { socket -> touchSender.execute { runCatching { socket.close() } } }
    }

    fun requestAudioRoute(onTablet: Boolean) {
        sendInput(byteArrayOf(Protocol.MSG_AUDIO_ROUTE.toByte(), if (onTablet) 1 else 0))
    }

    /** L'utilisateur a confirmé son identité : le PC accepte la saisie sur l'écran sécurisé. */
    fun armSecureInput() {
        sendInput(byteArrayOf(Protocol.MSG_SECURE_ARMED.toByte()))
    }

    /** Coupe la session si la taille change : le PC se reconnecte et relit l'orientation dans HELLO. */
    fun onScreenChanged(width: Int, height: Int) {
        // Clavier branché, mode sombre... : même taille, la session continue.
        if (width == screenWidth && height == screenHeight) return
        Log.i(TAG, "Écran : ${width}x$height, session ${client != null}")
        screenWidth = width
        screenHeight = height
        val current = client ?: return
        touchSender.execute { runCatching { current.close() } }
    }

    private class Authenticated(val socket: Socket, val input: DataInputStream, val out: OutputStream, val rx: Gcm, val tx: Gcm)

    private fun serve(socket: Socket, perAddress: AtomicInteger) {
        var session: Authenticated? = null
        try {
            session = authenticate(socket)
        } catch (e: IOException) {
            Log.i(TAG, "Connexion refusée : ${e.message}")
        } catch (e: Throwable) {
            Log.e(TAG, "Authentification interrompue", e)
        } finally {
            perAddress.decrementAndGet()
            pendingTotal.decrementAndGet()
            if (session == null) runCatching { socket.close() }
        }
        if (session != null) stream(session)
    }

    private fun authenticate(socket: Socket): Authenticated? {
        socket.tcpNoDelay = true
        // Délai court avant authentification : un inconnu du réseau ne bloque pas longtemps l'attente.
        socket.soTimeout = HANDSHAKE_TIMEOUT_MS
        val input = DataInputStream(BufferedInputStream(socket.getInputStream(), 256 * 1024))
        val out = socket.getOutputStream()

        val hello = ByteArray(6)
        input.readFully(hello)
        if (!hello.copyOfRange(0, 4).contentEquals(Protocol.MAGIC)) return null
        if (hello[4].toInt() != Protocol.VERSION) {
            out.write(reply(Protocol.STATUS_BAD_VERSION))
            return null
        }
        val nonceC = ByteArray(16).also(input::readFully)

        if (hello[5].toInt() == Protocol.MODE_PAIR) {
            val peer = ByteArray(64).also(input::readFully)
            synchronized(pairLock) {
                // Possible en Wi-Fi : le code comparé des deux côtés écarte un intrus du réseau.
                if (pairing.exists() && !pairingArmed) {
                    out.write(reply(Protocol.STATUS_PAIR_REFUSED))
                    return null
                }
                pair(socket, nonceC, peer, input, out)
            }
            return null
        }

        val pubA = ByteArray(64).also(input::readFully)
        val psk = pairing.load()
        if (psk == null) {
            out.write(reply(Protocol.STATUS_NOT_PAIRED))
            return null
        }
        // ECDH éphémère : une clé d'appairage volée plus tard ne déchiffre pas cette session.
        val ephemeral = PairingKey()
        val secret = ephemeral.agree(pubA)
        val pubT = ephemeral.publicBytes
        val nonceS = Crypto.randomBytes(16)
        val context = nonceC + nonceS + pubA + pubT
        out.write(reply(Protocol.STATUS_OK) + nonceS + pubT + Crypto.hmac(psk, "srv".toByteArray(), context))
        val clientMac = ByteArray(32).also(input::readFully)
        if (!Crypto.equal(clientMac, Crypto.hmac(psk, "cli".toByteArray(), context))) return null

        val keyed = context + secret
        val rx = Gcm(Crypto.hmac(psk, "a2t".toByteArray(), keyed))
        val tx = Gcm(Crypto.hmac(psk, "t2a".toByteArray(), keyed))
        secret.fill(0)
        keyed.fill(0)
        return Authenticated(socket, input, out, rx, tx)
    }

    private fun stream(session: Authenticated) {
        // Un PC revenu prend la place de l'ancienne session, qui se ferme d'abord.
        client?.let { old -> runCatching { old.close() } }
        synchronized(sessionLock) {
            val socket = session.socket
            if (!running) {
                runCatching { socket.close() }
                return
            }
            try {
                client = socket
                // En USB, le PC arrive par le tunnel d'adb, donc depuis la tablette elle-même.
                ui.onLink(socket.inetAddress.isLoopbackAddress)
                output = session.out
                tx = session.tx
                socket.soTimeout = READ_TIMEOUT_MS
                startFeeder()

                Log.i(TAG, "Session ouverte, écran ${screenWidth}x$screenHeight")
                send(ByteBuffer.allocate(8).put(Protocol.MSG_HELLO.toByte()).putShort(screenWidth.toShort())
                    .putShort(screenHeight.toShort()).putShort(maxFps.toShort()).put(3).array())

                var buffer = ByteArray(1024 * 1024)
                while (running) {
                    val length = session.input.readInt()
                    if (length < 16 || length > Protocol.MAX_RECORD) throw IOException("taille invalide $length")
                    if (buffer.size < length) buffer = ByteArray(length)
                    session.input.readFully(buffer, 0, length)
                    dispatch(session.rx.open(buffer, length))
                }
            } catch (e: IOException) {
                Log.i(TAG, "Session terminée : ${e.message}")
            } catch (e: Throwable) {
                // Même une erreur grave (mémoire) ne doit jamais empêcher la session suivante.
                Log.e(TAG, "Session interrompue", e)
            } finally {
                val stopStart = SystemClock.uptimeMillis()
                stopFeeder()
                decoder.stop()
                audio.stop()
                Log.i(TAG, "Décodeur arrêté en ${SystemClock.uptimeMillis() - stopStart} ms")
                tx = null
                output = null
                if (client === socket) client = null
                runCatching { socket.close() }
                ui.onSecureScreen(false)
                if (running) ui.onWaiting("En attente du PC")
            }
        }
    }

    private fun startFeeder() {
        videoQueue.clear()
        feeding = true
        feeder = thread(name = "decoder-input", priority = Thread.MAX_PRIORITY) {
            while (feeding) {
                val item = try {
                    videoQueue.poll(100, TimeUnit.MILLISECONDS)
                } catch (_: InterruptedException) {
                    break
                } ?: continue
                try {
                    when (item) {
                        is VideoItem.Frame -> decoder.submit(item.data, item.offset, item.length, item.pts, item.flags)
                        is VideoItem.Config -> {
                            decoder.start(item.mime, item.width, item.height, item.fps, item.rotation)
                            surface.setFrameRate(item.fps.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                            // Avec une rotation de 90 ou 270, l'image affichée a ses côtés inversés.
                            if (item.rotation % 180 == 0) ui.onStream(item.width, item.height, item.fps)
                            else ui.onStream(item.height, item.width, item.fps)
                        }
                    }
                } catch (e: RuntimeException) {
                    Log.w(TAG, "Décodeur", e)
                }
            }
        }
    }

    private fun stopFeeder() {
        feeding = false
        feeder?.let {
            it.interrupt()
            try {
                it.join(1000)
            } catch (_: InterruptedException) {
                // Arrêt de l'application : le thread du décodeur s'arrête de lui-même.
            }
        }
        feeder = null
        videoQueue.clear()
    }

    private fun reply(status: Int) = Protocol.MAGIC + status.toByte()

    /** ECDH : une clé n'est enregistrée qu'une fois le même code confirmé des deux côtés. */
    private fun pair(socket: Socket, nonceC: ByteArray, peer: ByteArray, input: DataInputStream, out: OutputStream) {
        val key = PairingKey()
        val secret = key.agree(peer)
        socket.soTimeout = PAIRING_TIMEOUT_MS
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

    /** Chaque message est vérifié avant lecture : un message trop court ou incohérent est ignoré. */
    private fun dispatch(msg: ByteArray) {
        if (msg.isEmpty()) return
        val b = ByteBuffer.wrap(msg)
        when (b.get().toInt()) {
            Protocol.MSG_VIDEO -> {
                if (b.remaining() < 9) return
                val flags = b.get().toInt()
                val pts = b.long
                val frame = VideoItem.Frame(msg, b.position(), b.remaining(), pts, flags)
                if (!videoQueue.offer(frame)) {
                    // Décodeur en retard : on vide la file et on repart d'une image clé. La configuration
                    // du flux et les images clés restent : sans elles, l'écran resterait noir.
                    videoQueue.removeIf { it is VideoItem.Frame && it.flags and (Protocol.FLAG_CONFIG or Protocol.FLAG_KEY) == 0 }
                    Log.i(TAG, "File du décodeur pleine : attente d'une image clé")
                    decoder.dropUntilKey()
                    videoQueue.offer(frame)
                }
            }
            Protocol.MSG_PING -> {
                if (b.remaining() < 8) return
                val t0 = b.long
                send(ByteBuffer.allocate(17).put(Protocol.MSG_PONG.toByte()).putLong(t0)
                    .putLong(SystemClock.elapsedRealtimeNanos() / 1000).array())
            }
            Protocol.MSG_CLOCK -> if (b.remaining() >= 8) decoder.clockOffsetUs = b.long
            Protocol.MSG_AUDIO -> {
                if (b.remaining() < 4) return
                val rate = b.int
                if (rate !in 8_000..192_000) return
                audio.write(msg, b.position(), b.remaining() and 3.inv(), rate)
            }
            Protocol.MSG_CONTROL_STATE -> {
                if (b.remaining() < 2) return
                val mode = b.get().toInt()
                val fps = b.get().toInt() and 0xFF
                ui.onControlState(mode, fps)
            }
            Protocol.MSG_AUDIO_STATE -> {
                if (b.remaining() < 1) return
                val onTablet = b.get().toInt() != 0
                if (!onTablet) audio.stop()
                ui.onAudioRoute(onTablet)
            }
            Protocol.MSG_SECURE_STATE -> if (b.remaining() >= 1) ui.onSecureScreen(b.get().toInt() != 0)
            Protocol.MSG_STREAM_CONFIG -> {
                if (b.remaining() < 13) throw IOException("configuration tronquée")
                val codec = b.get().toInt()
                val w = b.short.toInt() and 0xFFFF
                val h = b.short.toInt() and 0xFFFF
                val fps = b.short.toInt() and 0xFFFF
                b.int // débit, informatif
                val rotation = b.short.toInt() and 0xFFFF
                if (codec != Protocol.CODEC_AVC && codec != Protocol.CODEC_HEVC || w !in 16..8192 || h !in 16..8192 ||
                    fps !in 1..480 || rotation !in setOf(0, 90, 180, 270)) throw IOException("configuration invalide")
                val mime = if (codec == Protocol.CODEC_HEVC) "video/hevc" else "video/avc"
                videoQueue.clear()
                videoQueue.put(VideoItem.Config(mime, w, h, fps, rotation))
            }
            Protocol.MSG_CURSOR_POS -> {
                if (b.remaining() < 5) return
                val visible = b.get().toInt() != 0
                val x = b.short.toInt()
                val y = b.short.toInt()
                ui.onCursorPosition(visible, x, y)
            }
            Protocol.MSG_CURSOR_SHAPE -> {
                if (b.remaining() < 8) return
                val w = b.short.toInt() and 0xFFFF
                val h = b.short.toInt() and 0xFFFF
                val hx = b.short.toInt() and 0xFFFF
                val hy = b.short.toInt() and 0xFFFF
                // Un curseur Windows fait au plus 256 × 256 : au-delà, ou incomplet, il est ignoré.
                if (w > MAX_CURSOR || h > MAX_CURSOR || b.remaining().toLong() < w.toLong() * h * 4) return
                ui.onCursorShape(w, h, hx, hy, msg, b.position())
            }
        }
    }

    private fun sendStats(s: VideoDecoder.Stats) {
        Log.i(TAG, "stats $s")
        ui.onStats(s.frames, s.avgLatUs / 1000)
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
        // Une tablette déjà appairée n'accepte un nouveau PC qu'à la demande de l'utilisateur.
        // Hors de l'instance : l'autorisation survit à une recréation de l'écran.
        @Volatile private var pairingArmed = false
        private const val TAG = "S36X"
        private const val READ_TIMEOUT_MS = 6000
        private const val HANDSHAKE_TIMEOUT_MS = 3000
        private const val PAIRING_TIMEOUT_MS = 120_000
        private const val MAX_PENDING = 16
        private const val MAX_PENDING_PER_ADDRESS = 2
        private const val MAX_PENDING_LOOPBACK = 4
        private const val MAX_CURSOR = 256
        private const val VIDEO_QUEUE = 4
    }
}
