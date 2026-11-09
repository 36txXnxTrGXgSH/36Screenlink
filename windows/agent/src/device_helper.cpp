#include "device_helper.h"

// WIN32_LEAN_AND_MEAN retire COM de windows.h : UI Automation en a besoin.
#include <ole2.h>
#include <UIAutomation.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include "channel.h"
#include "desktop.h"
#include "protocol.h"

namespace {

constexpr DWORD kTickMs = 100;
constexpr int kScreensEveryTicks = 10; // une fois par seconde : la disposition change rarement
constexpr int kFocusEveryTicks = 2;    // cinq fois par seconde : le curseur suit la frappe
constexpr int kMaxScreens = 8;
constexpr int kFocusMovePx = 4;

uint16_t norm(LONG v, LONG origin, LONG size) {
    if (size <= 0) return 0;
    const double t = std::clamp(double(v - origin) / size, 0.0, 1.0);
    return static_cast<uint16_t>(t * 65535.0 + 0.5);
}

// Rectangle ramené à l'écran de l'appareil, en 0..65535 sur chaque axe.
void putNormalized(proto::Writer& w, const RECT& r, const RECT& screen) {
    const LONG sw = screen.right - screen.left, sh = screen.bottom - screen.top;
    const uint16_t x0 = norm(r.left, screen.left, sw), y0 = norm(r.top, screen.top, sh);
    const uint16_t x1 = norm(r.right, screen.left, sw), y1 = norm(r.bottom, screen.top, sh);
    w.u16(x0).u16(y0).u16(static_cast<uint16_t>(x1 - x0)).u16(static_cast<uint16_t>(y1 - y0));
}

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) {
        auto* list = reinterpret_cast<std::vector<std::pair<RECT, bool>>*>(param);
        list->push_back({info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    }
    return TRUE;
}

struct TaskbarSearch {
    RECT screen;
    RECT found;
    bool ok;
};

BOOL CALLBACK findTaskbar(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<TaskbarSearch*>(param);
    wchar_t name[32] = {};
    GetClassNameW(hwnd, name, 32);
    if (wcscmp(name, L"Shell_TrayWnd") != 0 && wcscmp(name, L"Shell_SecondaryTrayWnd") != 0) return TRUE;
    RECT r{}, inter{};
    if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r) || !IntersectRect(&inter, &r, &search->screen)) return TRUE;
    // Barre masquée automatiquement : il n'en reste qu'un liseré, inutilisable au doigt.
    if (inter.right - inter.left < 8 || inter.bottom - inter.top < 8) return TRUE;
    search->found = inter;
    search->ok = true;
    return FALSE;
}

bool isReadOnly(IUIAutomationElement* e) {
    VARIANT v;
    VariantInit(&v);
    const bool ro = SUCCEEDED(e->GetCurrentPropertyValue(UIA_ValueIsReadOnlyPropertyId, &v)) && v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE;
    VariantClear(&v);
    return ro;
}

bool hasValuePattern(IUIAutomationElement* e) {
    VARIANT v;
    VariantInit(&v);
    const bool has = SUCCEEDED(e->GetCurrentPropertyValue(UIA_IsValuePatternAvailablePropertyId, &v)) && v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE;
    VariantClear(&v);
    return has;
}

// Curseur de saisie classique de Windows, s'il y en a un : plus précis que le champ entier.
bool systemCaret(RECT& out) {
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(0, &info) || !info.hwndCaret || IsRectEmpty(&info.rcCaret)) return false;
    out = info.rcCaret;
    MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT*>(&out), 2);
    return true;
}

} // namespace

DeviceHelper::~DeviceHelper() {
    stop();
}

void DeviceHelper::start(SecureChannel& ch, const RECT& device, bool windowMoves) {
    ch_ = &ch;
    device_ = device;
    windowMoves_ = windowMoves;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void DeviceHelper::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::optional<RECT> DeviceHelper::screen(uint8_t index) {
    std::lock_guard<std::mutex> lock(screensMutex_);
    if (index >= screens_.size()) return std::nullopt;
    return screens_[index];
}

void DeviceHelper::run() {
    if (gSecureMode) attachInputDesktop();
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IUIAutomation* automation = nullptr;
    uint8_t lastFlags = 0;
    for (int tick = 0; running_; ++tick, Sleep(kTickMs)) {
        if (tick % kScreensEveryTicks == 0) sendScreens(tick == 0);
        const uint8_t flags = keyboardFlags_;
        if (flags != 0 && tick % kFocusEveryTicks == 0) {
            if (!automation && com) {
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                 reinterpret_cast<void**>(&automation));
                // Une application figée ne doit pas bloquer ce suivi plus d'une demi-seconde.
                IUIAutomation2* a2 = nullptr;
                if (automation && SUCCEEDED(automation->QueryInterface(__uuidof(IUIAutomation2), reinterpret_cast<void**>(&a2)))) {
                    a2->put_ConnectionTimeout(500);
                    a2->put_TransactionTimeout(500);
                    a2->Release();
                }
            }
            automation_ = automation;
            sendTextFocus(flags != lastFlags);
        }
        lastFlags = flags;
    }
    if (automation) automation->Release();
    automation_ = nullptr;
    if (com) CoUninitialize();
}

