// W2-G took -replayviewer off the engine command line (a2ad110) and moved the launch request into the
// ECHOREPLAY_OPEN environment variable, because an unregistered switch makes ApplyCommandLine stop at
// it and silently discard -offline, -level, -region and the four host pins that follow.
//
// DetectSessionMode still keyed SessionMode::Replay on that switch. Two failures in one:
//   - the Replay branch became unreachable, so a replay launch fell through to LiveOnlineMatch, which is
//     the ONLY classification that disarms the egress blocker;
//   - and had the switch ever been present, the classification would have been read off a command line
//     the engine had already thrown away.
//
// These cases pin both directions. The suite drives ClassifySession, which is pure, so every outcome is
// reachable without a game, an environment, or a loaded rlay.dll.

#include <cstring>
#include <cwchar>
#include <string>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/config.h"
#include "bootstrap/inbound.h"
#include "control/launchargs.h"

using namespace replay::bootstrap;
using replay::control::kOpenEnvVar;

namespace {

// The three containment gates as the worker decides them, from the same pure inputs the worker uses.
// It reads back exactly what bootstrap.cpp computes: the egress blocker disarms iff the session is a
// LiveOnlineMatch; the offline-bit lock is applied iff the session is NOT live-online-exempt; and the
// loopback-only bind rewrite (net.loopback_only=auto) arms iff the offline lock ended up Locked - which
// it does exactly when the lock was applied. Mirrored here so a discriminating test can assert all three
// at once for a whole session rather than one predicate in isolation.
struct GateDecision {
    bool egressArmed;
    bool offlineLockApplied;
    bool loopbackArmed;
};

GateDecision GatesFor(SessionMode session, bool offlineSwitchSeen) {
    const bool exempt = IsLiveOnlineExempt(session, offlineSwitchSeen);
    GateDecision g{};
    g.egressArmed = session != SessionMode::LiveOnlineMatch;
    g.offlineLockApplied = !exempt;
    // In the real game the lock reaches Locked whenever it is applied, so offlineLocked == applied.
    const bool offlineLocked = g.offlineLockApplied;
    g.loopbackArmed = replay::inbound::Decide(replay::inbound::Mode::Auto, offlineLocked).arm;
    return g;
}

}  // namespace

TEST_CASE("the launch request is the environment variable, not a switch") {
    CHECK(ClassifySession(L"echovr.exe", true, false) == SessionMode::Replay);
    CHECK(ClassifySession(L"echovr.exe -offline", true, true) == SessionMode::Replay);
}

TEST_CASE("no launch request and no offline hint is a live online match") {
    // The one classification that DISARMS the blocker, so it has to be reachable only on purpose.
    CHECK(ClassifySession(L"echovr.exe", false, false) == SessionMode::LiveOnlineMatch);
    CHECK(ClassifySession(L"echovr.exe -windowed", false, true) == SessionMode::LiveOnlineMatch);
}

TEST_CASE("a stale -replayviewer is a refusal, never a replay and never a live match") {
    const SessionMode bare = ClassifySession(L"echovr.exe -replayviewer", false, false);
    CHECK(bare == SessionMode::RefusedStaleSwitch);
    CHECK(bare != SessionMode::Replay);
    CHECK(bare != SessionMode::LiveOnlineMatch);

    SUBCASE("it wins over the environment variable, because the line is still voided") {
        // Both can be true - a stale launcher that also learned the new carrier. The switch is still on
        // the line, so -offline and the pins were still discarded, and that is the more dangerous fact.
        CHECK(ClassifySession(L"echovr.exe -replayviewer x.echodemo", true, false) ==
              SessionMode::RefusedStaleSwitch);
    }

    SUBCASE("it wins over a credible -offline, because that -offline never reached the engine") {
        // Without this arm the session reads as Offline: a claim that there is no live match, made from
        // a switch the engine discarded before it got to it.
        REQUIRE(ClassifySession(L"echovr.exe -offline", false, true) == SessionMode::Offline);
        CHECK(ClassifySession(L"echovr.exe -offline -replayviewer", false, true) ==
              SessionMode::RefusedStaleSwitch);
    }

    SUBCASE("the match is a whole token, not a substring") {
        CHECK(ClassifySession(L"echovr.exe -replayviewerx", false, false) ==
              SessionMode::LiveOnlineMatch);
        CHECK(ClassifySession(L"echovr.exe -noreplayviewer", false, false) ==
              SessionMode::LiveOnlineMatch);
        CHECK(ClassifySession(L"echovr.exe -replayviewer=x", false, false) ==
              SessionMode::RefusedStaleSwitch);
    }

    SUBCASE("it names itself in the log, and the name is not one of the session kinds") {
        const std::string name = SessionModeName(SessionMode::RefusedStaleSwitch);
        CHECK(name.find("refused") != std::string::npos);
        CHECK(name.find("-replayviewer") != std::string::npos);
        CHECK(name != SessionModeName(SessionMode::Replay));
        CHECK(name != SessionModeName(SessionMode::LiveOnlineMatch));
    }
}

