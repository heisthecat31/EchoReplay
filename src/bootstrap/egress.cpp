#include "bootstrap/egress.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <wininet.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <MinHook.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/log.h"
#include "bootstrap/sha256.h"

namespace replay {
namespace egress {
namespace {

constexpr uint32_t kMaxDestinations = 128;

void RecordText(const char* text, const char* via, bool local);

struct Destination {
    char     text[64];
    // 17 bytes minimum: "InternetConnectW" is exactly 16 characters, so a 16-byte field truncated it
    // to "InternetConnect" and the record could not tell the W and A wininet paths apart.
    char     via[24];
    uint64_t count;
    bool     local;
};

struct State {
    Report                report;
    Destination           destinations[kMaxDestinations];
    std::atomic<uint32_t> destinationCount;
    std::atomic<uint64_t> allowedLocal;
    std::atomic<uint64_t> refusedRemote;
    std::atomic<uint32_t> overflow;
    CRITICAL_SECTION      lock;
    std::atomic<long>     lockReady;
    std::atomic<uint64_t> extensionsRefused;
    std::atomic<uint64_t> socketsCreated;
    std::atomic<uint64_t> firstSocketMs;
    HANDLE                soleInstance;
    bool                  attempted;
    std::atomic<long>     enforcing;      // 1 while the blocker refuses; 0 after Release
    bool                  live[32];
};

State g;

void EnsureLock() {
    if (InterlockedCompareExchange(reinterpret_cast<volatile long*>(&g.lockReady), 1, 0) == 0) {
        InitializeCriticalSection(&g.lock);
    }
}

// Records one destination by its printable form. Shared by the sockaddr path and the WININET path,
// whose destination is a host string rather than an address.
void RecordText(const char* text, const char* via, bool local) {
    EnterCriticalSection(&g.lock);
    const uint32_t count = g.destinationCount.load(std::memory_order_relaxed);
    bool merged = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(g.destinations[i].text, text) == 0 &&
            std::strcmp(g.destinations[i].via, via) == 0) {
            ++g.destinations[i].count;
            merged = true;
            break;
        }
    }
    if (!merged) {
        if (count < kMaxDestinations) {
            Destination& d = g.destinations[count];
            strncpy_s(d.text, sizeof d.text, text, _TRUNCATE);
            strncpy_s(d.via, sizeof d.via, via, _TRUNCATE);
            d.count = 1;
            d.local = local;
            g.destinationCount.store(count + 1, std::memory_order_release);
        } else {
            g.overflow.fetch_add(1, std::memory_order_relaxed);
        }
    }
    LeaveCriticalSection(&g.lock);
}

// Records the destination and returns true when the caller must be refused.
// ★ The passthrough gate. One relaxed load on every guarded call: after Release the hooks are still
// installed but refuse nothing, which is how an arm taken on a hint is given up without unhooking.
bool EnforcingNow() { return g.enforcing.load(std::memory_order_relaxed) != 0; }

bool RecordAndDecide(const sockaddr* addr, int len, const char* via) {
    if (!EnforcingNow()) {
        return false;   // allowed, and not recorded: this is a live match's own traffic
    }
    char text[64] = {0};
    const bool local = IsLocalDestination(addr, len, text, sizeof text);
    if (local) {
        g.allowedLocal.fetch_add(1, std::memory_order_relaxed);
    } else {
        g.refusedRemote.fetch_add(1, std::memory_order_relaxed);
    }
    RecordText(text, via, local);
    return !local;
}

using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using WsaConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using WsaSendToFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

ConnectFn    g_realConnect;
WsaConnectFn g_realWsaConnect;
SendToFn     g_realSendTo;
WsaSendToFn  g_realWsaSendTo;

int WSAAPI HookConnect(SOCKET s, const sockaddr* name, int len) {
    if (RecordAndDecide(name, len, "connect")) {
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return g_realConnect(s, name, len);
}

int WSAAPI HookWsaConnect(SOCKET s, const sockaddr* name, int len, LPWSABUF cd, LPWSABUF ud,
                          LPQOS sqos, LPQOS gqos) {
    if (RecordAndDecide(name, len, "WSAConnect")) {
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return g_realWsaConnect(s, name, len, cd, ud, sqos, gqos);
}

int WSAAPI HookSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    if (RecordAndDecide(to, tolen, "sendto")) {
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return g_realSendTo(s, buf, len, flags, to, tolen);
}

int WSAAPI HookWsaSendTo(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags,
                         const sockaddr* to, int tolen, LPWSAOVERLAPPED ov,
                         LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (RecordAndDecide(to, tolen, "WSASendTo")) {
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return g_realWsaSendTo(s, bufs, count, sent, flags, to, tolen, ov, cr);
}

using WsaIoctlFn = int(WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD,
                                LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
using WsaSendFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using SocketFn = SOCKET(WSAAPI*)(int, int, int);

WsaIoctlFn g_realWsaIoctl;
SendFn     g_realSend;
WsaSendFn  g_realWsaSend;
SocketFn   g_realSocket;

// A connected socket can only have been aimed by connect or ConnectEx, and both are closed. This is
// the belt to that braces: it catches a socket that arrived already connected - inherited from a
// parent, or handed over by a module we never watched being created.
bool PeerIsRemote(SOCKET s, const char* via) {
    if (!EnforcingNow()) {
        return false;   // and no getpeername: this is a live match's send path
    }
    sockaddr_storage peer{};
    int len = static_cast<int>(sizeof peer);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &len) != 0) {
        return false;   // not connected: there is nothing for it to reach
    }
    return RecordAndDecide(reinterpret_cast<const sockaddr*>(&peer), len, via);
}

// â˜… WSAIoctl is where SIO_GET_EXTENSION_FUNCTION_POINTER hands out ConnectEx and
// WSASendMsg, and echovr.exe IMPORTS WSAIoctl - so without this the four original trampolines were not
// exhaustive and either extension would have connected straight past connect(). Refusing at the LOOKUP
// is fail-closed: the pointers never come into existence, so there is no per-call check to get wrong and
// no window between obtaining one and using it.
int WSAAPI HookWsaIoctl(SOCKET s, DWORD code, LPVOID inBuf, DWORD inLen, LPVOID outBuf, DWORD outLen,
                        LPDWORD returned, LPWSAOVERLAPPED ov,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (EnforcingNow() && code == SIO_GET_EXTENSION_FUNCTION_POINTER && inBuf != nullptr &&
        inLen >= sizeof(GUID)) {
        static const GUID kConnectEx = WSAID_CONNECTEX;
        static const GUID kWsaSendMsg = WSAID_WSASENDMSG;
        const GUID* want = static_cast<const GUID*>(inBuf);
        if (std::memcmp(want, &kConnectEx, sizeof(GUID)) == 0 ||
            std::memcmp(want, &kWsaSendMsg, sizeof(GUID)) == 0) {
            g.extensionsRefused.fetch_add(1, std::memory_order_relaxed);
            WSASetLastError(WSAEOPNOTSUPP);
            return SOCKET_ERROR;
        }
    }
    return g_realWsaIoctl(s, code, inBuf, inLen, outBuf, outLen, returned, ov, cr);
}

int WSAAPI HookSend(SOCKET s, const char* buf, int len, int flags) {
    if (PeerIsRemote(s, "send")) {
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return g_realSend(s, buf, len, flags);
}

int WSAAPI HookWsaSend(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags,
                       LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (PeerIsRemote(s, "WSASend")) {
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return g_realWsaSend(s, bufs, count, sent, flags, ov, cr);
}

// Never refuses. It dates the first socket in the process, which turns "armed early enough" from an
// assumption into a measurement: if the first socket is created after we armed, the unguarded window
// provably contained no socket at all.
SOCKET WSAAPI HookSocket(int af, int type, int protocol) {
    uint64_t expected = 0;
    g.firstSocketMs.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_acq_rel);
    g.socketsCreated.fetch_add(1, std::memory_order_relaxed);
    return g_realSocket(af, type, protocol);
}

using WsaSocketWFn = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
using WsaSocketAFn = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);

WsaSocketWFn g_realWsaSocketW;
WsaSocketAFn g_realWsaSocketA;

// echovr.exe imports WSASocketW, not just socket(). Without this the recorder undercounts and
// firstSocketAfterMs could report "no socket before we armed" over a process that had made several.
SOCKET WSAAPI HookWsaSocketW(int af, int type, int protocol, LPWSAPROTOCOL_INFOW info, GROUP grp,
                             DWORD flags) {
    uint64_t expected = 0;
    g.firstSocketMs.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_acq_rel);
    g.socketsCreated.fetch_add(1, std::memory_order_relaxed);
    return g_realWsaSocketW(af, type, protocol, info, grp, flags);
}

// wininet.dll resolves WSASocketA, not the W form, so the recorder needs both or it undercounts
// exactly the sockets the crash uploader makes.
SOCKET WSAAPI HookWsaSocketA(int af, int type, int protocol, LPWSAPROTOCOL_INFOA info, GROUP grp,
                             DWORD flags) {
    uint64_t expected = 0;
    g.firstSocketMs.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_acq_rel);
    g.socketsCreated.fetch_add(1, std::memory_order_relaxed);
    return g_realWsaSocketA(af, type, protocol, info, grp, flags);
}

// ---------------------------------------------------------------------------------------------------
// WININET.
//
// â˜… This is a SECOND transport, not a second spelling of the first. BugSplat64.dll - the
// crash reporter shipped beside echovr.exe - statically imports ten WININET functions including
// InternetConnectW and HttpSendRequestW, and WININET does its own socket work inside its own DLL. Every
// WS2_32 trampoline in this file is blind to it, so a crash during a sweep could upload a dump while the
// blocker reported zero refusals. It was enumerable from the import directory the whole time.
//
// The destination here is a host STRING rather than a sockaddr, so the loopback test is textual.
using InternetConnectWFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, LPCWSTR, LPCWSTR,
                                              DWORD, DWORD, DWORD_PTR);
