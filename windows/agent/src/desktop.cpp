#include "desktop.h"

#include <windows.h>
#include <wtsapi32.h>

bool gSystemMode = false;
bool gSecureMode = false;

std::wstring inputDesktopName() {
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!desk) return {};
    wchar_t name[64] = {};
    GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), nullptr);
    CloseDesktop(desk);
    return name;
}

namespace {

bool sessionLocked() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &buffer, &bytes)) return false;
    const auto* info = reinterpret_cast<WTSINFOEXW*>(buffer);
    const bool locked = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    WTSFreeMemory(buffer);
    return locked;
}

} // namespace

bool secureScreenActive() {
    const std::wstring desktop = inputDesktopName();
    return (!desktop.empty() && desktop != L"Default") || sessionLocked();
}

bool attachInputDesktop() {
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!desk) return false;
    const bool ok = SetThreadDesktop(desk) != FALSE;
    // En cas de succès, le thread garde ce handle jusqu'à sa fin.
    if (!ok) CloseDesktop(desk);
    return ok;
}