TEST_CASE("an unreadable command line makes no claim") {
    CHECK(ClassifySession(nullptr, false, false) == SessionMode::Unknown);
    CHECK(ClassifySession(L"", false, true) == SessionMode::Unknown);

    SUBCASE("but a launch request set in the environment is still a reading") {
        CHECK(ClassifySession(nullptr, true, false) == SessionMode::Replay);
    }
}

TEST_CASE("an empty ECHOREPLAY_OPEN is absent, and the two Win32 spellings disagree about that") {
    // The classification hangs on this and it is not obvious. GetEnvironmentVariableW answers the
    // character count EXCLUDING the terminator when the value fits the buffer it was given - so an empty
    // value and an absent one are both 0 - but the (nullptr, 0) form answers the size a buffer WOULD
    // need, which is 1 for an empty value and 0 only when the variable does not exist. Written the second
    // way, `ECHOREPLAY_OPEN=` classifies as a launch request. Measured here rather than believed.
    wchar_t saved[512];
    const DWORD had = GetEnvironmentVariableW(kOpenEnvVar, saved, _countof(saved));

    SUBCASE("absent") {
        SetEnvironmentVariableW(kOpenEnvVar, nullptr);
        CHECK(GetEnvironmentVariableW(kOpenEnvVar, nullptr, 0) == 0);
        CHECK_FALSE(OpenRequestPresent());
        CHECK(ClassifySession(L"echovr.exe", OpenRequestPresent(), false) ==
              SessionMode::LiveOnlineMatch);
    }

    SUBCASE("set to nothing is not a launch request") {
        REQUIRE(SetEnvironmentVariableW(kOpenEnvVar, L"") != FALSE);
        // The trap, pinned: the size-query form says 1 here, which is why the predicate does not use it.
        CHECK(GetEnvironmentVariableW(kOpenEnvVar, nullptr, 0) == 1);
        CHECK_FALSE(OpenRequestPresent());
        CHECK(ClassifySession(L"echovr.exe", OpenRequestPresent(), false) ==
              SessionMode::LiveOnlineMatch);
    }

    SUBCASE("set to a path is a launch request") {
        REQUIRE(SetEnvironmentVariableW(kOpenEnvVar, L"C:\\captures\\m.echodemo") != FALSE);
        CHECK(OpenRequestPresent());
        CHECK(ClassifySession(L"echovr.exe", OpenRequestPresent(), false) == SessionMode::Replay);
    }

    SUBCASE("a one-character value is a launch request too") {
        // kOpenMostRecent. The predicate probes with a two-wchar buffer, so a value of exactly one
        // character is the boundary between "it fit" and "it did not"; both answers must be true.
        REQUIRE(SetEnvironmentVariableW(kOpenEnvVar, replay::control::kOpenMostRecent) != FALSE);
        CHECK(OpenRequestPresent());
        CHECK(ClassifySession(L"echovr.exe", OpenRequestPresent(), false) == SessionMode::Replay);
    }

    SetEnvironmentVariableW(kOpenEnvVar, had != 0 ? saved : nullptr);
}

