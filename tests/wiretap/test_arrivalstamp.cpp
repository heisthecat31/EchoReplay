// Where a tapped message's arrival time comes from.
//
// A dispatch carries no time the tap may read: the engine's per-dispatch qword is all ones for a
// locally raised event and uninterpreted on the network path (docs/spikes/W1-L.md S9.2). A consumer
// that stored it as microseconds recorded arrival times of 6, 34, 36, 65, 66, 16369959894457074595
// and 0xFFFFFFFFFFFFFFFF across one 32 s match. The tap now stamps every message itself, and these
// cases drive that through the real chain -- echovr.exe mapped (engineimage.cpp), a CBroadcaster
// built by the engine's own container code (synthengine.cpp), registration through
// NRadEngine::CBroadcaster::Listen, dispatch through the node's invoker with the six arguments both
// engine dispatchers pass. Nothing on the path under test is simulated.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <type_traits>
#include <vector>

#include <doctest.h>

#include "capture/manifestbuild.h"
#include "engine/wiretap.h"

#include "engineimage.h"
#include "synthengine.h"
#include "../local_inputs.h"

namespace {

// The stamps the killed run actually recorded, off its own chapter index. Feeding
// these back in is what makes the case retroactive: under the old code each one of them WAS the
// arrival time, and the assertions below say where each may and may not appear now.
const uint64_t kRecordedSenderStamps[] = {
    0xFFFFFFFFFFFFFFFFull, 6ull, 34ull, 36ull, 65ull, 66ull, 16369959894457074595ull,  // facts-ok: LOBBY.JOIN_FREE_SENTINEL an all-ones u64 "none" local to this code, not the lobby join sentinel
};
constexpr size_t kRecordedCount = sizeof(kRecordedSenderStamps) / sizeof(kRecordedSenderStamps[0]);

// src/capture/manifestbuild.cpp's kTaps as of this pin. A row added there without a look at this
// file leaves the new kind's stamp unexercised, which is the state S_EVENT was already in.
constexpr size_t kTapRowsPinned = 38;

// The one row in kTaps the tap cannot register: ReplayIdentitySnapshot is ours, not a wire message,
// and CMatSym is only defined over names the binary spells with a leading 'S'.
constexpr size_t kUnregisterableRowsPinned = 1;


using ProxyFn = void(__fastcall*)(void*, void**, const void*, uint64_t, uint64_t, uint64_t);

uint64_t QpcNow() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

double QpcUsPerTick() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return 1e6 / static_cast<double>(f.QuadPart);
}

std::wstring EnginePath() { return replay_test::FindEchoVrExeW(); }

// The struct names out of the linked kTaps rather than a second copy of the list. This used to scan
// manifestbuild.cpp for `{"` and silently lost the row that is spelled with a constant.
bool ReadTapNames(std::vector<std::string>& out, std::string& error) {
    const replay::capture::TapType* taps = replay::capture::TapTable();
    const uint16_t                  n    = replay::capture::TapTypeCount();
    for (uint16_t i = 0; i < n; ++i) {
        out.emplace_back(taps[i].structName);
    }
    if (out.empty()) {
        error = "capture::TapTable() is empty";
    }
    return !out.empty();
}

struct Delivery {
    size_t   tap;
    uint64_t arrivalUs;
    uint64_t senderStamp;
};

std::vector<Delivery> gDeliveries;

void Sink(const replay::WireMessage& m, void* ctx) {
    Delivery d{};
    d.tap         = static_cast<size_t>(reinterpret_cast<uintptr_t>(ctx));
    d.arrivalUs   = m.arrivalUs;
    d.senderStamp = m.stamp;
    gDeliveries.push_back(d);
}

// One run of the whole chain, so both cases below read the same dispatches.
struct Run {
    bool                     ok = false;
    std::string              why;
    std::vector<std::string> names;
    size_t                   registered   = 0;
    size_t                   unregisterable = 0;
    double                   measuredUs   = 0.0;
};

