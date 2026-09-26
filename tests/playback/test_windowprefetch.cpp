// WINDOW-DECODE-1. The worker that decodes the next sample window off the tick thread.
//
// ---------------------------------------------------------------------------------------------
// ★★★ A SECOND DECODE PATH IS A SECOND SOURCE OF TRUTH UNLESS IT IS THE SAME CODE. The design is
// "same function, different thread, different scratch", so the case that earns this packet is not
// "the worker produced a window" -- it is that the window the WORKER produced and the window the
// TICK would have produced are the SAME WINDOW, for every centre chapter of a real capture, walked
// forwards and then backwards. A prefetch that is subtly different is worse than the hitch it
// replaces: the hitch is visible and a wrong pose is not.
//
// ★★ WHY THE COMPARISON WALKS SAMPLES AND NOT BYTES. A Window holds pool pointers and cursors, so
// two windows built from the same chapters are never equal memcmp'd -- their addresses differ.
// What must be identical is everything a frame READS out of one: the coverage span, the tick table,
// every track's sample run and the netvar key set. That is what is compared here, sample by sample,
// which is a stronger claim than byte equality of two objects that are not byte-comparable.
//
// ★ WHAT THIS SUITE CANNOT REACH. Everything above the module seam -- the adoption swap, the
// tick-thread decode counter, the direction-flip discard -- lives in playback_module.cpp, which is
// linked by playback_armtest and not by this target. Those are named in the packet report as
// deferred rather than claimed here.
// ---------------------------------------------------------------------------------------------

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "foundation/demofile/demofile.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"
#include "playback/core/windowprefetch.h"

namespace {

using replay::playback::BodyTrack;
using replay::playback::BuildStats;
using replay::playback::PlayerTrack;
using replay::playback::Window;
using replay::playback::WindowCaps;
namespace winprefetch = replay::playback::winprefetch;

#define REPLAY_WIDEN_(x) L##x
#define REPLAY_WIDEN(x) REPLAY_WIDEN_(x)
#define REPLAY_PREFETCH_CAPTURE "2026-09-04_08-01-50_0000000000000000_0000000000000000.echodemo"

// ★ THE CAPTURE IS NOT IN THE REPO -- `.gitignore:2` is `*.echodemo`, because a recorded session
// carries the owner's own config. So this looks in the two places it can be and SAYS SO when it is
// in neither, exactly as test_playback.cpp's own fixture helpers do. A fixture that cannot fail is
// worse than one that is absent, and an absence nobody is told about is worse than both.
bool ResolveCapturePath(wchar_t* out, size_t chars) {
    static const wchar_t kInTree[] =
        REPLAY_WIDEN(REPLAY_PLAYBACK_FIXTURE_DIR) L"/" REPLAY_WIDEN(REPLAY_PREFETCH_CAPTURE);
    replay::DemoReader probe;
    if (probe.Open(kInTree)) {
        probe.Close();
        wcsncpy_s(out, chars, kInTree, _TRUNCATE);
        return true;
    }
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (home == nullptr) {
        return false;
    }
    wchar_t alt[1024];
    _snwprintf_s(alt, _TRUNCATE, L"%s/Documents/EchoReplay/%s", home,
                 REPLAY_WIDEN(REPLAY_PREFETCH_CAPTURE));
    if (!probe.Open(alt)) {
        return false;
    }
    probe.Close();
    wcsncpy_s(out, chars, alt, _TRUNCATE);
    return true;
}

// Everything the load does before a window can be built, so a case can build one. Returns false
// when the capture is absent, which every case reports and skips on.
struct Fixture {
    replay::DemoReader reader;
    replay::playback::TrackScan scan;
    replay::playback::NetVarPrimer primer;
    WindowCaps caps;
    wchar_t path[1024] = {};