TEST_CASE("a genuine live online match is exempt from all three containment gates") {
    // ★ The defect ENG-LIVE-CLASSIFY-FIX repairs, in the exact shape CAPTURE-LIVE observed: rlay.dll
    // loaded, -offline ABSENT, no open-request. The egress blocker already disarmed for this session;
    // the offline-bit lock still armed and dragged the loopback-only bind rewrite with it, and rewriting
    // the client's own wildcard binds to 127.0.0.1 is what returned "network unreachable" on every send
    // to the live host. All three gates must now be off.
    const SessionMode live = ClassifySession(L"echovr.exe -spectatorstream", false, true);
    CHECK(live == SessionMode::LiveOnlineMatch);
    CHECK(IsLiveOnlineExempt(live, /*offlineSwitchSeen=*/false));

    const GateDecision g = GatesFor(live, /*offlineSwitchSeen=*/false);
    CHECK_FALSE(g.egressArmed);
    CHECK_FALSE(g.offlineLockApplied);
    CHECK_FALSE(g.loopbackArmed);

    SUBCASE("and the same holds whether or not rlay.dll happens to be loaded") {
        const SessionMode noRlay = ClassifySession(L"echovr.exe -spectatorstream", false, false);
        CHECK(noRlay == SessionMode::LiveOnlineMatch);
        const GateDecision gn = GatesFor(noRlay, false);
        CHECK_FALSE(gn.egressArmed);
        CHECK_FALSE(gn.offlineLockApplied);
        CHECK_FALSE(gn.loopbackArmed);
    }
}

TEST_CASE("a replay and an offline session stay fully contained by all three gates") {
    // ★ The safety invariant, both contained sessions. A replay must NEVER be able to phone out, and an
    // offline session must stay unreachable. If any of these six gate assertions flips, containment has
    // been widened past a genuine live match.
    SUBCASE("a replay (open-request present) arms every gate") {
        const SessionMode replay = ClassifySession(L"echovr.exe", /*openRequestPresent=*/true, false);
        REQUIRE(replay == SessionMode::Replay);
        CHECK_FALSE(IsLiveOnlineExempt(replay, /*offlineSwitchSeen=*/false));
        const GateDecision g = GatesFor(replay, false);
        CHECK(g.egressArmed);
        CHECK(g.offlineLockApplied);
        CHECK(g.loopbackArmed);
    }
    SUBCASE("a credible offline session (-offline with rlay) arms every gate") {
        const SessionMode offline = ClassifySession(L"echovr.exe -offline", false, /*rlayLoaded=*/true);
        REQUIRE(offline == SessionMode::Offline);
        CHECK_FALSE(IsLiveOnlineExempt(offline, /*offlineSwitchSeen=*/true));
        const GateDecision g = GatesFor(offline, /*offlineSwitchSeen=*/true);
        CHECK(g.egressArmed);
        CHECK(g.offlineLockApplied);
        CHECK(g.loopbackArmed);
    }
}

TEST_CASE("the live-online exemption is precise and falls to the contained side on ambiguity") {
    // ★ The exemption must be exactly (LiveOnlineMatch AND no -offline). Every other combination is
    // contained. The load-order case is the one that matters most: a bare -offline that is not yet
    // credible (rlay loading after us) classifies LiveOnlineMatch on the first look, but a genuine live
    // match never carries -offline - so an -offline seen at all keeps this session contained rather than
    // exempt. This is the mutation that a naive `session == LiveOnlineMatch` would let through.
    CHECK(IsLiveOnlineExempt(SessionMode::LiveOnlineMatch, false));
    CHECK_FALSE(IsLiveOnlineExempt(SessionMode::LiveOnlineMatch, /*offlineSwitchSeen=*/true));

    // Nothing that is not decisively a live online match is ever exempt.
    for (const bool seen : {false, true}) {
        CAPTURE(seen);
        CHECK_FALSE(IsLiveOnlineExempt(SessionMode::Replay, seen));
        CHECK_FALSE(IsLiveOnlineExempt(SessionMode::Offline, seen));
        CHECK_FALSE(IsLiveOnlineExempt(SessionMode::RefusedStaleSwitch, seen));
        CHECK_FALSE(IsLiveOnlineExempt(SessionMode::Unknown, seen));
    }

    SUBCASE("the ambiguous -offline-without-rlay session is contained on all three gates") {
        // session is LiveOnlineMatch on the first look (rlay absent so -offline is not credible), but the
        // switch was seen, so the offline-lock and loopback gates contain it rather than exempt it. The
        // egress blocker leaves its own re-check to arm late; the two final gates fail safe now.
        const SessionMode look = ClassifySession(L"echovr.exe -offline", false, /*rlayLoaded=*/false);
        REQUIRE(look == SessionMode::LiveOnlineMatch);
        const GateDecision g = GatesFor(look, /*offlineSwitchSeen=*/true);
        CHECK_FALSE(g.egressArmed);        // deferred to the late-rlay re-check, by design
        CHECK(g.offlineLockApplied);       // ★ contained now, the safe side
        CHECK(g.loopbackArmed);            // ★ contained now, the safe side
    }
}

