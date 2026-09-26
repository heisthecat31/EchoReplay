// SMOOTH-1 T1: the 1:1 samplers are byte-identical to the build before motion smoothing existed.
//
// This file uses nothing smoothing added, so the same source compiles in the pre-smoothing tree; the
// digest below is what it printed there (eng-emulatejoin e35fe79). A change to the OFF path's output
// moves it.

#include "doctest.h"

#include "playback/core/sample.h"
#include "smoothing_fixture.h"

using namespace smoothing_fixture;
using replay::playback::BodyTrack;
using replay::playback::PlayerTrack;

TEST_CASE("smoothing T1: the OFF samplers are byte-identical to the pre-smoothing build") {
    Window w;
    FillSmoothWindow(w, 400, 0.004, 11);  // facts-ok: AUDIO.LISTENER_COUNT_OFF a sample count, fixed because the digest below was taken with it
    Lcg r(99);
    uint64_t h = 1469598103934665603ull;
    const uint64_t span = w.BodyTimes(w.BodyAt(0))[w.BodyAt(0).count - 1] - kT0;
    for (int pass = 0; pass < 4; ++pass) {
        const bool hermite = pass & 1;
        const bool reversed = (pass & 2) != 0;
        for (int k = 0; k < 5000; ++k) {
            int64_t t;
            if (k < 2000) t = (int64_t)(kT0 - 30000 + k * (span + 60000) / 2000);        // forward
            else if (k < 4000) t = (int64_t)(kT0 + span + 30000 - (k - 2000) * (span + 60000) / 2000);
            else t = (int64_t)(kT0 + (uint64_t)r.Uniform(0, (double)span));              // seeks
            BodyState b;
            memset(&b, 0, sizeof b);
            BodyTrack& bt = w.BodyAt(0);
            const bool bok = replay::playback::SampleBodyAt(w.BodyFrames(bt), w.BodyTimes(bt), bt.count, bt.cursor, t,
                                              b, reversed, hermite, nullptr);
            PlayerState p;
            memset(&p, 0, sizeof p);
            PlayerTrack& pt = w.PlayerAt(0);
            const bool pok = replay::playback::SamplePlayerAt(w.PlayerFrames(pt), w.PlayerTimes(pt), pt.count, pt.cursor,
                                                t, p, reversed);
            h = Fnv(h, &bok, 1);
            h = Fnv(h, &pok, 1);
            h = Fnv(h, &b, sizeof b);
            h = Fnv(h, &p, sizeof p);
        }
    }
    MESSAGE("OFF digest " << h);
    CHECK(h == 18006815160815037548ull);
}

