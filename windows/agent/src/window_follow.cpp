#include "window_follow.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include "desktop.h"

namespace {

constexpr uint64_t kTapMaxMs = 600;     // au-delà, c'est un glissement
constexpr uint64_t kAfterTapMs = 1500;  // délai laissé à Windows pour activer la fenêtre
constexpr DWORD kPollMs = 30;
// Assez long pour choisir une miniature quand l'application a plusieurs fenêtres.
constexpr uint64_t kSendWindowMs = 8000;

bool isShellWindow(HWND hwnd) {
    wchar_t name[64] = {};
    GetClassNameW(hwnd, name, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"TaskListThumbnailWnd",
                                 L"XamlExplorerHostIslandWindow", L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow",
                                 L"TopLevelWindowForOverflowXamlIsland"}) {
        if (wcscmp(name, shell) == 0) return true;
    }
    return false;
}

// Vraie fenêtre d'application : ni élément de Windows, ni boîte de dialogue qui suit sa fenêtre.
bool isMovable(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || isShellWindow(hwnd)) return false;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked == 0;
}

bool moveToScreen(HWND hwnd, HMONITOR tablet) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(tablet, &info)) return false;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    const bool maximized = IsZoomed(hwnd) != FALSE;
    if (maximized) ShowWindow(hwnd, SW_RESTORE);

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    const RECT& work = info.rcWork;
    const int workW = work.right - work.left, workH = work.bottom - work.top;
    const int w = std::min<int>(r.right - r.left, workW), h = std::min<int>(r.bottom - r.top, workH);
    const BOOL moved = SetWindowPos(hwnd, nullptr, work.left + (workW - w) / 2, work.top + (workH - h) / 2, w, h,
                                    SWP_NOZORDER | SWP_NOACTIVATE);
    if (maximized) ShowWindow(hwnd, SW_MAXIMIZE);
    return moved != FALSE;
}

} // namespace

WindowFollower::~WindowFollower() {
    stop();
}

void WindowFollower::start(const RECT& tablet) {
    tablet_ = tablet;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void WindowFollower::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void WindowFollower::onContact(bool down) {
    const uint64_t now = GetTickCount64();
    if (down && !held_) downMs_ = now;
    if (!down && held_ && now - downMs_ < kTapMaxMs) tapMs_ = now;
    held_ = down;
}

void WindowFollower::sendTo(const RECT& screen) {
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        sendScreen_ = screen;
    }
    tapMs_ = GetTickCount64();
    sendUntilMs_ = GetTickCount64() + kSendWindowMs;
}

void WindowFollower::run() {
    if (gSecureMode) attachInputDesktop();
    const HMONITOR tablet = MonitorFromRect(&tablet_, MONITOR_DEFAULTTONEAREST);
    HWND previous = nullptr;

    for (; running_; Sleep(kPollMs)) {
        const HWND foreground = GetForegroundWindow();
        if (held_ || !foreground) continue;
        const uint64_t now = GetTickCount64();
        const bool sending = now < sendUntilMs_;
        if (!sending && now - tapMs_ > kAfterTapMs) {
            previous = foreground;
            continue;
        }
        HMONITOR destination = tablet;
        if (sending) {
            std::lock_guard<std::mutex> lock(sendMutex_);
            destination = MonitorFromRect(&sendScreen_, MONITOR_DEFAULTTONEAREST);
        }
        const auto away = [destination](HWND hwnd) { return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != destination; };
        // Barre des tâches touchée alors que la fenêtre était déjà active : Windows la réduit.
        // L'utilisateur voulait la voir, on la rouvre à l'endroit voulu. Envoyée exprès sur un
        // écran, elle est rouverte même si elle y était déjà.
        HWND target = nullptr;
        if (previous && IsIconic(previous) && isMovable(previous) && (sending || away(previous))) target = previous;
        else if (foreground != previous && isMovable(foreground) && (sending || away(foreground))) target = foreground;
        if (!target) continue;

        bool moved = true;
        if (away(target)) moved = moveToScreen(target, destination);
        else if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
        if (target != GetForegroundWindow()) SetForegroundWindow(target);
        std::printf(sending ? "Fenêtre envoyée sur l'écran choisi : %s\n" : "Fenêtre amenée sur la tablette : %s\n", moved ? "oui" : "non");
        previous = target;
        sendUntilMs_ = 0;
    }
}
