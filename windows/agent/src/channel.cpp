#include "channel.h"

#include <ws2tcpip.h>

#include <cstring>

#include "protocol.h"

namespace {

Key256 derive(const Key256& psk, const char* label, const std::vector<uint8_t>& context) {
    std::vector<uint8_t> msg(label, label + std::strlen(label));
    msg.insert(msg.end(), context.begin(), context.end());
    return hmacSha256(psk, msg.data(), msg.size());
}

} // namespace

SecureChannel::~SecureChannel() {
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
}

void SecureChannel::shutdown() {
    if (sock_ != INVALID_SOCKET) ::shutdown(sock_, SD_BOTH);
}

bool SecureChannel::connectTo(const std::string& host, uint16_t port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        error_ = "résolution impossible";
        return false;
    }
    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    u_long nonBlocking = 1;
    ioctlsocket(sock_, FIONBIO, &nonBlocking);
    connect(sock_, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);

    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(sock_, &wr);
    FD_SET(sock_, &ex);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &wr, &ex, &tv) <= 0 || FD_ISSET(sock_, &ex)) {
        error_ = "connexion refusée ou expirée";
        return false;
    }
    nonBlocking = 0;
    ioctlsocket(sock_, FIONBIO, &nonBlocking);

    // Petite file d'envoi, environ deux images à 80 Mb/s : si le lien sature, l'envoi bloque
    // aussitôt et le débit baisse, au lieu d'accumuler des centaines de millisecondes de retard.
    BOOL noDelay = TRUE;
    int sendBuf = 192 * 1024;
    DWORD recvTimeout = 5000;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&noDelay), sizeof(noDelay));
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sendBuf), sizeof(sendBuf));
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&recvTimeout), sizeof(recvTimeout));
    return true;
}

bool SecureChannel::writeAll(const void* p, size_t n) {
    auto b = static_cast<const char*>(p);
    while (n > 0) {
        const int sent = ::send(sock_, b, static_cast<int>(n), 0);
        if (sent <= 0) {
            error_ = "écriture socket : " + std::to_string(WSAGetLastError());
            return false;
        }
        b += sent;
        n -= static_cast<size_t>(sent);
    }
    return true;
}

