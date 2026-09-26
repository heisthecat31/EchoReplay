// AUDIT_ARCH21 CONDITION-B6. The span cache latched a SUCCESS for the whole binding.
//
// EntityBind advances the epoch only when the resolved SpaceIdentity {netGame, space, entries}
// differs from the bound one. A component pool that is FREED, with its address left unmapped and
// those three unchanged, moves nothing the cache keys on: `b == base` holds, the epoch has not
// advanced, and Prove hands back a base into unmapped memory for the rest of the binding. A pool
// that MOVES was already safe -- the base is re-read per call and the comparison fails -- which is
// exactly why the freed-at-the-same-address case is the one that needed a cadence.
//
// The page here is this test's own VirtualAlloc, released with MEM_RELEASE: the address stays
// numerically what it was and the mapping does not.

#include "doctest.h"

#include <windows.h>

#include "engine/entity.h"
#include "generated/facts_generated.h"

TEST_CASE("a proof freed at an unchanged base stops being handed back, within the cadence") {
    const size_t bytes = 4096;
    void* p = ::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(p != nullptr);
    const uint8_t* const p8 = static_cast<const uint8_t*>(p);
    const uint32_t bound = replay::entity::SpanProofMaxCachedReads();
    REQUIRE(bound > 0u);

    replay::entity::SpanProof span;

    const uint64_t probe0 = replay::entity::SpanProbeCount();
    REQUIRE(span.Prove(p, bytes) == p8);
    CHECK(replay::entity::SpanProbeCount() - probe0 == 1u);

    // The cache is still a cache: the whole burst it is sized for costs nothing.
    const uint64_t probe1 = replay::entity::SpanProbeCount();
    for (uint32_t i = 0; i < bound; ++i) REQUIRE(span.Prove(p, bytes) == p8);
    CHECK(replay::entity::SpanProbeCount() - probe1 == 0u);

    // And then it asks again -- once, and while the page is still mapped the answer is the same.
    REQUIRE(span.Prove(p, bytes) == p8);
    CHECK(replay::entity::SpanProbeCount() - probe1 == 1u);

    REQUIRE(::VirtualFree(p, 0, MEM_RELEASE) != 0);

    // Nothing the binding keys on has changed. Only the cadence can catch this.
    const uint64_t probe2 = replay::entity::SpanProbeCount();
    uint32_t stale = 0;
    const uint8_t* got = p8;
    for (uint32_t i = 0; i <= bound + 1u; ++i) {
        got = span.Prove(p, bytes);
        if (got == nullptr) break;
        ++stale;
    }
    CHECK(got == nullptr);
    CHECK(stale <= bound);
    // The refusal came from a FRESH probe, not from a remembered one: a refusal is an answer about
    // the instant it was taken, and Prove caches neither direction.
    CHECK(replay::entity::SpanProbeCount() - probe2 == 1u);

    // A refusal is not latched either -- but since ENG-W1E-SPANPROOF-REFUSAL it IS remembered for
    // an interval, and this allocation lands on the address the free above just released often
    // enough that "the next mapped base is proved on its own merits" is not the property to
    // assert. The property the cadence buys is: whatever address this lands on, the mapped span is
    // answered within one interval and not never.
    void* q = ::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(q != nullptr);
    const uint32_t reprobe = replay::entity::SpanProofReprobeTicks();
    uint32_t calls = 0;
    const uint8_t* mapped = nullptr;
    while (calls <= reprobe + 1u && (mapped = span.Prove(q, bytes)) == nullptr) ++calls;
    CHECK(mapped == static_cast<const uint8_t*>(q));
    CHECK(calls <= reprobe);
    ::VirtualFree(q, 0, MEM_RELEASE);
}

// The other half of the cadence: a REFUSAL used to cost a fresh
// VirtualQuery every call, so a pool slot that is not there was a syscall per slot per capture
// tick for the life of the binding -- ENG-GATHER-COST reproduced 64 refusing slots at 64 probes a
// tick, which is ~1 ms a tick at the 17 us a probe cost ENG-STATSPERF measured late in a match.

