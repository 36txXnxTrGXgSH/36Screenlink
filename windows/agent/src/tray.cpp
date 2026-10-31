#include "tray.h"

#include <windows.h>
#include <shellapi.h>

#include <mutex>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStatusTimer = 1;
constexpr UINT kCmdQuit = 1;
constexpr UINT kCmdLog = 2;

std::function<std::wstring()> gStatus;
std::function<void()> gQuit;
std::wstring gLogPath;
NOTIFYICONDATAW gIcon{};
HWND gWindow = nullptr;
std::mutex gAlertMutex;
std::wstring gLastAlert;

void refreshTip() {
    wcsncpy_s(gIcon.szTip, gStatus().c_str(), _TRUNCATE);
    gIcon.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &gIcon);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kTrayMessage:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, gStatus().c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCmdLog, L"Ouvrir le journal");
            AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quitter");
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == kCmdQuit) DestroyWindow(hwnd);
            if (cmd == kCmdLog) ShellExecuteW(nullptr, L"open", gLogPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    case WM_TIMER:
        refreshTip();
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &gIcon);
        gWindow = nullptr;
        gQuit();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

void runTray(const std::function<std::wstring()>& status, const std::function<void()>& onQuit, const std::wstring& logPath) {
    gStatus = status;
    gQuit = onQuit;
    gLogPath = logPath;

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ScreenlinkTray";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    gIcon.cbSize = sizeof(gIcon);
    gIcon.hWnd = hwnd;
    gIcon.uID = 1;
    gIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gIcon.uCallbackMessage = kTrayMessage;
    gIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(gIcon.szTip, L"36Screenlink");
    Shell_NotifyIconW(NIM_ADD, &gIcon);
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
