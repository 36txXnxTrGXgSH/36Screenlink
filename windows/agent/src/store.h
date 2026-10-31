#pragma once
#include <optional>
#include <string>

#include "crypto.h"
#include "desktop.h"

// Clé d'appairage protégée par DPAPI, donc liée à la session Windows. Mode portable :
// un fichier `portable.txt` à côté de l'exe y déplace aussi les données.
std::optional<Key256> loadPsk();
bool savePsk(const Key256& key);
std::string loadTabletAddress();
void saveTabletAddress(const std::string& address);
std::wstring exeDirectory();
// Chemin du journal, dossier créé au besoin.
std::wstring logFilePath();
std::wstring dataFilePath(const wchar_t* name);
bool loadAudioOnTablet();
void saveAudioOnTablet(bool onTablet);