TEST_CASE("the carrier is spelled the same in both packets") {
    // src/bootstrap cannot include control/launchargs.h - src/control depends on this library, so the
    // include would close the loop. The two spellings are kept in step here instead of by hand: a
    // rename in either packet turns this red rather than making the classification unreachable again,
    // which is the exact failure this file exists for.
    CHECK(std::wcscmp(OpenEnvVarName(), kOpenEnvVar) == 0);
    CHECK(std::wcscmp(OpenEnvVarName(), L"ECHOREPLAY_OPEN") == 0);
}

TEST_CASE("the host netvar setter detours are asked for only where a replay can drive netvars") {
    CHECK(std::string(ResolveHostNetvarHook("auto", false, true, true)) == "hook");
    CHECK(std::string(ResolveHostNetvarHook(nullptr, false, true, true)) == "hook");
    // "hook" in the file is not a way around the other three conditions.
    CHECK(std::string(ResolveHostNetvarHook("hook", /*liveOnlineExempt=*/true, true, true)) == "off");

    CHECK(std::string(ResolveHostNetvarHook("auto", /*liveOnlineExempt=*/true, true, true)) == "off");
    CHECK(std::string(ResolveHostNetvarHook("auto", false, /*offlineLocked=*/false, true)) == "off");
    CHECK(std::string(ResolveHostNetvarHook("auto", false, true, /*gamestateDriven=*/false)) == "off");
    CHECK(std::string(ResolveHostNetvarHook("off", false, true, true)) == "off");
}

TEST_CASE("the OnScoreCB detour is asked for only where a replay can drive the disc") {
    CHECK(std::string(ResolveHostScoreHook("auto", false, true, true)) == "hook");
    CHECK(std::string(ResolveHostScoreHook(nullptr, false, true, true)) == "hook");
    CHECK(std::string(ResolveHostScoreHook("hook", /*liveOnlineExempt=*/true, true, true)) == "off");
    CHECK(std::string(ResolveHostScoreHook("auto", /*liveOnlineExempt=*/true, true, true)) == "off");
    CHECK(std::string(ResolveHostScoreHook("auto", false, /*offlineLocked=*/false, true)) == "off");
    CHECK(std::string(ResolveHostScoreHook("auto", false, true, /*discDriven=*/false)) == "off");
    CHECK(std::string(ResolveHostScoreHook("off", false, true, true)) == "off");
}

TEST_CASE("ledger row 2.9 is declared exactly when the provider resolved the score detour to hook") {
    namespace config = replay::config;
    const char* key = "playback.host_score_hook";
    for (const char* resolved : {"hook", "off"}) {
        CAPTURE(resolved);
        config::ResetForTest();
        REQUIRE(config::LoadFromTextForTest("{}", 2));
        CHECK(config::RowDeclarationState(key) == config::RowState::Unresolved);
        REQUIRE(config::SetEffectiveString(key, resolved));
        CHECK(config::RowDeclarationState(key) == (std::string(resolved) == "hook"
                                                       ? config::RowState::Declared
                                                       : config::RowState::NotDeclared));
        // Row 2.8 is another provider's answer and this publish is not it.
        CHECK(config::RowDeclarationState("playback.host_netvar_hook") ==
              config::RowState::Unresolved);
    }
    config::ResetForTest();
}

TEST_CASE("ledger row 2.8 is declared exactly when the provider resolved the setter detours to hook") {
    namespace config = replay::config;
    const char* key = "playback.host_netvar_hook";
    for (const char* resolved : {"hook", "off"}) {
        CAPTURE(resolved);
        config::ResetForTest();
        REQUIRE(config::LoadFromTextForTest("{}", 2));
        CHECK(config::RowDeclarationState(key) == config::RowState::Unresolved);
        REQUIRE(config::SetEffectiveString(key, resolved));
        CHECK(config::RowDeclarationState(key) == (std::string(resolved) == "hook"
                                                       ? config::RowState::Declared
                                                       : config::RowState::NotDeclared));
    }
    config::ResetForTest();
}
