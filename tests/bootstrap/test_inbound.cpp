// ARCH-18 S6.6, the six tests W1-F owes offline for ledger row 1X.4, plus the two the packet brief
// adds: the ordinal-import case and the retroactive red against L2's recorded bind sequence.
//
// The recorded artifact these are written against is
// launch run 20260903-214703Z, its gamelogs, read read-only:
//
//   [17:47:23]: [NETGAME] Bound HTTP listener to 127.0.0.1:6719, responding to /session
//   [17:47:25]: [NSLOBBY] creating offline session
//   [17:47:25]: [NSLOBBY] attempting to accept 1 player sessions for join request from
//               [100.64.100.100:6894]
//   [17:47:25]: [NSLOBBY] accepting peer [100.64.100.100:6894] into 1 slot(s)
//
// The session's own bind is not logged with an address - the engine never prints one, which is half of
// why this went unseen for so long - but ARCH-18 1.4 byte-verified that it writes sin_addr = 0, and a
// 100.64/10 peer reaching it is the observable consequence. So the sequence a launch presents this hook
// with is: one loopback bind (the free positive control of S6.5) and one wildcard bind.
//
// * A NOTE ON WHY THESE BIND FOR REAL. Every assertion below could have been made against ClassifyBind
//   alone, and a suite that did only that would pass with the trampoline never installed. The socket
//   cases exist so the answer comes back out of the OS through getsockname rather than out of our own
//   classifier agreeing with itself.

#include <cstdint>
#include <cstring>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/hookmgr.h"
#include "bootstrap/inbound.h"
#include "bootstrap/log.h"

namespace {

using replay::inbound::Mode;
using replay::inbound::Outcome;
using replay::inbound::Verdict;

struct WinsockUp {
    WinsockUp()  { WSADATA d{}; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
    ~WinsockUp() { if (ok) { WSACleanup(); } }
    bool ok;
};

sockaddr_in V4(const char* text, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, text, &a.sin_addr);
    return a;
}

// What the OS says this socket is actually bound to, as text. The whole point of reading it back is
// that it is not our own bookkeeping.
std::string BoundAddress(SOCKET s) {
    sockaddr_in got{};
    int len = sizeof got;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&got), &len) != 0) {
        return "<getsockname failed>";
    }
    char text[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &got.sin_addr, text, sizeof text);
    return text;
}

// Binds a UDP socket to `addr` and returns what the OS ended up with. Port 0 throughout: this suite
// must not collide with the engine's 6719, and the address is the whole subject.
std::string BindAndReadBack(const sockaddr_in& addr) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s != INVALID_SOCKET);
    const int rc = bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    REQUIRE(rc == 0);
    const std::string got = BoundAddress(s);
    closesocket(s);
    return got;
}

}  // namespace

TEST_CASE("the arming predicate is the offline lock, never the presence of -offline") {
    // The decision table, all eight cells. R4: an unregistered switch earlier on the engine's command
    // line voids the whole line silently, so a guard keyed on -offline's presence is keyed on what was
    // asked for rather than on what the engine did.
    using replay::inbound::ArmTrigger;
    using replay::inbound::Decide;

    CHECK(Decide(Mode::Auto, true).arm);
    CHECK(Decide(Mode::Auto, true).trigger == ArmTrigger::OfflineLocked);

    // ONLINE PLAY. This is the assertion that proves the plugin does not break the owner's ordinary
    // matches, and it is the one that is easy to omit.
    CHECK_FALSE(Decide(Mode::Auto, false).arm);
    CHECK(Decide(Mode::Auto, false).trigger == ArmTrigger::NotOfflineSession);

    CHECK(Decide(Mode::On, false).arm);
    CHECK(Decide(Mode::On, true).trigger == ArmTrigger::ConfigForcedOn);
    CHECK_FALSE(Decide(Mode::Off, true).arm);
    CHECK(Decide(Mode::Off, true).trigger == ArmTrigger::DisabledByConfig);

    // An unreadable override is not an override. Taking a typo as "off" would leave an offline session
    // reachable while the config file looks like it said something.
    CHECK(replay::inbound::ParseMode("Off") == Mode::Unrecognised);
    CHECK(replay::inbound::ParseMode(nullptr) == Mode::Auto);
    CHECK(Decide(replay::inbound::ParseMode("yes please"), true).arm);
    CHECK(std::string(replay::inbound::ModeName(Mode::Unrecognised)) == "unrecognised");

    // ★ And this is the one that protects the owner's live capture.
    // `net.loopback_only=on` rewrote the live client's binds to loopback and the join died on
    // 2026-09-18. `on` is a request made before the session existed; a live online match is what
    // the session turned out to be, and no config value can make the rewrite safe there. So the
    // session outranks the key -- and it is refused by its own name, not silently folded into
    // "not an offline session", because an operator who set it has to be told it was ignored.
    CHECK_FALSE(Decide(Mode::On, false, true).arm);
    CHECK(Decide(Mode::On, false, true).trigger == ArmTrigger::LiveOnlineRefused);
    CHECK_FALSE(Decide(Mode::On, true, true).arm);          // even with the lock somehow held
    CHECK_FALSE(Decide(Mode::Auto, true, true).arm);        // and auto never arms live either
    CHECK(Decide(Mode::Auto, true, true).trigger == ArmTrigger::NotOfflineSession);
    CHECK(std::string(replay::inbound::ArmTriggerName(ArmTrigger::LiveOnlineRefused)) ==
          "live-online-refused");

    // The control: with the exemption absent, `on` still arms. Without this, the four checks above
    // would pass against a build whose Decide had simply stopped arming at all.
    CHECK(Decide(Mode::On, false, false).arm);
    CHECK(Decide(Mode::On, false, false).trigger == ArmTrigger::ConfigForcedOn);
}

