// Agent Windows : capture d'un écran, encodage NVENC, envoi chiffré vers la tablette.
#include <winsock2.h>
#include <windows.h>
#include <shellscalingapi.h>
#include <timeapi.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "audio.h"
#include "capture.h"
#include "channel.h"
#include "desktop.h"
#include "discovery.h"
#include "encoder_mf.h"
#include "encoder_nvenc.h"
#include "input.h"
#include "logw.h"
#include "monitor.h"
#include "protocol.h"
#include "store.h"
#include "tray.h"
#include "window_follow.h"

#pragma comment(lib, "winmm.lib")

namespace {

enum class Link { Usb, Wifi };

struct Options {
    Link link = Link::Usb;
    bool autoLink = false; // USB si la tablette y répond, sinon Wi-Fi
    bool system = false;
    std::string wifiHost;
    std::wstring adbPath;
    int outputIndex = -1;
    // 120 Hz par défaut : mesuré plus réactif que 60 Hz, le décodeur tournant plus vite.
    uint32_t fps = 120;
    uint32_t bitrateMbps = 80;
    bool hevc = true;
    bool injectTouch = true;
    bool tray = false;
    bool virtualMonitor = true;
    bool forceMediaFoundation = false; // test : encodeur générique, même sur NVIDIA
    bool simulateCongestion = false; // test : force le signal de saturation de 5 à 10 s
    int durationSec = 0;
    bool listOnly = false;
    bool stopOthers = false;
    bool open = false;
};

std::atomic<bool> gStop{false};
std::atomic<bool> gAudioOnTablet{false};
std::atomic<uint8_t> gDisplayMode{proto::kDisplayExtended};
std::atomic<uint32_t> gFps{120};
std::atomic<bool> gRestartSession{false};
bool gUserQuit = false;
std::mutex gStatusMutex;
std::wstring gStatus = L"Démarrage";
VirtualMonitor gMonitor;
// Après ce délai sans tablette, le moniteur est retiré et Windows récupère les fenêtres.
constexpr auto kMonitorGrace = std::chrono::seconds(10);
std::chrono::steady_clock::time_point gLastSessionEnd{};
constexpr uint32_t kMinBitrate = 15'000'000;
constexpr uint32_t kCongestedLatUs = 150'000;

void setStatus(const std::wstring& s) {
    std::lock_guard lock(gStatusMutex);
    gStatus = s;
}

uint64_t qpcFrequency() {
    static const uint64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return static_cast<uint64_t>(v.QuadPart);
    }();
    return f;
}

uint64_t qpcToUs(uint64_t qpc) { return qpc / qpcFrequency() * 1'000'000 + qpc % qpcFrequency() * 1'000'000 / qpcFrequency(); }

uint64_t nowUs() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return qpcToUs(static_cast<uint64_t>(v.QuadPart));
}

bool runAdb(const Options& opt, const std::wstring& args, std::string* output = nullptr);

std::wstring findAdb(const Options& opt) {
    if (!opt.adbPath.empty()) return opt.adbPath;
    const std::filesystem::path local = std::filesystem::path(exeDirectory()) / L"adb.exe";
    // Un agent SYSTEM ne doit jamais exécuter un adb trouvé dans un dossier modifiable.
    if (std::filesystem::exists(local) || gSystemMode) return local.wstring();
    // Jamais un nom nu : CreateProcess chercherait d'abord dans le dossier courant. On ne
    // retient qu'un chemin absolu trouvé dans le PATH.
    std::wstring list(32767, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"PATH", list.data(), static_cast<DWORD>(list.size()));
    list.resize(n < list.size() ? n : 0);
    size_t start = 0;
    while (start < list.size()) {
        const size_t end = std::min(list.find(L';', start), list.size());
        std::wstring dir = list.substr(start, end - start);
        dir.erase(std::remove(dir.begin(), dir.end(), L'"'), dir.end());
        std::error_code ec;
        const std::filesystem::path candidate = std::filesystem::path(dir) / L"adb.exe";
        if (!dir.empty() && candidate.is_absolute() && std::filesystem::is_regular_file(candidate, ec)) return candidate.wstring();
        start = end + 1;
    }
    return local.wstring();
}

// Exécute adb sans fenêtre et renvoie sa sortie standard.
bool runAdb(const Options& opt, const std::wstring& args, std::string* output) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    CreatePipe(&readPipe, &writePipe, &sa, 0);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + findAdb(opt) + L"\" " + args;
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!started) {
        CloseHandle(readPipe);
        return false;
    }
    std::string text;
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) text.append(buf, got);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (output) *output = text;
    return code == 0;
}

// UAC et verrouillage s'affichent sur l'écran principal : la tablette doit alors le montrer.
int primaryIndex(const std::vector<OutputInfo>& outputs) {
    for (size_t i = 0; i < outputs.size(); ++i)
        if (outputs[i].primary) return static_cast<int>(i);
    return 0;
}

const OutputInfo* pickOutput(const std::vector<OutputInfo>& outputs, int index) {
    if (outputs.empty()) return nullptr;
    if (index >= 0) return index < static_cast<int>(outputs.size()) ? &outputs[index] : nullptr;
    // Jamais un second écran physique du PC : notre moniteur, sinon miroir de l'écran principal.
    for (const auto& o : outputs)
        if (o.isVirtual) return &o;
    return &outputs[primaryIndex(outputs)];
}

// Fréquence réellement appliquée, lue dans la configuration d'affichage : après un retour de
// « Tablette seule », EnumDisplaySettings peut annoncer une fréquence qui n'est plus la bonne.
uint32_t actualRefreshHz(const std::wstring& device) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return 0;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = paths[i].sourceInfo.adapterId;
        name.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || device != name.viewGdiDeviceName) continue;
        const auto& r = paths[i].targetInfo.refreshRate;
        return r.Denominator ? static_cast<uint32_t>(static_cast<double>(r.Numerator) / r.Denominator + 0.5) : 0;
    }
    return 0;
}