using InternetConnectAFn = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, INTERNET_PORT, LPCSTR, LPCSTR,
                                              DWORD, DWORD, DWORD_PTR);
using InternetOpenUrlWFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
using InternetOpenUrlAFn = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
using HttpSendRequestWFn = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD);
using HttpSendRequestAFn = BOOL(WINAPI*)(HINTERNET, LPCSTR, DWORD, LPVOID, DWORD);

InternetConnectWFn g_realInternetConnectW;
InternetConnectAFn g_realInternetConnectA;
InternetOpenUrlWFn g_realInternetOpenUrlW;
InternetOpenUrlAFn g_realInternetOpenUrlA;
HttpSendRequestWFn g_realHttpSendRequestW;
HttpSendRequestAFn g_realHttpSendRequestA;

bool HostIsLocalA(const char* host) {
    if (host == nullptr || host[0] == 0) {
        return true;
    }
    // Only these three are local. Anything else - including a name that would RESOLVE to loopback -
    // is refused, because resolving it is itself a query that leaves the machine.
    return _stricmp(host, "localhost") == 0 || std::strncmp(host, "127.", 4) == 0 ||
           std::strcmp(host, "::1") == 0 || std::strcmp(host, "[::1]") == 0;
}

bool HostIsLocalW(const wchar_t* host) {
    if (host == nullptr || host[0] == 0) {
        return true;
    }
    char narrow[256] = {0};
    for (size_t i = 0; i + 1 < sizeof narrow && host[i] != 0; ++i) {
        narrow[i] = static_cast<char>(host[i] < 128 ? host[i] : '?');
    }
    return HostIsLocalA(narrow);
}

bool RefuseHost(const char* host, const char* via) {
    if (!EnforcingNow()) {
        return false;
    }
    char text[64];
    _snprintf_s(text, sizeof text, _TRUNCATE, "%s", host != nullptr ? host : "(null)");
    const bool local = HostIsLocalA(host);
    if (local) {
        g.allowedLocal.fetch_add(1, std::memory_order_relaxed);
    } else {
        g.refusedRemote.fetch_add(1, std::memory_order_relaxed);
    }
    RecordText(text, via, local);
    return !local;
}

HINTERNET WINAPI HookInternetConnectW(HINTERNET h, LPCWSTR server, INTERNET_PORT port, LPCWSTR user,
                                      LPCWSTR pass, DWORD service, DWORD flags, DWORD_PTR ctx) {
    char narrow[256] = {0};
    if (server != nullptr) {
        for (size_t i = 0; i + 1 < sizeof narrow && server[i] != 0; ++i) {
            narrow[i] = static_cast<char>(server[i] < 128 ? server[i] : '?');
        }
    }
    if (EnforcingNow() && !HostIsLocalW(server)) {
        RefuseHost(narrow, "InternetConnectW");
        SetLastError(ERROR_INTERNET_CANNOT_CONNECT);
        return nullptr;
    }
    RefuseHost(narrow, "InternetConnectW");
    return g_realInternetConnectW(h, server, port, user, pass, service, flags, ctx);
}

