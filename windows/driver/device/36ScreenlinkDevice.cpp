// Crée le périphérique logiciel du pilote et le garde ouvert. À la fermeture de ce
// programme, même après un plantage, PnP retire le périphérique donc le moniteur.
// SwDeviceCreate exige des droits Administrateur.
//
// Sans argument : moniteur actif jusqu'à Ctrl+C.
// --parent <pid> : piloté par l'agent via deux événements, arrêt à la fin de l'agent.
#include <windows.h>
#include <swdevice.h>

#include <cstdio>
#include <cwchar>
#include <string>

#pragma comment(lib, "swdevice.lib")
#pragma comment(lib, "onecore.lib")

namespace {

HANDLE gCreated = nullptr;
HANDLE gStop = nullptr;
HRESULT gResult = E_PENDING;

VOID WINAPI onCreated(HSWDEVICE, HRESULT result, PVOID, PCWSTR) {
    gResult = result;
    SetEvent(gCreated);
}

HSWDEVICE createDevice() {
    SW_DEVICE_CREATE_INFO info = {};
    info.cbSize = sizeof(info);
    info.pszInstanceId = L"36ScreenlinkIdd";
    info.pszzHardwareIds = L"36ScreenlinkIdd\0\0";
    info.pszzCompatibleIds = L"36ScreenlinkIdd\0\0";
    info.pszDeviceDescription = L"36Screenlink Display";
    info.CapabilityFlags = SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;

    ResetEvent(gCreated);
    gResult = E_PENDING;
    HSWDEVICE device = nullptr;
    HRESULT hr = SwDeviceCreate(L"36ScreenlinkIdd", L"HTREE\\ROOT\\0", &info, 0, nullptr, onCreated, nullptr, &device);
    if (FAILED(hr)) {
        wprintf(L"SwDeviceCreate : 0x%08lx\n", static_cast<unsigned long>(hr));
        return nullptr;
    }
    if (WaitForSingleObject(gCreated, 15000) != WAIT_OBJECT_0 || FAILED(gResult)) {
        wprintf(L"Création du périphérique : 0x%08lx\n", static_cast<unsigned long>(gResult));
        SwDeviceClose(device);
        return nullptr;
    }
    return device;
}

int runStandalone() {
    gStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler([](DWORD) -> BOOL { SetEvent(gStop); return TRUE; }, TRUE);
    HSWDEVICE device = createDevice();
    if (!device) return 1;
    wprintf(L"Moniteur actif. Fermez cette fenêtre pour le retirer.\n");
    WaitForSingleObject(gStop, INFINITE);
    SwDeviceClose(device);
    return 0;
}

int runForParent(DWORD pid) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    const std::wstring id = std::to_wstring(pid);
    HANDLE raise = OpenEventW(SYNCHRONIZE, FALSE, (L"Local\\ScreenlinkRaise-" + id).c_str());
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, (L"Local\\ScreenlinkRelease-" + id).c_str());
    if (!parent || !raise || !release) return 1;

    HSWDEVICE device = nullptr;
    HANDLE handles[] = {parent, raise, release};
    for (;;) {
        const DWORD wait = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 1) {
            if (!device) device = createDevice();
        } else if (wait == WAIT_OBJECT_0 + 2) {
            if (device) SwDeviceClose(device);
            device = nullptr;
        } else {
            break;
        }
    }
    if (device) SwDeviceClose(device);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    gCreated = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (argc == 3 && wcscmp(argv[1], L"--parent") == 0) return runForParent(static_cast<DWORD>(_wtoi(argv[2])));
    return runStandalone();
}
