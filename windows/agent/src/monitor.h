#pragma once
#include <windows.h>

#include <string>

// Pilote 36ScreenlinkDevice.exe, seul composant élevé : une invite UAC au démarrage,
// puis le moniteur suit la tablette sans nouvelle invite. L'assistant s'arrête avec l'agent.
class VirtualMonitor {
public:
    ~VirtualMonitor();
    // Sans effet si notre moniteur virtuel existe déjà.
    void start();
    // Demande le moniteur et attend qu'il apparaisse dans DXGI.
    void raise();
    void release();

private:
    bool managed_ = false;
    bool raised_ = false;
    HANDLE helper_ = nullptr;
    HANDLE raiseEvent_ = nullptr;
    HANDLE releaseEvent_ = nullptr;
};