HINTERNET WINAPI HookInternetConnectA(HINTERNET h, LPCSTR server, INTERNET_PORT port, LPCSTR user,
                                      LPCSTR pass, DWORD service, DWORD flags, DWORD_PTR ctx) {
    if (RefuseHost(server, "InternetConnectA")) {
        SetLastError(ERROR_INTERNET_CANNOT_CONNECT);
        return nullptr;
    }
    return g_realInternetConnectA(h, server, port, user, pass, service, flags, ctx);
}

HINTERNET WINAPI HookInternetOpenUrlW(HINTERNET h, LPCWSTR url, LPCWSTR headers, DWORD len,
                                      DWORD flags, DWORD_PTR ctx) {
    if (EnforcingNow() && !HostIsLocalW(url)) {
        RefuseHost("(url)", "InternetOpenUrlW");
        SetLastError(ERROR_INTERNET_CANNOT_CONNECT);
        return nullptr;
    }
    return g_realInternetOpenUrlW(h, url, headers, len, flags, ctx);
}

// Recorded as "(url)" like the W form: a URL can carry credentials in its userinfo or query (the
// game's own login URL does), and the destination record must never hold them.
HINTERNET WINAPI HookInternetOpenUrlA(HINTERNET h, LPCSTR url, LPCSTR headers, DWORD len, DWORD flags,
                                      DWORD_PTR ctx) {
    if (EnforcingNow() && !HostIsLocalA(url)) {
        RefuseHost("(url)", "InternetOpenUrlA");
        SetLastError(ERROR_INTERNET_CANNOT_CONNECT);
        return nullptr;
    }
    return g_realInternetOpenUrlA(h, url, headers, len, flags, ctx);
}

// The belt to InternetConnect's braces: a handle obtained before we armed can still be sent on.
BOOL WINAPI HookHttpSendRequestW(HINTERNET req, LPCWSTR headers, DWORD headerLen, LPVOID optional,
                                 DWORD optionalLen) {
    if (!EnforcingNow()) {
        return g_realHttpSendRequestW(req, headers, headerLen, optional, optionalLen);
    }
    g.refusedRemote.fetch_add(1, std::memory_order_relaxed);
    RecordText("(http request)", "HttpSendRequestW", false);
    SetLastError(ERROR_INTERNET_CANNOT_CONNECT);
    return FALSE;
}

BOOL WINAPI HookHttpSendRequestA(HINTERNET req, LPCSTR headers, DWORD headerLen, LPVOID optional,
                                 DWORD optionalLen) {
    if (!EnforcingNow()) {
        return g_realHttpSendRequestA(req, headers, headerLen, optional, optionalLen);
    }
    g.refusedRemote.fetch_add(1, std::memory_order_relaxed);
    RecordText("(http request)", "HttpSendRequestA", false);
    SetLastError(ERROR_INTERNET_CANNOT_CONNECT);
    return FALSE;
}

struct Row {
    const char* name;
    void*       detour;
    void**      original;
};

const Row kRows[] = {
    {"connect",    reinterpret_cast<void*>(&HookConnect),    reinterpret_cast<void**>(&g_realConnect)},
    {"WSAConnect", reinterpret_cast<void*>(&HookWsaConnect), reinterpret_cast<void**>(&g_realWsaConnect)},
    {"sendto",     reinterpret_cast<void*>(&HookSendTo),     reinterpret_cast<void**>(&g_realSendTo)},
    {"WSASendTo",  reinterpret_cast<void*>(&HookWsaSendTo),  reinterpret_cast<void**>(&g_realWsaSendTo)},
    {"WSAIoctl",   reinterpret_cast<void*>(&HookWsaIoctl),   reinterpret_cast<void**>(&g_realWsaIoctl)},
    {"send",       reinterpret_cast<void*>(&HookSend),       reinterpret_cast<void**>(&g_realSend)},
    {"WSASend",    reinterpret_cast<void*>(&HookWsaSend),    reinterpret_cast<void**>(&g_realWsaSend)},
    {"socket",     reinterpret_cast<void*>(&HookSocket),     reinterpret_cast<void**>(&g_realSocket)},
    {"WSASocketW", reinterpret_cast<void*>(&HookWsaSocketW), reinterpret_cast<void**>(&g_realWsaSocketW)},
    {"WSASocketA", reinterpret_cast<void*>(&HookWsaSocketA), reinterpret_cast<void**>(&g_realWsaSocketA)},
};

constexpr uint32_t kRowCount = sizeof kRows / sizeof kRows[0];

// WININET is a separate DLL and a separate transport, so it gets its own pass. It is armed only if the
// module is already loaded: this never calls LoadLibrary, so a process that has no WININET keeps none.
const Row kInetRows[] = {
    {"InternetConnectW",  reinterpret_cast<void*>(&HookInternetConnectW),
     reinterpret_cast<void**>(&g_realInternetConnectW)},
    {"InternetConnectA",  reinterpret_cast<void*>(&HookInternetConnectA),
     reinterpret_cast<void**>(&g_realInternetConnectA)},
    {"InternetOpenUrlW",  reinterpret_cast<void*>(&HookInternetOpenUrlW),
     reinterpret_cast<void**>(&g_realInternetOpenUrlW)},
    {"InternetOpenUrlA",  reinterpret_cast<void*>(&HookInternetOpenUrlA),
     reinterpret_cast<void**>(&g_realInternetOpenUrlA)},
    {"HttpSendRequestW",  reinterpret_cast<void*>(&HookHttpSendRequestW),
     reinterpret_cast<void**>(&g_realHttpSendRequestW)},
    {"HttpSendRequestA",  reinterpret_cast<void*>(&HookHttpSendRequestA),
     reinterpret_cast<void**>(&g_realHttpSendRequestA)},
};

constexpr uint32_t kInetRowCount = sizeof kInetRows / sizeof kInetRows[0];

}  // namespace

const char* ArmStateName(ArmState state) {
    switch (state) {
        case ArmState::NotAttempted:   return "not-attempted";
        case ArmState::Armed:          return "armed";
        case ArmState::PartiallyArmed: return "partially-armed";
        case ArmState::Failed:         return "failed";
    }
    return "?";
}

bool ArmTallyConsistent(uint32_t live, uint32_t requested) {
    return requested > 0 && live <= requested;
}

