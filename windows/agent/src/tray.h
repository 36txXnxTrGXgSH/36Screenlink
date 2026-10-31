#pragma once
#include <functional>
#include <string>

struct TrayConfig {
    std::function<std::wstring()> status;
    std::function<void()> onQuit;
    std::function<bool()> audioOnTablet;
    std::function<void(bool)> setAudioOnTablet;
    std::wstring logPath;
    // Lancé par le service sous SYSTEM : ni « Quitter », que le service annulerait, ni
    // ouverture du journal, qui lancerait un éditeur avec les droits SYSTEM.
    bool managedByService = false;
    // Version portable uniquement : dossier du script qui retire le pilote du PC.
    std::wstring portableDirectory;
};

// Boucle de message de l'icône de notification. Bloque jusqu'à « Quitter ».
void runTray(const TrayConfig& config);
// Ferme l'icône depuis un autre thread, par le même chemin que « Quitter ».
void trayClose();
// Notification Windows, une seule fois tant que le message ne change pas.
void trayAlert(const std::wstring& message);
void trayClearAlert();