    bool Open(bool want_primer) {
        if (!ResolveCapturePath(path, 1024)) {
            return false;
        }
        if (!reader.Open(path)) {
            return false;
        }
        const char* why = "unstated";
        if (!replay::playback::ScanTrackSources(reader, scan, &why)) {
            return false;
        }
        // The pre-load defaults, narrowed by the scan. The module sizes from the file's own
        // measured cadence, which is a module-private step; the defaults are the 90 Hz ceiling and
        // so are never SMALLER than what this 45 Hz capture needs. Both paths build against the
        // same caps, which is what the comparison requires.
        caps = replay::playback::CapsForScan(WindowCaps(), scan);
        if (want_primer) {
            replay::playback::ScanNetVarPrimer(reader, primer);
        }
        return true;
    }
    ~Fixture() { reader.Close(); }
};

// The first difference by name, or nullptr when the two are the same window twice.
const char* WindowsDiffer(Window& a, Window& b) {
    if (a.OuterBeginUs() != b.OuterBeginUs()) return "outer begin";
    if (a.OuterEndUs() != b.OuterEndUs()) return "outer end";
    if (a.BodyCount() != b.BodyCount()) return "body count";
    if (a.PlayerCount() != b.PlayerCount()) return "player count";
    if (a.Ticks().Count() != b.Ticks().Count()) return "tick count";
    for (size_t i = 0; i < a.Ticks().Count(); ++i) {
        if (a.Ticks().At(i) != b.Ticks().At(i)) return "tick boundary";
    }
    if (a.NetVars().KeyCount() != b.NetVars().KeyCount()) return "netvar key count";
    for (size_t i = 0; i < a.BodyCount(); ++i) {
        BodyTrack& ta = a.BodyAt(i);
        BodyTrack& tb = b.BodyAt(i);
        if (ta.count != tb.count) return "body sample count";
        if (ta.id.value != tb.id.value) return "body identity value";
        if (ta.id.tag != tb.id.tag) return "body identity tag";
        if (ta.id.fill != tb.id.fill) return "body identity fill";
        const replay::BodyState* fa = a.BodyFrames(ta);
        const replay::BodyState* fb = b.BodyFrames(tb);
        const uint64_t* xa = a.BodyTimes(ta);
        const uint64_t* xb = b.BodyTimes(tb);
        for (size_t k = 0; k < ta.count; ++k) {
            if (xa[k] != xb[k]) return "body sample time";
            if (memcmp(&fa[k], &fb[k], sizeof(replay::BodyState)) != 0) return "body sample state";
        }
    }
    for (size_t i = 0; i < a.PlayerCount(); ++i) {
        PlayerTrack& ta = a.PlayerAt(i);
        PlayerTrack& tb = b.PlayerAt(i);
        if (ta.count != tb.count) return "player sample count";
        if (ta.user_slot != tb.user_slot) return "player user slot";
        if (ta.generation != tb.generation) return "player generation";
        const replay::PlayerState* fa = a.PlayerFrames(ta);
        const replay::PlayerState* fb = b.PlayerFrames(tb);
        const uint64_t* xa = a.PlayerTimes(ta);
        const uint64_t* xb = b.PlayerTimes(tb);
        for (size_t k = 0; k < ta.count; ++k) {
            if (xa[k] != xb[k]) return "player sample time";
            if (memcmp(&fa[k], &fb[k], sizeof(replay::PlayerState)) != 0) {
                return "player sample state";
            }
        }
    }
    return nullptr;
}

// Drives one prefetch to completion. Bounded rather than blocking: a worker that never answers must
// fail the case, not hang the suite.
bool DecodeThroughWorker(size_t centre, uint32_t serial, Window* buffer, winprefetch::Result& out) {
    if (!winprefetch::HasBuffer()) {
        if (!winprefetch::GiveBuffer(buffer)) {
            return false;
        }
    }
    if (!winprefetch::Request(centre, serial)) {
        return false;
    }
    for (int spin = 0; spin < 20000; ++spin) {
        if (winprefetch::Collect(out)) {
            return true;
        }
        Sleep(1);
    }
    return false;
}

const char* const kAbsent =
    "SKIPPED: the WINDOW-DECODE-1 fixture capture is absent from both tests/playback/fixtures and "
    "%USERPROFILE%/Documents/EchoReplay";

}  // namespace