TEST_CASE("a wildcard sockaddr classifies as wildcard and a loopback one does not") {
    const sockaddr_in any = V4("0.0.0.0", 0);
    const sockaddr_in lo = V4("127.0.0.1", 0);
    const sockaddr_in lo8 = V4("127.9.9.9", 0);
    const sockaddr_in lan = V4("192.168.1.5", 0);
    const sockaddr_in cgnat = V4("100.64.100.100", 0);

    CHECK(replay::inbound::IsWildcardBind(&any, sizeof any));
    CHECK_FALSE(replay::inbound::IsWildcardBind(&lo, sizeof lo));
    CHECK(replay::inbound::IsLoopbackBind(&lo, sizeof lo));
    // The whole 127/8 block, not the one literal the engine happens to use.
    CHECK(replay::inbound::IsLoopbackBind(&lo8, sizeof lo8));
    CHECK_FALSE(replay::inbound::IsLoopbackBind(&lan, sizeof lan));
    CHECK_FALSE(replay::inbound::IsLoopbackBind(&cgnat, sizeof cgnat));

    // A length too short to hold the family it claims is neither, and must not be rewritten: a rewrite
    // needs bytes that are not there.
    CHECK_FALSE(replay::inbound::IsWildcardBind(&any, 8));
    CHECK_FALSE(replay::inbound::IsWildcardBind(nullptr, sizeof any));

    sockaddr_in6 any6{};
    any6.sin6_family = AF_INET6;
    any6.sin6_addr = in6addr_any;
    CHECK(replay::inbound::IsWildcardBind(&any6, sizeof any6));
    sockaddr_in6 lo6{};
    lo6.sin6_family = AF_INET6;
    lo6.sin6_addr = in6addr_loopback;
    CHECK(replay::inbound::IsLoopbackBind(&lo6, sizeof lo6));
    CHECK_FALSE(replay::inbound::IsWildcardBind(&lo6, sizeof lo6));
}

TEST_CASE("already-loopback is not rewritten and the caller's sockaddr is left byte-identical") {
    // ARCH-18 S6.6/3. It is also S6.5's free positive control: the engine's own HTTP listener binds the
    // literal 127.0.0.1, so any launch that reaches that code owes at least one already-loopback.
    const sockaddr_in lo = V4("127.0.0.1", 0);
    sockaddr_in copy = lo;
    sockaddr_storage out{};
    std::memset(&out, 0xAB, sizeof out);
    CHECK(replay::inbound::ClassifyBind(&copy, sizeof copy, &out, sizeof out) ==
          Outcome::AlreadyLoopback);
    CHECK(std::memcmp(&copy, &lo, sizeof lo) == 0);
    // And `out` was not written either, so a caller that passed it on regardless would be passing on
    // uninitialised bytes rather than a silently different address.
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(&out);
    CHECK(raw[0] == 0xAB);

    // The rewrite writes the COPY and still leaves the caller's own buffer alone. The engine memsets
    // and reuses that struct, so mutating it would change what the game believes it asked for.
    sockaddr_in any = V4("0.0.0.0", 0);
    const sockaddr_in anyBefore = any;
    CHECK(replay::inbound::ClassifyBind(&any, sizeof any, &out, sizeof out) == Outcome::Rewritten);
    CHECK(std::memcmp(&any, &anyBefore, sizeof any) == 0);
    CHECK(replay::inbound::IsLoopbackBind(&out, sizeof(sockaddr_in)));
    // The PORT survives the rewrite untouched, which is what keeps the engine's own getsockname
    // read-back (ARCH-18 1.7) telling it the truth.
    sockaddr_in withPort = V4("0.0.0.0", 0);
    // Derived rather than typed. Any registry port number spelled here would be a transcription
    // carrying a meaning this test never intended, and the subject is only that the field survives.
    withPort.sin_port = static_cast<uint16_t>(GetCurrentProcessId());
    const uint16_t portBefore = withPort.sin_port;
    REQUIRE(replay::inbound::ClassifyBind(&withPort, sizeof withPort, &out, sizeof out) ==
            Outcome::Rewritten);
    CHECK(reinterpret_cast<const sockaddr_in*>(&out)->sin_port == portBefore);
}