const char* CoverageNote() {
    // W1-F's sentence, adopted verbatim because it states what the number does NOT mean, which is the
    // reading that would actually hurt somebody.
    //
    // On WININET, settled from the import tables and correcting BOTH earlier readings: wininet.dll
    // DELAY-loads ws2_32 and imports 29 entries from it, including connect, send, WSASend and socket.
    // So WinINet does bottom out in ws2_32 after all - the retraction read only the static table and
    // missed the delay table, the same one-table error as the earlier "18 of 37". A delay-loaded import
    // resolves through GetProcAddress to the real address, and MinHook patches the prologue IN PLACE,
    // so a pointer resolved after we armed still lands on the trampoline. WININET traffic is therefore
    // covered twice: once at its own entry points and once underneath at ws2_32.
    return "A refused=0 reading means 'nothing tried to leave through ws2_32 after we armed'. It does "
           "NOT mean 'nothing left'. Covered: ws2_32 connect/WSAConnect/sendto/WSASendTo/send/WSASend, "
           "the WSAIoctl extension lookups (ConnectEx, WSASendMsg), socket/WSASocketW/WSASocketA as "
           "recorders, and wininet InternetConnect/OpenUrl/HttpSendRequest - which also bottoms out in "
           "the ws2_32 hooks, so it is covered twice. NOT covered: DNS name resolution (getaddrinfo, "
           "GetAddrInfoExW, gethostbyname, getnameinfo - these resolve through the DNS Client service, "
           "not our sockets, so a login-host lookup leaves carrying the hostname with nothing to "
           "intercept), WinHTTP, raw NtDeviceIoControlFile to the AFD device, a socket duplicated from "
           "another process, and anything before armedAtMs.";
}

// â˜… Sized for every row in both tables, and the size is checked before each write rather than
// derived by subtracting a running total.
//
// This function used to accumulate snprintf's return value, which is the length it WOULD have written.
// Once the sixteen live names passed 160 bytes, `sizeof buffer - n` underflowed to SIZE_MAX - k and the
// remaining rows were written unbounded: 76 bytes past the end, in every launch that had wininet mapped
// - which is every launch of the game. The offline suite never saw it because the test host has no
// wininet.dll, so ten names fitted with room to spare. The capacity below is for all sixteen; the
// per-write check is what makes a seventeenth safe rather than fatal.
constexpr size_t kHookNamesCap = 384;
constexpr char kTruncatedMarker[] = "\"(truncated)\"";
// Kept free at all times so a name that does not fit can always be REPORTED as missing. A silently
// short list is the failure this whole array exists to prevent: the four names the overflow used to eat
// were the WININET refusers, and the by-name list replaced a count precisely because a count had hidden
// a missing hook once already.
constexpr size_t kMarkerReserve = sizeof kTruncatedMarker + 2;

// Appends `"name"` if the whole thing fits, leaving the marker's room spare. False the moment one does
// not: a name is written whole or not at all, never half.
bool AppendHookName(char* buffer, size_t* n, const char* name) {
    const size_t sep = (*n != 0) ? 2u : 0u;
    const size_t need = sep + 2u + std::strlen(name);
    if (*n + need + kMarkerReserve > kHookNamesCap) {
        return false;
    }
    if (sep != 0u) {
        buffer[(*n)++] = ',';
        buffer[(*n)++] = ' ';
    }
    buffer[(*n)++] = '"';
    std::memcpy(buffer + *n, name, std::strlen(name));
    *n += std::strlen(name);
    buffer[(*n)++] = '"';
    buffer[*n] = 0;
    return true;
}

const char* HookNamesJson() {
    static char buffer[kHookNamesCap];
    size_t n = 0;
    buffer[0] = 0;
    bool complete = true;
    for (uint32_t i = 0; i < kRowCount && complete; ++i) {
        if (g.live[i]) {
            complete = AppendHookName(buffer, &n, kRows[i].name);
        }
    }
    for (uint32_t i = 0; i < kInetRowCount && complete; ++i) {
        if (g.live[kRowCount + i]) {
            complete = AppendHookName(buffer, &n, kInetRows[i].name);
        }
    }
    if (!complete) {
        if (n != 0) {
            buffer[n++] = ',';
            buffer[n++] = ' ';
        }
        std::memcpy(buffer + n, kTruncatedMarker, sizeof kTruncatedMarker);
        n += sizeof kTruncatedMarker - 1;
        buffer[n] = 0;
    }
    return buffer;
}

bool IsLocalDestination(const void* addrRaw, int len, char* out, size_t outCap) {
    const sockaddr* addr = static_cast<const sockaddr*>(addrRaw);
    if (addr == nullptr) {
        strncpy_s(out, outCap, "(null)", _TRUNCATE);
        return true;
    }
    if (addr->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in))) {
        const sockaddr_in* v4 = reinterpret_cast<const sockaddr_in*>(addr);
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof ip);
        _snprintf_s(out, outCap, _TRUNCATE, "%s:%u", ip, ntohs(v4->sin_port));
        const uint32_t host = ntohl(v4->sin_addr.s_addr);
        // 127.0.0.0/8 is loopback; INADDR_ANY has nowhere to go.
        return (host >> 24) == 127u || host == INADDR_ANY;
    }
    if (addr->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6))) {
        const sockaddr_in6* v6 = reinterpret_cast<const sockaddr_in6*>(addr);
        char ip[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof ip);
        _snprintf_s(out, outCap, _TRUNCATE, "[%s]:%u", ip, ntohs(v6->sin6_port));
        return IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr) != 0 ||
               IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr) != 0;
    }
    // A family we do not parse - AF_UNIX, AF_BTH, or a malformed length. None of them reaches an
    // internet peer, and refusing them would break local IPC for nothing.
    _snprintf_s(out, outCap, _TRUNCATE, "family=%d", static_cast<int>(addr->sa_family));
    return true;
}

const char* ArmTriggerName(ArmTrigger trigger) {
    switch (trigger) {
        case ArmTrigger::Unspecified:      return "unspecified";
        case ArmTrigger::OfflineHint:       return "offline-hint";
        case ArmTrigger::ReplayHint:        return "replay-hint";
        case ArmTrigger::ModeUnknown:       return "mode-unknown";
        case ArmTrigger::OfflineSession:    return "offline-session";
        case ArmTrigger::ReplayMode:        return "replay-mode";
        case ArmTrigger::ConfigForcedOn:    return "config-forced-on";
        case ArmTrigger::LiveOnlineCapture: return "live-online-capture";
        case ArmTrigger::SweepOverride:    return "sweep-override";
        case ArmTrigger::DisabledByConfig: return "disabled-by-config";
    }
    return "?";
}

