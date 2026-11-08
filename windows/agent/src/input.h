#pragma once
#include <windows.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct Contact {
    uint8_t id;
    bool down;
    uint16_t x, y; // 0..65535 sur la surface d'affichage
};

// Tactile vers Windows via un périphérique synthétique : ni pilote HID, ni droits
// admin. Limite : l'injection n'atteint pas les fenêtres élevées (UIPI).
class TouchInjector {
public:
    ~TouchInjector();
    void setTarget(const RECT& desktop) { target_ = desktop; }
    void setEnabled(bool enabled) { enabled_ = enabled; }
    void apply(const std::vector<Contact>& contacts);
    uint64_t received() const { return received_; }
    void releaseAll();

private:
    bool ensureDevice();

    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    RECT target_{};
    bool enabled_ = true;
    uint64_t received_ = 0;
    std::map<uint8_t, POINT> active_;
};

struct PenSample {
    uint8_t state;
    uint16_t x, y, pressure;
    int8_t tiltX, tiltY;
    uint16_t rotation;
};

// Vrai stylet Windows : pression et inclinaison arrivent aux logiciels de dessin.
class PenInjector {
public:
    ~PenInjector();
    void setTarget(const RECT& desktop) { target_ = desktop; }
    void setEnabled(bool enabled) { enabled_ = enabled; }
    void apply(const PenSample& sample);
    uint64_t received() const { return received_; }

private:
    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    RECT target_{};
    bool enabled_ = true;
    bool inRange_ = false, contact_ = false;
    POINT last_{};
    uint64_t received_ = 0;
};

// Rejoue les touches vers la fenêtre active de Windows, avec la même limite UIPI.
// Les codes physiques laissent Windows appliquer sa propre disposition de clavier.
class KeyInjector {
public:
    ~KeyInjector();
    void setEnabled(bool enabled) { enabled_ = enabled; }
    void key(bool down, uint16_t code);
    void text(const std::string& utf8);
    uint64_t received() const { return received_; }

private:
    void send(uint16_t code, bool down);

    bool enabled_ = true;
    uint64_t received_ = 0;
    std::set<uint16_t> held_;
};

struct MouseSample {
    uint8_t buttons;
    uint16_t x, y; // 0..65535 sur la surface d'affichage
    int16_t wheelV, wheelH; // crans de 120, comme Windows
};

// Souris branchée à la tablette : position absolue sur l'écran étendu, boutons et molettes.
class MouseInjector {
public:
    ~MouseInjector();
    void setTarget(const RECT& desktop) { target_ = desktop; }
    void setEnabled(bool enabled) { enabled_ = enabled; }
    void apply(const MouseSample& sample);

private:
    RECT target_{};
    bool enabled_ = true;
    uint8_t buttons_ = 0;
};
