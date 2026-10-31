#include "store.h"

#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path().wstring();
}

namespace {

fs::path dataDir() {
    if (gSystemMode) {
        PWSTR programData = nullptr;
        SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData);
        fs::path dir = fs::path(programData) / L"36Screenlink";
        CoTaskMemFree(programData);
        return dir;
    }
    const fs::path exe = exeDirectory();
    if (fs::exists(exe / L"portable.txt")) return exe / L"data";
    PWSTR appData = nullptr;
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);
    fs::path dir = fs::path(appData) / L"36Screenlink";
    CoTaskMemFree(appData);
    return dir;
}

} // namespace

std::wstring logFilePath() {
    // Sous-dossier distinct : en mode service, seul le journal est lisible par l'utilisateur.
    const fs::path dir = gSystemMode ? dataDir() / L"logs" : dataDir();
    fs::create_directories(dir);
    return (dir / L"agent.log").wstring();
}

std::optional<Key256> loadPsk() {
    std::ifstream in(dataDir() / L"pairing.bin", std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<char> blob((std::istreambuf_iterator<char>(in)), {});
    DATA_BLOB enc{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())}, plain{};
    if (!CryptUnprotectData(&enc, nullptr, nullptr, nullptr, nullptr, 0, &plain)) return std::nullopt;
    std::optional<Key256> key;
    if (plain.cbData == 32) {
        key.emplace();
        std::copy_n(plain.pbData, 32, key->begin());
    }
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return key;
}

bool savePsk(const Key256& key) {
    const fs::path dir = dataDir();
    fs::create_directories(dir);
    DATA_BLOB plain{32, const_cast<BYTE*>(key.data())}, enc{};
    // Le compte SYSTEM n'a pas de session utilisateur : la clé est liée à la machine.
    const DWORD flags = gSystemMode ? CRYPTPROTECT_LOCAL_MACHINE : 0;
    if (!CryptProtectData(&plain, L"36Screenlink", nullptr, nullptr, nullptr, flags, &enc)) return false;
    std::ofstream out(dir / L"pairing.bin", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<char*>(enc.pbData), enc.cbData);
    LocalFree(enc.pbData);
    return out.good();
}

std::string loadTabletAddress() {
    std::ifstream in(dataDir() / L"tablet.txt");
    std::string address;
    in >> address;
    return address;
}

void saveTabletAddress(const std::string& address) {
    const fs::path dir = dataDir();
    fs::create_directories(dir);
    std::ofstream(dir / L"tablet.txt", std::ios::trunc) << address;
}
