#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

// Capture ce que joue le PC et coupe ses haut-parleurs le temps de l'envoi vers la tablette.
class AudioStreamer {
public:
    // Stéréo 16 bits little-endian à la fréquence du périphérique Windows.
    using Sink = std::function<bool(uint32_t rate, const int16_t* samples, size_t frames)>;

    ~AudioStreamer() { stop(); }
    void start(Sink sink);
    void stop();
    // Rétablit le son si l'agent s'est arrêté brutalement pendant un envoi.
    static void recoverMute();

private:
    void run(const Sink& sink);
    void captureUntilStopped(const Sink& sink);

    std::thread thread_;
    std::atomic<bool> running_{false};
};
