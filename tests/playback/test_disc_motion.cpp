// DISC MOTION: a simulated 1x pass over the owner's real capture, through the real container
// reader, the real window build and the real sampler, into a fake engine sink.
//
// This is the test that would have caught a truly frozen disc, and the one that makes "the source
// stamp is past the end of the playable span" a red test rather than a launch report: it asserts on
// the poses that REACHED the engine, not on a log line, and it asserts every installed stamp is
// inside the span the timeline is drawn over (DIAG-DISC-FROZEN-REGRESSION §4.3 T2).
//
// The capture is the owner's and is not in the repo. A missing file SKIPS: this case exists to
// measure a real recording and has nothing to say without one.

#include "doctest.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <map>
#include <set>
#include <vector>

#include "fake_disc_entity.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/apply.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::playback;
using disctest::DiscEntity;

namespace discmotion {
namespace {

#define DISCMOTION_CAPTURE "2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo"

// ★ WHICH TRACK IS THE DISC, without an engine to ask. The wire bodies carry the recorded user
// slot they belong to; the disc carries a COMPONENT id (tag 1), which is what BindBodyHandles
// counts as `not_a_net_id` and what the live bind hands to PhysOfComponentId. It is the only such
// track in this capture's windows. Asking the classifier rather than pinning a track index means a
// re-keyed window cannot quietly hand this test a different body.
constexpr uint16_t kDiscHandle = 5;

// The content window the launches argue over, and the rate the disc was recorded at.
constexpr int64_t kContentFromUs = 23'000'000;
constexpr int64_t kContentToUs = 48'500'000;
constexpr int64_t kStepUs = 1'000'000 / 45;

bool OpenRealCapture(DemoReader& reader, char* where, size_t chars) {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (home == nullptr) {
        _snprintf_s(where, chars, _TRUNCATE, "USERPROFILE is not set");
        return false;
    }
    wchar_t path[1024];
    _snwprintf_s(path, _TRUNCATE, L"%s/Documents/EchoReplay/%S", home, DISCMOTION_CAPTURE);
    _snprintf_s(where, chars, _TRUNCATE, "%%USERPROFILE%%/Documents/EchoReplay/%s",
                DISCMOTION_CAPTURE);
    return reader.Open(path);
}

// The one track the file says is the disc. Absent is an answer -- a window built over a stretch
// with no disc record has none -- so the caller decides what that means.
BodyTrack* DiscTrackOf(Window& w) {
    BodyTrack* found = nullptr;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        if (ClassifyBodyIdentity(tr.id.fill, tr.id.tag) != IdKind::ComponentIdByTag) continue;
        REQUIRE_MESSAGE(found == nullptr, "two tracks claim the disc's recorded id");
        found = &tr;
    }
    return found;
}

// What the engine sink was handed, in order.
struct Drive {
    std::vector<float> x, y, z;
    std::vector<uint64_t> t;
    size_t installs = 0;

    void Add(const BodyState& s, uint64_t at) {
        x.push_back(s.com[0]);
        y.push_back(s.com[1]);
        z.push_back(s.com[2]);
        t.push_back(at);
        ++installs;
    }
    size_t DistinctPositions() const {
        std::set<uint64_t> seen;
        for (size_t i = 0; i < x.size(); ++i) {
            uint64_t h = 1469598103934665603ull;
            const float p[3] = {x[i], y[i], z[i]};
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
            for (size_t k = 0; k < sizeof p; ++k) {
                h ^= b[k];
                h *= 1099511628211ull;
            }
            seen.insert(h);
        }
        return seen.size();
    }
    double PathMetres() const {
        double sum = 0.0;
        for (size_t i = 1; i < x.size(); ++i) {
            const double dx = x[i] - x[i - 1], dy = y[i] - y[i - 1], dz = z[i] - z[i - 1];
            sum += sqrt(dx * dx + dy * dy + dz * dz);
        }
        return sum;
    }
};

// ★ THE ORACLE, TAKEN OFF THE RECORD ITSELF. The drive is faithful or it is not, and the only
// thing that can say so is the polyline through the RECORDED disc samples over the same window --
// deduplicated by stamp, because consecutive windows overlap. The brief's "capture truth 285 m"
// is not in the document it cites; this is measured.
struct Recorded {
    std::map<uint64_t, std::array<float, 3>> byStamp;

