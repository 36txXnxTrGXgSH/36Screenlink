#include "monitor.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "capture.h"
#include "store.h"
#include "tray.h"

namespace {

std::vector<std::wstring> virtualOutputs() {
    std::vector<std::wstring> names;
    for (const auto& o : enumerateOutputs())
        if (o.isVirtual) names.push_back(o.deviceName);
    return names;
}

bool contains(const std::vector<std::wstring>& list, const std::wstring& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

} // namespace

VirtualMonitor::~VirtualMonitor() {
    if (helper_) CloseHandle(helper_);
    for (int i = 0; i < kSlots; ++i) {
        if (raiseEvents_[i]) CloseHandle(raiseEvents_[i]);
        if (releaseEvents_[i]) CloseHandle(releaseEvents_[i]);
    }
}

void VirtualMonitor::start() {
    const std::filesystem::path exe = std::filesystem::path(exeDirectory()) / L"36ScreenlinkDevice.exe";
    if (!std::filesystem::exists(exe)) {
        std::printf("36ScreenlinkDevice.exe absent : miroir de l'écran principal.\n");
        return;
    }
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    for (int i = 0; i < kSlots; ++i) {
        const std::wstring suffix = pid + L"-" + std::to_wstring(i);
        raiseEvents_[i] = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\ScreenlinkRaise-" + suffix).c_str());
        releaseEvents_[i] = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\ScreenlinkRelease-" + suffix).c_str());
    }

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

std::wstring VirtualMonitor::raise(int slot) {
    // Un moniteur à la fois : celui qui apparaît est forcément celui qu'on vient de demander.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!managed_) return {};
    auto present = virtualOutputs();
    if (raised_[slot] && contains(present, names_[slot])) return names_[slot];

    std::vector<std::wstring> before = present;
    SetEvent(raiseEvents_[slot]);
    raised_[slot] = true;
    for (int i = 0; i < 100; ++i) {
        present = virtualOutputs();
        for (const auto& name : present) {
            // Le nouveau venu, ou à défaut celui que l'autre emplacement n'occupe pas.
            bool otherOwns = false;
            for (int other = 0; other < kSlots; ++other)
                otherOwns |= other != slot && raised_[other] && name == names_[other];
            if (!otherOwns && (!contains(before, name) || i > 20)) {
                names_[slot] = name;
                return name;
            }
        }
        Sleep(100);
    }
    std::printf("Le moniteur virtuel n'est pas apparu : pilote installé ?\n");
    trayAlert(L"Le moniteur virtuel n'est pas apparu. Vérifiez que le pilote est installé.");
    return {};
}

void VirtualMonitor::release(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!managed_ || !raised_[slot]) return;
    SetEvent(releaseEvents_[slot]);
    raised_[slot] = false;
    names_[slot].clear();
}
