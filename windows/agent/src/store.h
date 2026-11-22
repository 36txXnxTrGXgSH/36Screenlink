#pragma once
#include <optional>
#include <string>

#include "crypto.h"
#include "desktop.h"

// Clé d'appairage protégée par DPAPI, donc liée à la session Windows. Mode portable :
// un fichier `portable.txt` à côté de l'exe y déplace aussi les données.
// Une clé par appareil appairé. L'appareil 0 garde les fichiers d'avant : une tablette déjà
// appairée le reste.
inline constexpr int kMaxDevices = 4;
std::optional<Key256> loadPsk(int device);
bool savePsk(int device, const Key256& key);
// Dernière adresse Wi-Fi et numéro de série USB connus d'un appareil : de quoi savoir, avant
// de s'y connecter, qu'il est déjà en session.
std::string loadDeviceAddress(int device);
void saveDeviceAddress(int device, const std::string& address);
std::string loadDeviceSerial(int device);
void saveDeviceSerial(int device, const std::string& serial);
std::wstring exeDirectory();
// Chemin du journal, dossier créé au besoin.
std::wstring logFilePath();
std::wstring dataFilePath(const wchar_t* name);
bool loadAudioOnTablet();
void saveAudioOnTablet(bool onTablet);