ArmState Arm(ArmTrigger trigger) {
    if (g.attempted) {
        return g.report.state;   // idempotent: report the first call rather than re-arming
    }
    g.attempted = true;
    g.report.trigger = trigger;
    g.enforcing.store(1, std::memory_order_release);

    // The per-launch redaction salt is generated HERE, at init, so every token in this launch's
    // record shares one salt and an RNG failure becomes a refusal now rather than at record time.
    // The salt itself never leaves this process: it is no field of Report and reaches no log line.
    (void)RedactionSalt();

    // â˜… EXACTLY ONE BLOCKER PER PROCESS, asserted rather than assumed.
    //
    // MinHook refuses a second MH_CreateHook on a target that already has one, so a second copy of this
    // code in the same process arms nothing and reports PartiallyArmed - over a process that is in fact
    // fully guarded. A safety field that under-reports safety trains people to ignore it, and the mirror
    // case (over-reporting) is worse. Neither is acceptable from a field the sweep reads as a verdict.
    //
    // The named mutex is process-scoped and released when this process exits. A second instance says so
    // loudly and refuses to arm, instead of quietly producing a misleading number.
    wchar_t soleName[64];
    _snwprintf_s(soleName, sizeof soleName / sizeof soleName[0], _TRUNCATE,
                 L"Local\\echoreplay.egress.%lu", GetCurrentProcessId());
    g.soleInstance = CreateMutexW(nullptr, TRUE, soleName);
    if (g.soleInstance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g.soleInstance);
        g.soleInstance = nullptr;
        g.report.state = ArmState::Failed;
        log::Direct(LogLevel::Error, "egress",
                    "a second egress blocker is already armed in this process. MinHook would refuse the "
                    "duplicate hooks and this copy would report partially-armed over a process that is "
                    "fully guarded - refusing instead, so the report cannot mislead. Consume "
                    "replay::egress rather than carrying a second copy.");
        return g.report.state;
    }
    g.report.hooksRequested = kRowCount;
    EnsureLock();

    const uint64_t startMs = GetTickCount64();

    // MH_Initialize is process-global and hookmgr has usually already called it from DllMain.
    // ALREADY_INITIALIZED is success here, not failure: treating it as failure would leave egress
    // unguarded while this function reported nothing wrong, and the harness records a silent probe as
    // `unknown`, never `safe`.
    if (!hookmgr::Initialized()) {
        const MH_STATUS rc = MH_Initialize();
        if (rc != MH_OK && rc != MH_ERROR_ALREADY_INITIALIZED) {
            g.report.state = ArmState::Failed;
            log::Direct(LogLevel::Error, "egress", "MH_Initialize returned %s; egress is NOT guarded",
                        MH_StatusToString(rc));
            return g.report.state;
        }
    }

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (ws2 == nullptr) {
        // Impossible for this executable, which imports WS2_32 statically - but say so rather than
        // report a clean arm over an empty hook set.
        g.report.state = ArmState::Failed;
        log::Direct(LogLevel::Error, "egress", "ws2_32.dll is not loaded; egress is NOT guarded");
        return g.report.state;
    }

    uint32_t live = 0;
    for (uint32_t i = 0; i < kRowCount; ++i) {
        void* target = reinterpret_cast<void*>(GetProcAddress(ws2, kRows[i].name));
        if (target == nullptr) {
            log::Direct(LogLevel::Error, "egress", "%s not found in ws2_32.dll", kRows[i].name);
            continue;
        }
        if (MH_CreateHook(target, kRows[i].detour, kRows[i].original) != MH_OK) {
            log::Direct(LogLevel::Error, "egress", "MH_CreateHook failed for %s", kRows[i].name);
            continue;
        }
        // â˜… QUEUED, not enabled here. MH_EnableHook suspends every other thread in the
        // process to rewrite one prologue, so enabling fifteen hooks one at a time is fifteen
        // freeze/unfreeze cycles - measured at 735 ms of a 750 ms unprotected window, i.e. the window is
        // almost entirely this loop rather than our startup. MH_ApplyQueued does the whole set inside a
        // single freeze. Still on the worker: the freeze is exactly why this may never run in DllMain.
        if (MH_QueueEnableHook(target) != MH_OK) {
            log::Direct(LogLevel::Error, "egress", "MH_QueueEnableHook failed for %s", kRows[i].name);
            continue;
        }
        g.live[i] = true;
        ++live;
    }

    // â˜… The second transport. BugSplat64.dll statically imports ten WININET functions,
    // including InternetConnectW and HttpSendRequestW, and WININET does its socket work inside its own
    // DLL - so every trampoline above is blind to it and a crash could upload a dump while this blocker
    // reported zero refusals. GetModuleHandleA, never LoadLibrary: if WININET is not loaded, there is
    // nothing to guard and nothing is dragged in.
    HMODULE inet = GetModuleHandleA("wininet.dll");
    g.report.wininetLoaded = inet != nullptr;
    if (inet != nullptr) {
        g.report.hooksRequested += kInetRowCount;
        for (uint32_t i = 0; i < kInetRowCount; ++i) {
            void* target = reinterpret_cast<void*>(GetProcAddress(inet, kInetRows[i].name));
            if (target == nullptr ||
                MH_CreateHook(target, kInetRows[i].detour, kInetRows[i].original) != MH_OK ||
                MH_QueueEnableHook(target) != MH_OK) {
                log::Direct(LogLevel::Error, "egress", "could not hook wininet!%s", kInetRows[i].name);
                continue;
            }
            g.live[kRowCount + i] = true;
            ++live;
        }
    } else {
        log::Direct(LogLevel::Info, "egress",
                    "wininet.dll is not loaded, so there is no WININET surface to guard in this process");
    }

    // One freeze for the whole set. If this fails nothing is live, whatever the loops above counted -
    // so `live` is reset rather than left describing hooks that were only ever queued.
    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK) {
        log::Direct(LogLevel::Error, "egress", "MH_ApplyQueued returned %s; NOTHING is armed",
                    MH_StatusToString(applied));
        live = 0;
        std::memset(g.live, 0, sizeof g.live);
    }

    g.report.hooksLive = live;
    const uint64_t nowMs = GetTickCount64();
    g.report.armDurationMs = nowMs - startMs;
    // From attach, not from the top of this function. The unprotected window starts when the DLL is
    // mapped, and most of it is the worker getting here - config load, the 35 MB md5 - not the hooks.
    const uint64_t attachMs = bootstrap::AttachTickMs();
    g.report.armedAtMs = (attachMs != 0 && nowMs >= attachMs) ? (nowMs - attachMs)
                                                              : g.report.armDurationMs;
    if (live == g.report.hooksRequested) {
        g.report.state = ArmState::Armed;
    } else if (live > 0) {
        // Its own state on purpose. Some entry points guarded is not "guarded": whichever one did not
        // take the trampoline is an open path, and calling this Armed would be the exact lie this
        // component exists to prevent.
        g.report.state = ArmState::PartiallyArmed;
    } else {
        g.report.state = ArmState::Failed;
    }

    // The denominator is what the loops above actually asked for - kRows, plus kInetRows when WININET is
    // loaded - never the size of one table. Both numbers go through the same check before either is
    // printed: this line is read as evidence about outbound blocking, and a ratio the count can exceed
    // is a coverage claim that cannot fail.
    const uint32_t requested = g.report.hooksRequested;
    if (ArmTallyConsistent(live, requested)) {
        log::Direct(g.report.state == ArmState::Armed ? LogLevel::Info : LogLevel::Error, "egress",
                    "%s: %u of %u entry point(s) live %llu ms after ATTACH (the unprotected window); the "
                    "trampolines themselves took %llu ms",
                    ArmStateName(g.report.state), live, requested,
                    static_cast<unsigned long long>(g.report.armedAtMs),
                    static_cast<unsigned long long>(g.report.armDurationMs));
    } else {
        log::Direct(LogLevel::Error, "egress",
                    "%s: refusing to print a coverage ratio that does not describe one set of entry "
                    "points - %u live against %u requested. Read this launch's egress coverage as "
                    "UNKNOWN. Armed %llu ms after ATTACH; the trampolines took %llu ms",
                    ArmStateName(g.report.state), live, requested,
                    static_cast<unsigned long long>(g.report.armedAtMs),
                    static_cast<unsigned long long>(g.report.armDurationMs));
    }
    return g.report.state;
}