    void Take(const Window& w, const BodyTrack& tr, int64_t fromUs, int64_t toUs) {
        const BodyState* f = w.BodyFrames(tr);
        const uint64_t* t = w.BodyTimes(tr);
        for (size_t i = 0; i < tr.count; ++i) {
            if ((int64_t)t[i] < fromUs || (int64_t)t[i] > toUs) continue;
            byStamp[t[i]] = {f[i].com[0], f[i].com[1], f[i].com[2]};
        }
    }
    double PathMetres() const {
        double sum = 0.0;
        bool first = true;
        std::array<float, 3> prev{};
        for (const auto& kv : byStamp) {
            if (!first) {
                const double dx = kv.second[0] - prev[0], dy = kv.second[1] - prev[1],
                             dz = kv.second[2] - prev[2];
                sum += sqrt(dx * dx + dy * dy + dz * dz);
            }
            prev = kv.second;
            first = false;
        }
        return sum;
    }
};

// One simulated 1x pass, rebuilding the window exactly where the transport would.
void Pass(DemoReader& reader, const TrackScan& scan, int64_t fromUs, int64_t toUs, Drive& out,
          Recorded* oracle, size_t& absentSamples) {
    Window w;
    DiscEntity ent;
    StateApplier applier;
    applier.Bind(&ent, nullptr, nullptr, nullptr);

    size_t centre = (size_t)-1;
    for (int64_t t = fromUs; t <= toUs; t += kStepUs) {
        const size_t want = ChapterForTime(reader, t);
        if (want != centre) {
            BuildStats st;
            REQUIRE_MESSAGE(BuildWindowRange(reader, want, w, st, &scan.clocks), st.refusal);
            centre = want;
            // A rebuild throws every track away, so the bind the applier needs is re-made here --
            // the same thing BindBodyHandles does on the live path after every window build.
            if (BodyTrack* disc = DiscTrackOf(w)) {
                disc->handle = PhysHandle(kDiscHandle);
                disc->handle_valid = true;
                disc->claim_owner = true;
                if (oracle) oracle->Take(w, *disc, fromUs, toUs);
            }
        }
        ApplyOptions opt;
        const ApplyStats stats = applier.Apply(w, t, opt);
        if (stats.disc_drive == DiscDrive::Installed) {
            const DiscEntity::Ring& ring = ent.ring[kDiscHandle % DiscEntity::kBodies];
            out.Add(ring.s, ring.t);
        } else if (stats.disc_drive == DiscDrive::Absent) {
            ++absentSamples;
        } else {
            FAIL_CHECK("the disc was not driven at " << (long long)t << " us: "
                                                     << doctest::String(DiscDriveName(stats.disc_drive)));
        }
    }
}

}  // namespace
}  // namespace discmotion

using discmotion::Drive;
using discmotion::kContentFromUs;
using discmotion::kContentToUs;

TEST_CASE("DISCMOTION: a 1x pass over content 23-48 s drives the disc along the recorded path") {
    DemoReader reader;
    char where[1024];
    if (!discmotion::OpenRealCapture(reader, where, sizeof where)) {
        WARN_MESSAGE(false, "SKIPPED, absent or unopenable: ", doctest::String(where));
        return;
    }

    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);
    // ★ THE SPAN THE CONTENT AXIS IS MEASURED FROM. The chapter table's first core chapter begins
    // at the manifest origin and runs for 822 s before this capture's first tick, which is exactly
    // the gap that made an in-range stamp look 2.386 s past the end of the file.
    REQUIRE(scan.have_tick_span);
    const int64_t spanFirst = scan.first_tick_us;
    const int64_t spanLast = scan.last_tick_us;
    REQUIRE(spanLast - spanFirst >= kContentToUs);

    Drive drive;
    discmotion::Recorded recorded;
    size_t absent = 0;
    discmotion::Pass(reader, scan, spanFirst + kContentFromUs, spanFirst + kContentToUs, drive,
                     &recorded, absent);

    CHECK(absent == 0);
    // ~1150 frames at 45 Hz. The recorded disc is not still: a drive that installed one pose over
    // and over -- the defect three launch reports believed they were seeing -- fails here.
    CHECK(drive.installs > 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(drive.DistinctPositions() >= 200);
    // The drive reproduces the recorded path, and the recorded path is not a point. A disc pinned
    // at the arena centre reads 0 m here; the 45 Hz drive of a 45 Hz recording reads the record's
    // own polyline to within the interpolant's rounding.
    const double path = drive.PathMetres();
    const double truth = recorded.PathMetres();
    INFO("drive metres: ", path, " recorded metres: ", truth);
    CHECK(truth > 100.0);
    CHECK(path == doctest::Approx(truth).epsilon(0.02));

    // ★ THE `out = 0` COLUMN, AS AN ASSERTION. Every stamp the engine was handed is inside the
    // playable span, on the file's own axis.
    uint64_t lo = drive.t.empty() ? 0 : drive.t[0], hi = lo;
    for (uint64_t at : drive.t) {
        lo = at < lo ? at : lo;
        hi = at > hi ? at : hi;
    }
    CHECK(lo >= (uint64_t)spanFirst);
    CHECK(hi <= (uint64_t)spanLast);
}

// The axis, on a capture whose origin is NOT zero: `t0Us` is 20.9 s and the first tick is 822 s
// after that, so a sample time that lost either subtraction lands outside the span and this case
// says so. A fixture with t0 == 0 is worthless here -- the mutant survives it.
TEST_CASE("DISCMOTION: the drive's stamps stay inside the span of a capture whose t0 is not 0") {
    DemoReader reader;
    char where[1024];
    if (!discmotion::OpenRealCapture(reader, where, sizeof where)) {
        WARN_MESSAGE(false, "SKIPPED, absent or unopenable: ", doctest::String(where));
        return;
    }
    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);
    REQUIRE(scan.have_tick_span);

    const int64_t t0 = (int64_t)reader.GetManifest().t0Us;
    REQUIRE(t0 != 0);
    // The two origins a reader can confuse, and the distance between them: content seconds are
    // measured from the first TICK, the transport's own playhead from the file origin.
    CHECK(scan.first_tick_us > t0);

    Drive drive;
    size_t absent = 0;
    discmotion::Pass(reader, scan, scan.first_tick_us + 30'000'000,
                     scan.first_tick_us + 32'000'000, drive, nullptr, absent);
    REQUIRE(drive.installs > 0);

    for (uint64_t at : drive.t) {
        // On the file's axis, inside the span; on the content axis, inside [0, span length].
        CHECK((int64_t)at >= scan.first_tick_us);
        CHECK((int64_t)at <= scan.last_tick_us);
        const int64_t content = (int64_t)at - scan.first_tick_us;
        CHECK(content >= 0);
        CHECK(content <= scan.last_tick_us - scan.first_tick_us);
    }
}