TEST_CASE("a family we do not handle passes through unmodified and nothing is ever refused") {
    // ARCH-18 S6.6/4, and S4's A2 rejection: refusing a bind stops the in-client host from starting,
    // which breaks locked constraint 2. Passed unmodified and RECORDED, never refused.
    sockaddr_storage out{};
    sockaddr other{};
    other.sa_family = AF_IRDA;
    CHECK(replay::inbound::ClassifyBind(&other, sizeof other, &out, sizeof out) ==
          Outcome::PassedThrough);
    CHECK(replay::inbound::ClassifyBind(nullptr, 0, &out, sizeof out) == Outcome::PassedThrough);
    // A wildcard we cannot rewrite because the output has nowhere to go still passes through. It never
    // becomes a refusal and it never becomes a silent half-write.
    const sockaddr_in any = V4("0.0.0.0", 0);
    CHECK(replay::inbound::ClassifyBind(&any, sizeof any, &out, 4) == Outcome::PassedThrough);
    // A real non-loopback interface address is passed through rather than moved: the caller named an
    // interface. ARCH-18 S6.4 makes it visible anyway - the harness classifies the bound endpoint and
    // any non-loopback entry is asked-fail - so it is a finding rather than something this hook hides.
    const sockaddr_in lan = V4("192.168.1.5", 0);
    CHECK(replay::inbound::ClassifyBind(&lan, sizeof lan, &out, sizeof out) == Outcome::PassedThrough);
}

TEST_CASE("armed: a wildcard bind reaches the OS as loopback; not armed: it does not") {
    // ARCH-18 S6.6/1, BOTH directions, read back out of getsockname rather than out of our counters.
    WinsockUp ws;
    REQUIRE(ws.ok);

    // NOT ARMED FIRST, and deliberately in that order: if this ran after the armed case and the reset
    // failed to unhook, the assertion would still pass for the wrong reason. Running it first means the
    // trampoline provably does not exist yet.
    replay::inbound::ResetForTest();
    CHECK(BindAndReadBack(V4("0.0.0.0", 0)) == "0.0.0.0");
    CHECK(replay::inbound::Current().rewritten == 0);
    CHECK_FALSE(replay::inbound::Current().armed);

    REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::OfflineLocked, Mode::Auto,
                                 "the test") == replay::inbound::ArmState::Armed);
    CHECK(replay::inbound::Current().armed);
    CHECK(replay::inbound::Current().hooksLive == replay::inbound::Current().hooksRequested);

    CHECK(BindAndReadBack(V4("0.0.0.0", 0)) == "127.0.0.1");
    CHECK(replay::inbound::Current().rewritten == 1);

    // And the reverse direction again, through a real ResetForTest, because "we did not break online
    // play" is the assertion that has to survive the hook having existed.
    replay::inbound::ResetForTest();
    CHECK(BindAndReadBack(V4("0.0.0.0", 0)) == "0.0.0.0");
    CHECK(replay::inbound::Current().rewritten == 0);
}

TEST_CASE("the hook fires for a bind reached through the ordinal IAT slot") {
    // ARCH-18 1.9: echovr.exe imports bind BY ORDINAL (2), so an IAT-by-name hook would miss every one
    // of the engine's binds while reporting itself armed. MinHook rewrites the ws2_32 prologue in
    // place, so a pointer the loader resolved from the ordinal lands on the same trampoline. This
    // resolves it the way the loader would and calls through it.
    WinsockUp ws;
    REQUIRE(ws.ok);
    replay::inbound::ResetForTest();

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    REQUIRE(ws2 != nullptr);
    using BindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    BindFn byOrdinal = reinterpret_cast<BindFn>(GetProcAddress(ws2, MAKEINTRESOURCEA(2)));
    REQUIRE(byOrdinal != nullptr);
    // The control that makes the case mean something: ordinal 2 really is bind, so a hook installed on
    // the by-name address is on the same bytes.
    CHECK(reinterpret_cast<void*>(byOrdinal) ==
          reinterpret_cast<void*>(GetProcAddress(ws2, "bind")));

    REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::OfflineLocked, Mode::Auto,
                                 "the test") == replay::inbound::ArmState::Armed);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s != INVALID_SOCKET);
    const sockaddr_in any = V4("0.0.0.0", 0);
    REQUIRE(byOrdinal(s, reinterpret_cast<const sockaddr*>(&any), sizeof any) == 0);
    CHECK(BoundAddress(s) == "127.0.0.1");
    CHECK(replay::inbound::Current().rewritten == 1);
    closesocket(s);
    replay::inbound::ResetForTest();
}