TEST_CASE("WINDOW-DECODE-1: the worker's window is the tick's window, every centre, both directions") {
    Fixture fx;
    if (!fx.Open(true)) {
        MESSAGE(kAbsent);
        return;
    }

    Window* sync_win = new Window();
    Window* async_win = new Window();
    const char* why = "unstated";
    REQUIRE(sync_win->Reserve(fx.caps, &why));
    REQUIRE(async_win->Reserve(fx.caps, &why));

    const char* start_why = "unstated";
    REQUIRE(winprefetch::Start(fx.path, &fx.scan.clocks, &fx.primer, &start_why));

    size_t cores[1024];
    size_t core_n = 0;
    for (size_t i = 0; i < fx.reader.ChapterCount() && core_n < 1024; ++i) {
        if (fx.reader.ChapterAt(i).section == replay::kSectionCore) {
            cores[core_n++] = i;
        }
    }
    REQUIRE(core_n > 0);

    // Ascending, then descending. The descending pass is not decoration: a decode scratch that
    // failed to reset between builds passes the ascending walk and fails this one, and reverse is
    // exactly the direction the prefetch has to serve.
    size_t compared = 0;
    const char* first_diff = nullptr;
    size_t first_diff_centre = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t k = 0; k < core_n; ++k) {
            const size_t centre = pass == 0 ? cores[k] : cores[core_n - 1 - k];
            BuildStats sync_stats;
            const bool sync_ok = replay::playback::BuildWindowRange(
                fx.reader, centre, *sync_win, sync_stats, &fx.scan.clocks, &fx.primer);
            winprefetch::Result r;
            REQUIRE(DecodeThroughWorker(centre, 1u, async_win, r));
            CHECK(r.centre == centre);
            CHECK(r.window == async_win);
            CHECK(r.built == sync_ok);
            if (!sync_ok) {
                continue;
            }
            const char* diff = WindowsDiffer(*sync_win, *async_win);
            if (diff != nullptr) {
                if (first_diff == nullptr) {
                    first_diff = diff;
                    first_diff_centre = centre;
                }
            }
            ++compared;
        }
    }
    if (first_diff != nullptr) {
        MESSAGE("centre ", first_diff_centre, " differs in ", first_diff);
    }
    CHECK(first_diff == nullptr);
    CHECK(compared >= core_n);

    REQUIRE(winprefetch::Stop(5000));
    CHECK_FALSE(winprefetch::Running());
    delete sync_win;
    delete async_win;
}

TEST_CASE("WINDOW-DECODE-1: a request burst costs one decode, latest wins") {
    Fixture fx;
    if (!fx.Open(false)) {
        MESSAGE(kAbsent);
        return;
    }
    Window* buffer = new Window();
    const char* why = "unstated";
    REQUIRE(buffer->Reserve(fx.caps, &why));

    const char* start_why = "unstated";
    REQUIRE(winprefetch::Start(fx.path, &fx.scan.clocks, nullptr, &start_why));
    REQUIRE(winprefetch::GiveBuffer(buffer));

    // ★★ THE SEEK STORM, AND THE ACCOUNTING IS EXACT RATHER THAN APPROXIMATE. There is ONE buffer,
    // so a post is accepted only while the worker has not started -- every accepted post therefore
    // either replaces a queued one (a coalesce) or becomes the decode. Nothing else can happen to
    // it, which is why `requests == decodes + coalesced` is an equality and not a bound, and it is
    // what makes a drag cost one decode per landing rather than one per step.
    size_t posted = 0;
    for (int i = 0; i < 40; ++i) {
        if (winprefetch::Request((size_t)(i % 4), 1u)) {
            ++posted;
        }
    }
    REQUIRE(posted >= 1);
    winprefetch::Result r;
    bool got = false;
    for (int spin = 0; spin < 20000; ++spin) {
        if (winprefetch::Collect(r)) {
            got = true;
            break;
        }
        Sleep(1);
    }
    REQUIRE(got);
    const winprefetch::Counters c = winprefetch::ReadCounters();
    CHECK(c.requests == posted);
    CHECK(c.requests == c.decodes + c.coalesced);
    CHECK(c.decodes == 1);
    CHECK(c.collected == 1);
    // The only buffer left with the result, so nothing can be posted until the tick has collected.
    CHECK_FALSE(winprefetch::HasBuffer());

    REQUIRE(winprefetch::Stop(5000));
    delete buffer;
}