void DeviceHelper::sendScreens(bool force) {
    std::vector<std::pair<RECT, bool>> monitors;
    if (windowMoves_) EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors));
    // Écran principal d'abord, puis de gauche à droite : la numérotation reste stable.
    std::stable_sort(monitors.begin(), monitors.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second;
        return a.first.left != b.first.left ? a.first.left < b.first.left : a.first.top < b.first.top;
    });
    if (monitors.size() > kMaxScreens) monitors.resize(kMaxScreens);

    uint8_t deviceIndex = 0xFF;
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (EqualRect(&monitors[i].first, &device_)) deviceIndex = static_cast<uint8_t>(i);
    }
    TaskbarSearch search{device_, {}, false};
    if (windowMoves_) EnumWindows(findTaskbar, reinterpret_cast<LPARAM>(&search));
    // Sans barre des tâches ni second écran sur l'appareil, le geste n'a pas lieu d'être.
    if (deviceIndex == 0xFF || monitors.size() < 2 || !search.ok) monitors.clear();

    proto::Writer w(proto::kScreens);
    w.u8(static_cast<uint8_t>(monitors.size())).u8(monitors.empty() ? 0 : deviceIndex);
    for (const auto& [r, primary] : monitors) {
        w.u16(static_cast<uint16_t>(static_cast<int16_t>(r.left))).u16(static_cast<uint16_t>(static_cast<int16_t>(r.top)))
            .u16(static_cast<uint16_t>(r.right - r.left)).u16(static_cast<uint16_t>(r.bottom - r.top));
    }
    if (!monitors.empty()) putNormalized(w, search.found, device_);
    else w.u16(0).u16(0).u16(0).u16(0);

    if (!force && w.data() == lastScreens_) return;
    {
        std::lock_guard<std::mutex> lock(screensMutex_);
        screens_.clear();
        for (const auto& m : monitors) screens_.push_back(m.first);
    }
    lastScreens_ = w.data();
    ch_->send(w.data());
}

void DeviceHelper::sendTextFocus(bool force) {
    auto* automation = static_cast<IUIAutomation*>(automation_);
    bool editable = false;
    RECT area{};
    IUIAutomationElement* element = nullptr;
    if (automation && SUCCEEDED(automation->GetFocusedElement(&element)) && element) {
        CONTROLTYPEID type = 0;
        element->get_CurrentControlType(&type);
        RECT caret{};
        const bool hasCaret = systemCaret(caret);
        if (type == UIA_EditControlTypeId) editable = !isReadOnly(element);
        // Une page web est aussi un « document » : seul un document modifiable compte.
        else if (type == UIA_DocumentControlTypeId) editable = hasValuePattern(element) ? !isReadOnly(element) : hasCaret;
        if (editable) {
            element->get_CurrentBoundingRectangle(&area);
            if (hasCaret) area = caret;
        }
        element->Release();
    }
    if (editable) {
        const POINT center{(area.left + area.right) / 2, (area.top + area.bottom) / 2};
        editable = !IsRectEmpty(&area) && PtInRect(&device_, center);
    }

    proto::Writer w(proto::kTextFocus);
    w.u8(editable ? 1 : 0);
    if (editable) putNormalized(w, area, device_);
    else w.u16(0).u16(0).u16(0).u16(0);

    // Un curseur qui bouge de quelques pixels ne change rien pour l'appareil.
    if (!force && lastFocus_.size() == w.data().size()) {
        bool same = lastFocus_[1] == w.data()[1];
        const LONG tol = (device_.bottom - device_.top) > 0 ? 65535 * kFocusMovePx / (device_.bottom - device_.top) : 0;
        for (size_t i = 2; same && i + 1 < lastFocus_.size(); i += 2) {
            const int a = (lastFocus_[i] << 8) | lastFocus_[i + 1], b = (w.data()[i] << 8) | w.data()[i + 1];
            same = std::abs(a - b) <= tol;
        }
        if (same) return;
    }
    lastFocus_ = w.data();
    ch_->send(w.data());
}