bool TriggerIsReleasable(ArmTrigger trigger) {
    // Only the three guesses. An observed SessionFlags bit, an entered replay, a sweep and an explicit
    // config key are facts, and nothing the session does later can make them wrong.
    return trigger == ArmTrigger::OfflineHint || trigger == ArmTrigger::ReplayHint ||
           trigger == ArmTrigger::ModeUnknown;
}

bool Release(ArmTrigger reason) {
    if (g.report.state != ArmState::Armed && g.report.state != ArmState::PartiallyArmed) {
        return false;   // nothing to give up
    }
    if (!TriggerIsReleasable(g.report.trigger)) {
        log::Direct(LogLevel::Warn, "egress",
                    "refusing to release a blocker armed for %s: that is an observed fact, not a hint, "
                    "and giving it up on a later reading is how a guarded session becomes an unguarded "
                    "one",
                    ArmTriggerName(g.report.trigger));
        return false;
    }
    if (!EnforcingNow()) {
        return true;   // already released; idempotent
    }
    g.report.refusedBeforeRelease = g.refusedRemote.load(std::memory_order_relaxed);
    g.report.released = true;
    g.report.releaseReason = reason;
    g.enforcing.store(0, std::memory_order_release);
    log::Direct(LogLevel::Warn, "egress",
                "released: armed on %s, but the session state says %s. The hooks stay installed and now "
                "pass everything through. %llu destination(s) were refused before this point",
                ArmTriggerName(g.report.trigger), ArmTriggerName(reason),
                static_cast<unsigned long long>(g.report.refusedBeforeRelease));
    return true;
}

bool Enforcing() { return EnforcingNow(); }

void RecordNotArmed(ArmTrigger trigger) {
    // ★ `attempted` is deliberately NOT set. A capture session that later begins playback must be able
    // to arm then, and a flag here would refuse it silently - the exact class of failure this reporting
    // exists to prevent.
    g.report.trigger = trigger;
}

const Report& Current() {
    g.report.refusedRemote = g.refusedRemote.load(std::memory_order_relaxed);
    g.report.allowedLocal = g.allowedLocal.load(std::memory_order_relaxed);
    g.report.distinctDestinations = g.destinationCount.load(std::memory_order_acquire);
    g.report.destinationOverflow = g.overflow.load(std::memory_order_relaxed);
    g.report.extensionsRefused = g.extensionsRefused.load(std::memory_order_relaxed);
    g.report.socketsCreated = g.socketsCreated.load(std::memory_order_relaxed);
    const uint64_t firstSocket = g.firstSocketMs.load(std::memory_order_relaxed);
    const uint64_t attachMs = bootstrap::AttachTickMs();
    g.report.firstSocketAfterMs =
        (firstSocket == 0 || attachMs == 0) ? -1 : static_cast<int64_t>(firstSocket - attachMs);
    return g.report;
}

