// SC4's two halves, in one binary: the per-tick cost histogram, and the allocation trap demonstrated
// firing and then removed.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS MEASURES AND WHAT IT CANNOT, because a cost number without a scope is a claim, not a
// measurement.
//
// MEASURES  the work the capture tick actually does per frame: staging a tick record, a sampled body
//           record per entity, and an as-seen record with its per-player rows, then pushing each into
//           the SPSC ring. That is a memcpy per record and one atomic store per push, and it is the
//           whole of what S6.1's tick is allowed to do in this implementation -- the decode, the delta
//           coding and the bit packing are on the worker (capture_types.h says why).
// CANNOT    measure the ENGINE READS. IEntity::ReadBody walks a component pool that only exists in a
//           loaded game, so its cache behaviour -- which S6.2 says is the point of gathering from
//           already-hot post-callback structures -- is not in this number. ★ THE FIGURE BELOW IS
//           THEREFORE A LOWER BOUND on the tick cost, not the SC4 result. SC4 is measured in-game
//           under S6.4's protocol (N=3 runs of >=120 s, interleaved with a no-capture control) and
//           that belongs to W4-A. Reporting this as SC4 would be reporting the half that was easy.
//
// ★ AND THE TRAP HALF IS A CONTROL, NOT A FORMALITY. S6.4 requires the trap to be DEMONSTRATED firing
// on a deliberately-inserted allocation and then removed, because a trap that has never fired is
// indistinguishable from a trap that cannot. This binary fires it on purpose, reports the site, and
// then runs the real gather with the trap armed and asserts zero.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "capture/capture_types.h"
#include "capture/costmeter.h"
#include "capture/ringset.h"
#include "capture/streamcodec.h"
#include "foundation/ring/alloc_trap.h"

using namespace replay;
using namespace replay::capture;