TEST_CASE("every bind lands in exactly one counter and refused stays zero") {
    // ARCH-18 S6.5's four buckets, driven through the trampoline rather than through ClassifyBind, so
    // the counters the launch record publishes are the ones under test.
    WinsockUp ws;
    REQUIRE(ws.ok);
    replay::inbound::ResetForTest();
    REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::ConfigForcedOn, Mode::On,
                                 "the test") == replay::inbound::ArmState::Armed);

    const sockaddr_in lo = V4("127.0.0.1", 0);
    SOCKET a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(a != INVALID_SOCKET);
    CHECK(bind(a, reinterpret_cast<const sockaddr*>(&lo), sizeof lo) == 0);
    closesocket(a);

    const sockaddr_in any = V4("0.0.0.0", 0);
    SOCKET b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(b != INVALID_SOCKET);
    CHECK(bind(b, reinterpret_cast<const sockaddr*>(&any), sizeof any) == 0);
    closesocket(b);

    // A namelen too short for any family: the OS refuses it, and the point is that WE did not - the
    // bind still reached ws2_32 unmodified and was counted rather than dropped.
    SOCKET c = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(c != INVALID_SOCKET);
    CHECK(bind(c, reinterpret_cast<const sockaddr*>(&any), 4) == SOCKET_ERROR);
    closesocket(c);

    const replay::inbound::Report& r = replay::inbound::Current();
    CHECK(r.rewritten == 1);
    CHECK(r.alreadyLoopback == 1);
    CHECK(r.passedThrough == 1);
    // The falsifiable half of "this module never refuses a bind". ARCH-18 rejected A2 because refusing
    // stops the in-client host from starting; a non-zero here means somebody built it anyway.
    CHECK(r.refused == 0);
    replay::inbound::ResetForTest();
}

TEST_CASE("arming ws2_32!bind declares no SC7 site") {
    // ARCH-18 S6.2, the offline half of S6.6/6. SC7 diffs echovr.exe's .text and reads its declared
    // sites off hookmgr's registrations; this row patches ws2_32's .text and registers nothing, so
    // declared_sites cannot move and unattributed[] cannot gain an entry. The launch-time half - the
    // full .text diff with 1X.4 installed - is L2 #2's, and it is named in the report as such.
    WinsockUp ws;
    REQUIRE(ws.ok);
    replay::inbound::ResetForTest();
    const size_t sitesBefore = replay::hookmgr::Count();
    REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::OfflineLocked, Mode::Auto,
                                 "the test") == replay::inbound::ArmState::Armed);
    CHECK(replay::hookmgr::Count() == sitesBefore);
    CHECK(replay::hookmgr::ArmedCount() == 0);
    replay::inbound::ResetForTest();
}

TEST_CASE("the four-state verdict: a wildcard bind alone is asked-fail, no peer required") {
    using replay::inbound::Classify;
    using replay::inbound::VerdictInput;

    // Online play. Not armed is NEVER-ASKED and it is not a pass - which is the whole reason two states
    // cannot carry this row.
    VerdictInput online{};
    online.armed = false;
    CHECK(Classify(online) == Verdict::NeverAsked);

    VerdictInput clean{};
    clean.armed = true;
    clean.endpointsRead = true;
    clean.alreadyLoopback = 1;
    clean.rewritten = 1;
    CHECK(Classify(clean) == Verdict::AskedPass);

    // ANTI-VACUITY. The same clean table on a launch where the hook never fired is indistinguishable
    // from a launch that bound nothing, so it is could-not-ask rather than a pass.
    VerdictInput neverFired = clean;
    neverFired.rewritten = 0;
    neverFired.alreadyLoopback = 0;
    CHECK(Classify(neverFired) == Verdict::CouldNotAsk);

    VerdictInput noReading = clean;
    noReading.endpointsRead = false;
    CHECK(Classify(noReading) == Verdict::CouldNotAsk);

    // A wildcard bound endpoint with nobody connecting IS the violation.
    VerdictInput wildcard = clean;
    wildcard.boundNonLoopback = 1;
    CHECK(Classify(wildcard) == Verdict::AskedFail);

    VerdictInput peer = clean;
    peer.inboundNonLoopback = 1;
    CHECK(Classify(peer) == Verdict::AskedFail);

    // And a wildcard bind whose counters are also zero is a FAILURE, not an instrument problem. The
    // ordering here is the assertion: could-not-ask would let the one case this row exists for read as
    // "we could not tell".
    VerdictInput both = wildcard;
    both.rewritten = 0;
    both.alreadyLoopback = 0;
    both.endpointsRead = false;
    CHECK(Classify(both) == Verdict::AskedFail);
}