bool SecureChannel::readAll(void* p, size_t n) {
    auto b = static_cast<char*>(p);
    while (n > 0) {
        const int got = recv(sock_, b, static_cast<int>(n), 0);
        if (got <= 0) {
            error_ = "lecture socket : " + std::to_string(WSAGetLastError());
            return false;
        }
        b += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

bool SecureChannel::readStatus() {
    uint8_t reply[5];
    if (!readAll(reply, sizeof(reply))) return false;
    if (std::memcmp(reply, proto::kMagic, 4) != 0) {
        error_ = "réponse invalide";
        return false;
    }
    status_ = reply[4];
    if (reply[4] != proto::kStatusOk) {
        error_ = reply[4] == proto::kStatusNotPaired ? "tablette non appairée" : "refus de la tablette";
        return false;
    }
    return true;
}

bool SecureChannel::pair(Key256& psk, const std::function<bool(uint32_t code)>& confirm) {
    EcdhP256 ecdh;
    uint8_t nonceC[16], pubA[64], pubT[64], raw[32];
    randomBytes(nonceC, sizeof(nonceC));
    if (!ecdh.generate() || !ecdh.publicKey(pubA)) {
        error_ = "génération de clé";
        return false;
    }
    std::vector<uint8_t> hello(proto::kMagic, proto::kMagic + 4);
    hello.push_back(proto::kVersion);
    hello.push_back(proto::kModePair);
    hello.insert(hello.end(), nonceC, nonceC + 16);
    hello.insert(hello.end(), pubA, pubA + 64);
    if (!writeAll(hello.data(), hello.size()) || !readStatus() || !readAll(pubT, 64)) return false;
    if (!ecdh.agree(pubT, raw)) {
        error_ = "clé de la tablette invalide";
        return false;
    }

    Key256 secret;
    std::copy_n(raw, 32, secret.begin());
    std::vector<uint8_t> transcript(nonceC, nonceC + 16);
    transcript.insert(transcript.end(), pubA, pubA + 64);
    transcript.insert(transcript.end(), pubT, pubT + 64);
    const auto labeledHash = [&](const Key256& key, const char* label) {
        std::vector<uint8_t> msg(label, label + std::strlen(label));
        msg.insert(msg.end(), transcript.begin(), transcript.end());
        return hmacSha256(key, msg.data(), msg.size());
    };

    const Key256 sas = labeledHash(secret, "sas");
    const uint32_t code = ((uint32_t(sas[0]) << 24) | (uint32_t(sas[1]) << 16) | (uint32_t(sas[2]) << 8) | sas[3]) % 1000000;
    if (!confirm(code)) {
        error_ = "code refusé";
        return false;
    }

    psk = labeledHash(secret, "psk");
    const Key256 clientMac = labeledHash(psk, "cli");
    if (!writeAll(clientMac.data(), clientMac.size())) return false;

    // La tablette attend ici le geste de l'utilisateur.
    DWORD timeout = 120000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    uint8_t mac[32];
    if (!readStatus() || !readAll(mac, 32)) return false;
    const Key256 expected = labeledHash(psk, "srv");
    if (!constantTimeEqual(mac, expected.data(), 32)) {
        error_ = "confirmation d'appairage invalide";
        return false;
    }
    return true;
}

// Chaque session ajoute un ECDH éphémère à la clé d'appairage : une clé volée plus tard ne
// déchiffre pas un flux enregistré avant. Les MAC couvrent les clés éphémères des deux côtés.
bool SecureChannel::handshake(const Key256& psk) {
    EcdhP256 ecdh;
    uint8_t nonceC[16], nonceS[16], pubA[64], pubT[64], mac[32], raw[32];
    randomBytes(nonceC, 16);
    if (!ecdh.generate() || !ecdh.publicKey(pubA)) {
        error_ = "génération de clé";
        return false;
    }
    std::vector<uint8_t> hello(proto::kMagic, proto::kMagic + 4);
    hello.push_back(proto::kVersion);
    hello.push_back(proto::kModeSession);
    hello.insert(hello.end(), nonceC, nonceC + 16);
    hello.insert(hello.end(), pubA, pubA + 64);
    if (!writeAll(hello.data(), hello.size()) || !readStatus() || !readAll(nonceS, 16) || !readAll(pubT, 64) || !readAll(mac, 32)) return false;

    std::vector<uint8_t> context(nonceC, nonceC + 16);
    context.insert(context.end(), nonceS, nonceS + 16);
    context.insert(context.end(), pubA, pubA + 64);
    context.insert(context.end(), pubT, pubT + 64);
    const Key256 expected = derive(psk, "srv", context);
    if (!constantTimeEqual(mac, expected.data(), 32)) {
        error_ = "authentification de la tablette échouée";
        return false;
    }
    if (!ecdh.agree(pubT, raw)) {
        error_ = "clé éphémère de la tablette invalide";
        return false;
    }
    const Key256 clientMac = derive(psk, "cli", context);
    if (!writeAll(clientMac.data(), clientMac.size())) return false;

    std::vector<uint8_t> keyed = context;
    keyed.insert(keyed.end(), raw, raw + 32);
    SecureZeroMemory(raw, sizeof(raw));
    const bool ok = tx_.init(derive(psk, "a2t", keyed)) && rx_.init(derive(psk, "t2a", keyed));
    SecureZeroMemory(keyed.data(), keyed.size());
    return ok;
}

bool SecureChannel::send(const std::vector<uint8_t>& plain) {
    std::lock_guard lock(txMutex_);
    const size_t cipherLen = plain.size() + Aes256Gcm::kTag;
    txBuf_.resize(4 + cipherLen);
    for (int i = 0; i < 4; ++i) txBuf_[3 - i] = static_cast<uint8_t>(cipherLen >> (i * 8));
    if (!tx_.seal(plain.data(), plain.size(), txCounter_++, txBuf_.data() + 4)) {
        error_ = "chiffrement";
        return false;
    }
    return writeAll(txBuf_.data(), txBuf_.size());
}

bool SecureChannel::receive(std::vector<uint8_t>& plain) {
    uint8_t lenBytes[4];
    if (!readAll(lenBytes, 4)) return false;
    const uint32_t len = (uint32_t(lenBytes[0]) << 24) | (uint32_t(lenBytes[1]) << 16) | (uint32_t(lenBytes[2]) << 8) | lenBytes[3];
    if (len < Aes256Gcm::kTag || len > proto::kMaxIncomingRecord) {
        error_ = "taille d'enregistrement invalide";
        return false;
    }
    rxBuf_.resize(len);
    if (!readAll(rxBuf_.data(), len)) return false;
    plain.resize(len - Aes256Gcm::kTag);
    if (!rx_.open(rxBuf_.data(), len, rxCounter_++, plain.data())) {
        error_ = "authentification d'enregistrement échouée";
        return false;
    }
    return true;
}
