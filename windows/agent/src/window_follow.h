#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

// Une fenêtre activée depuis la tablette, par exemple par sa barre des tâches, vient s'afficher
// sur la tablette même si elle était sur un autre écran. Seule une touche brève compte : une
// fenêtre glissée vers un autre écran y reste.
class WindowFollower {
public:
    ~WindowFollower();
    void start(const RECT& tablet);
    void stop();
    // À appeler à chaque message tactile ou stylet, depuis un seul thread.
    void onContact(bool down);

private:
    void run();

    RECT tablet_{};
    std::thread thread_;
    std::atomic<bool> running_{false}, held_{false};
    std::atomic<uint64_t> tapMs_{0};
    uint64_t downMs_ = 0;
};