TEST_CASE("RETROACTIVE RED: L2's recorded bind sequence is asked-fail unfixed and asked-pass fixed") {
    // The sequence out of launch run 20260903-214703Z\'s gamelogs, in the order the
    // launch produced it: the HTTP listener's loopback bind at 17:47:23, then the offline session's
    // bind at 17:47:25, then a join accepted from [100.64.100.100:6894]. A check first seen green
    // proves nothing, so this runs the OLD code path first and requires it to fail.
    WinsockUp ws;
    REQUIRE(ws.ok);

    const sockaddr_in httpListener = V4("127.0.0.1", 0);   // the logged HTTP listener, loopback
    const sockaddr_in sessionBind = V4("0.0.0.0", 0);      // ARCH-18 1.4: sin_addr written 0
    const uint32_t recordedInboundPeers = 1;               // the accepted 100.64/10 mesh peer

    // ---- the old code path: nothing installed, exactly as the recorded launch ran ----
    replay::inbound::ResetForTest();
    SOCKET oldHttp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET oldSession = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(oldHttp != INVALID_SOCKET);
    REQUIRE(oldSession != INVALID_SOCKET);
    REQUIRE(bind(oldHttp, reinterpret_cast<const sockaddr*>(&httpListener), sizeof httpListener) == 0);
    REQUIRE(bind(oldSession, reinterpret_cast<const sockaddr*>(&sessionBind), sizeof sessionBind) == 0);
    const std::string oldHttpAddr = BoundAddress(oldHttp);
    const std::string oldSessionAddr = BoundAddress(oldSession);
    closesocket(oldHttp);
    closesocket(oldSession);

    CHECK(oldHttpAddr == "127.0.0.1");
    // This is the byte ARCH-18 1.4 verified and the reason a mesh peer could arrive.
    CHECK(oldSessionAddr == "0.0.0.0");

    replay::inbound::VerdictInput before{};
    before.armed = true;   // graded as though 1X.4 had been armed on that launch - which is the point:
                           // this is what the new gate says about the recorded artifact
    before.endpointsRead = true;
    before.boundNonLoopback = (oldSessionAddr == "0.0.0.0") ? 1u : 0u;
    before.inboundNonLoopback = recordedInboundPeers;
    before.rewritten = replay::inbound::Current().rewritten;
    before.alreadyLoopback = replay::inbound::Current().alreadyLoopback;
    CHECK(before.rewritten + before.alreadyLoopback == 0);
    CHECK(replay::inbound::Classify(before) == Verdict::AskedFail);

    // ---- the new code path: the same two binds, in the same order, with 1X.4 armed ----
    REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::OfflineLocked, Mode::Auto,
                                 "the test") == replay::inbound::ArmState::Armed);
    SOCKET newHttp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET newSession = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(newHttp != INVALID_SOCKET);
    REQUIRE(newSession != INVALID_SOCKET);
    REQUIRE(bind(newHttp, reinterpret_cast<const sockaddr*>(&httpListener), sizeof httpListener) == 0);
    REQUIRE(bind(newSession, reinterpret_cast<const sockaddr*>(&sessionBind), sizeof sessionBind) == 0);
    const std::string newHttpAddr = BoundAddress(newHttp);
    const std::string newSessionAddr = BoundAddress(newSession);
    closesocket(newHttp);
    closesocket(newSession);

    CHECK(newHttpAddr == "127.0.0.1");
    CHECK(newSessionAddr == "127.0.0.1");

    const replay::inbound::Report& r = replay::inbound::Current();
    CHECK(r.rewritten == 1);
    CHECK(r.alreadyLoopback == 1);   // the HTTP listener, S6.5's free positive control

    replay::inbound::VerdictInput after{};
    after.armed = true;
    after.endpointsRead = true;
    after.boundNonLoopback = (newSessionAddr == "0.0.0.0") ? 1u : 0u;
    after.inboundNonLoopback = 0;    // no endpoint left off-machine for that peer to have reached
    after.rewritten = r.rewritten;
    after.alreadyLoopback = r.alreadyLoopback;
    CHECK(replay::inbound::Classify(after) == Verdict::AskedPass);

    replay::inbound::ResetForTest();
}

// ---------------------------------------------------------------------------------------------------
// L2-RUN-2 F4: the hook armed on L2 #2 and its counters appear in no log line, so that run establishes
// nothing about whether a wildcard bind was ever rewritten. These cases are about the LINE - that it is
// emitted, that it carries the counts a launch would need, and that its verdict word moves.

namespace {

std::wstring ReportLogPath(const wchar_t* tag) {
    std::wstring dir(MAX_PATH, L' ');
    dir.resize(GetTempPathW(MAX_PATH, &dir[0]));
    std::wstring path(dir);
    path += L"echoreplay_";
    path += tag;
    path += L"_";
    path += std::to_wstring(GetCurrentProcessId());
    path += L".log";
    return path;
}

std::string ReadAll(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    // Sized from the file, not from a fixed buffer: several reports land in one of these logs and a
    // truncating read would silently answer a question about the LAST line with the first 8 KB.
    LARGE_INTEGER size{};
    std::string body;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
        body.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ReadFile(file, &body[0], static_cast<DWORD>(body.size()), &read, nullptr);
        body.resize(read);
    }
    CloseHandle(file);
    return body;
}