bool DestinationAt(size_t index, const char** text, const char** via, uint64_t* count, bool* local) {
    if (index >= g.destinationCount.load(std::memory_order_acquire)) {
        return false;
    }
    const Destination& d = g.destinations[index];
    *text = d.text;
    *via = d.via;
    *count = d.count;
    *local = d.local;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Redaction of the destination set. egress.h carries the rule and why it is shaped this way.

namespace {

const char* g_redactRefusal = nullptr;

// The redaction salt is PER LAUNCH, not per build.
//
// It was bootstrap::BuildId(), which the plugin log prints on every attach. A reader holding the
// log and the record therefore holds the salt, and eight hex digits of SHA-256 over a guessed host
// is a dictionary attack rather than a token -- the candidate list of hosts this game talks to is
// short. Sixteen bytes from the system RNG, generated once at egress init and written to no log, no
// record and no other emitted string, keeps the only property the record needs: one host is one
// token for the life of the launch, so a reader still counts distinct hosts and correlates rows. It
// gives up correlation ACROSS launches, which nothing reads.
uint8_t g_saltBytes[16];
char g_saltHex[sizeof g_saltBytes * 2 + 1];
bool g_saltReady = false;
char g_saltRefusalText[224];
const char* g_saltRefusal = nullptr;

// bcrypt.dll is resolved at call time rather than imported, for the reason src/bootstrap/sha256.h
// gives for hashing in-tree: a static import is mapped while the loader lock is held by the DLL that
// loaded us. Arm() is worker-thread only, so a LoadLibrary from here is not on that path.
bool FillFromSystemRng(uint8_t* out, size_t outLen) {
    using BCryptGenRandomFn = long(__stdcall*)(void*, unsigned char*, unsigned long, unsigned long);
    const unsigned long kUseSystemPreferredRng = 0x00000002;   // BCRYPT_USE_SYSTEM_PREFERRED_RNG
    HMODULE bcrypt = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (bcrypt == nullptr) {
        _snprintf_s(g_saltRefusalText, sizeof g_saltRefusalText, _TRUNCATE,
                    "REFUSED: bcrypt.dll would not load (error %lu), so this launch has no random "
                    "redaction salt -- and a guessable salt is not a fallback, it is the leak",
                    GetLastError());
        return false;
    }
    const BCryptGenRandomFn gen =
        reinterpret_cast<BCryptGenRandomFn>(GetProcAddress(bcrypt, "BCryptGenRandom"));
    bool ok = false;
    if (gen == nullptr) {
        _snprintf_s(g_saltRefusalText, sizeof g_saltRefusalText, _TRUNCATE,
                    "REFUSED: bcrypt.dll exports no BCryptGenRandom (error %lu), so this launch has "
                    "no random redaction salt", GetLastError());
    } else {
        const long status =
            gen(nullptr, out, static_cast<unsigned long>(outLen), kUseSystemPreferredRng);
        ok = status >= 0;   // NT_SUCCESS
        if (!ok) {
            _snprintf_s(g_saltRefusalText, sizeof g_saltRefusalText, _TRUNCATE,
                        "REFUSED: BCryptGenRandom returned status 0x%08lX, so this launch has no "
                        "random redaction salt -- a predictable one puts every token back within "
                        "reach of anyone holding the plugin log",
                        static_cast<unsigned long>(status));
        }
    }
    FreeLibrary(bcrypt);
    return ok;
}

// Idempotent: the first caller in the process fixes the salt for the whole launch, which is what
// makes one host one token inside one record.
bool EnsureRedactionSalt() {
    EnsureLock();
    EnterCriticalSection(&g.lock);
    if (!g_saltReady) {
        if (FillFromSystemRng(g_saltBytes, sizeof g_saltBytes)) {
            static const char kHexDigits[] = "0123456789abcdef";
            for (size_t i = 0; i < sizeof g_saltBytes; ++i) {
                g_saltHex[i * 2] = kHexDigits[g_saltBytes[i] >> 4];
                g_saltHex[i * 2 + 1] = kHexDigits[g_saltBytes[i] & 0x0Fu];
            }
            g_saltHex[sizeof g_saltBytes * 2] = 0;
            g_saltReady = true;
            g_saltRefusal = nullptr;
        } else {
            // Fail-closed, and say so through the channel the record reader already checks.
            g_saltRefusal = g_saltRefusalText;
            g_redactRefusal = g_saltRefusalText;
        }
    }
    LeaveCriticalSection(&g.lock);
    return g_saltReady;
}

// Splits "1.2.3.4:443", "[::1]:80", "login.example.net" or "family=1" into host and port. A trailing
// `:digits` is a port; anything else stays part of the host, so a bare name with a colon in it is not
// silently cut in half.
void SplitHostPort(const char* text, char* host, size_t hostCap, char* port, size_t portCap) {
    host[0] = '\0';
    port[0] = '\0';
    if (text == nullptr) {
        return;
    }
    const char* hostBegin = text;
    const char* hostEnd = text + std::strlen(text);
    const char* portBegin = nullptr;
    if (text[0] == '[') {
        const char* close = std::strchr(text, ']');
        if (close != nullptr) {
            hostBegin = text + 1;
            hostEnd = close;
            if (close[1] == ':') {
                portBegin = close + 2;
            }
        }
    } else {
        const char* colon = std::strrchr(text, ':');
        if (colon != nullptr) {
            portBegin = colon + 1;
            hostEnd = colon;
        }
    }
    if (portBegin != nullptr) {
        bool digits = portBegin[0] != '\0';
        for (const char* p = portBegin; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            strncpy_s(port, portCap, portBegin, _TRUNCATE);
        } else {
            hostEnd = text + std::strlen(text);   // not a port after all
        }
    }
    const size_t hostLen = static_cast<size_t>(hostEnd - hostBegin);
    strncpy_s(host, hostCap, hostBegin, hostLen < hostCap - 1 ? hostLen : hostCap - 1);
}

bool V4IsUnredactable(uint32_t hostOrder) {
    return (hostOrder >> 24) == 127u ||                        // 127.0.0.0/8 loopback
           hostOrder == 0u ||                                  // 0.0.0.0, nowhere to go
           (hostOrder >> 24) == 10u ||                         // 10.0.0.0/8
           (hostOrder & 0xfff00000u) == 0xac100000u ||         // 172.16.0.0/12
           (hostOrder & 0xffff0000u) == 0xc0a80000u ||         // 192.168.0.0/16
           (hostOrder & 0xffc00000u) == 0x64400000u ||         // 100.64.0.0/10 CGNAT
           (hostOrder & 0xffff0000u) == 0xa9fe0000u;           // 169.254.0.0/16 link-local
}

// True when the host is an address that cannot identify anybody's endpoint, so redacting it would
// destroy the only information the record exists to carry.
bool HostIsUnredactable(const char* host) {
    if (host[0] == '\0' || std::strcmp(host, "(null)") == 0 || _stricmp(host, "localhost") == 0 ||
        std::strncmp(host, "family=", 7) == 0) {
        return true;
    }
    in_addr v4;
    if (inet_pton(AF_INET, host, &v4) == 1) {
        return V4IsUnredactable(ntohl(v4.s_addr));
    }
    in6_addr v6;
    if (inet_pton(AF_INET6, host, &v6) == 1) {
        if (IN6_IS_ADDR_V4MAPPED(&v6) != 0) {
            uint32_t mapped = 0;
            std::memcpy(&mapped, v6.s6_addr + 12, sizeof mapped);
            return V4IsUnredactable(ntohl(mapped));
        }
        return IN6_IS_ADDR_LOOPBACK(&v6) != 0 || IN6_IS_ADDR_UNSPECIFIED(&v6) != 0 ||
               (v6.s6_addr[0] & 0xfeu) == 0xfcu ||                                    // fc00::/7 ULA
               (v6.s6_addr[0] == 0xfeu && (v6.s6_addr[1] & 0xc0u) == 0x80u);          // fe80::/10
    }
    return false;   // a DNS name, or a public literal
}

bool HostIsIpLiteral(const char* host) {
    in_addr v4;
    in6_addr v6;
    return inet_pton(AF_INET, host, &v4) == 1 || inet_pton(AF_INET6, host, &v6) == 1;
}

// The last label, when it looks like a TLD. Nothing else about the name survives.
const char* TldOf(const char* host) {
    const char* dot = std::strrchr(host, '.');
    if (dot == nullptr || dot[1] == '\0') {
        return nullptr;
    }
    const char* tld = dot + 1;
    const size_t len = std::strlen(tld);
    if (len > 24) {
        return nullptr;
    }
    for (const char* p = tld; *p != '\0'; ++p) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') || *p == '-';
        if (!ok) {
            return nullptr;
        }
    }
    return tld;
}

}  // namespace

const char* RedactionSalt() {
    return EnsureRedactionSalt() ? g_saltHex : nullptr;
}