// Choisit le mode du moniteur virtuel le plus proche de la tablette : d'abord sa résolution
// exacte, sinon la plus grande de même format qui tient dedans. Réglage non persistant.
void setBestMode(const std::wstring& device, uint32_t longSide, uint32_t shortSide, uint32_t fps, bool force = false) {
    DEVMODEW best{};
    double bestScore = -1;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(device.c_str(), i, &mode); ++i) {
        const uint32_t l = std::max(mode.dmPelsWidth, mode.dmPelsHeight), s = std::min(mode.dmPelsWidth, mode.dmPelsHeight);
        if (l > longSide || s > shortSide || mode.dmDisplayFrequency > fps) continue;
        const double aspect = 1.0 - std::min(1.0, std::abs(double(l) / s - double(longSide) / shortSide));
        const double score = aspect * 1e9 + double(l) * s + mode.dmDisplayFrequency;
        if (score > bestScore) {
            bestScore = score;
            best = mode;
        }
    }
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (bestScore < 0 || !EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &current)) return;
    const uint32_t actual = actualRefreshHz(device);
    const bool sameSize = std::max(current.dmPelsWidth, current.dmPelsHeight) == std::max(best.dmPelsWidth, best.dmPelsHeight) &&
                          std::min(current.dmPelsWidth, current.dmPelsHeight) == std::min(best.dmPelsWidth, best.dmPelsHeight);
    const bool rateOk = current.dmDisplayFrequency == best.dmDisplayFrequency &&
                        (actual == 0 || std::abs(static_cast<int>(actual) - static_cast<int>(best.dmDisplayFrequency)) <= 1);
    if (sameSize && rateOk && !force) return;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    // Même mode annoncé mais autre fréquence réelle : CDS_RESET force Windows à le réappliquer.
    // Au retour de « Tablette seule », Windows reprend ensuite le mode de sa base d'affichage :
    // on y inscrit donc celui du moniteur virtuel, qui n'appartient qu'à 36Screenlink.
    DWORD flags = sameSize || force ? CDS_RESET : 0;
    if (force) flags |= CDS_UPDATEREGISTRY;
    const LONG result = ChangeDisplaySettingsExW(device.c_str(), &best, nullptr, flags, nullptr);
    std::printf("Mode de l'écran : %lux%lu à %lu Hz (résultat %ld)\n", best.dmPelsWidth, best.dmPelsHeight, best.dmDisplayFrequency, result);
    Sleep(500);
}

// Mode « Tablette seule » : configuration temporaire, jamais enregistrée par Windows. Si
// l'agent disparaît, le retrait du moniteur rend de lui-même l'écran du PC.
struct DisplaySnapshot {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};
std::optional<DisplaySnapshot> gSavedDisplays;

std::optional<DisplaySnapshot> queryDisplays() {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return std::nullopt;
    DisplaySnapshot s;
    s.paths.resize(pathCount);
    s.modes.resize(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, s.paths.data(), &modeCount, s.modes.data(), nullptr) != ERROR_SUCCESS) return std::nullopt;
    s.paths.resize(pathCount);
    s.modes.resize(modeCount);
    return s;
}

bool useOnlyDisplay(const std::wstring& device) {
    auto current = queryDisplays();
    if (!current) return false;
    for (const auto& path : current->paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.sourceInfo.adapterId;
        name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || device != name.viewGdiDeviceName) continue;
        auto modes = current->modes;
        if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID) modes[path.sourceInfo.modeInfoIdx].sourceMode.position = {0, 0};
        DISPLAYCONFIG_PATH_INFO only = path;
        if (!gSavedDisplays) gSavedDisplays = current;
        const LONG result = SetDisplayConfig(1, &only, static_cast<UINT32>(modes.size()), modes.data(),
                                             SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        std::printf("Tablette seule : résultat %ld\n", result);
        Sleep(800);
        return result == ERROR_SUCCESS;
    }
    return false;
}

bool restoreDisplays() {
    if (!gSavedDisplays) return false;
    auto& s = *gSavedDisplays;
    LONG result = SetDisplayConfig(static_cast<UINT32>(s.paths.size()), s.paths.data(), static_cast<UINT32>(s.modes.size()), s.modes.data(),
                                   SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    // Si le moniteur virtuel a changé entre-temps, Windows reprend sa dernière configuration.
    if (result != ERROR_SUCCESS) result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT);
    std::printf("Écrans du PC rétablis : résultat %ld\n", result);
    gSavedDisplays.reset();
    Sleep(800);
    return true;
}

// Fait pivoter l'écran capturé pour suivre la tablette. Réglage non persistant.
void setOrientation(const std::wstring& device, bool portrait) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode)) return;
    const bool isPortrait = mode.dmDisplayOrientation == DMDO_90 || mode.dmDisplayOrientation == DMDO_270;
    if (isPortrait == portrait) return;
    std::swap(mode.dmPelsWidth, mode.dmPelsHeight);
    mode.dmDisplayOrientation = portrait ? DMDO_90 : DMDO_DEFAULT;
    mode.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
    const LONG result = ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, 0, nullptr);
    std::printf("Orientation de l'écran : %s (résultat %ld)\n", portrait ? "portrait" : "paysage", result);
    Sleep(500);
}

ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* device, UINT w, UINT h, D3D11_USAGE usage, UINT bind, UINT cpu) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = usage;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = cpu;
    ComPtr<ID3D11Texture2D> tex;
    device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
    return tex;
}

// NVENC sur NVIDIA, sinon encodeur matériel du GPU par Media Foundation (AMD, Intel, ou repli
// NVIDIA). Toujours sur le GPU de l'écran capturé, ce qui évite toute copie entre GPU.
std::unique_ptr<Encoder> openEncoder(const Options& opt, UINT vendorId, ID3D11Device* device, ID3D11Texture2D* input, EncoderConfig& cfg) {
    if (vendorId == 0x10DE && !opt.forceMediaFoundation) {
        auto nvenc = std::make_unique<NvencEncoder>();
        if (nvenc->open(device, input, cfg)) {
            std::printf("Encodeur matériel : NVENC\n");
            return nvenc;
        }
        std::printf("NVENC : %s\n", nvenc->lastError().c_str());
    }
    auto generic = std::make_unique<MediaFoundationEncoder>();
    if (generic->open(device, input, cfg)) return generic;
    std::printf("Media Foundation : %s\n", generic->lastError().c_str());
    return nullptr;
}

