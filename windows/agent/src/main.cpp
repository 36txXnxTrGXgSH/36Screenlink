// Agent Windows : capture d'un écran, encodage NVENC, envoi chiffré vers la tablette.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellscalingapi.h>
#include <timeapi.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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
#include "device_helper.h"
#include "window_follow.h"

#pragma comment(lib, "winmm.lib")

namespace {

enum class Link { Usb, Wifi, Tv };

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
// Le son ne part que vers un appareil à la fois : le dernier qui l'a demandé.
std::atomic<int> gAudioDevice{-1};
bool gUserQuit = false;
std::mutex gStatusMutex;
std::wstring gStatus = L"Démarrage";
std::wstring gSlotStatus[VirtualMonitor::kSlots];
VirtualMonitor gMonitor;
// Après ce délai sans appareil, son moniteur est retiré et Windows récupère les fenêtres.
constexpr auto kMonitorGrace = std::chrono::seconds(10);
constexpr uint32_t kMinBitrate = 15'000'000;
constexpr uint32_t kCongestedLatUs = 150'000;
// Télévision : environ 85 ms en moyenne au repos, P95 vers 130 ms. Au-delà du premier seuil, son
// décodeur ne suit plus ; en deçà du second, le débit peut remonter doucement.
constexpr uint32_t kTvCongestedLatUs = 250'000;
constexpr uint32_t kTvCalmLatUs = 180'000;
// Premier port local des tunnels adb, un par appareil USB.
constexpr uint16_t kFirstUsbPort = 7237;

// Réglages propres à chaque appareil appairé, gardés d'une session à l'autre.
struct DeviceSettings {
    std::atomic<uint8_t> displayMode{proto::kDisplayExtended};
    std::atomic<uint32_t> fps{120};
};
DeviceSettings gSettings[kMaxDevices];
// Changements de configuration d'affichage : un seul à la fois, quel que soit l'appareil.
std::mutex gDisplayMutex;
// Emplacements dont la session a éteint les écrans du PC (« Tablette seule »).
std::atomic<bool> gSlotAlone[VirtualMonitor::kSlots];

// « Tablette seule » : l'emplacement qui a l'écran pour lui seul, -1 sinon. Les autres appareils
// attendent qu'il le rende.
std::atomic<int> gExclusiveSlot{-1};
std::atomic<bool> gStopSlot[VirtualMonitor::kSlots];
// Appareils en session (index, type), annoncés à chacun pour ses menus.
std::mutex gConnectedMutex;
std::vector<std::pair<int, uint8_t>> gConnected;
std::atomic<uint32_t> gConnectedVersion{0};

// Emplacement de la session du thread courant, -1 hors session.
thread_local int tSlot = -1;

void setStatus(const std::wstring& s) {
    std::lock_guard lock(gStatusMutex);
    if (tSlot >= 0) gSlotStatus[tSlot] = s;
    else gStatus = s;
}

std::wstring trayStatus() {
    std::lock_guard lock(gStatusMutex);
    std::wstring all;
    for (const auto& s : gSlotStatus) {
        if (s.empty()) continue;
        if (!all.empty()) all += L" | ";
        all += s;
    }
    return all.empty() ? gStatus : all;
}

// Journal : chaque ligne d'une session porte le nom de son appareil, les sessions tournant en parallèle.
void logLine(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fputs((tLogTag + text).c_str(), stdout);
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
    // Plusieurs sessions appellent adb : un appel à la fois, sinon un adb hériterait du tuyau
    // d'un autre et sa sortie se perdrait.
    static std::mutex adbMutex;
    std::lock_guard adbLock(adbMutex);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    // Lancé par le service, l'agent ne peut parfois pas créer de tuyau : la sortie passe alors par
    // un fichier du dossier de données, réservé à SYSTEM et aux administrateurs.
    const bool piped = CreatePipe(&readPipe, &writePipe, &sa, 0) != FALSE;
    const std::wstring outPath = piped ? std::wstring() : dataFilePath(L"adb-sortie.tmp");
    if (piped) {
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    } else {
        writePipe = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (writePipe == INVALID_HANDLE_VALUE) writePipe = nullptr;
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + findAdb(opt) + L"\" " + args;
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (piped || writePipe) CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        logLine("adb : lancement impossible (%lu)\n", GetLastError());
        if (piped) CloseHandle(readPipe);
        return false;
    }
    std::string text;
    if (piped) {
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) text.append(buf, got);
        CloseHandle(readPipe);
    }
    WaitForSingleObject(pi.hProcess, 10000);
    if (!piped) {
        std::ifstream in(outPath, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
        in.close();
        DeleteFileW(outPath.c_str());
    }
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

const OutputInfo* pickOutput(const std::vector<OutputInfo>& outputs, int index, const std::wstring& virtualName) {
    if (outputs.empty()) return nullptr;
    if (index >= 0) return index < static_cast<int>(outputs.size()) ? &outputs[index] : nullptr;
    // Jamais un second écran physique du PC : le moniteur de cet appareil, sinon miroir de l'écran principal.
    for (const auto& o : outputs)
        if (!virtualName.empty() && o.deviceName == virtualName) return &o;
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
// Renvoie le mode choisi (grand côté, petit côté), ou {0, 0} si aucun ne convient.
std::pair<uint32_t, uint32_t> setBestMode(const std::wstring& device, uint32_t longSide, uint32_t shortSide, uint32_t fps, bool force = false) {
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
    if (bestScore < 0 || !EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &current)) return {0, 0};
    const std::pair<uint32_t, uint32_t> chosen{std::max(best.dmPelsWidth, best.dmPelsHeight), std::min(best.dmPelsWidth, best.dmPelsHeight)};
    const uint32_t actual = actualRefreshHz(device);
    const bool sameSize = std::max(current.dmPelsWidth, current.dmPelsHeight) == std::max(best.dmPelsWidth, best.dmPelsHeight) &&
                          std::min(current.dmPelsWidth, current.dmPelsHeight) == std::min(best.dmPelsWidth, best.dmPelsHeight);
    const bool rateOk = current.dmDisplayFrequency == best.dmDisplayFrequency &&
                        (actual == 0 || std::abs(static_cast<int>(actual) - static_cast<int>(best.dmDisplayFrequency)) <= 1);
    if (sameSize && rateOk && !force) return chosen;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    // Même mode annoncé mais autre fréquence réelle : CDS_RESET force Windows à le réappliquer.
    // Au retour de « Tablette seule », Windows reprend ensuite le mode de sa base d'affichage :
    // on y inscrit donc celui du moniteur virtuel, qui n'appartient qu'à 36Screenlink.
    DWORD flags = sameSize || force ? CDS_RESET : 0;
    if (force) flags |= CDS_UPDATEREGISTRY;
    const LONG result = ChangeDisplaySettingsExW(device.c_str(), &best, nullptr, flags, nullptr);
    logLine("Mode de l'écran : %lux%lu à %lu Hz (résultat %ld)\n", best.dmPelsWidth, best.dmPelsHeight, best.dmDisplayFrequency, result);
    Sleep(500);
    return chosen;
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

// Garde les écrans des appareils (36Screenlink) et éteint ceux du PC. Avec deux appareils,
// l'autre reste allumé : « seul » veut dire sans l'écran du PC.
bool useOnlyDisplay(const std::wstring& device, bool keepOtherDevices) {
    auto current = queryDisplays();
    if (!current) return false;
    std::vector<std::wstring> virtualNames;
    for (const auto& o : enumerateOutputs())
        if (o.isVirtual) virtualNames.push_back(o.deviceName);
    const auto gdiName = [](const DISPLAYCONFIG_PATH_INFO& path) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.sourceInfo.adapterId;
        name.header.id = path.sourceInfo.id;
        return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS ? std::wstring(name.viewGdiDeviceName) : std::wstring();
    };
    std::vector<DISPLAYCONFIG_PATH_INFO> kept;
    POINTL origin{};
    bool found = false;
    for (const auto& path : current->paths) {
        const std::wstring name = gdiName(path);
        if (std::find(virtualNames.begin(), virtualNames.end(), name) == virtualNames.end()) continue;
        if (!keepOtherDevices && name != device) continue;
        kept.push_back(path);
        if (name == device && path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
            origin = current->modes[path.sourceInfo.modeInfoIdx].sourceMode.position;
            found = true;
        }
    }
    if (!found) return false;
    // L'écran de cet appareil devient le principal (origine 0,0), les autres gardent leur place relative.
    auto modes = current->modes;
    for (const auto& path : kept) {
        if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) continue;
        auto& pos = modes[path.sourceInfo.modeInfoIdx].sourceMode.position;
        pos.x -= origin.x;
        pos.y -= origin.y;
    }
    if (!gSavedDisplays) gSavedDisplays = current;
    const LONG result = SetDisplayConfig(static_cast<UINT32>(kept.size()), kept.data(), static_cast<UINT32>(modes.size()), modes.data(),
                                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    logLine("Tablette seule : résultat %ld\n", result);
    Sleep(800);
    return result == ERROR_SUCCESS;
}

bool restoreDisplays() {
    if (!gSavedDisplays) return false;
    auto& s = *gSavedDisplays;
    LONG result = SetDisplayConfig(static_cast<UINT32>(s.paths.size()), s.paths.data(), static_cast<UINT32>(s.modes.size()), s.modes.data(),
                                   SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    // Si le moniteur virtuel a changé entre-temps, Windows reprend sa dernière configuration.
    if (result != ERROR_SUCCESS) result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT);
    logLine("Écrans du PC rétablis : résultat %ld\n", result);
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
    logLine("Orientation de l'écran : %s (résultat %ld)\n", portrait ? "portrait" : "paysage", result);
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
            logLine("Encodeur matériel : NVENC\n");
            return nvenc;
        }
        logLine("NVENC : %s\n", nvenc->lastError().c_str());
    }
    auto generic = std::make_unique<MediaFoundationEncoder>();
    if (generic->open(device, input, cfg)) return generic;
    logLine("Media Foundation : %s\n", generic->lastError().c_str());
    return nullptr;
}

