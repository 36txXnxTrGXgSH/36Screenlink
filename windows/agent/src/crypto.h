#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

using Key256 = std::array<uint8_t, 32>;

void randomBytes(void* out, size_t n);
Key256 hmacSha256(const Key256& key, const void* data, size_t n);
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);

// ECDH P-256 pour l'appairage. Clés publiques : X puis Y, 32 octets chacun, big-endian.
class EcdhP256 {
public:
    ~EcdhP256();
    bool generate();
    bool publicKey(uint8_t out[64]) const;
    // Secret partagé : coordonnée X, big-endian.
    bool agree(const uint8_t peer[64], uint8_t secret[32]) const;

private:
    void* key_ = nullptr;
};

// AES-256-GCM. Le compteur forme le nonce : chaque sens de la connexion a sa
// propre clé, donc un compteur strictement croissant ne se répète jamais.
class Aes256Gcm {
public:
    static constexpr size_t kTag = 16;

    Aes256Gcm() = default;
    ~Aes256Gcm();
    Aes256Gcm(const Aes256Gcm&) = delete;
    Aes256Gcm& operator=(const Aes256Gcm&) = delete;

    bool init(const Key256& key);
    bool seal(const uint8_t* in, size_t n, uint64_t counter, uint8_t* out);
    bool open(const uint8_t* in, size_t n, uint64_t counter, uint8_t* out);

private:
    void* key_ = nullptr;
};
