#pragma once
#include <functional>
#include <string>

// Boucle de message de l'icône de notification. Bloque jusqu'à « Quitter ».
void runTray(const std::function<std::wstring()>& status, const std::function<void()>& onQuit, const std::wstring& logPath);
// Ferme l'icône depuis un autre thread, par le même chemin que « Quitter ».
void trayClose();
// Notification Windows, une seule fois tant que le message ne change pas.
void trayAlert(const std::wstring& message);
void trayClearAlert();
