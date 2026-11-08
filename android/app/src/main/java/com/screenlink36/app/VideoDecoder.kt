package com.screenlink36.app

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.util.concurrent.ConcurrentHashMap
import kotlin.concurrent.thread

/** Décodage matériel. Le rendu est immédiat : on affiche toujours l'image la plus récente. */
class VideoDecoder(private val surface: Surface, private val onStats: (Stats) -> Unit, private val requestIdr: () -> Unit) {

    data class Stats(val frames: Int, val avgLatUs: Int, val p95LatUs: Int, val avgDecUs: Int, val drops: Int)

    @Volatile var clockOffsetUs = 0L
    @Volatile private var running = false
    private var codec: MediaCodec? = null
    private var outputThread: Thread? = null
    @Volatile private var waitingForKey = false
    private val queuedAt = ConcurrentHashMap<Long, Long>()
    private val latencies = IntArray(1024)
    private var latencyCount = 0
    private var decodeSum = 0L
    private var drops = 0

    fun start(mime: String, width: Int, height: Int, fps: Int, rotation: Int) {
        stop()
        val format = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            // Décodage le plus rapide possible quelle que soit la cadence : avec fps × 2, le décodeur
            // se calait sur 60 Hz et doublait la latence (21 ms contre 11 ms mesurés).
            setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())
            setInteger(MediaFormat.KEY_ROTATION, rotation)
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        }
        val name = pickDecoder(mime)
        Log.i(TAG, "Décodeur $name pour $mime ${width}x$height@$fps")
        val c = MediaCodec.createByCodecName(name)
        c.configure(format, surface, null, 0)
        c.start()
        codec = c
        running = true
        waitingForKey = false
        outputThread = thread(name = "decoder-output", priority = Thread.MAX_PRIORITY) { drain(c) }
    }

    private fun pickDecoder(mime: String): String {
        val decoders = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { !it.isEncoder && it.supportedTypes.any { t -> t.equals(mime, true) } }
        val best = decoders.firstOrNull { it.name.endsWith(".lowlatency") }
            ?: decoders.firstOrNull { it.isHardwareAccelerated }
            ?: decoders.first()
        return best.name
    }

    fun submit(data: ByteArray, offset: Int, length: Int, ptsUs: Long, flags: Int) {
        val c = codec ?: return
        val isConfig = flags and Protocol.FLAG_CONFIG != 0
        val isKey = flags and Protocol.FLAG_KEY != 0
        if (waitingForKey && !isConfig && !isKey) return

        val index = try {
            c.dequeueInputBuffer(INPUT_TIMEOUT_US)
        } catch (e: IllegalStateException) {
            Log.w(TAG, "Décodeur indisponible", e)
            return
        }
        if (index < 0) {
            // File du décodeur pleine : le flux P est rompu, il faut une image clé.
            Log.i(TAG, "Aucun tampon libre dans le décodeur : attente d'une image clé")
            synchronized(latencies) { drops++ }
            waitingForKey = true
            requestIdr()
            return
        }
        waitingForKey = false
        val input = c.getInputBuffer(index) ?: return
        input.clear()
        input.put(data, offset, length)
        val bufferFlags = if (isConfig) MediaCodec.BUFFER_FLAG_CODEC_CONFIG else if (isKey) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
        if (!isConfig) queuedAt[ptsUs] = nowUs()
        c.queueInputBuffer(index, 0, length, ptsUs, bufferFlags)
    }

    /** Images perdues avant le décodeur : on attend la prochaine image clé, demandée au PC. */
    fun dropUntilKey() {
        synchronized(latencies) { drops++ }
        if (!waitingForKey) requestIdr()
        waitingForKey = true
    }

    private fun drain(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        var lastReport = SystemClock.uptimeMillis()
        while (running) {
            try {
                var index = c.dequeueOutputBuffer(info, OUTPUT_TIMEOUT_US)
                if (index >= 0) {
                    // Si plusieurs images attendent, seule la dernière a encore un intérêt.
                    while (true) {
                        val next = c.dequeueOutputBuffer(info, 0)
                        if (next < 0) break
                        c.releaseOutputBuffer(index, false)
                        synchronized(latencies) { drops++ }
                        index = next
                    }
                    c.releaseOutputBuffer(index, true)
                    record(info.presentationTimeUs)
                }
            } catch (e: IllegalStateException) {
                if (running) Log.w(TAG, "Sortie du décodeur interrompue", e)
                return
            }
            val now = SystemClock.uptimeMillis()
            if (now - lastReport >= 1000) {
                lastReport = now
                report()
            }
        }
    }

    private fun record(ptsUs: Long) {
        // Avant la synchronisation des horloges, une latence calculée n'a aucun sens.
        if (clockOffsetUs == 0L) return
        val now = nowUs()
        val latency = (now - clockOffsetUs - ptsUs).coerceIn(0, Int.MAX_VALUE.toLong()).toInt()
        val queued = queuedAt.remove(ptsUs)
        synchronized(latencies) {
            if (latencyCount < latencies.size) latencies[latencyCount] = latency
            latencyCount++
            if (queued != null) decodeSum += now - queued
        }
    }

    private fun report() {
        val stats = synchronized(latencies) {
            val n = minOf(latencyCount, latencies.size)
            if (n == 0) return
            val sorted = latencies.copyOf(n).also { it.sort() }
            val s = Stats(latencyCount, sorted.average().toInt(), sorted[(n * 95 / 100).coerceAtMost(n - 1)],
                (decodeSum / latencyCount).toInt(), drops)
            latencyCount = 0
            decodeSum = 0
            s
        }
        // Les pertes sont un total cumulé ; les autres valeurs portent sur la dernière seconde.
        onStats(stats)
        queuedAt.keys.removeIf { it < nowUs() - clockOffsetUs - STALE_US }
    }

    fun stop() {
        running = false
        try {
            outputThread?.join(500)
        } catch (_: InterruptedException) {
            // Arrêt de l'application : la sortie s'arrête d'elle-même, running étant faux.
        }
        outputThread = null
        codec?.let {
            try {
                it.stop()
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Arrêt du décodeur", e)
            }
            it.release()
        }
        codec = null
        queuedAt.clear()
    }

    private fun nowUs() = SystemClock.elapsedRealtimeNanos() / 1000

    companion object {
        private const val TAG = "S36X"
        private const val INPUT_TIMEOUT_US = 20_000L
        private const val OUTPUT_TIMEOUT_US = 10_000L
        private const val STALE_US = 2_000_000L
    }
}