struct SessionStats {
    std::atomic<uint32_t> tabletFrames{0}, tabletAvgLat{0}, tabletP95Lat{0}, tabletAvgDec{0}, tabletDrops{0};
    std::atomic<bool> requestIdr{false};
    // UAC ou verrouillage montrés : aucune saisie avant la confirmation d'identité sur la tablette.
    bool secureDesktop = false;
    std::atomic<bool> armed{false};
    std::atomic<int64_t> lastPongUs{0};
};

void receiverLoop(SecureChannel& ch, TouchInjector& touch, KeyInjector& keys, PenInjector& pen, MouseInjector& mouse, WindowFollower& follower, SessionStats& stats, std::atomic<bool>& alive) {
    if (gSecureMode) attachInputDesktop();
    std::vector<uint8_t> msg;
    int64_t bestRtt = INT64_MAX;
    while (alive && ch.receive(msg)) {
        if (msg.empty()) continue;
        proto::Reader r(msg.data() + 1, msg.size() - 1);
        const bool inputAllowed = !stats.secureDesktop || stats.armed;
        switch (msg[0]) {
        case proto::kSecureArmed:
            if (stats.secureDesktop && !stats.armed) {
                stats.armed = true;
                std::printf("Écran sécurisé : saisie autorisée après confirmation sur la tablette.\n");
            }
            break;
        case proto::kPong: {
            const uint64_t t0 = r.u64(), tablet = r.u64(), t1 = nowUs();
            const int64_t rtt = static_cast<int64_t>(t1 - t0);
            stats.lastPongUs = static_cast<int64_t>(t1);
            // On ne garde que l'aller-retour le plus court : c'est l'estimation d'horloge la moins bruitée.
            if (rtt <= bestRtt) {
                bestRtt = rtt;
                const int64_t offset = static_cast<int64_t>(tablet) - static_cast<int64_t>((t0 + t1) / 2);
                proto::Writer w(proto::kClock);
                w.u64(static_cast<uint64_t>(offset));
                ch.send(w.data());
            }
            break;
        }
        case proto::kStats:
            stats.tabletFrames = r.u32();
            stats.tabletAvgLat = r.u32();
            stats.tabletP95Lat = r.u32();
            stats.tabletAvgDec = r.u32();
            stats.tabletDrops = r.u32();
            break;
        case proto::kRequestIdr:
            stats.requestIdr = true;
            break;
        case proto::kTouch: {
            const uint8_t n = r.u8();
            std::vector<Contact> contacts;
            for (uint8_t i = 0; i < n && r.has(6); ++i) {
                Contact c{};
                c.id = r.u8();
                c.down = r.u8() != 0;
                c.x = r.u16();
                c.y = r.u16();
                contacts.push_back(c);
            }
            if (!inputAllowed) break;
            touch.apply(contacts);
            follower.onContact(std::any_of(contacts.begin(), contacts.end(), [](const Contact& c) { return c.down; }));
            break;
        }
        case proto::kKey: {
            if (!inputAllowed) break;
            const bool down = r.u8() != 0;
            keys.key(down, r.u16());
            break;
        }
        case proto::kPen: {
            if (!inputAllowed) break;
            PenSample s{};
            s.state = r.u8();
            s.x = r.u16();
            s.y = r.u16();
            s.pressure = r.u16();
            s.tiltX = static_cast<int8_t>(r.u8());
            s.tiltY = static_cast<int8_t>(r.u8());
            s.rotation = r.u16();
            pen.apply(s);
            follower.onContact((s.state & proto::kPenContact) != 0);
            break;
        }
        case proto::kControl: {
            const uint8_t id = r.u8(), value = r.u8();
            if (id == proto::kControlDisplay && value <= proto::kDisplayTabletOnly) gDisplayMode = value;
            if (id == proto::kControlFps && (value == 60 || value == 120 || value == 144)) gFps = value;
            gRestartSession = true;
            break;
        }
        case proto::kAudioRoute: {
            const bool onTablet = r.u8() != 0;
            gAudioOnTablet = onTablet;
            saveAudioOnTablet(onTablet);
            break;
        }
        case proto::kMouse: {
            if (!inputAllowed) break;
            MouseSample m{};
            m.buttons = r.u8();
            m.x = r.u16();
            m.y = r.u16();
            m.wheelV = static_cast<int16_t>(r.u16());
            m.wheelH = static_cast<int16_t>(r.u16());
            mouse.apply(m);
            follower.onContact(m.buttons != 0);
            break;
        }
        case proto::kText:
            if (!inputAllowed) break;
            keys.text(std::string(reinterpret_cast<const char*>(msg.data() + 1), msg.size() - 1));
            break;
        default:
            break;
        }
    }
    alive = false;
}

// Après un refus, on attend avant de redemander : pas de fenêtres en rafale.
constexpr auto kPairingRetry = std::chrono::seconds(60);

