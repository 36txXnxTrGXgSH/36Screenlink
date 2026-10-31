#pragma once
#include <string>

// Mode SYSTEM : l'agent lancé par le service suit le bureau actif. Sans cela, ni l'écran
// de verrouillage, ni l'invite UAC, ni les fenêtres élevées ne sont accessibles.
extern bool gSystemMode;

// Nom du bureau recevant les entrées : « Default », ou « Winlogon » pour UAC et verrouillage.
std::wstring inputDesktopName();
// Vrai pendant une invite UAC ou le verrouillage, dès Win+L : la première étape du
// verrouillage reste sur le bureau « Default », seul l'état de session la révèle.
bool secureScreenActive();
// Attache le thread appelant au bureau actif. Le thread ne doit posséder aucune fenêtre.
bool attachInputDesktop();