// The WHOLE line for one occasion. Asserting on substrings of the file body would let a count from one
// report satisfy an assertion written about another - substring matching is the commonest false pass.
std::string ReportLine(const std::string& body, const std::string& occasion) {
    const std::string key = "loopback-only report (" + occasion + "):";
    const size_t at = body.find(key);
    if (at == std::string::npos) {
        return std::string();
    }
    const size_t end = body.find("\r\n", at);
    return body.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The LAST line for an occasion. The heartbeat repeats, and the claim worth making about it is that
// a later one carries counts an earlier one could not - which the first match cannot answer.
std::string LastReportLine(const std::string& body, const std::string& occasion) {
    const std::string key = "loopback-only report (" + occasion + "):";
    const size_t at = body.rfind(key);
    if (at == std::string::npos) {
        return std::string();
    }
    const size_t end = body.find("\r\n", at);
    return body.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

size_t CountOf(const std::string& body, const std::string& needle) {
    size_t n = 0;
    size_t at = 0;
    while ((at = body.find(needle, at)) != std::string::npos) {
        ++n;
        at += needle.size();
    }
    return n;
}

struct LogToFile {
    explicit LogToFile(const wchar_t* tag) : path(ReportLogPath(tag)) {
        DeleteFileW(path.c_str());
        replay::log::RedirectForTest(path.c_str());
        replay::log::SetLevel(replay::LogLevel::Info);
    }
    ~LogToFile() {
        replay::log::EndRedirectForTest();
        DeleteFileW(path.c_str());
    }
    std::string Body() const { return ReadAll(path); }
    std::wstring path;
};

}  // namespace

TEST_CASE("a named non-loopback bind is separated from the shapes we merely do not handle") {
    sockaddr_in lan{};
    lan.sin_family = AF_INET;
    lan.sin_port = htons(1);
    REQUIRE(inet_pton(AF_INET, "192.0.2.1", &lan.sin_addr) == 1);
    CHECK(replay::inbound::IsNamedNonLoopbackBind(&lan, sizeof lan));

    sockaddr_in loop = lan;
    REQUIRE(inet_pton(AF_INET, "127.0.0.5", &loop.sin_addr) == 1);
    CHECK_FALSE(replay::inbound::IsNamedNonLoopbackBind(&loop, sizeof loop));

    sockaddr_in wild = lan;
    wild.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK_FALSE(replay::inbound::IsNamedNonLoopbackBind(&wild, sizeof wild));

    // A short length and a family we never handle are passed through too, but they are not an exposure
    // and must not be able to drive the line's verdict to asked-fail.
    CHECK_FALSE(replay::inbound::IsNamedNonLoopbackBind(&lan, static_cast<int>(sizeof lan) - 1));
    sockaddr_in odd = lan;
    odd.sin_family = AF_IRDA;
    CHECK_FALSE(replay::inbound::IsNamedNonLoopbackBind(&odd, sizeof odd));
    CHECK_FALSE(replay::inbound::IsNamedNonLoopbackBind(nullptr, sizeof lan));
}

TEST_CASE("the report line is emitted on the first bind and carries that bind's counts") {
    WinsockUp ws;
    REQUIRE(ws.ok);
    replay::inbound::ResetForTest();
    LogToFile log(L"w1f2first");

    REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::OfflineLocked, Mode::Auto,
                                 "a unit test") == replay::inbound::ArmState::Armed);

    // Armed and nothing bound yet. This is the anti-vacuity arm: an armed hook that saw no bind is
    // indistinguishable from one whose trampoline was never reached, so it is not a pass.
    replay::inbound::LogReport("no-binds");
    const std::string quiet = ReportLine(log.Body(), "no-binds");
    CHECK(quiet.find("armed=true") != std::string::npos);
    CHECK(quiet.find("trigger=offline-lock-locked") != std::string::npos);
    CHECK(quiet.find("rewritten=0 alreadyLoopback=0 passedThrough=0 namedNonLoopback=0 refused=0")
          != std::string::npos);
    CHECK(quiet.find("armedAtMs=") != std::string::npos);
    CHECK(quiet.find("verdict=could-not-ask") != std::string::npos);
    CHECK(quiet.find("net.loopback_only=auto from a unit test") != std::string::npos);

    // One wildcard bind, through the live trampoline and through a real socket.
    sockaddr_in wild{};
    wild.sin_family = AF_INET;
    wild.sin_addr.s_addr = htonl(INADDR_ANY);
    wild.sin_port = 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s != INVALID_SOCKET);
    REQUIRE(bind(s, reinterpret_cast<const sockaddr*>(&wild), sizeof wild) == 0);
    CHECK(BoundAddress(s) == "127.0.0.1");

    const std::string first = ReportLine(log.Body(), "first-bind");
    // The count has to be IN the first-bind line: a line emitted before the bind was classified would
    // say rewritten=0 and could-not-ask, which is exactly the "armed and unmeasured" state F4 records.
    CHECK(first.find("rewritten=1 alreadyLoopback=0 passedThrough=0 namedNonLoopback=0 refused=0")
          != std::string::npos);
    CHECK(first.find("armed=true") != std::string::npos);
    CHECK(first.find("verdict=asked-pass") != std::string::npos);

    // Once. A line per bind would put thousands of them in a launch log.
    SOCKET s2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s2 != INVALID_SOCKET);
    REQUIRE(bind(s2, reinterpret_cast<const sockaddr*>(&wild), sizeof wild) == 0);
    CHECK(CountOf(log.Body(), "loopback-only report (first-bind):") == 1);

    // ... and the teardown line then carries the SECOND bind too, which is the point of having one.
    replay::inbound::LogReport("teardown");
    const std::string down = ReportLine(log.Body(), "teardown");
    CHECK(down.find("rewritten=2 alreadyLoopback=0 passedThrough=0 namedNonLoopback=0 refused=0")
          != std::string::npos);
    CHECK(down.find("verdict=asked-pass") != std::string::npos);

    closesocket(s);
    closesocket(s2);
    replay::inbound::ResetForTest();
}

