package com.screenlink36.app

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack

/** Joue le son du PC. Écriture non bloquante : un retard audio ne doit jamais ralentir la vidéo. */
class AudioPlayer {
    private var track: AudioTrack? = null
    private var rate = 0

    fun write(data: ByteArray, offset: Int, length: Int, sampleRate: Int) {
        if (sampleRate != rate) start(sampleRate)
        track?.write(data, offset, length, AudioTrack.WRITE_NON_BLOCKING)
    }

    private fun start(sampleRate: Int) {
        stop()
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRate)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()
        val minimum = AudioTrack.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(format)
            .setBufferSizeInBytes(minimum * 2)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
            .also { it.play() }
        rate = sampleRate
    }

    fun stop() {
        track?.let {
            it.pause()
            it.flush()
            it.release()
        }
        track = null
        rate = 0
    }
}