bool askPairingCode(uint32_t code) {
    wchar_t text[220];
    swprintf_s(text, L"Code d'appairage : %06u\n\nLe même code s'affiche-t-il sur la tablette ?\nRépondez Non s'il est différent.", code);
    return MessageBoxW(nullptr, text, L"36Screenlink", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
}

// Échange ECDH : rien n'est enregistré sans confirmation du même code des deux côtés.
void tryPairing(SecureChannel& ch) {
    // Une fenêtre au plus par minute, quelle que soit la réponse : un faux serveur du réseau
    // ne peut pas en faire surgir en rafale.
    static std::chrono::steady_clock::time_point lastAsked{};
    const auto now = std::chrono::steady_clock::now();
    if (lastAsked.time_since_epoch().count() != 0 && now - lastAsked < kPairingRetry) return;
    setStatus(L"Appairage en cours");
    Key256 psk{};
    bool asked = false;
    const auto confirm = [&asked](uint32_t code) {
        asked = true;
        lastAsked = std::chrono::steady_clock::now();
        return askPairingCode(code);
    };
    if (ch.pair(psk, confirm) && savePsk(psk)) {
        SecureZeroMemory(psk.data(), psk.size());
        std::printf("Appairage réussi.\n");
        trayClearAlert();
        return;
    }
    std::printf("Appairage annulé : %s\n", ch.lastError().c_str());
    // Tablette déjà appairée à un autre PC : elle attend que l'utilisateur l'autorise.
    if (!asked && ch.lastStatus() == proto::kStatusPairRefused)
        trayAlert(L"Sur la tablette, ouvrez l'application et touchez Nouvel appairage.");
}

// Une session complète. Renvoie false pour arrêter, true pour tenter une reconnexion.
bool runSession(const Options& opt, const std::string& host) {
    SecureChannel ch;
    if (!ch.connectTo(host, proto::kPort, 3000)) {
        std::printf("Connexion à %s : %s\n", host.c_str(), ch.lastError().c_str());
        return true;
    }

    auto psk = loadPsk();
    if (!psk) {
        tryPairing(ch);
        return true;
    }
    if (!ch.handshake(*psk)) {
        // Tablette réinstallée ou autre appareil : la clé n'est jamais renvoyée, il faut un
        // nouvel appairage. La tablette le refuse tant que l'utilisateur ne l'a pas demandé.
        // Application fermée en USB : le tunnel d'adb accepte puis ferme sans réponse. Il faut
        // attendre l'application, pas proposer un appairage.
        if (ch.lastStatus() < 0) {
            static bool reported = false;
            if (!reported) std::printf("Authentification : %s (application fermée ?)\n", ch.lastError().c_str());
            reported = true;
            setStatus(L"Ouvrez l'application sur la tablette");
            return true;
        }
        std::printf("Authentification : %s\n", ch.lastError().c_str());
        ch.shutdown();
        if (ch.lastStatus() == proto::kStatusBadVersion) {
            trayAlert(L"Mettez à jour l'application 36Screenlink de la tablette.");
            return true;
        }
        // Appairage seulement pour une tablette non appairée ou appairée à un autre PC (clé
        // différente) ; celle-ci le refuse tant que l'utilisateur ne l'a pas autorisé.
        if (ch.lastStatus() != proto::kStatusNotPaired && ch.lastStatus() != proto::kStatusOk) return true;
        SecureChannel again;
        if (again.connectTo(host, proto::kPort, 3000)) tryPairing(again);
        return true;
    }


    std::vector<uint8_t> msg;
    if (!ch.receive(msg) || msg.empty() || msg[0] != proto::kHello) return true;
    proto::Reader hello(msg.data() + 1, msg.size() - 1);
    const uint16_t tabletW = hello.u16(), tabletH = hello.u16(), tabletFps = hello.u16();
    const uint8_t codecs = hello.u8();
    std::printf("Tablette : %ux%u, %u Hz, codecs 0x%02x\n", tabletW, tabletH, tabletFps, codecs);

    bool secureDesktop = false;
    if (gSecureMode) {
        attachInputDesktop();
        secureDesktop = secureScreenActive();
    }
    const uint8_t displayMode = gDisplayMode;
    const uint32_t targetFps = gFps;
    // Après « Tablette seule », Windows peut remettre le moniteur virtuel à 60 Hz un peu plus
    // tard tout en annonçant encore l'ancien mode : on réapplique alors le mode voulu.
    const bool restored = displayMode != proto::kDisplayTabletOnly && restoreDisplays();
    // Dupliquer montre l'écran du PC : le moniteur virtuel n'a plus lieu d'être.
    if (displayMode == proto::kDisplayDuplicate) {
        gMonitor.release();
        Sleep(800);
    } else {
        gMonitor.raise();
    }
    auto outputs = enumerateOutputs();
    const bool mirrorPrimary = secureDesktop || displayMode == proto::kDisplayDuplicate;
    const int wanted = mirrorPrimary ? primaryIndex(outputs) : opt.outputIndex;
    const OutputInfo* output = pickOutput(outputs, wanted);
    if (!output) {
        std::printf("Aucun écran à capturer.\n");
        return false;
    }
    if (!mirrorPrimary && output->isVirtual) {
        setBestMode(output->deviceName, std::max(tabletW, tabletH), std::min(tabletW, tabletH), std::min<uint32_t>(targetFps, tabletFps), restored);
        setOrientation(output->deviceName, tabletH > tabletW);
        if (displayMode == proto::kDisplayTabletOnly) useOnlyDisplay(output->deviceName);
        // La rotation change les coordonnées de l'écran : on relit la description.
        outputs = enumerateOutputs();
        output = pickOutput(outputs, wanted);
        if (!output) return true;
    }
    logw(L"Capture : %ls sur %ls%ls\n", output->deviceName.c_str(), output->adapterName.c_str(),
                 output->primary ? L" (miroir de l'écran principal)" : L"");

    Capturer cap;
    if (!cap.open(*output)) {
        std::printf("Capture : %s\n", cap.lastError().c_str());
        return true;
    }
    std::printf("Capture : %ux%u, rotation %u\n", cap.width(), cap.height(), cap.rotationDegrees());

    const UINT width = cap.width() & ~1u, height = cap.height() & ~1u;
    ComPtr<ID3D11Texture2D> capTarget = makeTexture(cap.device(), width, height, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0);

    EncoderConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.fps = std::min<uint32_t>(targetFps, tabletFps ? tabletFps : targetFps);
    cfg.bitrate = opt.bitrateMbps * 1'000'000;
    cfg.hevc = opt.hevc && (codecs & 2);

    const auto encoder = openEncoder(opt, output->vendorId, cap.device(), capTarget.Get(), cfg);
    if (!encoder) {
        trayAlert(L"Aucun encodeur vidéo matériel disponible sur ce GPU. Mettez à jour son pilote.");
        return false;
    }

    proto::Writer config(proto::kStreamConfig);
    config.u8(cfg.hevc ? proto::kCodecHevc : proto::kCodecAvc)
        .u16(static_cast<uint16_t>(width)).u16(static_cast<uint16_t>(height))
        .u16(static_cast<uint16_t>(cfg.fps)).u32(cfg.bitrate).u16(static_cast<uint16_t>(cap.rotationDegrees()));
    if (!ch.send(config.data())) return true;

    const auto headers = encoder->sequenceHeaders();
    // Certains encodeurs placent ces en-têtes uniquement dans le flux.
    if (!headers.empty()) {
        proto::Writer w(proto::kVideo);
        w.u8(proto::kFlagConfig).u64(nowUs()).bytes(headers.data(), headers.size());
        if (!ch.send(w.data())) return true;
    }

    TouchInjector touch;
    touch.setTarget(output->desktop);
    touch.setEnabled(opt.injectTouch);
    KeyInjector keys;
    keys.setEnabled(opt.injectTouch);
    PenInjector pen;
    pen.setTarget(output->desktop);
    pen.setEnabled(opt.injectTouch);
    MouseInjector mouse;
    mouse.setTarget(output->desktop);
    mouse.setEnabled(opt.injectTouch);

    {
        proto::Writer w(proto::kControlState);
        w.u8(displayMode).u8(static_cast<uint8_t>(targetFps));
        if (!ch.send(w.data())) return true;
    }
    {
        // UAC ou verrouillage : la tablette demande d'abord le code de l'appareil.
        proto::Writer w(proto::kSecureState);
        w.u8(secureDesktop ? 1 : 0);
        if (!ch.send(w.data())) return true;
        if (secureDesktop) std::printf("Écran sécurisé montré : saisie bloquée jusqu'à confirmation sur la tablette.\n");
    }

    // Utile seulement en écran étendu : ailleurs, aucune fenêtre n'est sur un autre écran visible.
    WindowFollower follower;
    if (opt.injectTouch && !mirrorPrimary && output->isVirtual && displayMode == proto::kDisplayExtended) follower.start(output->desktop);

    trayClearAlert();
    SessionStats stats;
    stats.lastPongUs = static_cast<int64_t>(nowUs());
    stats.secureDesktop = secureDesktop;
    std::atomic<bool> alive{true};
    std::thread receiver(receiverLoop, std::ref(ch), std::ref(touch), std::ref(keys), std::ref(pen), std::ref(mouse), std::ref(follower), std::ref(stats), std::ref(alive));
    // Session en Wi-Fi alors que le câble est branché : on repasse par l'USB, plus stable.
    std::atomic<bool> usbAvailable{false};
    std::thread usbWatcher([&] {
        if (!opt.autoLink || opt.link != Link::Wifi) return;
        while (alive && !gStop) {
            for (int i = 0; i < 50 && alive && !gStop; ++i) Sleep(100);
            std::string state;
            if (alive && runAdb(opt, L"get-state", &state) && state.find("device") != std::string::npos) {
                usbAvailable = true;
                return;
            }
        }
    });
    std::thread pinger([&] {
        while (alive && !gStop) {
            proto::Writer w(proto::kPing);
            w.u64(nowUs());
            if (!ch.send(w.data())) break;
            if (static_cast<int64_t>(nowUs()) - stats.lastPongUs > 5'000'000) {
                std::printf("Plus de réponse de la tablette.\n");
                break;
            }
            Sleep(250);
        }
        alive = false;
        ch.shutdown();
    });

    timeBeginPeriod(1);
    const uint64_t intervalUs = 1'000'000 / cfg.fps;
    uint64_t lastEncode = 0, nextEncodeUs = 0, encoded = 0, bytesSent = 0, encodeUsSum = 0;
    uint64_t windowFrames = 0, windowBytes = 0, windowEncUs = 0, windowSendUs = 0, windowCaptured = 0, windowStart = nowUs();
    uint64_t windowCount = 0, lastDesktopCheck = 0;
    uint32_t currentBitrate = cfg.bitrate;
    int calmSeconds = 0;
    const uint64_t startUs = nowUs();
    bool forceIdr = true, pending = false;
    uint64_t pendingPts = 0;
    int refinements = 0;
    bool keepRunning = true;
    std::wstring linkName = opt.link == Link::Usb ? L"USB" : L"Wi-Fi";
    AudioStreamer audio;
    bool audioRunning = false, audioStateSent = false;
    uint64_t lastKeepAwake = 0;
    // La tablette est un écran utilisé : Windows ne doit pas éteindre les écrans, sinon la
    // composition s'arrête et l'image se fige. La demande seule ne suffit pas toujours :
    // un mouvement de souris nul remet aussi le minuteur d'inactivité à zéro.
    HANDLE displayRequest = nullptr;
    {
        REASON_CONTEXT reason{POWER_REQUEST_CONTEXT_VERSION, POWER_REQUEST_CONTEXT_SIMPLE_STRING};
        reason.Reason.SimpleReasonString = const_cast<LPWSTR>(L"Tablette 36Screenlink connectée");
        displayRequest = PowerCreateRequest(&reason);
        if (displayRequest != INVALID_HANDLE_VALUE) PowerSetRequest(displayRequest, PowerRequestDisplayRequired);
        else displayRequest = nullptr;
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    }

    while (alive && !gStop) {
        // Nouveau mode ou nouvelle fréquence : la session suivante repart sur le bon écran.
        if (gRestartSession.exchange(false)) break;
        if (usbAvailable) {
            std::printf("Câble USB détecté : passage de la session en USB.\n");
            break;
        }
        if (!secureDesktop && nowUs() - lastKeepAwake > 30'000'000) {
            lastKeepAwake = nowUs();
            LASTINPUTINFO last{sizeof(last)};
            if (GetLastInputInfo(&last) && GetTickCount() - last.dwTime > 30'000) {
                INPUT nudge{};
                nudge.type = INPUT_MOUSE;
                nudge.mi.dwFlags = MOUSEEVENTF_MOVE;
                SendInput(1, &nudge, sizeof(nudge));
            }
        }
        const bool audioWanted = gAudioOnTablet;
        if (audioWanted != audioRunning || !audioStateSent) {
            if (audioWanted && !audioRunning) {
                audio.start([&](uint32_t rate, const int16_t* samples, size_t frames) {
                    proto::Writer w(proto::kAudio);
                    w.u32(rate).bytes(samples, frames * 4);
                    return ch.send(w.data());
                });
            } else if (!audioWanted && audioRunning) {
                audio.stop();
            }
            audioRunning = audioWanted;
            audioStateSent = true;
            proto::Writer w(proto::kAudioState);
            w.u8(audioWanted ? 1 : 0);
            ch.send(w.data());
        }
        if (opt.durationSec > 0 && nowUs() - startUs > static_cast<uint64_t>(opt.durationSec) * 1'000'000) {
            keepRunning = false;
            break;
        }
        if (gSecureMode && nowUs() - lastDesktopCheck > 250'000) {
            lastDesktopCheck = nowUs();
            // Passage vers ou depuis UAC et verrouillage : nouvelle session sur le bon écran.
            if (secureScreenActive() != secureDesktop) {
                logw(L"Bureau actif : %ls, écran sécurisé %d\n", inputDesktopName().c_str(), secureDesktop ? 0 : 1);
                break;
            }
        }
        // Pas de capture avant l'échéance : l'écran principal peut tourner plus vite que le flux,
        // et chaque image capturée coûte une copie GPU. La duplication garde la plus récente.
        if (!pending && !forceIdr) {
            const uint64_t t = nowUs();
            if (t + 1500 < nextEncodeUs) Sleep(static_cast<DWORD>((nextEncodeUs - t) / 1000));
        }
        CapturedFrame frame;
        const UINT timeoutMs = pending ? 0 : static_cast<UINT>(intervalUs / 1000 + 1);
        const auto result = cap.acquire(timeoutMs, capTarget.Get(), frame);
        if (result == Capturer::Result::Lost) {
            // UAC, changement de mode ou verrouillage : la duplication est à refaire.
            // Sans suivi du bureau sécurisé, la capture attend simplement le retour de Windows.
            Sleep(gSecureMode ? 100 : 500);
            if (gSecureMode) attachInputDesktop();
            if (!cap.open(*output) && gSecureMode) std::printf("Reprise de capture : %s\n", cap.lastError().c_str());
            forceIdr = true;
            continue;
        }
        if (result == Capturer::Result::Error) {
            std::printf("Capture : %s\n", cap.lastError().c_str());
            break;
        }

        if (frame.cursorShapeChanged) {
            const auto& s = frame.cursorShape;
            proto::Writer w(proto::kCursorShape);
            w.u16(s.width).u16(s.height).u16(s.hotX).u16(s.hotY).bytes(s.bgra.data(), s.bgra.size());
            ch.send(w.data());
        }
        if (frame.cursorMoved) {
            proto::Writer w(proto::kCursorPos);
            w.u8(frame.cursorVisible ? 1 : 0).u16(static_cast<uint16_t>(frame.cursorX)).u16(static_cast<uint16_t>(frame.cursorY));
            ch.send(w.data());
        }

        if (frame.imageUpdated) {
            ++windowCaptured;
            pending = true;
            pendingPts = qpcToUs(frame.presentQpc);
            refinements = 3;

        }

        const uint64_t now = nowUs();
        // Écran immobile : quelques images de plus affinent le texte après un mouvement.
        const bool refine = !pending && refinements > 0 && now - lastEncode > 100'000;
        // Échéancier et non délai depuis la dernière image : une image un peu en avance
        // passe sans attendre, sinon la suivante l'écraserait et la cadence chuterait.
        const bool due = now >= nextEncodeUs;
        // La tablette ignore tout jusqu'à une image clé : sa demande passe même sur un écran immobile.
        const bool idrRequested = stats.requestIdr.exchange(false);
        if (idrRequested) forceIdr = true;
        if ((pending && due) || refine || (forceIdr && pending) || idrRequested) {
            const bool idr = forceIdr;
            forceIdr = false;
            const uint64_t pts = pending ? pendingPts : now;
            if (!pending && refine) --refinements;
            pending = false;
            lastEncode = now;
            nextEncodeUs = std::max(nextEncodeUs + intervalUs, now - intervalUs);
            const uint64_t before = nowUs();
            size_t frameBytes = 0;
            const bool ok = encoder->encode(pts, idr, [&](const uint8_t* data, size_t n, bool key) {
                frameBytes = n;
                proto::Writer w(proto::kVideo);
                w.u8(key ? proto::kFlagKey : 0).u64(pts).bytes(data, n);
                const uint64_t sendStart = nowUs();
                const bool sent = ch.send(w.data());
                windowSendUs += nowUs() - sendStart;
                return sent;
            });
            if (!ok) {
                std::printf("Encodage : %s\n", encoder->lastError().c_str());
                break;
            }
            const uint64_t spent = nowUs() - before;
            ++encoded;
            bytesSent += frameBytes;
            encodeUsSum += spent;
            ++windowFrames;
            windowBytes += frameBytes;
            windowEncUs += spent;
        } else if (pending) {
            Sleep(1);
        }

        if (now - windowStart >= 1'000'000) {
            const double secs = static_cast<double>(now - windowStart) / 1e6;
            const double mbps = static_cast<double>(windowBytes) * 8 / 1e6 / secs;
            const double frames = windowFrames ? static_cast<double>(windowFrames) : 1;
            const double sendMs = static_cast<double>(windowSendUs) / frames / 1000;
            const double encMs = static_cast<double>(windowEncUs) / frames / 1000 - sendMs;
            // En mode icône, une ligne sur dix : le journal d'une longue session reste lisible.
            if (!opt.tray || windowCount++ % 10 == 0) {
                std::printf("[%s] capture %5.1f fps  envoi %5.1f fps  %6.1f Mb/s  encodage %5.1f ms  socket %5.1f ms  latence moy %5.1f ms  p95 %5.1f ms  décodage %5.1f ms  pertes %u\n",
                            opt.link == Link::Usb ? "USB" : "WiFi", static_cast<double>(windowCaptured) / secs,
                            static_cast<double>(windowFrames) / secs, mbps, encMs, sendMs, stats.tabletAvgLat / 1000.0,
                            stats.tabletP95Lat / 1000.0, stats.tabletAvgDec / 1000.0, stats.tabletDrops.load());
                std::fflush(stdout);
            }
            // TCP freine l'envoi quand le lien sature : c'est le signal de baisse du débit.
            const double frameMs = 1000.0 / cfg.fps;
            const uint64_t elapsedSec = (now - startUs) / 1'000'000;
            const bool simulated = opt.simulateCongestion && elapsedSec >= 5 && elapsedSec < 10;
            // Les premières secondes, le décodeur démarre : son P95 n'indique pas une saturation.
            const bool warmingUp = elapsedSec < 3;
            const bool congested = simulated || sendMs > frameMs / 2 || (!warmingUp && stats.tabletP95Lat > kCongestedLatUs);
            uint32_t nextBitrate = currentBitrate;
            if (congested) {
                nextBitrate = std::max(kMinBitrate, currentBitrate / 4 * 3);
                calmSeconds = 0;
            } else if (currentBitrate < cfg.bitrate && ++calmSeconds >= 2) {
                // File d'envoi vide : on remonte franchement, sinon prudemment.
                const bool idle = sendMs < frameMs / 8;
                nextBitrate = std::min(cfg.bitrate, idle ? currentBitrate / 2 * 3 : currentBitrate / 4 * 5);
                calmSeconds = 0;
            }
            if (nextBitrate != currentBitrate && encoder->setBitrate(nextBitrate)) {
                std::printf("Débit : %u -> %u Mb/s (socket %.1f ms, p95 %.1f ms)\n", currentBitrate / 1'000'000, nextBitrate / 1'000'000,
                            sendMs, stats.tabletP95Lat / 1000.0);
                std::fflush(stdout);
                currentBitrate = nextBitrate;
            }

            wchar_t text[120];
            swprintf_s(text, L"%ls connecté · %.0f fps · %.0f ms", linkName.c_str(), static_cast<double>(windowFrames) / secs,
                       stats.tabletAvgLat / 1000.0);
            setStatus(text);
            windowStart = now;
            windowFrames = windowBytes = windowEncUs = windowSendUs = windowCaptured = 0;
        }
    }
    timeEndPeriod(1);
    audio.stop();
    if (displayRequest) {
        PowerClearRequest(displayRequest, PowerRequestDisplayRequired);
        CloseHandle(displayRequest);
    }
    SetThreadExecutionState(ES_CONTINUOUS);

    alive = false;
    ch.shutdown();
    pinger.join();
    receiver.join();
    usbWatcher.join();
    gLastSessionEnd = std::chrono::steady_clock::now();

    if (encoded > 0) {
        const double secs = static_cast<double>(nowUs() - startUs) / 1e6;
        std::printf("RESUME lien=%s images=%llu duree=%.1fs debit=%.1fMb/s encodage_envoi=%.2fms touch=%llu clavier=%llu stylet=%llu\n",
                    opt.link == Link::Usb ? "usb" : "wifi", static_cast<unsigned long long>(encoded), secs,
                    static_cast<double>(bytesSent) * 8 / 1e6 / secs, static_cast<double>(encodeUsSum) / static_cast<double>(encoded) / 1000,
                    static_cast<unsigned long long>(touch.received()), static_cast<unsigned long long>(keys.received()),
                    static_cast<unsigned long long>(pen.received()));
    }
    return keepRunning && !gStop;
}

std::string resolveHost(Options& opt) {
    if (opt.autoLink) opt.link = runAdb(opt, L"forward tcp:7236 tcp:7236") ? Link::Usb : Link::Wifi;
    if (opt.link == Link::Usb) {
        if (!runAdb(opt, L"forward tcp:7236 tcp:7236")) return {};
        // Mémorise l'adresse Wi-Fi tant que le lien USB la rend accessible.
        std::string out;
        if (runAdb(opt, L"shell ip -4 -o addr show wlan0", &out)) {
            const auto p = out.find("inet ");
            if (p != std::string::npos) saveTabletAddress(out.substr(p + 5, out.find('/', p) - p - 5));
        }
        return "127.0.0.1";
    }
    if (!opt.wifiHost.empty()) return opt.wifiHost;
    std::string found = discoverTablet(3000);
    if (found.empty()) return loadTabletAddress();
    saveTabletAddress(found);
    return found;
}

void serviceLoop(Options opt) {
    int backoffMs = 500;
    while (!gStop) {
        setStatus(L"Recherche de la tablette");
        if (std::chrono::steady_clock::now() - gLastSessionEnd > kMonitorGrace) {
            // Jamais d'écran du PC éteint sans tablette connectée.
            restoreDisplays();
            gDisplayMode = proto::kDisplayExtended;
            gMonitor.release();
        }
        const std::string host = resolveHost(opt);
        if (host.empty()) {
            std::printf("Tablette introuvable.\n");
        } else {
            const auto start = std::chrono::steady_clock::now();
            if (!runSession(opt, host)) return;
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) backoffMs = 500;
        }
        Sleep(backoffMs);
        backoffMs = std::min(backoffMs * 2, 2000);
    }
}