TEST_CASE("the line's verdict reaches asked-fail on a named interface bind, and never-asked unarmed") {
    WinsockUp ws;
    REQUIRE(ws.ok);
    replay::inbound::ResetForTest();

    {
        LogToFile log(L"w1f2fail");
        REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::ConfigForcedOn, Mode::On,
                                     "a unit test") == replay::inbound::ArmState::Armed);

        sockaddr_in wild{};
        wild.sin_family = AF_INET;
        wild.sin_addr.s_addr = htonl(INADDR_ANY);
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(s != INVALID_SOCKET);
        REQUIRE(bind(s, reinterpret_cast<const sockaddr*>(&wild), sizeof wild) == 0);

        // RFC 5737 TEST-NET-1: no machine owns it, so the real bind fails - but the detour classified
        // and counted it first, which is the whole claim. The caller named an interface and ARCH-18
        // decision 4 passes it through rather than moving it, so the RECORD has to show it.
        sockaddr_in named{};
        named.sin_family = AF_INET;
        named.sin_port = 0;
        REQUIRE(inet_pton(AF_INET, "192.0.2.1", &named.sin_addr) == 1);
        SOCKET s2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(s2 != INVALID_SOCKET);
        bind(s2, reinterpret_cast<const sockaddr*>(&named), sizeof named);

        replay::inbound::LogReport("teardown");
        const std::string down = ReportLine(log.Body(), "teardown");
        CHECK(down.find("rewritten=1 alreadyLoopback=0 passedThrough=1 namedNonLoopback=1 refused=0")
              != std::string::npos);
        // Classify's order is load-bearing: a non-loopback bind outranks a successful rewrite, so this
        // says asked-fail even though rewritten is 1.
        CHECK(down.find("verdict=asked-fail") != std::string::npos);
        CHECK(down.find("verdict=asked-pass") == std::string::npos);

        closesocket(s);
        closesocket(s2);
        replay::inbound::ResetForTest();
    }

    {
        LogToFile log(L"w1f2never");
        replay::inbound::RecordNotArmed(replay::inbound::ArmTrigger::NotOfflineSession, Mode::Auto,
                                        "a unit test");
        replay::inbound::LogReport("teardown");
        const std::string down = ReportLine(log.Body(), "teardown");
        CHECK(down.find("armed=false") != std::string::npos);
        CHECK(down.find("trigger=not-an-offline-session") != std::string::npos);
        // Online play is never-asked, not a pass and not a failure.
        CHECK(down.find("verdict=never-asked") != std::string::npos);
        replay::inbound::ResetForTest();
    }
}