Run& TheRun() {
    static Run  run;
    static bool done = false;
    if (done) {
        return run;
    }
    done = true;

    if (!ReadTapNames(run.names, run.why)) {
        return run;
    }

    static w1l::EngineImage image;
    if (!image.Map(EnginePath().c_str(), run.why)) {
        return run;
    }
    static w1l::SynthNetGame game;
    if (!game.Build(image, run.why)) {
        return run;
    }

    replay::IWiretap* tap =
        replay::WiretapCreateAt(game.NetGame(), reinterpret_cast<const void*>(image.Base()));
    if (tap == nullptr || !tap->Available()) {
        run.why = tap == nullptr ? "WiretapCreateAt returned null" : tap->Diagnostic();
        return run;
    }

    struct Target {
        ProxyFn proxy;
        void*   context;
        void**  memberSlot;
    };
    std::vector<Target> targets;
    void* const         bus = game.Broadcaster();

    // Every registration first, every read-back second. The engine grows its listener pool as it
    // fills, so a node address taken before the last Listen is a pointer into the old block.
    std::vector<std::pair<size_t, uint16_t>> handles;
    for (size_t i = 0; i < run.names.size(); ++i) {
        if (run.names[i].empty() || run.names[i][0] != 'S') {
            ++run.unregisterable;
            continue;
        }
        const uint16_t handle =
            tap->Listen(run.names[i].c_str(), Sink, reinterpret_cast<void*>(i));
        if (handle == replay::IWiretap::kInvalidHandle) {
            run.why = "Listen refused " + run.names[i] + ": " + tap->Diagnostic();
            replay::WiretapDestroy(tap);
            return run;
        }
        handles.push_back(std::make_pair(i, handle));
    }
    for (size_t k = 0; k < handles.size(); ++k) {
        w1l::ListenerNode node{};
        if (!w1l::ReadListenerNode(bus, handles[k].second, node) || node.invoker == nullptr) {
            run.why = "no listener node read back for " + run.names[handles[k].first];
            replay::WiretapDestroy(tap);
            return run;
        }
        Target t{};
        t.proxy      = reinterpret_cast<ProxyFn>(node.invoker);
        t.context    = node.context;
        t.memberSlot = w1l::ListenerMemberSlot(bus, handles[k].second);
        if (t.memberSlot == nullptr) {
            run.why = "no member slot for " + run.names[handles[k].first];
            replay::WiretapDestroy(tap);
            return run;
        }
        targets.push_back(t);
        ++run.registered;
    }

    // A synthetic sequence over every registered kind, spread over a few milliseconds so the
    // arrival clock has something to move through. The wait is a busy wait: Sleep's granularity is
    // coarser than the whole span being measured.
    const uint8_t  body[24]  = {};
    const double   usPerTick = QpcUsPerTick();
    const uint64_t began     = QpcNow();
    size_t         n         = 0;
    for (int round = 0; round < 4; ++round) {
        for (const Target& t : targets) {
            t.proxy(t.context, t.memberSlot, body, sizeof body,
                    kRecordedSenderStamps[n % kRecordedCount], 7ull);
            ++n;
        }
        const uint64_t until = QpcNow() + static_cast<uint64_t>(1500.0 / usPerTick);
        while (QpcNow() < until) {
            YieldProcessor();
        }
    }
    run.measuredUs = static_cast<double>(QpcNow() - began) * usPerTick;

    tap->UnlistenAll();
    replay::WiretapDestroy(tap);
    run.ok = true;
    return run;
}

}  // namespace

// The control for "a row left its stamp unset". A WireMessage cannot be built without one, so the
// uninitialised path is not reachable from any tap kind, present or future -- these fail to compile
// the moment someone restores the aggregate this struct used to be.
static_assert(!std::is_default_constructible<replay::WireMessage>::value,
              "WireMessage must not be default-constructible: it would leave arrivalUs unset");
static_assert(!std::is_constructible<replay::WireMessage, const void*, uint64_t, uint64_t,
                                     uint64_t>::value,
              "the four-argument WireMessage is the shape that had no arrival stamp");
static_assert(std::is_constructible<replay::WireMessage, const void*, uint64_t, uint64_t, uint64_t,
                                    uint64_t>::value,
              "every field of WireMessage is a constructor argument");

