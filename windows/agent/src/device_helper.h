#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

class SecureChannel;

// Ce que l'appareil doit savoir du bureau pour aider la saisie :
// - les écrans du PC et la barre des tâches affichée sur l'appareil, pour envoyer une fenêtre
//   d'un écran à l'autre en glissant son icône ;
// - le champ de saisie qui a le focus, pour ouvrir le clavier à l'écran et le garder visible.
class DeviceHelper {
public:
    ~DeviceHelper();
    // device : rectangle de l'écran montré sur l'appareil. windowMoves : faux si le glisser
    // d'icône n'a pas de sens (écran dupliqué, appareil seul, écran sécurisé).
    void start(SecureChannel& ch, const RECT& device, bool windowMoves);
    void stop();
    void setKeyboardFlags(uint8_t flags) { keyboardFlags_ = flags; }
    // Rectangle d'un écran tel que numéroté dans le dernier message envoyé à l'appareil.
    std::optional<RECT> screen(uint8_t index);

private:
    void run();
    void sendScreens(bool force);
    void sendTextFocus(bool force);

    SecureChannel* ch_ = nullptr;
    RECT device_{};
    bool windowMoves_ = false;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint8_t> keyboardFlags_{0};
    std::mutex screensMutex_;
    std::vector<RECT> screens_;
    std::vector<uint8_t> lastScreens_, lastFocus_;
    void* automation_ = nullptr; // IUIAutomation, propre au thread
};