// Identifiant de contact réservé à la touche simulée sur une icône de la barre des tâches.
constexpr uint8_t kWindowMoveContact = 200;

// Un appareil joignable : en USB par son tunnel adb, ou en Wi-Fi par son adresse.
struct Peer {
    Link link = Link::Wifi;
    std::string host;
    uint16_t port = proto::kPort;
    std::string serial;      // USB seulement
    std::string wifiAddress; // adresse Wi-Fi de l'appareil, connue même en USB
};

struct Session {
    int device = -1; // index de la clé d'appairage
    int slot = 0;    // moniteur virtuel
    Peer peer;
    std::unique_ptr<SecureChannel> channel;
    std::atomic<bool> restart{false};
    std::atomic<bool> finished{false};
    bool stopAll = false;
    std::thread thread;
};

struct SessionStats {
    std::atomic<uint32_t> tabletFrames{0}, tabletAvgLat{0}, tabletP95Lat{0}, tabletAvgDec{0}, tabletDrops{0};
    std::atomic<bool> requestIdr{false};
    // UAC ou verrouillage montrés : aucune saisie avant la confirmation d'identité sur la tablette.
    bool secureDesktop = false;
    std::atomic<bool> armed{false};
    std::atomic<int64_t> lastPongUs{0};
};

void receiverLoop(Session& session, SecureChannel& ch, TouchInjector& touch, KeyInjector& keys, PenInjector& pen, MouseInjector& mouse, WindowFollower& follower, DeviceHelper& helper, SessionStats& stats, std::atomic<bool>& alive) {
    tSlot = session.slot;
    tLogTag = "[Appareil " + std::to_string(session.device + 1) + "] ";
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
                logLine("Écran sécurisé : saisie autorisée après confirmation sur la tablette.\n");
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
            if (id == proto::kControlDisplay && value <= proto::kDisplayDevicesOnly) gSettings[session.device].displayMode = value;
            if (id == proto::kControlFps && (value == 60 || value == 120 || value == 144)) gSettings[session.device].fps = value;
            session.restart = true;
            break;
        }
        case proto::kAudioRoute: {
            const bool onTablet = r.u8() != 0;
            // Appareil visé : celui-ci, ou un autre appareil connecté choisi dans son menu.
            int target = session.device;
            if (r.has(1)) {
                const uint8_t wanted = r.u8();
                if (wanted != proto::kNoDevice && wanted < kMaxDevices) target = wanted;
            }
            if (onTablet) gAudioDevice = target;
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
        case proto::kKeyboardState:
            helper.setKeyboardFlags(r.u8());
            break;
        case proto::kWindowMove: {
            if (!inputAllowed || !r.has(5)) break;
            const uint16_t x = r.u16(), y = r.u16();
            const auto screen = helper.screen(r.u8());
            if (!screen) break;
            // Une touche sur l'icône active la fenêtre, ou montre ses miniatures : la fenêtre
            // choisie part alors sur l'écran voulu.
            follower.sendTo(*screen);
            touch.apply({Contact{kWindowMoveContact, true, x, y}});
            Sleep(60);
            touch.apply({Contact{kWindowMoveContact, false, x, y}});
            break;
        }
        case proto::kDeviceLog: {
            // Journal d'un appareil sans console (télévision) : une ligne courte, sans caractère de contrôle.
            std::string text(reinterpret_cast<const char*>(msg.data() + 1), std::min<size_t>(msg.size() - 1, 300));
            for (char& c : text)
                if (static_cast<unsigned char>(c) < 0x20) c = ' ';
            logLine("Appareil : %s\n", text.c_str());
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
    swprintf_s(text, L"Code d'appairage : %06u\n\nLe même code s'affiche-t-il sur l'appareil ?\nRépondez Non s'il est différent.", code);
    return MessageBoxW(nullptr, text, L"36Screenlink", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
}

// Échange ECDH : rien n'est enregistré sans confirmation du même code des deux côtés.
void tryPairing(SecureChannel& ch, int device, const Peer& peer) {
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
    if (ch.pair(psk, confirm) && savePsk(device, psk)) {
        SecureZeroMemory(psk.data(), psk.size());
        if (!peer.serial.empty()) saveDeviceSerial(device, peer.serial);
        if (!peer.wifiAddress.empty()) saveDeviceAddress(device, peer.wifiAddress);
        logLine("Appairage réussi : appareil %d.\n", device + 1);
        trayClearAlert();
        return;
    }
    logLine("Appairage annulé : %s\n", ch.lastError().c_str());
    // Tablette déjà appairée à un autre PC : elle attend que l'utilisateur l'autorise.
    if (!asked && ch.lastStatus() == proto::kStatusPairRefused)
        trayAlert(L"Sur l'appareil à ajouter, ouvrez l'application et touchez Nouvel appairage.");
}

// Une session complète, sur un canal déjà authentifié. Renvoie false pour tout arrêter.
bool runSession(const Options& opt, Session& s) {
    SecureChannel& ch = *s.channel;
    DeviceSettings& settings = gSettings[s.device];

    std::vector<uint8_t> msg;
    if (!ch.receive(msg) || msg.empty() || msg[0] != proto::kHello) return true;
    proto::Reader hello(msg.data() + 1, msg.size() - 1);
    const uint16_t tabletW = hello.u16(), tabletH = hello.u16(), tabletFps = hello.u16();
    const uint8_t codecs = hello.u8();
    // Type d'appareil, absent d'une ancienne application : tablette par défaut.
    const uint8_t kind = hello.has(1) ? hello.u8() : proto::kKindTablet;
    logLine("%s : %ux%u, %u Hz, codecs 0x%02x\n", kind == proto::kKindPhone ? "Téléphone" : kind == proto::kKindTv ? "Télévision" : "Tablette",
            tabletW, tabletH, tabletFps, codecs);
    {
        std::lock_guard lock(gConnectedMutex);
        gConnected.emplace_back(s.device, kind);
        ++gConnectedVersion;
    }
    // À la fin de la session, quel que soit le chemin de sortie.
    struct Leave {
        Session& s;
        ~Leave() {
            std::lock_guard lock(gConnectedMutex);
            gConnected.erase(std::remove_if(gConnected.begin(), gConnected.end(), [this](const auto& d) { return d.first == s.device; }),
                             gConnected.end());
            ++gConnectedVersion;
            gSlotAlone[s.slot] = false;
            int mine = s.slot;
            gExclusiveSlot.compare_exchange_strong(mine, -1);
        }
    } leave{s};

    bool secureDesktop = false;
    if (gSecureMode) {
        attachInputDesktop();
        secureDesktop = secureScreenActive();
    }
    const uint8_t displayMode = settings.displayMode;
    const uint32_t targetFps = settings.fps;
    // Deux appareils peuvent arriver en même temps : la configuration d'affichage se change
    // pour l'un, puis pour l'autre.
    std::unique_lock displayLock(gDisplayMutex);
    // Après « Tablette seule », Windows peut remettre le moniteur virtuel à 60 Hz un peu plus
    // tard tout en annonçant encore l'ancien mode : on réapplique alors le mode voulu. Les écrans
    // du PC ne reviennent pas tant que l'autre appareil les veut éteints.
    bool otherAlone = false;
    for (int slot = 0; slot < VirtualMonitor::kSlots; ++slot) otherAlone |= slot != s.slot && gSlotAlone[slot];
    const bool alone = displayMode == proto::kDisplayTabletOnly || displayMode == proto::kDisplayDevicesOnly;
    const bool restored = !alone && !otherAlone && restoreDisplays();
    gSlotAlone[s.slot] = alone;
    // « Tablette seule » : l'autre appareil rend la main et attend.
    if (displayMode == proto::kDisplayTabletOnly) {
        gExclusiveSlot = s.slot;
        // Les autres appareils rendent la main. Leur moniteur part tout de suite : retiré plus
        // tard, Windows pourrait rallumer l'écran du PC.
        for (int slot = 0; slot < VirtualMonitor::kSlots; ++slot) {
            if (slot == s.slot) continue;
            gStopSlot[slot] = true;
            if (gMonitor.raised(slot)) {
                gMonitor.release(slot);
                Sleep(800);
            }
        }
    } else {
        int mine = s.slot;
        gExclusiveSlot.compare_exchange_strong(mine, -1);
    }
    gStopSlot[s.slot] = false;
    // Dupliquer montre l'écran du PC : le moniteur virtuel n'a plus lieu d'être.
    std::wstring virtualName;
    if (displayMode == proto::kDisplayDuplicate) {
        gMonitor.release(s.slot);
        Sleep(800);
    } else {
        virtualName = gMonitor.raise(s.slot);
    }
    auto outputs = enumerateOutputs();
    const bool mirrorPrimary = secureDesktop || displayMode == proto::kDisplayDuplicate;
    const int wanted = mirrorPrimary ? primaryIndex(outputs) : opt.outputIndex;
    const OutputInfo* output = pickOutput(outputs, wanted, virtualName);
    if (!output) {
        logLine("Aucun écran à capturer.\n");
        return false;
    }
    // Mode voulu pour le moniteur de cet appareil, dans son orientation : {0, 0} s'il n'y en a pas.
    std::pair<uint32_t, uint32_t> wanted2{0, 0};
    const bool portrait = tabletH > tabletW;
    const auto modeDrifted = [&](const std::wstring& name) {
        DEVMODEW current{};
        current.dmSize = sizeof(current);
        if (wanted2.first == 0 || !EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &current)) return false;
        const uint32_t w = portrait ? wanted2.second : wanted2.first, h = portrait ? wanted2.first : wanted2.second;
        return current.dmPelsWidth != w || current.dmPelsHeight != h;
    };
    if (!mirrorPrimary && output->isVirtual) {
        const uint32_t fps = std::min<uint32_t>(targetFps, tabletFps);
        wanted2 = setBestMode(output->deviceName, std::max(tabletW, tabletH), std::min(tabletW, tabletH), fps, restored);
        setOrientation(output->deviceName, portrait);
        // L'arrivée du moniteur d'un autre appareil fait parfois reprendre à Windows le mode par
        // défaut de celui-ci : on le réapplique jusqu'à ce qu'il tienne.
        for (int attempt = 0; attempt < 3 && modeDrifted(output->deviceName); ++attempt) {
            logLine("Mode repris par Windows : réapplication\n");
            Sleep(500);
            setBestMode(output->deviceName, std::max(tabletW, tabletH), std::min(tabletW, tabletH), fps, true);
            setOrientation(output->deviceName, portrait);
        }
        if (alone) useOnlyDisplay(output->deviceName, displayMode == proto::kDisplayDevicesOnly);
        // La rotation change les coordonnées de l'écran : on relit la description.
        outputs = enumerateOutputs();
        output = pickOutput(outputs, wanted, virtualName);
        if (!output) return true;
    }
    displayLock.unlock();
    logw(L"Capture : %ls sur %ls%ls\n", output->deviceName.c_str(), output->adapterName.c_str(),
                 output->primary ? L" (miroir de l'écran principal)" : L"");

    Capturer cap;
    if (!cap.open(*output)) {
        logLine("Capture : %s\n", cap.lastError().c_str());
        return true;
    }
    logLine("Capture : %ux%u, rotation %u\n", cap.width(), cap.height(), cap.rotationDegrees());

    const UINT width = cap.width() & ~1u, height = cap.height() & ~1u;
    ComPtr<ID3D11Texture2D> capTarget = makeTexture(cap.device(), width, height, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0);

    EncoderConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.fps = std::min<uint32_t>(targetFps, tabletFps ? tabletFps : targetFps);
    cfg.bitrate = opt.bitrateMbps * 1'000'000;
    // Télévision : son port réseau est limité à 100 Mb/s, et chaque image doit le traverser avant
    // d'être décodée. À 30 Mb/s, une image transite en environ 5 ms contre 10 ms à 60 Mb/s, pour
    // une qualité au niveau d'un Blu-ray en 1080p. La régulation peut encore baisser ce débit.
    if (s.peer.link == Link::Tv) cfg.bitrate = std::min<uint32_t>(cfg.bitrate, 30'000'000);
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
        if (secureDesktop) logLine("Écran sécurisé montré : saisie bloquée jusqu'à confirmation sur la tablette.\n");
    }

    // Utile seulement en écran étendu : ailleurs, aucune fenêtre n'est sur un autre écran visible.
    WindowFollower follower;
    if (opt.injectTouch && !mirrorPrimary && output->isVirtual && displayMode == proto::kDisplayExtended) follower.start(output->desktop);

    trayClearAlert();
    SessionStats stats;
    stats.lastPongUs = static_cast<int64_t>(nowUs());
    stats.secureDesktop = secureDesktop;
    std::atomic<bool> alive{true};
    // Écrans, barre des tâches et champ de saisie : de quoi aider la saisie sur l'appareil.
    DeviceHelper helper;
    helper.start(ch, output->desktop, follower.running() && !secureDesktop);
    std::thread receiver(receiverLoop, std::ref(s), std::ref(ch), std::ref(touch), std::ref(keys), std::ref(pen), std::ref(mouse), std::ref(follower), std::ref(helper), std::ref(stats), std::ref(alive));
    // Session en Wi-Fi alors que le câble est branché : on repasse par l'USB, plus stable.
    std::atomic<bool> usbAvailable{false};
    std::thread usbWatcher([&] {
        const std::string serial = loadDeviceSerial(s.device);
        if (!opt.autoLink || s.peer.link != Link::Wifi || serial.empty()) return;
        const std::wstring args = L"-s " + std::wstring(serial.begin(), serial.end()) + L" get-state";
        while (alive && !gStop) {
            for (int i = 0; i < 50 && alive && !gStop; ++i) Sleep(100);
            std::string state;
            if (alive && runAdb(opt, args, &state) && state.find("device") != std::string::npos) {
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
                logLine("Plus de réponse de la tablette.\n");
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
    uint64_t windowCount = 0, lastDesktopCheck = 0, lastModeCheck = nowUs();
    const std::wstring capturedName = output->deviceName;
    const RECT capturedRect = output->desktop;
    uint32_t currentBitrate = cfg.bitrate;
    int calmSeconds = 0;
    int tvHold = 0;
    const uint64_t startUs = nowUs();
    bool forceIdr = true, pending = false;
    uint64_t pendingPts = 0;
    int refinements = 0;
    bool keepRunning = true;
    std::wstring linkName = std::wstring(L"Appareil ") + std::to_wstring(s.device + 1) + (s.peer.link == Link::Usb ? L" : USB" : s.peer.link == Link::Tv ? L" : télévision" : L" : Wi-Fi");
    AudioStreamer audio;
    bool audioRunning = false, audioStateSent = false;
    int audioTargetSent = -2;
    uint32_t connectedSent = UINT32_MAX;
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
        if (s.restart.exchange(false)) break;
        if (gStopSlot[s.slot].exchange(false)) {
            logLine("Écran laissé à l'autre appareil, en mode seul\n");
            break;
        }
        // Appareils connectés : chacun connaît les autres, pour sa sortie son et son affichage.
        if (gConnectedVersion != connectedSent) {
            std::lock_guard lock(gConnectedMutex);
            connectedSent = gConnectedVersion;
            proto::Writer w(proto::kDevices);
            w.u8(static_cast<uint8_t>(s.device)).u8(static_cast<uint8_t>(gConnected.size()));
            for (const auto& [device, deviceKind] : gConnected) w.u8(static_cast<uint8_t>(device)).u8(deviceKind);
            ch.send(w.data());
        }
        if (usbAvailable) {
            logLine("Câble USB détecté : passage de la session en USB.\n");
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
        // Son vers un seul appareil : celui qui l'a demandé, ou le premier connecté.
        int noOwner = -1;
        gAudioDevice.compare_exchange_strong(noOwner, s.device);
        const bool audioWanted = gAudioOnTablet && gAudioDevice == s.device;
        const int audioTarget = gAudioOnTablet ? gAudioDevice.load() : -1;
        if (audioWanted != audioRunning || !audioStateSent || audioTarget != audioTargetSent) {
            audioTargetSent = audioTarget;
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
            w.u8(audioWanted ? 1 : 0).u8(audioTarget < 0 ? proto::kNoDevice : static_cast<uint8_t>(audioTarget));
            ch.send(w.data());
        }
        if (opt.durationSec > 0 && nowUs() - startUs > static_cast<uint64_t>(opt.durationSec) * 1'000'000) {
            keepRunning = false;
            break;
        }
        // Un autre appareil qui arrive ou part peut faire changer de mode ce moniteur : la session
        // repart alors sur le bon mode, plutôt que d'envoyer une image à la mauvaise taille.
        if (nowUs() - lastModeCheck > 1'000'000) {
            lastModeCheck = nowUs();
            if (modeDrifted(capturedName)) {
                logLine("Mode de l'écran changé par Windows : nouvelle session\n");
                break;
            }
            // Un écran ajouté ou retiré décale souvent les autres : tactile, souris et liste des
            // écrans doivent repartir de la nouvelle position.
            DEVMODEW place{};
            place.dmSize = sizeof(place);
            if (EnumDisplaySettingsW(capturedName.c_str(), ENUM_CURRENT_SETTINGS, &place) &&
                (place.dmPosition.x != capturedRect.left || place.dmPosition.y != capturedRect.top ||
                 static_cast<LONG>(place.dmPelsWidth) != capturedRect.right - capturedRect.left ||
                 static_cast<LONG>(place.dmPelsHeight) != capturedRect.bottom - capturedRect.top)) {
                logLine("Écran déplacé par Windows : nouvelle session\n");
                break;
            }
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
            if (!cap.open(*output) && gSecureMode) logLine("Reprise de capture : %s\n", cap.lastError().c_str());
            forceIdr = true;
            continue;
        }
        if (result == Capturer::Result::Error) {
            logLine("Capture : %s\n", cap.lastError().c_str());
            break;
        }

        if (frame.cursorShapeChanged) {
            const auto& shape = frame.cursorShape;
            proto::Writer w(proto::kCursorShape);
            w.u16(shape.width).u16(shape.height).u16(shape.hotX).u16(shape.hotY).bytes(shape.bgra.data(), shape.bgra.size());
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
        // La télévision garde en réserve les dernières images reçues et compte le temps en images :
        // pour elle, le flux ne s'arrête jamais, une image à chaque échéance, même écran immobile.
        const bool keepTvFed = s.peer.link == Link::Tv && now >= nextEncodeUs;
        const bool refine = !pending && ((refinements > 0 && now - lastEncode > 100'000) || keepTvFed);
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
            if (!pending && refine && refinements > 0) --refinements;
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
                logLine("Encodage : %s\n", encoder->lastError().c_str());
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
                logLine("[%s] capture %5.1f fps  envoi %5.1f fps  %6.1f Mb/s  encodage %5.1f ms  socket %5.1f ms  latence moy %5.1f ms  p95 %5.1f ms  décodage %5.1f ms  pertes %u\n",
                            s.peer.link == Link::Usb ? "USB" : s.peer.link == Link::Tv ? "TV" : "WiFi", static_cast<double>(windowCaptured) / secs,
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
            // Télévision : son lecteur à faible latence n'a pas de tampon, sa latence mesure donc la
            // saturation de son décodeur. Après une baisse, on attend 3 s d'en voir l'effet.
            const bool tv = s.peer.link == Link::Tv;
            if (tvHold > 0) --tvHold;
            const bool tvCongested = tv && !warmingUp && tvHold == 0 && stats.tabletP95Lat > kTvCongestedLatUs;
            const bool congested = simulated || sendMs > frameMs / 2 || tvCongested ||
                                   (!warmingUp && !tv && stats.tabletP95Lat > kCongestedLatUs);
            uint32_t nextBitrate = currentBitrate;
            if (congested) {
                nextBitrate = std::max(kMinBitrate, currentBitrate / 4 * 3);
                calmSeconds = 0;
                if (tv) tvHold = 3;
            } else if (tv) {
                // Remontée lente : la saturation du téléviseur s'installe en dizaines de secondes.
                if (stats.tabletP95Lat < kTvCalmLatUs) ++calmSeconds; else calmSeconds = 0;
                if (currentBitrate < cfg.bitrate && calmSeconds >= 5) {
                    nextBitrate = std::min(cfg.bitrate, currentBitrate / 10 * 11);
                    calmSeconds = 0;
                }
            } else if (currentBitrate < cfg.bitrate && ++calmSeconds >= 2) {
                // File d'envoi vide : on remonte franchement, sinon prudemment.
                const bool idle = sendMs < frameMs / 8;
                nextBitrate = std::min(cfg.bitrate, idle ? currentBitrate / 2 * 3 : currentBitrate / 4 * 5);
                calmSeconds = 0;
            }
            if (nextBitrate != currentBitrate && encoder->setBitrate(nextBitrate)) {
                logLine("Débit : %u -> %u Mb/s (socket %.1f ms, p95 %.1f ms)\n", currentBitrate / 1'000'000, nextBitrate / 1'000'000,
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

    if (encoded > 0) {
        const double secs = static_cast<double>(nowUs() - startUs) / 1e6;
        logLine("RESUME lien=%s images=%llu duree=%.1fs debit=%.1fMb/s encodage_envoi=%.2fms touch=%llu clavier=%llu stylet=%llu\n",
                    s.peer.link == Link::Usb ? "usb" : s.peer.link == Link::Tv ? "tv" : "wifi", static_cast<unsigned long long>(encoded), secs,
                    static_cast<double>(bytesSent) * 8 / 1e6 / secs, static_cast<double>(encodeUsSum) / static_cast<double>(encoded) / 1000,
                    static_cast<unsigned long long>(touch.received()), static_cast<unsigned long long>(keys.received()),
                    static_cast<unsigned long long>(pen.received()));
    }
    return keepRunning && !gStop;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Adresse IPv4 Wi-Fi d'un appareil USB, lue par adb.
std::string usbWifiAddress(const Options& opt, const std::string& serial) {
    std::string out;
    if (!runAdb(opt, L"-s " + widen(serial) + L" shell ip -4 -o addr show wlan0", &out)) return {};
    const auto p = out.find("inet ");
    if (p == std::string::npos) return {};
    const auto end = out.find('/', p);
    return end == std::string::npos ? std::string() : out.substr(p + 5, end - p - 5);
}

// Tous les appareils joignables : chaque appareil USB par son propre tunnel adb, puis ceux du Wi-Fi.
std::vector<Peer> findPeers(const Options& opt, std::map<std::string, uint16_t>& usbPorts) {
    std::vector<Peer> peers;
    if (opt.autoLink || opt.link == Link::Usb) {
        std::string list;
        if (runAdb(opt, L"devices", &list)) {
            size_t start = 0;
            while (start < list.size()) {
                size_t end = list.find('\n', start);
                if (end == std::string::npos) end = list.size();
                std::string line = list.substr(start, end - start);
                start = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto tab = line.find('\t');
                if (tab == std::string::npos || line.substr(tab + 1) != "device") continue;
                const std::string serial = line.substr(0, tab);
                // Un numéro de série ne contient ni espace ni guillemet : rien d'autre ne part vers adb.
                if (serial.find_first_of(" \"&|<>^%") != std::string::npos) continue;
                auto port = usbPorts.find(serial);
                if (port == usbPorts.end()) port = usbPorts.emplace(serial, static_cast<uint16_t>(kFirstUsbPort + usbPorts.size())).first;
                if (!runAdb(opt, L"-s " + widen(serial) + L" forward tcp:" + std::to_wstring(port->second) + L" tcp:" + std::to_wstring(proto::kPort))) continue;
                Peer p;
                p.link = Link::Usb;
                p.host = "127.0.0.1";
                p.port = port->second;
                p.serial = serial;
                p.wifiAddress = usbWifiAddress(opt, serial);
                peers.push_back(p);
            }
        }
    }
    static std::string lastUsb;
    std::string usb;
    for (const auto& p : peers) usb += p.serial + (p.wifiAddress.empty() ? "" : " (" + p.wifiAddress + ")") + " ";
    if (usb != lastUsb) logLine("Appareils USB : %s\n", usb.empty() ? "aucun" : usb.c_str());
    lastUsb = usb;
    if (opt.autoLink || opt.link == Link::Wifi) {
        std::vector<std::string> addresses;
        if (!opt.wifiHost.empty()) {
            addresses.push_back(opt.wifiHost);
        } else {
            addresses = discoverDevices(1500);
            for (int d = 0; d < kMaxDevices; ++d) {
                const std::string known = loadDeviceAddress(d);
                // Une télévision ne s'atteint pas ainsi : c'est elle qui se connecte au PC.
                if (!known.empty() && loadPsk(d) && loadDeviceSerial(d).rfind("tv-", 0) != 0) addresses.push_back(known);
            }
        }
        for (const auto& address : addresses) {
            // Un appareil branché en USB passe par le câble, pas par le Wi-Fi.
            const bool viaUsb = std::any_of(peers.begin(), peers.end(), [&](const Peer& p) { return p.wifiAddress == address; });
            const bool listed = std::any_of(peers.begin(), peers.end(), [&](const Peer& p) { return p.link == Link::Wifi && p.host == address; });
            if (viaUsb || listed) continue;
            Peer p;
            p.host = address;
            p.wifiAddress = address;
            peers.push_back(p);
        }
    }
    return peers;
}

// Cet appareil est-il déjà en session, par ce lien ou par un autre ? Une seconde connexion
// authentifiée prendrait sa place sur l'appareil et couperait la session en cours.
bool peerInSession(const Peer& peer, const std::vector<std::unique_ptr<Session>>& sessions) {
    for (const auto& s : sessions) {
        const std::string serial = loadDeviceSerial(s->device), address = loadDeviceAddress(s->device);
        if (!peer.serial.empty() && (peer.serial == s->peer.serial || peer.serial == serial)) return true;
        if (!peer.wifiAddress.empty() &&
            (peer.wifiAddress == s->peer.wifiAddress || peer.wifiAddress == address || peer.wifiAddress == s->peer.host)) return true;
    }
    return false;
}

bool deviceInSession(int device, const std::vector<std::unique_ptr<Session>>& sessions) {
    return std::any_of(sessions.begin(), sessions.end(), [device](const auto& s) { return s->device == device; });
}

// Emplacement de clé pour un nouvel appairage : celui de cet appareil s'il en avait un, sinon
// un libre, sinon le premier qui n'est pas en session.
int pairingSlotFor(const Peer& peer, const std::vector<std::unique_ptr<Session>>& sessions) {
    for (int d = 0; d < kMaxDevices; ++d) {
        if (deviceInSession(d, sessions)) continue;
        if ((!peer.serial.empty() && loadDeviceSerial(d) == peer.serial) ||
            (!peer.wifiAddress.empty() && loadDeviceAddress(d) == peer.wifiAddress)) return d;
    }
    for (int d = 0; d < kMaxDevices; ++d)
        if (!loadPsk(d)) return d;
    for (int d = 0; d < kMaxDevices; ++d)
        if (!deviceInSession(d, sessions)) return d;
    return -1;
}

// Se connecte à un appareil et retrouve sa clé parmi celles des appareils appairés. Renvoie le
// canal authentifié et l'index de l'appareil, ou rien (appairage lancé si besoin).
std::pair<std::unique_ptr<SecureChannel>, int> identify(const Peer& peer, const std::vector<std::unique_ptr<Session>>& sessions) {
    // D'abord la clé de l'appareil déjà vu à ce numéro de série ou à cette adresse.
    std::vector<int> order;
    for (int d = 0; d < kMaxDevices; ++d)
        if (loadPsk(d)) order.push_back(d);
    std::stable_partition(order.begin(), order.end(), [&](int d) {
        return (!peer.serial.empty() && loadDeviceSerial(d) == peer.serial) ||
               (!peer.wifiAddress.empty() && loadDeviceAddress(d) == peer.wifiAddress);
    });

    bool skippedActive = false;
    for (int d : order) {
        // La bonne clé d'un appareil en session couperait sa session : on ne l'essaie pas.
        if (deviceInSession(d, sessions)) {
            skippedActive = true;
            continue;
        }
        auto psk = loadPsk(d);
        auto ch = std::make_unique<SecureChannel>();
        if (!ch->connectTo(peer.host, peer.port, 3000)) {
            // Un appareil éteint ou application fermée : noté une fois, pas à chaque tour.
            static std::map<std::string, bool> reported;
            const std::string who = peer.link == Link::Usb ? peer.serial : peer.host;
            if (!reported[who]) logLine("Connexion à %s : %s\n", who.c_str(), ch->lastError().c_str());
            reported[who] = true;
            return {};
        }
        if (psk && ch->handshake(*psk)) {
            if (peer.link == Link::Usb) saveDeviceSerial(d, peer.serial);
            if (!peer.wifiAddress.empty()) saveDeviceAddress(d, peer.wifiAddress);
            return {std::move(ch), d};
        }
        const int status = ch->lastStatus();
        ch->shutdown();
        // Application fermée en USB : le tunnel d'adb accepte puis ferme sans réponse. Il faut
        // attendre l'application, pas proposer un appairage.
        if (status < 0) {
            static std::map<std::string, bool> reported;
            const std::string who = peer.link == Link::Usb ? peer.serial : peer.host;
            if (!reported[who]) logLine("Authentification %s : %s (application fermée ?)\n", who.c_str(), ch->lastError().c_str());
            reported[who] = true;
            setStatus(L"Ouvrez l'application sur l'appareil");
            return {};
        }
        if (status == proto::kStatusBadVersion) {
            trayAlert(L"Mettez à jour l'application 36Screenlink de l'appareil.");
            return {};
        }
        // Appareil sans clé : inutile d'essayer les suivantes. Sinon, c'était la clé d'un autre.
        if (status == proto::kStatusNotPaired) break;
        if (status != proto::kStatusOk) return {};
    }
    // Peut-être l'appareil en session joint par un autre chemin : pas d'appairage s'il est impossible
    // de les distinguer. Un numéro de série USB, lui, suffit à les distinguer.
    if (skippedActive && peer.link != Link::Usb) {
        const bool distinct = std::all_of(sessions.begin(), sessions.end(), [](const auto& s) {
            return !loadDeviceAddress(s->device).empty() || !loadDeviceSerial(s->device).empty();
        });
        if (!distinct) return {};
    }
    const int device = pairingSlotFor(peer, sessions);
    if (device < 0) return {};
    // Un appareil qui refuse (déjà appairé ailleurs) n'est sollicité qu'une fois toutes les 30 s.
    static std::map<std::string, std::chrono::steady_clock::time_point> lastPairing;
    const std::string who = peer.link == Link::Usb ? peer.serial : peer.host;
    const auto now = std::chrono::steady_clock::now();
    const auto last = lastPairing.find(who);
    if (last != lastPairing.end() && now - last->second < std::chrono::seconds(30)) return {};
    lastPairing[who] = now;
    SecureChannel ch;
    if (ch.connectTo(peer.host, peer.port, 3000)) tryPairing(ch, device, peer);
    return {};
}

// Télévisions : une application web ne peut pas attendre de connexion, c'est donc elle qui se
// connecte au PC, en WebSocket. Chaque connexion acceptée attend ici que la boucle principale
// l'authentifie.
struct Incoming {
    std::unique_ptr<SecureChannel> channel;
    std::string address;
    std::string keyId; // identifiant de la clé de la télévision, vide avant le premier appairage
};
std::mutex gIncomingMutex;
std::vector<Incoming> gIncoming;

// Identifiant public d'une clé : la télévision l'annonce pour que le PC choisisse la bonne clé
// sans en essayer plusieurs. Il ne permet pas de retrouver la clé.
std::string keyIdOf(const Key256& psk) {
    const Key256 h = hmacSha256(psk, "id", 2);
    static const char hex[] = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 8; ++i) {
        id += hex[h[i] >> 4];
        id += hex[h[i] & 15];
    }
    return id;
}

void handleTvConnection(SOCKET client, const std::string& address) {
    DWORD timeout = 3000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    std::string request;
    char buf[1024];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 4096) {
        const int got = recv(client, buf, sizeof(buf), 0);
        if (got <= 0) break;
        request.append(buf, static_cast<size_t>(got));
    }
    const auto lineEnd = request.find("\r\n");
    const std::string line = request.substr(0, lineEnd);
    const auto header = [&](const std::string& name) {
        std::string lower = request;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        const auto at = lower.find("\r\n" + name + ":");
        if (at == std::string::npos) return std::string();
        const auto start = request.find_first_not_of(' ', at + name.size() + 3);
        return request.substr(start, request.find("\r\n", start) - start);
    };
    // Recherche du PC par la télévision : une réponse brève, sans rien révéler d'autre.
    if (line.rfind("GET /36screenlink/ping ", 0) == 0) {
        static const char reply[] = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: text/plain\r\nContent-Length: 12\r\nConnection: close\r\n\r\n36Screenlink";
        send(client, reply, static_cast<int>(sizeof(reply) - 1), 0);
        closesocket(client);
        return;
    }
    const std::string key = header("sec-websocket-key");
    if (line.rfind("GET /36screenlink/ws", 0) != 0 || key.empty() || key.size() > 64) {
        closesocket(client);
        return;
    }
    std::string keyId;
    const auto idAt = line.find("id=");
    if (idAt != std::string::npos) {
        for (size_t i = idAt + 3; i < line.size() && keyId.size() < 32 && std::isalnum(static_cast<unsigned char>(line[i])); ++i) keyId += line[i];
    }
    auto channel = std::make_unique<SecureChannel>();
    if (!channel->adoptWebSocket(client, key)) return;
    std::lock_guard lock(gIncomingMutex);
    if (gIncoming.size() >= 4) return;
    gIncoming.push_back({std::move(channel), address, keyId});
}

// La télévision se connecte au PC (une page web ne peut pas écouter) : le pare-feu doit laisser
// entrer le port de la télévision. Règle posée au démarrage, sur les réseaux privés et de domaine
// seulement, jamais public : le flux ne concerne que le réseau local de l'utilisateur.
void ensureTvFirewallRule() {
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return;
    const std::wstring netsh = std::wstring(sys) + L"\\netsh.exe";
    const std::wstring name = L"36Screenlink (télévision)";
    auto run = [&](const std::wstring& args) {
        std::wstring cmd = L"\"" + netsh + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 8000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    };
    // Idempotent : on retire l'éventuelle règle existante (même nom), puis on pose la bonne.
    run(L"advfirewall firewall delete rule name=\"" + name + L"\"");
    run(L"advfirewall firewall add rule name=\"" + name + L"\" dir=in action=allow protocol=TCP localport=" +
        std::to_wstring(proto::kTvPort) + L" profile=private,domain");
}

void tvListener() {
    ensureTvFirewallRule();
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(proto::kTvPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (server == INVALID_SOCKET || bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(server, 4) != 0) {
        logLine("Télévisions : port %u indisponible\n", proto::kTvPort);
        if (server != INVALID_SOCKET) closesocket(server);
        return;
    }
    while (!gStop) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(server, &ready);
        timeval tv{0, 500000};
        if (select(0, &ready, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in from{};
        int fromLen = sizeof(from);
        SOCKET client = accept(server, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (client == INVALID_SOCKET) continue;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        handleTvConnection(client, text);
    }
    closesocket(server);
}

void serviceLoop(Options opt) {
    std::vector<std::unique_ptr<Session>> sessions;
    std::map<std::string, uint16_t> usbPorts;
    std::chrono::steady_clock::time_point slotEnd[VirtualMonitor::kSlots]{};
    auto lastEnd = std::chrono::steady_clock::time_point{};
    int lastSlot[kMaxDevices];
    std::fill(std::begin(lastSlot), std::end(lastSlot), -1);
    bool stopAll = false;
    std::thread televisions(tvListener);

    const auto slotUsedBy = [&](int slot) {
        return std::any_of(sessions.begin(), sessions.end(), [slot](const auto& s) { return s->slot == slot; });
    };
    const auto startSession = [&](std::unique_ptr<SecureChannel> channel, int device, const Peer& peer) {
        auto session = std::make_unique<Session>();
        session->device = device;
        session->peer = peer;
        session->channel = std::move(channel);
        // Le même moniteur qu'à la dernière session : Windows a gardé sa place.
        int slot = lastSlot[device];
        if (slot < 0 || slotUsedBy(slot)) {
            for (slot = 0; slot < VirtualMonitor::kSlots && slotUsedBy(slot); ++slot) {}
        }
        session->slot = lastSlot[device] = slot;
        Session* raw = session.get();
        raw->thread = std::thread([&opt, raw] {
            tSlot = raw->slot;
            tLogTag = "[Appareil " + std::to_string(raw->device + 1) + "] ";
            raw->stopAll = !runSession(opt, *raw);
            raw->finished = true;
        });
        sessions.push_back(std::move(session));
    };

    while (!gStop && !stopAll) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!(*it)->finished) {
                ++it;
                continue;
            }
            (*it)->thread.join();
            stopAll |= (*it)->stopAll;
            slotEnd[(*it)->slot] = lastEnd = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(gStatusMutex);
                gSlotStatus[(*it)->slot].clear();
            }
            it = sessions.erase(it);
        }
        if (stopAll) break;
        const auto slotUsed = [&](int slot) {
            return std::any_of(sessions.begin(), sessions.end(), [slot](const auto& s) { return s->slot == slot; });
        };
        // Un moniteur sans appareil depuis un moment est retiré : Windows récupère les fenêtres.
        for (int slot = 0; slot < VirtualMonitor::kSlots; ++slot) {
            if (!slotUsed(slot) && gMonitor.raised(slot) && now - slotEnd[slot] > kMonitorGrace) gMonitor.release(slot);
        }
        if (sessions.empty()) {
            setStatus(L"Recherche des appareils");
            if (now - lastEnd > kMonitorGrace) {
                // Jamais d'écran du PC éteint sans appareil connecté.
                std::lock_guard lock(gDisplayMutex);
                restoreDisplays();
                for (auto& settings : gSettings) settings.displayMode = proto::kDisplayExtended;
            }
        }

        if (sessions.size() < VirtualMonitor::kSlots) {
            for (const Peer& peer : findPeers(opt, usbPorts)) {
                if (gStop || sessions.size() >= VirtualMonitor::kSlots) break;
                if (peerInSession(peer, sessions)) continue;
                // Un appareil a l'écran pour lui seul : les autres attendent.
                if (gExclusiveSlot >= 0) break;
                auto [channel, device] = identify(peer, sessions);
                if (!channel) continue;
                if (deviceInSession(device, sessions)) {
                    channel->shutdown();
                    continue;
                }
                startSession(std::move(channel), device, peer);
            }
        }
        // Télévisions connectées au PC depuis le dernier tour.
        std::vector<Incoming> incoming;
        {
            std::lock_guard lock(gIncomingMutex);
            incoming.swap(gIncoming);
        }
        for (auto& in : incoming) {
            Peer peer;
            peer.link = Link::Tv;
            peer.host = peer.wifiAddress = in.address;
            peer.serial = "tv-" + in.address;
            if (sessions.size() >= VirtualMonitor::kSlots || gExclusiveSlot >= 0 || peerInSession(peer, sessions)) {
                in.channel->shutdown();
                continue;
            }
            int device = -1;
            std::optional<Key256> psk;
            for (int d = 0; d < kMaxDevices && device < 0 && !in.keyId.empty(); ++d) {
                psk = loadPsk(d);
                if (psk && keyIdOf(*psk) == in.keyId) device = d;
            }
            if (device >= 0 && deviceInSession(device, sessions)) {
                in.channel->shutdown();
            } else if (device >= 0) {
                if (!in.channel->handshake(*psk)) {
                    logLine("Télévision %s : %s\n", in.address.c_str(), in.channel->lastError().c_str());
                    continue;
                }
                saveDeviceAddress(device, in.address);
                saveDeviceSerial(device, peer.serial);
                startSession(std::move(in.channel), device, peer);
            } else {
                // Télévision inconnue : premier appairage, le code s'affiche sur le PC et sur la TV.
                const int slotForKey = pairingSlotFor(peer, sessions);
                if (slotForKey >= 0) tryPairing(*in.channel, slotForKey, peer);
            }
        }
        for (int i = 0; i < 10 && !gStop; ++i) Sleep(100);
    }
    gStop = true;
    for (auto& s : sessions) s->thread.join();
    televisions.join();
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
        logLine("36Screenlink tourne déjà.\n");
        if (opt.tray) MessageBoxW(nullptr, L"36Screenlink tourne déjà.", L"36Screenlink", MB_ICONINFORMATION);
        if (instance) CloseHandle(instance);
        WSACleanup();
        return 0;
    }

    gSystemMode = opt.system;
    for (auto& settings : gSettings) settings.fps = opt.fps;

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
        tray.status = trayStatus;
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
