#include "tray.h"

#include <windows.h>
#include <shellapi.h>

#include <mutex>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStatusTimer = 1;
constexpr UINT kCmdQuit = 1;
constexpr UINT kCmdLog = 2;
constexpr UINT kCmdAudioPc = 3;
constexpr UINT kCmdAudioTablet = 4;
constexpr UINT kCmdRemoveDriver = 5;

TrayConfig gConfig;
NOTIFYICONDATAW gIcon{};
bool gIconAdded = false;
UINT gTaskbarCreated = 0;
HWND gWindow = nullptr;
std::mutex gAlertMutex;
std::wstring gLastAlert;

// Le service démarre avant l'ouverture de session : la barre des tâches peut ne pas encore exister.
void ensureIcon() {
    if (gIconAdded) return;
    gIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gIconAdded = Shell_NotifyIconW(NIM_ADD, &gIcon) != FALSE;
}

// Retire le pilote : l'assistant s'arrête avec l'agent, donc le script attend sa fin.
void removeDriverAndQuit(HWND hwnd) {
    const int answer = MessageBoxW(hwnd, L"Retirer le pilote de ce PC et quitter 36Screenlink ?\n\nIl sera réinstallé au prochain lancement.",
                                   L"36Screenlink", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    if (answer != IDYES) return;
    const std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \"Start-Sleep 3; & '" +
                              gConfig.portableDirectory + L"\\driver.ps1' -Action Remove\"";
    ShellExecuteW(nullptr, L"runas", L"powershell.exe", args.c_str(), nullptr, SW_HIDE);
    DestroyWindow(hwnd);
}

void refreshTip() {
    ensureIcon();
    wcsncpy_s(gIcon.szTip, gConfig.status().c_str(), _TRUNCATE);
    gIcon.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &gIcon);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kTrayMessage:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            const bool onTablet = gConfig.audioOnTablet();
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, gConfig.status().c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | (onTablet ? 0 : MF_CHECKED), kCmdAudioPc, L"Son sur le PC");
            AppendMenuW(menu, MF_STRING | (onTablet ? MF_CHECKED : 0), kCmdAudioTablet, L"Son sur la tablette");
            if (!gConfig.managedByService) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kCmdLog, L"Ouvrir le journal");
                if (!gConfig.portableDirectory.empty()) AppendMenuW(menu, MF_STRING, kCmdRemoveDriver, L"Retirer le pilote et quitter");
                AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quitter");
            }
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == kCmdQuit) DestroyWindow(hwnd);
            if (cmd == kCmdLog) ShellExecuteW(nullptr, L"open", gConfig.logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (cmd == kCmdRemoveDriver) removeDriverAndQuit(hwnd);
            if (cmd == kCmdAudioPc) gConfig.setAudioOnTablet(false);
            if (cmd == kCmdAudioTablet) gConfig.setAudioOnTablet(true);
        }
        return 0;
    case WM_TIMER:
        refreshTip();
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &gIcon);
        gWindow = nullptr;
        gConfig.onQuit();
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == gTaskbarCreated && gTaskbarCreated != 0) {
            gIconAdded = false; // Explorateur relancé : l'icône est à recréer
            ensureIcon();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

void runTray(const TrayConfig& config) {
    gConfig = config;
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ScreenlinkTray";
    RegisterClassW(&wc);
    // Fenêtre invisible et non « message-only » : seule une fenêtre de premier niveau reçoit TaskbarCreated.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    ChangeWindowMessageFilterEx(hwnd, gTaskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, kTrayMessage, MSGFLT_ALLOW, nullptr);

    gIcon.cbSize = sizeof(gIcon);
    gIcon.hWnd = hwnd;
    gIcon.uID = 1;
    gIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gIcon.uCallbackMessage = kTrayMessage;
    gIcon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wcscpy_s(gIcon.szTip, L"36Screenlink");
    ensureIcon();
    gWindow = hwnd;
    SetTimer(hwnd, kStatusTimer, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
}

void trayClose() {
    if (gWindow) PostMessageW(gWindow, WM_CLOSE, 0, 0);
}

void trayAlert(const std::wstring& message) {
    std::lock_guard lock(gAlertMutex);
    if (message == gLastAlert || !gWindow) return;
    gLastAlert = message;
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = gIcon.hWnd;
    n.uID = gIcon.uID;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_WARNING;
    wcscpy_s(n.szInfoTitle, L"36Screenlink");
    wcsncpy_s(n.szInfo, message.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void trayClearAlert() {
    std::lock_guard lock(gAlertMutex);
    gLastAlert.clear();
}