TEST_CASE("WINDOW-DECODE-1: an unload during an in-flight decode joins before anything is freed") {
    Fixture fx;
    if (!fx.Open(false)) {
        MESSAGE(kAbsent);
        return;
    }
    Window* buffer = new Window();
    const char* why = "unstated";
    REQUIRE(buffer->Reserve(fx.caps, &why));

    // Ten unloads landing on a decode in flight. One would be an anecdote; the loop is what makes
    // the Unload ordering a property rather than a lucky interleaving. Every iteration writes the
    // buffer after the join -- which is the assertion that the worker is genuinely finished with it
    // and not merely asked to stop.
    for (int i = 0; i < 10; ++i) {
        const char* start_why = "unstated";
        REQUIRE(winprefetch::Start(fx.path, &fx.scan.clocks, nullptr, &start_why));
        REQUIRE(winprefetch::GiveBuffer(buffer));
        REQUIRE(winprefetch::Request(1u, (uint32_t)i));
        REQUIRE(winprefetch::Stop(5000));
        CHECK_FALSE(winprefetch::Running());
        CHECK_FALSE(winprefetch::HasBuffer());
        CHECK_FALSE(winprefetch::Busy());
        buffer->Clear();
    }
    delete buffer;
}

TEST_CASE("WINDOW-DECODE-1: a result carries the epoch it was asked for") {
    Fixture fx;
    if (!fx.Open(false)) {
        MESSAGE(kAbsent);
        return;
    }
    Window* buffer = new Window();
    const char* why = "unstated";
    REQUIRE(buffer->Reserve(fx.caps, &why));
    const char* start_why = "unstated";
    REQUIRE(winprefetch::Start(fx.path, &fx.scan.clocks, nullptr, &start_why));

    // The epoch IS the discriminator the module drops a stale window on: a result stamped with a
    // serial that is no longer the live load is a window decoded for a file that has been closed.
    // If the stamp did not round-trip, that drop would be checking a constant.
    winprefetch::Result r;
    REQUIRE(DecodeThroughWorker(1u, 4242u, buffer, r));
    CHECK(r.load_serial == 4242u);
    CHECK(r.request_id != 0);
    const uint64_t first_id = r.request_id;
    REQUIRE(DecodeThroughWorker(1u, 4243u, buffer, r));
    CHECK(r.load_serial == 4243u);
    CHECK(r.request_id > first_id);

    REQUIRE(winprefetch::Stop(5000));
    delete buffer;
}

TEST_CASE("WINDOW-DECODE-1: nothing is accepted while the worker is stopped") {
    // Inertness, and it is the claim the config default rests on: with the key off nothing starts
    // the worker, and every entry point refuses rather than queueing work for a thread that is not
    // there. A live session is exactly this state, permanently.
    CHECK_FALSE(winprefetch::Running());
    CHECK_FALSE(winprefetch::HasBuffer());
    CHECK_FALSE(winprefetch::Busy());
    CHECK_FALSE(winprefetch::Request(3u, 1u));
    winprefetch::Result r;
    CHECK_FALSE(winprefetch::Collect(r));
    Window w;
    CHECK_FALSE(winprefetch::GiveBuffer(&w));
}
