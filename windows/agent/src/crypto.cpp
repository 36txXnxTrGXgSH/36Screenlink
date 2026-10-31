#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace {

BCRYPT_ALG_HANDLE gcmProvider() {
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        BCryptOpenAlgorithmProvider(&h, BCRYPT_AES_ALGORITHM, nullptr, 0);
        BCryptSetProperty(h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        return h;
    }();
    return alg;
}

void makeNonce(uint64_t counter, uint8_t nonce[12]) {
    std::memset(nonce, 0, 12);
    for (int i = 0; i < 8; ++i) nonce[11 - i] = static_cast<uint8_t>(counter >> (i * 8));
}

} // namespace

void randomBytes(void* out, size_t n) {
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

Key256 hmacSha256(const Key256& key, const void* data, size_t n) {
    Key256 out{};
    BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
               static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), out.data(),
               static_cast<ULONG>(out.size()));
    return out;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

EcdhP256::~EcdhP256() {
    if (key_) BCryptDestroyKey(key_);
}

bool EcdhP256::generate() {
    return BCRYPT_SUCCESS(BCryptGenerateKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, &key_, 256, 0)) && BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key_, 0));
}

bool EcdhP256::publicKey(uint8_t out[64]) const {
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG size = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &size, 0))) return false;
    std::memcpy(out, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    return true;
}

bool EcdhP256::agree(const uint8_t peer[64], uint8_t secret[32]) const {
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), peer, 64);

    BCRYPT_KEY_HANDLE peerKey = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey, blob, sizeof(blob), 0))) return false;
    BCRYPT_SECRET_HANDLE agreed = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptSecretAgreement(key_, peerKey, &agreed, 0));
    if (ok) {
        ULONG size = 0;
        ok = BCRYPT_SUCCESS(BCryptDeriveKey(agreed, BCRYPT_KDF_RAW_SECRET, nullptr, secret, 32, &size, 0)) && size == 32;
        BCryptDestroySecret(agreed);
        // BCrypt livre le secret brut en little-endian.
        if (ok) std::reverse(secret, secret + 32);
    }
    BCryptDestroyKey(peerKey);
    return ok;
}

Aes256Gcm::~Aes256Gcm() {
    if (key_) BCryptDestroyKey(key_);
}

bool Aes256Gcm::init(const Key256& key) {
    if (key_) {
        BCryptDestroyKey(key_);
        key_ = nullptr;
    }
    return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(gcmProvider(), &key_, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                                     static_cast<ULONG>(key.size()), 0));
}

bool Aes256Gcm::seal(const uint8_t* in, size_t n, uint64_t counter, uint8_t* out) {
    uint8_t nonce[12];
    makeNonce(counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbTag = out + n;
    info.cbTag = kTag;
    ULONG written = 0;
    return BCRYPT_SUCCESS(BCryptEncrypt(key_, const_cast<PUCHAR>(in), static_cast<ULONG>(n), &info, nullptr, 0, out,
                                        static_cast<ULONG>(n), &written, 0));
}

bool Aes256Gcm::open(const uint8_t* in, size_t n, uint64_t counter, uint8_t* out) {
    if (n < kTag) return false;
    const size_t plain = n - kTag;
    uint8_t nonce[12];
    makeNonce(counter, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbTag = const_cast<PUCHAR>(in + plain);
    info.cbTag = kTag;
    ULONG written = 0;
    return BCRYPT_SUCCESS(BCryptDecrypt(key_, const_cast<PUCHAR>(in), static_cast<ULONG>(plain), &info, nullptr, 0, out,
                                        static_cast<ULONG>(plain), &written, 0));
}