Options parseArgs(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--usb") o.link = Link::Usb;
        else if (a == L"--wifi") {
            o.link = Link::Wifi;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                for (const wchar_t c : next()) o.wifiHost.push_back(static_cast<char>(c));
            }
        }
        else if (a == L"--adb") o.adbPath = next();
        else if (a == L"--output") o.outputIndex = std::stoi(next());
        else if (a == L"--fps") o.fps = static_cast<uint32_t>(std::stoi(next()));
        else if (a == L"--bitrate") o.bitrateMbps = static_cast<uint32_t>(std::stoi(next()));
        else if (a == L"--codec") o.hevc = next() != L"h264";
        else if (a == L"--no-input") o.injectTouch = false;
        else if (a == L"--tray") o.tray = true;
        else if (a == L"--no-monitor") o.virtualMonitor = false;
        else if (a == L"--auto") o.autoLink = true;
        else if (a == L"--system") o.system = true;
        else if (a == L"--secure") gSecureMode = true;
        else if (a == L"--simulate-congestion") o.simulateCongestion = true;
        else if (a == L"--duration") o.durationSec = std::stoi(next());
        else if (a == L"--list") o.listOnly = true;
        else if (a == L"--stop") o.stopOthers = true;
        else if (a == L"--open") o.open = true;
        else if (a == L"--encoder") o.forceMediaFoundation = next() == L"mf";
    }
    return o;
}

