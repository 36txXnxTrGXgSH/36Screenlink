#pragma once
#include <windows.h>

#include <mutex>
#include <string>

// Pilote 36ScreenlinkDevice.exe, seul composant élevé : une invite UAC au démarrage,
// puis les moniteurs suivent les appareils sans nouvelle invite. L'assistant s'arrête avec l'agent.
// Trois emplacements, un par appareil connecté en même temps (tablette, téléphone, télévision).
class VirtualMonitor {
public:
    static constexpr int kSlots = 3;

    ~VirtualMonitor();
    // Lance l'assistant. Sans lui, chaque appareil montre l'écran principal.
    void start();
    bool available() const { return managed_; }
    // Crée le moniteur de cet emplacement si besoin et renvoie son nom Windows (\\.\DISPLAYn),
    // ou vide s'il n'est pas apparu.
    std::wstring raise(int slot);
    void release(int slot);
    bool raised(int slot) const { return raised_[slot]; }

private:
    bool managed_ = false;
    bool raised_[kSlots] = {};
    std::wstring names_[kSlots];
    HANDLE helper_ = nullptr;
    HANDLE raiseEvents_[kSlots] = {};
    HANDLE releaseEvents_[kSlots] = {};
    std::mutex mutex_;
};