namespace {

RingSet g_rings;
uint8_t g_stage[4096];

bool Push(uint16_t kind, const void* a, uint32_t na, const void* b = nullptr, uint32_t nb = 0) {
    const uint32_t total = static_cast<uint32_t>(sizeof(RecHeader)) + na + nb;
    RecHeader h{};
    h.kind = kind;
    h.bytes = static_cast<uint16_t>(na + nb);
    memcpy(g_stage, &h, sizeof h);
    memcpy(g_stage + sizeof(RecHeader), a, na);
    if (nb) {
        memcpy(g_stage + sizeof(RecHeader) + na, b, nb);
    }
    return g_rings.Push(g_stage, total);
}

// One frame's worth of gather, with the engine reads replaced by reads from a resident array. The
// record shapes, counts and sizes are the real ones: 8 players, the disc, the tick spine and the
// as-seen row, which is S6.4's stated load condition minus the parts that need a game.
void OneFrame(uint32_t frame, uint64_t sourceUs, const BodyState* bodies, uint32_t bodyCount,
              uint32_t players) {
    TickRec t{};
    t.sourceTimeUs = sourceUs;
    t.simTimeUs = sourceUs;
    t.peerTimeUs = sourceUs;
    t.frameIndex = frame;
    t.scaledDeltaUs = 3700;
    t.frameDeltaUs = 11111;
    t.asSeenValid = 0x1F;
    Push(kRecTick, &t, sizeof t);

    for (uint32_t i = 0; i < bodyCount; ++i) {
        SampledBodyRec r{};
        r.sourceTimeUs = sourceUs;
        r.frameIndex = frame;
        r.handle = static_cast<uint16_t>(i);
        // The bench measures the tick's cost, so it carries the same shape the real gather pushes:
        // a transform with no identity, and a mask that says so.
        r.idValue = 0;
        r.idTag = 0;
        r.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits;
        r.state = bodies[i];
        Push(kRecSampledBody, &r, sizeof r);
    }

    AsSeenRec as{};
    as.sourceTimeUs = sourceUs;
    as.frameIndex = frame;
    as.playerCount = static_cast<uint16_t>(players);
    as.sample.peerTimeUs = sourceUs;
    as.sample.renderDelaySec = -0.04444f;
    as.sample.valid = 0x19;
    uint8_t per[AsSeenFrame::kMaxPlayers * sizeof(AsSeenPerPlayer)];
    for (uint32_t p = 0; p < players; ++p) {
        AsSeenPerPlayer pp{};
        pp.peerTimeUs = sourceUs - uint64_t(p) * 100;
        pp.rttMs = static_cast<uint16_t>(20 + p);
        memcpy(per + size_t(p) * sizeof(AsSeenPerPlayer), &pp, sizeof pp);
    }
    Push(kRecAsSeen, &as, sizeof as, per, players * static_cast<uint32_t>(sizeof(AsSeenPerPlayer)));
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t frames = 200000;
    uint32_t players = 8;
    if (argc > 1) frames = static_cast<uint32_t>(atoi(argv[1]));
    if (frames == 0) frames = 1;

    std::vector<uint8_t> mem(8u * 1024u * 1024u, 0);
    if (!g_rings.Init(mem.data(), static_cast<uint32_t>(mem.size()))) {
        printf("BENCH-RESULT status=NOT-RUN reason=ring-init\n");
        return 2;
    }

    // 8 players plus the disc, S6.4's load condition.
    const uint32_t bodyCount = players + 1;
    std::vector<BodyState> bodies(bodyCount);
    for (uint32_t i = 0; i < bodyCount; ++i) {
        for (int k = 0; k < 3; ++k) {
            bodies[i].com[k] = 1.0f * float(i) + float(k);
            bodies[i].vel[k] = 0.25f * float(k);
            bodies[i].angVel[k] = 0.5f;
            bodies[i].lastHeldCom[k] = 2.0f;
        }
        bodies[i].ori[3] = 1.0f;
        bodies[i].bits11 = i;
    }

    // ---- part 1: the trap, demonstrated firing --------------------------------------------------
    const uint32_t live = TrapInstall(kTrapAll);
    printf("trap install : asked 0x%X, live 0x%X%s\n", unsigned(kTrapAll), unsigned(live),
           live == kTrapAll ? "" : "  <- a site that did not install is a trap that cannot fire");
    TrapArm();
    TrapNoteAllocation("bench: deliberate");
    TrapReport fired = TrapTake();
    TrapDisarm();
    const bool trapFires = fired.allocations > 0;
    printf("trap control : deliberate allocation -> allocations=%u site=%s  [%s]\n",
           fired.allocations, fired.firstSite ? fired.firstSite : "(none)",
           trapFires ? "FIRES" : "DID NOT FIRE");

    // ---- part 2: the gather, with the trap armed ------------------------------------------------
    CostMeter meter;
    meter.Init();

    // Warm the paths once so the histogram measures steady state rather than first-touch page faults.
    for (uint32_t i = 0; i < 1000; ++i) {
        OneFrame(i, 1700000000000000ull + uint64_t(i) * 11111, bodies.data(), bodyCount, players);
        uint32_t got = 0;
        uint8_t sink[4096];
        while (g_rings.Pop(sink, sizeof sink, &got) != 0) {
        }
    }

    TrapArm();
    uint64_t sourceUs = 1700000000000000ull;
    uint8_t sink[4096];
    for (uint32_t f = 0; f < frames; ++f) {
        const uint64_t t0 = meter.Start();
        OneFrame(f, sourceUs, bodies.data(), bodyCount, players);
        meter.Stop(t0);
        sourceUs += 11111;
        // Drain outside the measured window: the worker does this on its own thread and its cost is
        // not the tick's. Measuring it here would attribute the consumer to the producer.
        uint32_t got = 0;
        while (g_rings.Pop(sink, sizeof sink, &got) != 0) {
        }
    }
    const TrapReport during = TrapTake();
    TrapDisarm();
    TrapUninstall();

    CostReport rep{};
    meter.Report(rep);

    printf("\nper-tick cost, %u frames, %u players + disc, gather and push only:\n", frames, players);
    printf("  samples      : %" PRIu64 "\n", rep.samples);
    printf("  median       : <= %u us   (S6.4 budget 20)\n", rep.medianUs);
    printf("  p99          : <= %u us   (S6.4 budget 50)\n", rep.p99Us);
    printf("  p99.9        : <= %u us   (S6.4 hard fail above 200)\n", rep.p999Us);
    printf("  max          : %u us\n", rep.maxUs);
    printf("  histogram    :");
    for (uint32_t i = 0; i < kCostBuckets; ++i) {
        if (rep.buckets[i]) {
            printf(" <=%uus:%" PRIu64, rep.bucketUpperUs[i], rep.buckets[i]);
        }
    }
    printf("\n");
    printf("  ring         : %" PRIu64 " pushed, %" PRIu64 " dropped\n", g_rings.PushedRecords(),
           g_rings.DroppedRecords());
    printf("  with the trap ARMED over the whole run: allocations=%u locks=%u fileOps=%u first=%s\n",
           during.allocations, during.locks, during.fileOps,
           during.firstSite ? during.firstSite : "(none)");

    // A trap that never fired proves nothing, so the control decides whether the zero above is worth
    // anything. Both conditions have to hold.
    const bool clean = during.allocations == 0 && during.locks == 0 && during.fileOps == 0;
    const bool ok = trapFires && clean && rep.samples > 0 && g_rings.DroppedRecords() == 0;
    printf("BENCH-RESULT status=%s median_us=%u p99_us=%u p999_us=%u samples=%" PRIu64
           " trap_control=%s trap_clean=%s\n",
           ok ? "PASS" : "FAIL", rep.medianUs, rep.p99Us, rep.p999Us, rep.samples,
           trapFires ? "fired" : "SILENT", clean ? "yes" : "no");
    printf("  ★ this is a LOWER BOUND on the tick cost and is not SC4: the engine pool reads are not\n"
           "    in it, and S6.4's protocol is three interleaved in-game runs, which is W4-A's.\n");
    return ok ? 0 : 1;
}