// L2-RUN-3 F7. The two occasions that exist because `teardown` does not survive a kill. Both are
// asserted on the WHOLE line for their own occasion, and both are asserted to carry a NON-ZERO
// rewritten count taken through the live trampoline: a line that prints rewritten=0 whatever happened
// is a line that cannot fail.
TEST_CASE("close-request carries the counts at the stop, and the heartbeat keeps its cadence") {
    WinsockUp ws;
    REQUIRE(ws.ok);
    replay::inbound::ResetForTest();
    LogToFile log(L"w1f3close");

    REQUIRE(replay::inbound::Arm(replay::inbound::ArmTrigger::OfflineLocked, Mode::Auto,
                                 "a unit test") == replay::inbound::ArmState::Armed);

    sockaddr_in wild{};
    wild.sin_family = AF_INET;
    wild.sin_addr.s_addr = htonl(INADDR_ANY);
    wild.sin_port = 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s != INVALID_SOCKET);
    REQUIRE(bind(s, reinterpret_cast<const sockaddr*>(&wild), sizeof wild) == 0);
    CHECK(BoundAddress(s) == "127.0.0.1");

    // The seam W2-A calls. It is what the capture stop path reaches, and it is the occasion a killed
    // launch actually leaves behind.
    replay::inbound::NoteCaptureCloseServiced();
    const std::string closeLine = ReportLine(log.Body(), "close-request");
    CHECK(closeLine.find("armed=true") != std::string::npos);
    CHECK(closeLine.find("trigger=offline-lock-locked") != std::string::npos);
    CHECK(closeLine.find("rewritten=1 alreadyLoopback=0 passedThrough=0 namedNonLoopback=0 refused=0")
          != std::string::npos);
    CHECK(closeLine.find("verdict=asked-pass") != std::string::npos);
    // One per serviced stop, and the three occasions do not collide: a reader keying on the word must
    // not find a heartbeat where it asked for a close.
    CHECK(CountOf(log.Body(), "loopback-only report (close-request):") == 1);
    CHECK(CountOf(log.Body(), "loopback-only report (first-bind):") == 1);
    CHECK(CountOf(log.Body(), "loopback-only report (heartbeat):") == 0);

    // Cadence, on injected stamps, so it is exact rather than a minute of waiting.
    const uint64_t t = 1000000;
    CHECK_FALSE(replay::inbound::MaybeLogHeartbeat(t));            // the baseline never logs
    CHECK_FALSE(replay::inbound::MaybeLogHeartbeat(t + replay::inbound::kHeartbeatIntervalMs - 1));
    CHECK(CountOf(log.Body(), "loopback-only report (heartbeat):") == 0);
    CHECK(replay::inbound::MaybeLogHeartbeat(t + replay::inbound::kHeartbeatIntervalMs));
    CHECK(CountOf(log.Body(), "loopback-only report (heartbeat):") == 1);
    // Rescheduled from the tick that fired: the same instant does not fire a second time, and the
    // poll loop's 250 ms ticks in between are silent.
    CHECK_FALSE(replay::inbound::MaybeLogHeartbeat(t + replay::inbound::kHeartbeatIntervalMs));
    CHECK_FALSE(replay::inbound::MaybeLogHeartbeat(t + 2 * replay::inbound::kHeartbeatIntervalMs - 1));
    CHECK(replay::inbound::MaybeLogHeartbeat(t + 2 * replay::inbound::kHeartbeatIntervalMs));
    CHECK(CountOf(log.Body(), "loopback-only report (heartbeat):") == 2);

    // A bind after the last heartbeat, and the NEXT one carries it. This is the whole point of the
    // occasion: what a killed launch leaves on disk is at most one interval out of date.
    SOCKET s2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s2 != INVALID_SOCKET);
    REQUIRE(bind(s2, reinterpret_cast<const sockaddr*>(&wild), sizeof wild) == 0);
    CHECK(replay::inbound::MaybeLogHeartbeat(t + 3 * replay::inbound::kHeartbeatIntervalMs));
    const std::string beat = LastReportLine(log.Body(), "heartbeat");
    CHECK(beat.find("rewritten=2 alreadyLoopback=0 passedThrough=0 namedNonLoopback=0 refused=0")
          != std::string::npos);
    CHECK(beat.find("verdict=asked-pass") != std::string::npos);
    // ... and the earlier one still says what it saw at the time. A heartbeat that were rewritten in
    // place would make the cadence pointless.
    CHECK(ReportLine(log.Body(), "heartbeat").find("rewritten=1 ") != std::string::npos);

    closesocket(s);
    closesocket(s2);
    replay::inbound::ResetForTest();
}

TEST_CASE("the heartbeat is silent unarmed, and close-request still says so") {
    replay::inbound::ResetForTest();
    LogToFile log(L"w1f3quiet");

    replay::inbound::RecordNotArmed(replay::inbound::ArmTrigger::NotOfflineSession, Mode::Auto,
                                    "a unit test");
    const uint64_t t = 1000000;
    CHECK_FALSE(replay::inbound::MaybeLogHeartbeat(t));
    CHECK_FALSE(replay::inbound::MaybeLogHeartbeat(t + 10 * replay::inbound::kHeartbeatIntervalMs));
    CHECK(CountOf(log.Body(), "loopback-only report (heartbeat):") == 0);

    // Online play must not fill the log with a line a minute. But an occasion somebody ASKED for still
    // prints, unarmed, so absence of a close-request line is never read as "the hook was off".
    replay::inbound::NoteCaptureCloseServiced();
    const std::string closeLine = ReportLine(log.Body(), "close-request");
    CHECK(closeLine.find("armed=false") != std::string::npos);
    CHECK(closeLine.find("trigger=not-an-offline-session") != std::string::npos);
    CHECK(closeLine.find("verdict=never-asked") != std::string::npos);

    replay::inbound::ResetForTest();
}
