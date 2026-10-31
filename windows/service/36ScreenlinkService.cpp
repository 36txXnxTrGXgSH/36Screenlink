// Service Windows : lance l'agent avec le compte SYSTEM dans la session de la console.
// Ce compte seul peut lire et piloter l'écran de verrouillage, l'invite UAC et les
// fenêtres élevées. Ce binaire et l'agent doivent rester dans un dossier réservé aux
// administrateurs, sinon tout utilisateur pourrait les remplacer et devenir SYSTEM.
#include <windows.h>

#include <filesystem>
#include <string>

namespace {

constexpr wchar_t kServiceName[] = L"36Screenlink";
constexpr DWORD kPollMs = 2000;
constexpr DWORD kNoSession = 0xFFFFFFFF;

SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
SERVICE_STATUS gStatus{};
HANDLE gStopEvent = nullptr;

void report(DWORD state) {
    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwCurrentState = state;
    gStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(gStatusHandle, &gStatus);
}

DWORD WINAPI control(DWORD code, DWORD, LPVOID, LPVOID) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);
        SetEvent(gStopEvent);
    }
    return NO_ERROR;
}

HANDLE startAgent(DWORD session) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    const std::wstring exe = (dir / L"36Screenlink.exe").wstring();
    std::wstring cmd = L"\"" + exe + L"\" --system --auto --tray";
    DWORD secure = 0, size = sizeof(secure);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\36Screenlink", L"SecureScreens", RRF_RT_REG_DWORD, nullptr, &secure, &size) == ERROR_SUCCESS && secure == 1)
        cmd += L" --secure";

    HANDLE self = nullptr, token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &self)) return nullptr;
    const BOOL duplicated = DuplicateTokenEx(self, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &token);
    CloseHandle(self);
    if (!duplicated) return nullptr;
    if (!SetTokenInformation(token, TokenSessionId, &session, sizeof(session))) {
        CloseHandle(token);
        return nullptr;
    }

    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = desktop;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                              dir.c_str(), &si, &pi);
    CloseHandle(token);
    if (!started) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

void stopAgent(HANDLE& agent) {
    if (!agent) return;
    // Arrêt demandé d'abord : l'agent doit rétablir le son du PC avant de partir.
    if (HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Global\\ScreenlinkQuit")) {
        SetEvent(quit);
        CloseHandle(quit);
    }
    if (WaitForSingleObject(agent, 3000) != WAIT_OBJECT_0) TerminateProcess(agent, 0);
    WaitForSingleObject(agent, 5000);
    CloseHandle(agent);
    agent = nullptr;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    gStatusHandle = RegisterServiceCtrlHandlerExW(kServiceName, control, nullptr);
    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    report(SERVICE_RUNNING);

    HANDLE agent = nullptr;
    DWORD agentSession = kNoSession;
    while (WaitForSingleObject(gStopEvent, kPollMs) == WAIT_TIMEOUT) {
        const DWORD session = WTSGetActiveConsoleSessionId();
        if (agent && (session != agentSession || WaitForSingleObject(agent, 0) == WAIT_OBJECT_0)) stopAgent(agent);
        if (!agent && session != kNoSession) {
            agent = startAgent(session);
            agentSession = session;
        }
    }
    stopAgent(agent);
    report(SERVICE_STOPPED);
}

} // namespace

int wmain() {
    wchar_t name[] = L"36Screenlink";
    const SERVICE_TABLE_ENTRYW table[] = {{name, serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
