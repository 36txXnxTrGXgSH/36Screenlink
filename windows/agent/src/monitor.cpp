#include "monitor.h"

#include <shellapi.h>

#include <cstdio>
#include <filesystem>

#include "capture.h"
#include "store.h"
#include "tray.h"

namespace {

bool hasSecondaryOutput() {
    for (const auto& o : enumerateOutputs())
        if (!o.primary) return true;
    return false;
}

} // namespace

VirtualMonitor::~VirtualMonitor() {
    if (helper_) CloseHandle(helper_);
    if (raiseEvent_) CloseHandle(raiseEvent_);
    if (releaseEvent_) CloseHandle(releaseEvent_);
}

void VirtualMonitor::start() {
    if (hasSecondaryOutput()) return;

    const std::filesystem::path exe = std::filesystem::path(exeDirectory()) / L"36ScreenlinkDevice.exe";
    if (!std::filesystem::exists(exe)) {
        std::printf("36ScreenlinkDevice.exe absent : miroir de l'écran principal.\n");
        return;
    }
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    raiseEvent_ = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\ScreenlinkRaise-" + pid).c_str());
    releaseEvent_ = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\ScreenlinkRelease-" + pid).c_str());

    const std::wstring params = L"--parent " + pid;
    if (gSystemMode) {
        // Déjà élevé : pas d'invite UAC.
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + params;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::printf("Lancement de l'assistant impossible : %lu\n", GetLastError());
            return;
        }
        CloseHandle(pi.hThread);
        helper_ = pi.hProcess;
        managed_ = true;
        return;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        std::printf("Élévation refusée : miroir de l'écran principal.\n");
        return;
    }
    helper_ = info.hProcess;
    managed_ = true;
}

void VirtualMonitor::raise() {
    if (!managed_ || raised_) return;
    SetEvent(raiseEvent_);
    raised_ = true;
    for (int i = 0; i < 100 && !hasSecondaryOutput(); ++i) Sleep(100);
    if (!hasSecondaryOutput()) {
        std::printf("Le moniteur virtuel n'est pas apparu : pilote installé ?\n");
        trayAlert(L"Le moniteur virtuel n'est pas apparu. Vérifiez que le pilote est installé.");
    }
}

void VirtualMonitor::release() {
    if (!managed_ || !raised_) return;
    SetEvent(releaseEvent_);
    raised_ = false;
}