TEST_CASE("the tap's arrival clock is monotonic and reads microseconds") {
    // Runs with or without echovr.exe: the clock is the source every tap kind draws on, so its own
    // properties are worth a case that nothing else can make vacuous.
    uint64_t last      = replay::WiretapArrivalNowUs();
    size_t   wentBack   = 0;
    size_t   everMoved  = 0;
    for (int i = 0; i < 2000; ++i) {
        const uint64_t now = replay::WiretapArrivalNowUs();
        wentBack += now < last ? 1 : 0;
        everMoved += now > last ? 1 : 0;
        last = now;
    }
    REQUIRE(wentBack == 0);
    REQUIRE(everMoved > 0);

    const double   usPerTick = QpcUsPerTick();
    const uint64_t began     = QpcNow();
    const uint64_t until     = began + static_cast<uint64_t>(3000.0 / usPerTick);
    while (QpcNow() < until) {
        YieldProcessor();
    }
    const double measuredUs = static_cast<double>(QpcNow() - began) * usPerTick;
    const double clockUs    = static_cast<double>(replay::WiretapArrivalNowUs() - last);

    // 3 ms of wall clock has to read as roughly 3000 of these units. Ticks would read ~30000 and
    // seconds ~0, which is the width question the recorded 6 / 34 / 66 raised.
    CHECK(clockUs >= measuredUs * 0.5);
    CHECK(clockUs <= measuredUs * 2.0 + 1000.0);
}

TEST_CASE("every registered tap kind stamps its own arrival, and none of them takes the engine's") {
    if (EnginePath().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    Run& run = TheRun();
    REQUIRE_MESSAGE(run.ok, run.why.c_str());

    // Staleness, both ways: a row added to kTaps and a row this scan stopped seeing.
    REQUIRE_MESSAGE(run.names.size() == kTapRowsPinned,
                    "kTaps row count changed -- register the new kind here and repin");
    REQUIRE(run.unregisterable == kUnregisterableRowsPinned);
    REQUIRE(run.registered == kTapRowsPinned - kUnregisterableRowsPinned);

    // Not the equality of two empty runs: every registered kind has to have been dispatched to.
    REQUIRE(gDeliveries.size() == run.registered * 4);
    std::vector<size_t> perTap(run.names.size(), 0);
    for (const Delivery& d : gDeliveries) {
        ++perTap[d.tap];
    }
    for (size_t i = 0; i < run.names.size(); ++i) {
        if (run.names[i][0] != 'S') {
            continue;
        }
        CHECK_MESSAGE(perTap[i] == 4, run.names[i].c_str());
    }

    // Counted rather than asserted per delivery: 148 dispatches of the same property is one fact,
    // and a failure names how many rather than burying the count in repeats.
    size_t unstamped = 0, wentBack = 0, tookTheSender = 0, tookARecordedValue = 0;
    uint64_t previous = 0;
    for (const Delivery& d : gDeliveries) {
        unstamped += d.arrivalUs == 0 ? 1 : 0;
        wentBack += d.arrivalUs < previous ? 1 : 0;
        previous = d.arrivalUs;

        // The discriminating property: whatever the engine put on the wire is not the time.
        tookTheSender += d.arrivalUs == d.senderStamp ? 1 : 0;
        for (size_t k = 0; k < kRecordedCount; ++k) {
            tookARecordedValue += d.arrivalUs == kRecordedSenderStamps[k] ? 1 : 0;
        }
    }
    CHECK(unstamped == 0);
    CHECK(wentBack == 0);
    CHECK(tookTheSender == 0);
    CHECK(tookARecordedValue == 0);

    const double span = static_cast<double>(gDeliveries.back().arrivalUs -
                                            gDeliveries.front().arrivalUs);
    CHECK(span >= 3000.0);
    CHECK(span <= run.measuredUs + 2000.0);
}

TEST_CASE("the engine's per-dispatch qword still reaches the consumer, on its own field") {
    if (EnginePath().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    Run& run = TheRun();
    REQUIRE_MESSAGE(run.ok, run.why.c_str());
    REQUIRE(gDeliveries.size() == run.registered * 4);

    // Constraint 8 is lossless: the qword is still data, and every one of the killed run's values
    // comes back byte for byte. This is also what proves the recorded 6 / 34 / 0xFFFF... came from
    // this argument -- under the old code these are exactly the values that became stampUs.
    size_t seen[kRecordedCount] = {};
    size_t altered              = 0;
    for (size_t i = 0; i < gDeliveries.size(); ++i) {
        altered += gDeliveries[i].senderStamp != kRecordedSenderStamps[i % kRecordedCount] ? 1 : 0;
        ++seen[i % kRecordedCount];
    }
    CHECK(altered == 0);
    for (size_t k = 0; k < kRecordedCount; ++k) {
        CHECK(seen[k] > 0);
    }
}