TEST_CASE("A pool of refusing slots costs one probe per slot per interval") {
    const uint32_t reprobe = replay::entity::SpanProofReprobeTicks();
    REQUIRE(reprobe > 1u);

    constexpr size_t   kSlotBytes = 4096;
    constexpr uint32_t kSlots     = 64;      // L3-RUN-15/16's pool walk
    constexpr uint32_t kTicks     = 10000;

    // Reserved and never committed: unreadable at every slot, with no page this process could
    // fault into by accident.
    void* region = ::VirtualAlloc(nullptr, kSlotBytes * kSlots, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(region != nullptr);
    uint8_t* const r8 = static_cast<uint8_t*>(region);

    // One line per slot, which is the shape GatherBodies walks.
    replay::entity::SpanProof lines[kSlots];

    // Every slot really refuses. A slot that answered would make the count below a measurement of
    // nothing, and it is asserted here rather than assumed of MEM_RESERVE.
    for (uint32_t s = 0; s < kSlots; ++s)
        REQUIRE(lines[s].Prove(r8 + s * kSlotBytes, kSlotBytes) == nullptr);

    LARGE_INTEGER freq, t0, t1;
    ::QueryPerformanceFrequency(&freq);
    const uint64_t before = replay::entity::SpanProbeCount();
    uint32_t answered = 0;
    uint32_t worstTick = 0;
    ::QueryPerformanceCounter(&t0);
    for (uint32_t t = 0; t < kTicks; ++t) {
        const uint64_t tickStart = replay::entity::SpanProbeCount();
        for (uint32_t s = 0; s < kSlots; ++s)
            if (lines[s].Prove(r8 + s * kSlotBytes, kSlotBytes) != nullptr) ++answered;
        const uint32_t inTick = uint32_t(replay::entity::SpanProbeCount() - tickStart);
        if (inTick > worstTick) worstTick = inTick;
    }
    ::QueryPerformanceCounter(&t1);
    const uint64_t probes    = replay::entity::SpanProbeCount() - before;
    const double   perTickUs =
        double(t1.QuadPart - t0.QuadPart) * 1e6 / double(freq.QuadPart) / double(kTicks);

    MESSAGE("ENG-W1E-SPANPROOF-REFUSAL: " << kSlots << " refusing slot(s) x " << kTicks
                                          << " ticks, " << perTickUs << " us a tick, " << probes
                                          << " span probe(s) total, worst tick " << worstTick);

    CHECK(answered == 0);                 // reserved memory stayed unreadable for the whole walk
    // The finding's number was probes == kTicks * kSlots. The bound is one probe per slot per
    // interval, derived from the shipped cadence rather than from a literal.
    CHECK(probes <= uint64_t(kSlots) * ((kTicks + reprobe - 1u) / reprobe));
    CHECK(probes >= kSlots);              // and it IS still asking: a cadence, not a latch
    // Nor may the interval turn a flat cost into a spike: 64 lines that start refusing on the same
    // tick must not come due on the same tick, or this is the 1 ms tick once a second instead of
    // always. The stagger is what holds this, and nothing else in the suite sees it.
    CHECK(worstTick <= (kSlots + reprobe - 1u) / reprobe + 1u);

    // The wall clock as a same-run ratio: one VirtualQuery, the syscall this cache exists to
    // remove, measured on the same box under the same load, so both numbers move together. The
    // pre-fix walk is kSlots probes' worth of tick; the bound below is a small multiple of the
    // ~kSlots/reprobe it should now be, and an order-of-magnitude regression cannot fit in it.
    MEMORY_BASIC_INFORMATION mbi;
    size_t probed = 0;
    LARGE_INTEGER q0, q1;
    ::QueryPerformanceCounter(&q0);
    for (uint32_t i = 0; i < kSlots; ++i) probed += ::VirtualQuery(r8, &mbi, sizeof mbi);
    ::QueryPerformanceCounter(&q1);
    REQUIRE(probed == size_t(kSlots) * sizeof mbi);   // the baseline really ran
    const double perProbeUs =
        double(q1.QuadPart - q0.QuadPart) * 1e6 / double(freq.QuadPart) / double(kSlots);
    REQUIRE(perProbeUs > 0.0);
    MESSAGE("ENG-W1E-SPANPROOF-REFUSAL: baseline " << perProbeUs << " us a VirtualQuery, so the "
                                                   << "walk is " << (perTickUs / perProbeUs)
                                                   << " probes' worth a tick");
    CHECK(perTickUs <= 12.0 * perProbeUs);
    CHECK(perTickUs <= 30.0);             // the gather's whole per-tick budget, on this host

    REQUIRE(::VirtualFree(region, 0, MEM_RELEASE) != 0);
}

TEST_CASE("a refused span that becomes readable is answered within one re-probe interval") {
    const uint32_t reprobe = replay::entity::SpanProofReprobeTicks();

    // The interval is one second of the engine's fixed step, and it is far shorter than the
    // success cadence: a stale success hands back a base that has been freed, a stale refusal only
    // hides one that has arrived.
    CHECK(reprobe * facts::CLOCK_FIXEDSTEP_30HZ_US <= 1000000u);
    CHECK((reprobe + 1u) * facts::CLOCK_FIXEDSTEP_30HZ_US > 1000000u);
    CHECK(reprobe < replay::entity::SpanProofMaxCachedReads());

    const size_t bytes = 4096;
    void* p = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(p != nullptr);

    replay::entity::SpanProof span;
    REQUIRE(span.Prove(p, bytes) == nullptr);

    // The refusal is remembered, so the next tick's walk of this slot costs nothing.
    const uint64_t before = replay::entity::SpanProbeCount();
    REQUIRE(span.Prove(p, bytes) == nullptr);
    CHECK(replay::entity::SpanProbeCount() - before == 0u);

    // And then the slot is built -- the case the old "a refusal is never cached" comment was
    // written to protect, which the cadence protects one interval later instead of never.
    REQUIRE(::VirtualAlloc(p, bytes, MEM_COMMIT, PAGE_READWRITE) == p);

    uint32_t calls = 0;
    const uint8_t* got = nullptr;
    while (calls <= reprobe + 1u && (got = span.Prove(p, bytes)) == nullptr) ++calls;
    MESSAGE("ENG-W1E-SPANPROOF-REFUSAL: readable again after " << calls << " refused call(s), "
                                                               << "interval " << reprobe);
    CHECK(got == static_cast<const uint8_t*>(p));
    CHECK(calls <= reprobe);

    REQUIRE(::VirtualFree(p, 0, MEM_RELEASE) != 0);
}
