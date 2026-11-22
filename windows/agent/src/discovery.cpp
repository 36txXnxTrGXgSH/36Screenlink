#include "discovery.h"

#include <windows.h>
#include <windns.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <mutex>

namespace {

struct BrowseContext {
    std::mutex mutex;
    std::vector<std::string> addresses;
};

VOID WINAPI onBrowse(DWORD status, PVOID context, PDNS_RECORD records) {
    auto* ctx = static_cast<BrowseContext*>(context);
    if (status == ERROR_SUCCESS) {
        for (PDNS_RECORD r = records; r; r = r->pNext) {
            if (r->wType != DNS_TYPE_A) continue;
            in_addr addr{};
            addr.s_addr = r->Data.A.IpAddress;
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr, text, sizeof(text));
            std::lock_guard lock(ctx->mutex);
            if (std::find(ctx->addresses.begin(), ctx->addresses.end(), text) == ctx->addresses.end()) ctx->addresses.push_back(text);
            break;
        }
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
}

} // namespace

std::vector<std::string> discoverDevices(int timeoutMs) {
    BrowseContext ctx;
    DNS_SERVICE_BROWSE_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.QueryName = L"_screenlink._tcp.local";
    request.pBrowseCallback = onBrowse;
    request.pQueryContext = &ctx;

    DNS_SERVICE_CANCEL cancel{};
    if (DnsServiceBrowse(&request, &cancel) != DNS_REQUEST_PENDING) return {};

    // Tous les appareils qui répondent dans le délai, pas seulement le premier.
    Sleep(static_cast<DWORD>(timeoutMs));
    DnsServiceBrowseCancel(&cancel);
    // Le rappel d'annulation peut encore arriver : on lui laisse le temps de finir avant de détruire ctx.
    Sleep(100);
    std::lock_guard lock(ctx.mutex);
    return ctx.addresses;
}