bool RedactDestination(const char* text, const char* salt, char* out, size_t outCap) {
    if (out == nullptr || outCap == 0) {
        return false;
    }
    out[0] = '\0';
    if (text == nullptr || salt == nullptr || salt[0] == '\0') {
        return false;   // fail-closed: no salt, no stable token, and never the raw text
    }
    char host[64];
    char port[8];
    SplitHostPort(text, host, sizeof host, port, sizeof port);
    if (HostIsUnredactable(host)) {
        strncpy_s(out, outCap, text, _TRUNCATE);
        return std::strlen(out) == std::strlen(text);
    }

    char lower[64];
    strncpy_s(lower, sizeof lower, host, _TRUNCATE);
    for (char* p = lower; *p != '\0'; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = static_cast<char>(*p - 'A' + 'a');
        }
    }
    Sha256 sha;
    sha.Update(salt, std::strlen(salt));
    sha.Update("", 1);   // the NUL keeps salt|host from colliding with a different split of the same bytes
    sha.Update(lower, std::strlen(lower));
    uint8_t digest[Sha256::kDigestBytes];
    sha.Final(digest);
    char hex[kSha256HexChars];
    Sha256::ToHex(digest, hex);
    hex[8] = '\0';

    const char* tld = HostIsIpLiteral(host) ? nullptr : TldOf(lower);
    const int wrote = _snprintf_s(out, outCap, _TRUNCATE, "redacted-%s%s%s%s%s", hex,
                                  tld != nullptr ? "." : "", tld != nullptr ? tld : "",
                                  port[0] != '\0' ? ":" : "", port);
    return wrote > 0;
}

namespace {

// Sized so overflow is impossible rather than handled: every element is at most the raw text (63),
// the via (15), a 20-digit count and the fixed punctuation.
constexpr size_t kDestJsonCap = kMaxDestinations * 160 + 8;

char g_refusedJson[kDestJsonCap];
char g_allowedJson[kDestJsonCap];

const char* BuildDestJson(bool wantLocal, char* buf, size_t cap) {
    const char* salt = RedactionSalt();
    if (salt == nullptr) {
        g_redactRefusal =
            g_saltRefusal != nullptr
                ? g_saltRefusal
                : "REFUSED: the per-launch redaction salt is unavailable, so no destination token "
                  "would be stable and the raw set must not be emitted in its place";
        return nullptr;
    }
    buf[0] = '\0';
    size_t used = 0;
    EnsureLock();
    EnterCriticalSection(&g.lock);
    const uint32_t count = g.destinationCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        const Destination& d = g.destinations[i];
        if (d.local != wantLocal) {
            continue;
        }
        char dest[96];
        if (!RedactDestination(d.text, salt, dest, sizeof dest)) {
            LeaveCriticalSection(&g.lock);
            g_redactRefusal =
                "REFUSED: a recorded destination could not be redacted, and a set with one raw host in "
                "it is a leak rather than a partial answer";
            return nullptr;
        }
        // The rendering is built from an address, a hex token, a filtered TLD and digits, so it cannot
        // carry a JSON metacharacter. Asserted rather than assumed: a quote here would silently produce
        // a record the harness discards whole.
        if (std::strpbrk(dest, "\"\\") != nullptr) {
            LeaveCriticalSection(&g.lock);
            g_redactRefusal =
                "REFUSED: a redacted destination contained a JSON metacharacter, which the rendering "
                "rule cannot produce -- refusing rather than emitting a record that may not parse";
            return nullptr;
        }
        const int wrote = _snprintf_s(buf + used, cap - used, _TRUNCATE,
                                      "%s{\"dest\": \"%s\", \"via\": \"%s\", \"count\": %llu}",
                                      used == 0 ? "" : ", ", dest, d.via,
                                      static_cast<unsigned long long>(d.count));
        if (wrote <= 0) {
            LeaveCriticalSection(&g.lock);
            g_redactRefusal =
                "REFUSED: the destination set did not fit its buffer. A cut element is invalid JSON and "
                "a cut SET is a false all-clear, so nothing is filed";
            return nullptr;
        }
        used += static_cast<size_t>(wrote);
    }
    LeaveCriticalSection(&g.lock);
    g_redactRefusal = nullptr;
    return buf;
}

}  // namespace

const char* RefusedDestinationsJson() { return BuildDestJson(false, g_refusedJson, kDestJsonCap); }

const char* AllowedDestinationsJson() { return BuildDestJson(true, g_allowedJson, kDestJsonCap); }

const char* RedactionRefusalReason() { return g_redactRefusal; }

void RecordDestinationForTest(const char* text, const char* via, bool local) {
    EnsureLock();
    RecordText(text, via, local);
    if (local) {
        g.allowedLocal.fetch_add(1, std::memory_order_relaxed);
    } else {
        g.refusedRemote.fetch_add(1, std::memory_order_relaxed);
    }
}

void ResetForTest() {
    // Unwinds properly rather than just zeroing counters. Leaving the trampolines installed and the
    // sole-instance handle open made a second Arm() in the same process report Failed - which is the
    // guard behaving correctly, but it made the seam lie about the state it was resetting to.
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (ws2 != nullptr) {
        for (uint32_t i = 0; i < kRowCount; ++i) {
            if (!g.live[i]) {
                continue;
            }
            void* target = reinterpret_cast<void*>(GetProcAddress(ws2, kRows[i].name));
            if (target != nullptr) {
                MH_DisableHook(target);
                MH_RemoveHook(target);
            }
        }
    }
    HMODULE inetModule = GetModuleHandleA("wininet.dll");
    if (inetModule != nullptr) {
        for (uint32_t i = 0; i < kInetRowCount; ++i) {
            if (!g.live[kRowCount + i]) {
                continue;
            }
            void* target = reinterpret_cast<void*>(GetProcAddress(inetModule, kInetRows[i].name));
            if (target != nullptr) {
                MH_DisableHook(target);
                MH_RemoveHook(target);
            }
        }
    }
    if (g.soleInstance != nullptr) {
        ReleaseMutex(g.soleInstance);
        CloseHandle(g.soleInstance);
        g.soleInstance = nullptr;
    }
    g.report = Report{};
    g.destinationCount.store(0);
    g.allowedLocal.store(0);
    g.refusedRemote.store(0);
    g.overflow.store(0);
    g.attempted = false;
    g.enforcing.store(0);
    g.extensionsRefused.store(0);
    g.socketsCreated.store(0);
    g.firstSocketMs.store(0);
    std::memset(g.live, 0, sizeof g.live);
    std::memset(g.destinations, 0, sizeof g.destinations);
    // A second egress init draws a new salt, which is exactly what the per-launch property means
    // and what the two-init control asserts.
    g_saltReady = false;
    g_saltRefusal = nullptr;
    g_redactRefusal = nullptr;
    std::memset(g_saltBytes, 0, sizeof g_saltBytes);
    std::memset(g_saltHex, 0, sizeof g_saltHex);
}

}  // namespace egress
}  // namespace replay
