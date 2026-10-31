package com.screenlink36.app

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.File
import java.math.BigInteger
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object Crypto {
    private val random = SecureRandom()

    fun randomBytes(n: Int): ByteArray = ByteArray(n).also(random::nextBytes)

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        parts.forEach(mac::update)
        return mac.doFinal()
    }

    fun derive(psk: ByteArray, label: String, nonceC: ByteArray, nonceS: ByteArray): ByteArray =
        hmac(psk, label.toByteArray(Charsets.US_ASCII), nonceC, nonceS)

    fun equal(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)
}

/** ECDH P-256 de l'appairage. Clés publiques : X puis Y, 32 octets chacun, big-endian. */
class PairingKey {
    private val pair = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()

    val publicBytes: ByteArray = (pair.public as ECPublicKey).w.let { fixed(it.affineX) + fixed(it.affineY) }

    fun agree(peer: ByteArray): ByteArray {
        val point = ECPoint(BigInteger(1, peer.copyOfRange(0, 32)), BigInteger(1, peer.copyOfRange(32, 64)))
        val params = (pair.public as ECPublicKey).params
        val peerKey = KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(point, params))
        return KeyAgreement.getInstance("ECDH").run {
            init(pair.private)
            doPhase(peerKey, true)
            generateSecret()
        }
    }

    private fun fixed(value: BigInteger): ByteArray {
        val raw = value.toByteArray().takeLast(32).toByteArray()
        return ByteArray(32 - raw.size) + raw
    }
}

/** Clé d'appairage chiffrée par une clé d'Android Keystore, qui ne quitte jamais le matériel. */
class PairingStore(private val file: File) {
    private fun wrappingKey(): SecretKey {
        val store = KeyStore.getInstance(PROVIDER).apply { load(null) }
        (store.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, PROVIDER).apply {
            init(KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build())
        }.generateKey()
    }

    fun save(psk: ByteArray) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, wrappingKey())
        file.writeBytes(cipher.iv + cipher.doFinal(psk))
    }

    fun load(): ByteArray? {
        if (!file.exists()) return null
        val data = file.readBytes()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, wrappingKey(), GCMParameterSpec(128, data, 0, 12))
        return cipher.doFinal(data, 12, data.size - 12)
    }

    private companion object {
        const val PROVIDER = "AndroidKeyStore"
        const val ALIAS = "screenlink36-pairing"
    }
}

/** AES-256-GCM à nonce compteur : chaque sens a sa clé, le compteur ne se répète jamais. */
class Gcm(key: ByteArray) {
    private val spec = SecretKeySpec(key, "AES")
    private val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    private var counter = 0L

    private fun nonce(): GCMParameterSpec {
        val iv = ByteArray(12)
        var c = counter++
        for (i in 11 downTo 4) {
            iv[i] = c.toByte()
            c = c ushr 8
        }
        return GCMParameterSpec(128, iv)
    }

    fun seal(plain: ByteArray): ByteArray {
        cipher.init(Cipher.ENCRYPT_MODE, spec, nonce())
        return cipher.doFinal(plain)
    }

    fun open(data: ByteArray, length: Int): ByteArray {
        cipher.init(Cipher.DECRYPT_MODE, spec, nonce())
        return cipher.doFinal(data, 0, length)
    }
}
