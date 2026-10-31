package com.screenlink36.app

// Miroir de windows/agent/src/protocol.h : toute modification se reporte des deux côtés.
object Protocol {
    const val PORT = 7236
    const val VERSION = 2
    const val MAX_RECORD = 8 * 1024 * 1024
    val MAGIC = byteArrayOf('S'.code.toByte(), '3'.code.toByte(), '6'.code.toByte(), 'X'.code.toByte())

    const val MODE_SESSION = 0
    const val MODE_PAIR = 1

    const val STATUS_OK = 0
    const val STATUS_NOT_PAIRED = 1
    const val STATUS_BAD_VERSION = 2
    const val STATUS_PAIR_REFUSED = 3

    const val MSG_HELLO = 1
    const val MSG_STREAM_CONFIG = 2
    const val MSG_VIDEO = 3
    const val MSG_TOUCH = 4
    const val MSG_PING = 5
    const val MSG_PONG = 6
    const val MSG_STATS = 7
    const val MSG_REQUEST_IDR = 8
    const val MSG_CURSOR_POS = 9
    const val MSG_CURSOR_SHAPE = 10
    const val MSG_CLOCK = 11
    const val MSG_KEY = 12
    const val MSG_TEXT = 13
    const val MSG_PEN = 14

    const val PEN_IN_RANGE = 1
    const val PEN_CONTACT = 2
    const val PEN_BARREL = 4
    const val PEN_ERASER = 8

    const val CODEC_AVC = 1
    const val CODEC_HEVC = 2

    const val FLAG_CONFIG = 1
    const val FLAG_KEY = 2
}
