#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

// Une fenêtre activée depuis la tablette, par exemple par sa barre des tâches, vient s'afficher
// sur la tablette même si elle était sur un autre écran. Seule une touche brève compte : une
// fenêtre glissée vers un autre écran y reste.
class WindowFollower {
public:
    ~WindowFollower();
    void start(const RECT& tablet);
    void stop();
    bool running() const { return running_; }
    // À appeler à chaque message tactile ou stylet, depuis un seul thread.
    void onContact(bool down);
    // Icône glissée sur un écran depuis l'appareil : la prochaine fenêtre activée, juste après
    // la touche sur son icône ou sur une de ses miniatures, part sur cet écran.
    void sendTo(const RECT& screen);

private:
    void run();

    RECT tablet_{};
    std::thread thread_;
    std::atomic<bool> running_{false}, held_{false};
    std::atomic<uint64_t> tapMs_{0};
    uint64_t downMs_ = 0;
    std::mutex sendMutex_;
    RECT sendScreen_{};
    std::atomic<uint64_t> sendUntilMs_{0};
};