// Journal de l'icône : chaque ligne est datée, et le journal de la session précédente est
// gardé à côté (agent.log.1), ce qui permet de comprendre un arrêt suivi d'une relance.
void startTimestampedLog(const std::wstring& logPath) {
    MoveFileExW(logPath.c_str(), (logPath + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    // _wfsopen partagé : le journal reste lisible pendant que l'agent tourne.
    FILE* file = _wfsopen(logPath.c_str(), L"wb", _SH_DENYNO);
    int fds[2];
#pragma warning(suppress : 4996)
    if (!file || _pipe(fds, 64 * 1024, _O_BINARY) != 0 || !freopen("NUL", "w", stdout)) {
#pragma warning(suppress : 4996)
        if (_wfreopen(logPath.c_str(), L"w", stdout)) setvbuf(stdout, nullptr, _IONBF, 0);
        return;
    }
    _dup2(fds[1], _fileno(stdout));
    _close(fds[1]);
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::thread([file, readFd = fds[0]] {
        std::string pending;
        char buf[4096];
        int got;
        while ((got = _read(readFd, buf, sizeof(buf))) > 0) {
            pending.append(buf, static_cast<size_t>(got));
            size_t eol;
            while ((eol = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, eol);
                pending.erase(0, eol + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                SYSTEMTIME t;
                GetLocalTime(&t);
                std::fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, line.c_str());
                std::fflush(file);
            }
        }
    }).detach();
}

// Nom lié à la session : chaque agent surveille cet événement, signalé par --stop.
constexpr wchar_t kQuitEvent[] = L"Global\\ScreenlinkQuit";
constexpr wchar_t kInstanceMutex[] = L"Global\\ScreenlinkAgent";

bool agentRunning() {
    HANDLE instance = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex);
    const DWORD error = GetLastError();
    if (instance) CloseHandle(instance);
    return instance != nullptr || error == ERROR_ACCESS_DENIED;
}

// Raccourci du menu Démarrer : relance le service s'il est arrêté, puis indique quoi faire.
int openFromShortcut() {
    if (!agentRunning()) {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        SC_HANDLE service = manager ? OpenServiceW(manager, L"36Screenlink", SERVICE_START) : nullptr;
        if (service) {
            StartServiceW(service, 0, nullptr);
            CloseServiceHandle(service);
        }
        if (manager) CloseServiceHandle(manager);
        for (int i = 0; i < 30 && !agentRunning(); ++i) Sleep(200);
    }
    MessageBoxW(nullptr,
                agentRunning() ? L"36Screenlink est actif.\n\nOuvrez l'application sur la tablette : l'écran apparaît tout seul.\n\nSur la tablette, glissez depuis le bord gauche pour ouvrir le menu."
                               : L"36Screenlink n'a pas pu démarrer. Réinstallez l'application.",
                L"36Screenlink", MB_ICONINFORMATION | MB_SETFOREGROUND);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Les DLL chargées plus tard ne viennent que de System32, jamais du dossier courant.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetConsoleCtrlHandler([](DWORD) -> BOOL { gStop = true; return TRUE; }, TRUE);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    Options opt = parseArgs(argc, argv);
    const bool portable = std::filesystem::exists(std::filesystem::path(exeDirectory()) / L"portable.txt");
    // Double-clic sur l'exe : la version portable démarre, la version installée informe.
    if (argc == 1) {
        if (portable) {
            opt.autoLink = true;
            opt.tray = true;
        } else {
            opt.open = true;
        }
    }
    if (opt.listOnly) {
        int i = 0;
        for (const auto& o : enumerateOutputs())
            logw(L"%d: %ls  %ldx%ld  %ls%ls\n", i++, o.deviceName.c_str(), o.desktop.right - o.desktop.left,
                         o.desktop.bottom - o.desktop.top, o.adapterName.c_str(), o.primary ? L"  [principal]" : L"");
        return 0;
    }


    if (opt.open) return openFromShortcut();

    if (opt.stopOthers) {
        HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, kQuitEvent);
        if (quit) SetEvent(quit);
        return quit ? 0 : 1;
    }

    // Une seule instance sur la machine, service compris : deux agents se disputeraient la tablette.
    SetLastError(ERROR_SUCCESS);
    HANDLE instance = CreateMutexW(nullptr, FALSE, kInstanceMutex);
    const DWORD instanceError = GetLastError();
    if (instanceError == ERROR_ALREADY_EXISTS || instanceError == ERROR_ACCESS_DENIED) {
        std::printf("36Screenlink tourne déjà.\n");
        if (opt.tray) MessageBoxW(nullptr, L"36Screenlink tourne déjà.", L"36Screenlink", MB_ICONINFORMATION);
        if (instance) CloseHandle(instance);
        WSACleanup();
        return 0;
    }

    gSystemMode = opt.system;
    gFps = opt.fps;

    HANDLE quitEvent = CreateEventW(nullptr, TRUE, FALSE, kQuitEvent);
    std::thread quitWatcher([quitEvent] {
        WaitForSingleObject(quitEvent, INFINITE);
        gStop = true;
        trayClose();
    });

    if (opt.tray) {
        const std::wstring logPath = logFilePath();
        FreeConsole();
        startTimestampedLog(logPath);
        AudioStreamer::recoverMute();
        gAudioOnTablet = loadAudioOnTablet();
        if (opt.virtualMonitor && opt.outputIndex < 0) gMonitor.start();
        std::thread worker(serviceLoop, opt);
        TrayConfig tray;
        tray.status = [] { std::lock_guard lock(gStatusMutex); return gStatus; };
        tray.onQuit = [] { gStop = true; };
        tray.onUserQuit = [] { gUserQuit = true; };
        tray.audioOnTablet = [] { return gAudioOnTablet.load(); };
        tray.setAudioOnTablet = [](bool onTablet) { gAudioOnTablet = onTablet; saveAudioOnTablet(onTablet); };
        tray.logPath = logPath;
        tray.managedByService = opt.system;
        tray.portableDirectory = portable && !opt.system ? exeDirectory() : L"";
        runTray(tray);
        worker.join();
    } else {
        if (opt.virtualMonitor && opt.outputIndex < 0) gMonitor.start();
        serviceLoop(opt);
    }
    restoreDisplays();
    SetEvent(quitEvent);
    quitWatcher.join();
    WSACleanup();
    // Le service reconnaît ce code et ne relance pas l'agent : voir kUserQuitCode côté service.
    return gUserQuit && opt.system ? 36 : 0;
}
