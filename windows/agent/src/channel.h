#pragma once
#include <winsock2.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "crypto.h"

// Connexion TCP chiffrée vers la tablette, serveur dans tous les modes : le PC
// sort vers elle, donc aucune règle de pare-feu entrante n'est nécessaire.
class SecureChannel {
public:
    SecureChannel() = default;
    ~SecureChannel();

    bool connectTo(const std::string& host, uint16_t port, int timeoutMs);
    // Échange ECDH. `confirm` montre le code à l'utilisateur, qui le compare à celui de la
    // tablette : c'est ce qui écarte une application malveillante à l'écoute du port.
    bool pair(Key256& psk, const std::function<bool(uint32_t code)>& confirm);
    bool handshake(const Key256& psk);
    bool send(const std::vector<uint8_t>& plain);
    bool receive(std::vector<uint8_t>& plain);
    void shutdown();
    const std::string& lastError() const { return error_; }
    // Dernier statut lu dans la réponse de la tablette, -1 si elle n'a rien répondu.
    int lastStatus() const { return status_; }

private:
    bool writeAll(const void* p, size_t n);
    bool readAll(void* p, size_t n);
    bool readStatus();

    SOCKET sock_ = INVALID_SOCKET;
    Aes256Gcm tx_, rx_;
    uint64_t txCounter_ = 0, rxCounter_ = 0;
    std::mutex txMutex_;
    std::vector<uint8_t> txBuf_, rxBuf_;
    std::string error_;
    int status_ = -1;
};
