#pragma once

// The per-frame gather, as templates a test can instantiate.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS IS A HEADER AND NOT PRIVATE TO capture.cpp.
//
// W2-B's end-to-end chain test runs synthetic engine state through the real capture path, to a real
// .echodemo, back through the real decoders and into the applier, and asserts the installed state
// equals what the capture was given. It could not start at the beginning: the gathers were templates
// in a .cpp with no header, so the test began at the RING RECORD and mirrored the fill masks BY HAND
// from this packet's source. A gather that set them differently would not have been caught -- and the
// fill mask is precisely the thing that says whether a field is a measurement.
//
// So the gather is the seam now. It takes a SINK rather than pushing into the module's ring, which is
// what lets a test drive it, and it reports what it saw through GatherReport rather than logging --
// logging is the module's business, and a header that needed ILog would drag the bootstrap into every
// consumer of this file.
//
// The templates stay templates for the reason capture.cpp records: services.h forward-declares
// IEntity and IAsSeen, and while those were incomplete types the bodies could not be instantiated at
// all. That is fixed now, but the shape is still the right one -- it is what lets a test pass a fake.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>

#include "capture/capture_types.h"
#include "capture/streamcodec.h"
#include "engine/asseen.h"
#include "engine/entity.h"

namespace replay {
namespace capture {

// Returns false when the record could not be taken. The gather counts it and carries on, because one
// refused record must not cost the rest of the frame.
using GatherSink = bool (*)(uint16_t kind, const void* head, uint32_t headBytes, const void* tail,
                            uint32_t tailBytes, void* ctx);

struct GatherConfig {
    bool streamEnabled[32];
};

// What the gather actually saw. The module turns this into log lines and DEGRADED records; a test
// asserts on it directly.
// LocalPlayerAt's ABSENCE word. It is the IEntity default's own return for "no such slot", not a
// value out of the fact registry, and it is named once here so no caller carries the literal.
constexpr uint16_t kNoUserSlot = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value

struct GatherReport {
    // The pool size the engine answered with, kept apart from what was walked: a pool of 0 and a
    // pool this build clamped are different capture failures and `bodiesPresent` alone shows both
    // as "few bodies". B1: three captures carried zero body samples and no counter said which.
    uint32_t bodyPoolCount = 0;
    uint32_t bodiesBeyondCap = 0;
    uint32_t bodiesPresent = 0;
    uint32_t bodiesSampled = 0;
    // Present bodies whose ReadBody returned false. THE B1 ARM: the reader takes the newest history
    // node, and a locally simulated body has no history, so every body the old locally-controlled
    // selector chose was one this reader could not read. pawnsRefused exists for exactly this
    // distinction on the pawn side (DIAG-L2 F4) and bodies had no equivalent, so "the pool was
    // empty" and "the pool resolved and the reader refused" produced the same zero.
    uint32_t bodiesRefused = 0;
    // Of the walked bodies, how many the engine's own ownership predicate claimed. Recorded, not
    // used as a filter -- see GatherBodies.
    uint32_t bodiesLocallyControlled = 0;
    // WHICH SOURCE ANSWERED, per reading. 5abca72 made the body read two-sourced (the newest history
    // node, or the live CPhBody this machine integrates) and until now the capture threw that away:
    // a file whose bodies all came from history is a recording of everyone but the owner, and it is
    // byte-indistinguishable from a healthy one by count alone. NotAsked is kept apart from None
    // because an IEntity that predates ReadBodyDetailed must not be reported as an engine that had
    // no body -- that is a claim about the engine, made by a reader that never asked.
    // Every present body contributes exactly one of these four, so their sum is bodiesPresent.
    uint32_t bodiesFromLocal = 0;
    uint32_t bodiesFromHistory = 0;
    uint32_t bodiesSourceNone = 0;
    uint32_t bodiesSourceNotAsked = 0;
    // WHICH GATE refused, per reading. L2 #9 saw `refused 1` every tick with five candidate causes
    // and no way to tell them apart; ENG-W1E-LOCALBODY-3 gave BodyReading a 19-arm BodyRefusal, and
    // a count that still cannot name its cause buys another launch that reproduces the same zero.
    // Index 0 is BodyRefusal::None -- nothing refused -- and IS counted, so this array plus
    // bodyRefusalUnknown sums to bodiesPresent the same way the four source counters do. It is not
    // published: "none" on every healthy read is not a refusal.
    uint32_t bodyRefusals[kBodyRefusalCount] = {};
    // A refusedAt this build has no enumerator for. Kept apart from None rather than folded into it:
    // an out-of-range value is a reading this counter cannot interpret, not a read that succeeded.
    uint32_t bodyRefusalUnknown = 0;
    // WHICH ARM produced the angular velocity, per reading. The engine stores omega on a history
    // node and stores it NOWHERE for a live CPhBody (it least-squares one out of the particle
    // cloud on demand), and the ncaTransform arm has no cloud to derive one from at all -- so a
    // structural zero and a measured zero are the same three floats in the file and only this
    // says which was recorded. Every reading contributes exactly one, refused ones included
    // (they reached no arm and say NotAsked), so this sums to bodiesPresent like the four
    // source counters do.
    uint32_t angVelBy[kAngVelSourceCount] = {};
    // A source this build has no enumerator for -- kept apart from NotAsked for the reason the
    // refusal histogram keeps its own unknown: an uninterpretable reading is not a read that
    // never asked.
    uint32_t angVelUnknown = 0;
    // THE FIRST REFUSAL OF THE WALK, whole. The histogram above says WHICH gate refused and
    // how often; it does not say what the gates SAW when the walk first went wrong, and L2 #11
    // concluded "the bit is CLEAR" from an arm that cannot tell clear from never-asked. Kept
    // once -- `firstRefusalSeen` is the latch -- because a diag the next refusal overwrites is
    // the LAST one wearing the first one's name, and because after the first there is nothing
    // left to pay per body.
    bool firstRefusalSeen = false;
    replay::BodyReading firstRefusal{};
    uint32_t pawnsPresent = 0;
    uint32_t pawnsSampled = 0;
    uint32_t asSeenPlayers = 0;
    // The fill mask ReadLocalPlayer actually returned, translated into PlayerFill. This IS the derived
    // gap list: read from the engine, never declared in a comment.
    uint32_t localFillMask = 0;
    // Present local pawns whose ReadLocalPlayer returned an EMPTY mask, so no row was written for
    // them. DIAG-L2 F4: `pawnsPresent` non-zero with `pawnsSampled` zero was reported as a gap in the
    // file with no arm named, and "the pool was empty" then looked the same as "the pool resolved and
    // the accessor refused". This counts the second.
    uint32_t pawnsRefused = 0;
    // Entries LocalPlayerAt answered with its no-such-slot word, dropped before they reached the
    // frame set. W1-E R5: pushed through, each one became a present-and-unsampled entity and a
    // DROPPED row for a player that does not exist.
    uint32_t pawnsUnknownSlot = 0;
    // Present pawns the ENGINE sourced -- a non-empty group mask -- that this build could not store,
    // because the mask carried no bit LocalMaskToFill has a line for. A subset of pawnsRefused, and a
    // different owner: the accessor answered and the CAPTURE dropped the row. L2-RUN-2 F2 was exactly
    // this and was reported as the accessor refusing, because nothing counted the two apart.
    uint32_t pawnsUntranslated = 0;
    // PawnsUntranslated above is the TOTAL loss -- it can only be reached when the
    // translated fill came out zero and no row was written at all. The more dangerous half is
    // PARTIAL: a pawn whose mask is kLocalPose|<a bit with no line> translates to a non-zero fill, the
    // row IS written, and it is written with that group missing for every frame of the match. These
    // two count any dropped bit, on the success path as well, so a file cannot lack a group silently.
    uint32_t pawnsGroupLoss = 0;         // pawns with >= 1 sourced group this build cannot store
    uint32_t rowsWithGroupLoss = 0;      // of those, the ones whose row was WRITTEN anyway
    uint32_t lostLocalGroupBits = 0;     // union of the engine bits dropped, for the log
    // The two actor sources disagreeing is a fact about the READ, not about a refusal:
    // ENG-W1E-R5's own CLIENT rig disagrees and fills all six groups, which is a SUCCESSFUL read, so
    // the refusal-only report below says nothing about it. Counted for every reading, so the file can
    // say the local-pawn groups may have come from an actor the other source did not name.
    uint32_t localDisagreements = 0;
    bool localDisagreeKnown = false;
    uint8_t localDisagreeSource = 0;     // ActorSource of the FIRST disagreeing read
    // The FIRST refusal reason the engine gave, as IEntity::LocalRefusal. Carried out of the gather
    // so the log and the sentinel can name the arm rather than a count (DIAG-L2 F4).
    uint8_t localRefusal = 0;
    // 0 IS A REAL ENUMERATOR (LocalRefusal::NotAsked), so it cannot also mean `unset`. Without this
    // flag the first pawn to answer NotAsked leaves the slot looking empty and a later pawn overwrites
    // the reason the first one gave -- the FIRST refusal is the one the line claims to report.
    bool localRefusalKnown = false;
    // The engine's own mask for that first refused pawn, UNtranslated. `ok` beside an empty file is
    // only readable next to the mask the accessor actually returned.
    uint32_t localRefusedEngineMask = 0;
    // The three facts the reading carries that decide the group-level cause when an actor DID resolve:
    // which source named the actor, whether it carried the echo component every pose group hangs off,
    // and whether the two actor sources disagreed. Raw enum values, rendered at the log.
    uint8_t localActorSource = 0;
    bool localEchoComponent = false;
    bool localSourcesDisagree = false;
    bool sawLocalPawn = false;
    bool sawBodyWithoutNetId = false;
    uint32_t sinkRefusals = 0;
};

// IEntity's kLocal groups mapped onto the file's PlayerFill groups. One line per group, so a group
// added on either side has exactly one place to be handled and no default that silently drops it.
constexpr uint32_t LocalMaskToFill(uint32_t localMask) {
    static_assert(kLocalPose == kFillPose, "pose groups must correspond");
    static_assert(kLocalRefVel == kFillRefVel, "refvel groups must correspond");
    static_assert(kLocalBody == kFillBody, "body groups must correspond");
    static_assert(kLocalTrigger == kFillTrigger, "trigger groups must correspond");
    static_assert(kLocalAnchor == kFillAnchor, "anchor groups must correspond");
    uint32_t fill = 0;
    if (localMask & kLocalPose) fill |= kFillPose;
    if (localMask & kLocalRefVel) fill |= kFillRefVel;
    if (localMask & kLocalBody) fill |= kFillBody;
    if (localMask & kLocalTrigger) fill |= kFillTrigger;
    if (localMask & kLocalAnchor) fill |= kFillAnchor;
    // W1-E DID add the bit and this line is where it arrived. The two enums do NOT agree here:
    // kLocalHolding is 1<<5 and kFillHolding is 1<<6, while 1<<5 on the file side is kFillActionWord --
    // so an assignment, or a static_assert of the kind above, would have stored the hold ids under the
    // action word's bit. The translation is the only correct form and it has to be written out.
    if (localMask & kLocalHolding) fill |= kFillHolding;
    // There is deliberately no line for kFillActionWord: IEntity declares no bit that could set it.
    // bits28 is the A1 gap FOR THE LOCAL PAWN -- ReadPlayer sources it for a remote -- and the absence
    // of a line here is the code stating it.
    //
    // L2-RUN-2 F2, AND THE REASON THIS ASSERT EXISTS. kLocalHolding was declared in entity.h and had
    // no line here, so a pawn whose ONLY sourced group was holding translated to 0, GatherLocalPawns
    // counted it refused, no row was written, and the reading still said `ok` -- 971 lost records and
    // `players : 0` in a file whose accessor had answered. A group bit added on the engine side and
    // nowhere here must break the BUILD; there is no runtime path on which that mistake is visible.
    return fill;
}

// THE TOTALITY PROPERTY, stated as a property rather than as a restatement. It asks the
// TRANSLATOR, one bit at a time: every group bit IEntity can set must come back from LocalMaskToFill
// as a non-zero fill. The assert this replaced compared kLocalAll against an OR list written beside
// it, which is why deleting the kLocalHolding translation line built clean (AUD-M9 MU3) and why the
// mechanical repair for a NEW bit was to extend that list -- silencing the build without adding a
// translation and restoring the exact defect the assert was written for.
constexpr bool EveryLocalGroupTranslates() {
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((replay::kLocalAll & bit) != 0 && LocalMaskToFill(bit) == 0) {
            return false;
        }
    }
    return true;
}
static_assert(EveryLocalGroupTranslates(),
              "a kLocal* group bit goes into LocalMaskToFill and comes back as fill 0. The engine "
              "sources that group and this build would drop it -- L2-RUN-2 F2, exactly. The only "
              "repair is a translation line INSIDE LocalMaskToFill mapping the bit to its kFill* "
              "group; there is no list here that adding a name to would fix it.");

// The kLocal* bits this build can actually store, DERIVED by asking the translator. The residue
// `engineMask & ~kLocalBitsWithFill` is the group loss a row can carry silently, and
// deriving it here means a bit that loses its line changes this value too.
constexpr uint32_t TranslatedLocalBits() {
    uint32_t bits = 0;
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if (LocalMaskToFill(bit) != 0) {
            bits |= bit;
        }
    }
    return bits;
}
constexpr uint32_t kLocalBitsWithFill = TranslatedLocalBits();

// EVERY body in the physics pool, each tick. NOT `IsLocallyControlled` any more, and that is the
// whole of AUDIT_FINAL_E2E B1: the predicate selects the bodies this machine SIMULATES, and
// Entity::ReadBody returns the newest HISTORY node -- which a simulated body does not have (see its
// own comment: "for a locally simulated one there is no history and this returns false"). Selector
// and reader named complementary sets, their intersection was empty by construction, and every
// capture ever taken carried zero S_BODY_SAMPLED records while `BodyState.com` is the only carrier
// of a player's world position in the whole format. Nothing moved in any replay.
//
// Walking the pool fixes it from the reader's side rather than the predicate's: net-driven bodies --
// the other players, the disc -- do have history, so they read, and they carry a net id playback can
// bind. Locality stops being a filter and becomes a counter; a body the reader still refuses is
// counted and reported instead of vanishing.
template <class E>
void GatherBodies(E* e, const GatherConfig& cfg, uint64_t sourceUs, uint32_t frameIndex,
                  GatherSink sink, void* ctx, GatherReport& rep) {
    if (!e || !e->Available() || !cfg.streamEnabled[kStreamBodySampled]) {
        return;
    }
    const size_t bodies = e->PhysCount();
    rep.bodyPoolCount = static_cast<uint32_t>(bodies);
    if (bodies > kMaxBodies) {
        rep.bodiesBeyondCap = static_cast<uint32_t>(bodies - kMaxBodies);
    }

    // A3. What the ENGINE HELD this frame, pushed BEFORE the samples so the accountant can tell a body
    // we skipped from one that was not there. Only the tick enumerates the pool, so only the tick can
    // make this statement, and ReadBody returning false is a real path that used to produce no record
    // and no observation at all.
    uint16_t presentIds[kMaxBodies];
    uint16_t presentCount = 0;
    for (size_t i = 0; i < bodies && i < kMaxBodies; ++i) {
        const PhysHandle h(static_cast<uint16_t>(i));
        if (e->IsLocallyControlled(h)) {
            ++rep.bodiesLocallyControlled;
        }
        presentIds[presentCount++] = static_cast<uint16_t>(i);
    }
    rep.bodiesPresent = presentCount;

    FrameSetRec fs{};
    fs.tUs = sourceUs;
    fs.frameIndex = frameIndex;
    fs.stream = kStreamBodySampled;
    fs.count = presentCount;
    if (!sink(kRecFrameSet, &fs, sizeof fs, presentIds,
              uint32_t(presentCount) * sizeof(uint16_t), ctx)) {
        ++rep.sinkRefusals;
    }

    for (uint16_t k = 0; k < presentCount; ++k) {
        const PhysHandle h(presentIds[k]);
        SampledBodyRec rec{};
        replay::BodyReading reading{};
        const bool read = e->ReadBodyDetailed(h, rec.state, reading);
        switch (reading.source) {
            case replay::BodySource::Local:    ++rep.bodiesFromLocal; break;
            case replay::BodySource::History:  ++rep.bodiesFromHistory; break;
            case replay::BodySource::None:     ++rep.bodiesSourceNone; break;
            case replay::BodySource::NotAsked: ++rep.bodiesSourceNotAsked; break;
        }
        const unsigned arm = unsigned(reading.refusedAt);
        if (arm < kBodyRefusalCount) {
            ++rep.bodyRefusals[arm];
        } else {
            ++rep.bodyRefusalUnknown;
        }
        const unsigned av = unsigned(reading.diag.angVel);
        if (av < kAngVelSourceCount) {
            ++rep.angVelBy[av];
        } else {
            ++rep.angVelUnknown;
        }
        if (!read) {
            ++rep.bodiesRefused;
            if (!rep.firstRefusalSeen) {
                rep.firstRefusalSeen = true;
                rep.firstRefusal = reading;
            }
            continue;   // present and not recorded: EndFrameSet turns this into a DROPPED record
        }
        rec.sourceTimeUs = sourceUs;
        rec.frameIndex = frameIndex;
        rec.handle = presentIds[k];
        rec.fill = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits;
        // The identity, through the net-actor table. NetIdOf returns 0 for a body with no entry -- the
        // honest answer for a purely local one -- and the fill bits stay clear, so a row that cannot
        // name its subject says so rather than carrying a zero that looks like a key.
        //
        // kFillBodyIdNetActor rather than kFillBodyIdTag: the accessor GUARANTEES a net id whatever
        // tag byte the component held, and that is a different claim from having read the tag.
        rec.idValue = e->NetIdOf(h);
        rec.idTag = 0;
        if (rec.idValue != 0) {
            rec.fill |= kFillBodyIdValue | kFillBodyIdNetActor;
        } else {
            rep.sawBodyWithoutNetId = true;
        }
        if (!sink(kRecSampledBody, &rec, sizeof rec, nullptr, 0, ctx)) {
            ++rep.sinkRefusals;
            continue;
        }
        ++rep.bodiesSampled;
    }
}

// The one sentence a launch needs about its bodies, DERIVED from the counters rather than written
// beside them, so a capture cannot report a body stream it does not have. Returns nullptr when the
// tick sampled at least one body; otherwise it names which arm produced the zero.
inline const char* BodyGapArm(const GatherReport& rep) {
    if (rep.bodiesSampled != 0) return nullptr;
    if (rep.bodyPoolCount == 0) {
        return "the physics pool answered 0 bodies -- the component system resolved but its pool is "
               "not built, or PhysCount could not be read";
    }
    if (rep.bodiesRefused >= rep.bodiesPresent) {
        return "every body in the pool was REFUSED by the body reader -- ReadBody found no history "
               "node to read, so this capture carries no world position for anyone (B1)";
    }
    if (rep.sinkRefusals != 0) {
        return "the ring refused the body records -- the pool and the reader both answered";
    }
    return "no body record reached the file and no arm named itself -- treat as unexplained";
}

// The local pawn, sampled off the pools at frame rate. S6.2 calls this the single biggest fidelity win
// available, and it is what makes a client capture contain the owner's own hands.
//
// W1-E did NOT detour SR15NetPlayerStateMsg::Send to get it, and that judgement protects these
// captures: the send is interval-gated, so a capture taken from it would lose the local pawn whenever
// the network is idle, including the offline sessions the owner most wants recorded.
//
// Keyed by USER SLOT, the same u16 PlayerOf takes, so a local row and a remote row for one player sit
// in one index space rather than two that have to be reconciled at playback.
//
// ★ THAT CLAIM IS ABOUT S_PLAYER_SAMPLED ONLY, and audit A4 exists because it did not say so.
// S_PLAYER_WIRE is a DIFFERENT index space -- a dense first-seen index over the frame header word,
// assigned by ChapterBuilder, not a user slot -- so wire slot k and sampled slot k are not the same
// player and must never be merged on the index alone. A reader binds a wire row to a person through
// the header word the keyframe carries beside the slot, never by matching the two numbers.
template <class E>
void GatherLocalPawns(E* e, const GatherConfig& cfg, uint64_t sourceUs, uint32_t frameIndex,
                      GatherSink sink, void* ctx, GatherReport& rep) {
    if (!e || !e->Available() || !cfg.streamEnabled[kStreamPlayerSampled]) {
        return;
    }
    const size_t n = e->LocalPlayerCount();

    uint16_t presentIds[kMaxEntities];
    uint16_t presentCount = 0;
    for (size_t i = 0; i < n && i < kMaxEntities; ++i) {
        const uint16_t slot = e->LocalPlayerAt(i).v;
        // 0xFFFF is LocalPlayerAt's ABSENCE word, not a user slot (entity.h: "absence gets its own
        // value rather than a number"). Counted through as a present entity it puts a player that
        // does not exist into the frame set and then reports a DROPPED row for it every frame, so
        // the file would state a loss that never happened.
        if (slot == kNoUserSlot) {
            ++rep.pawnsUnknownSlot;
            continue;
        }
        presentIds[presentCount++] = slot;
    }
    rep.pawnsPresent = presentCount;
    rep.sawLocalPawn = presentCount > 0;

    FrameSetRec fs{};
    fs.tUs = sourceUs;
    fs.frameIndex = frameIndex;
    fs.stream = kStreamPlayerSampled;
    fs.count = presentCount;
    if (!sink(kRecFrameSet, &fs, sizeof fs, presentIds,
              uint32_t(presentCount) * sizeof(uint16_t), ctx)) {
        ++rep.sinkRefusals;
    }

    for (uint16_t k = 0; k < presentCount; ++k) {
        SampledPlayerRec rec{};
        // A MASK, NOT A BOOL. A group whose bit is clear left its fields zero because nothing sourced
        // them, and storing that as an ordinary zero would make "no trigger source on this build"
        // indistinguishable from "the trigger was at rest".
        //
        // TRANSLATED, not assigned: the two enums are declared in different headers by different
        // packets and their values coincide today, so a direct assignment worked on a coincidence
        // nothing enforced. LocalMaskToFill makes the correspondence a statement with asserts behind it.
        // The detailed read, whose default forwards to ReadLocalPlayer, so an IEntity that predates
        // it still compiles and answers NotAsked instead of inventing a cause.
        replay::IEntity::LocalPlayerReading reading;
        const uint32_t engineMask =
            e->ReadLocalPlayerDetailed(UserIndex(presentIds[k]), rec.state, reading);
        rec.fill = LocalMaskToFill(engineMask);
        // BEFORE the refusal branch, because a group is lost whether or not the ROW survived. Inside
        // that branch this was only ever reachable on total loss.
        const uint32_t lostBits = engineMask & ~kLocalBitsWithFill;
        if (lostBits != 0) {
            ++rep.pawnsGroupLoss;
            rep.lostLocalGroupBits |= lostBits;
            if (rec.fill != 0) {
                ++rep.rowsWithGroupLoss;
            }
        }
        if (reading.actorSourcesDisagree) {
            ++rep.localDisagreements;
            if (!rep.localDisagreeKnown) {
                rep.localDisagreeKnown = true;
                rep.localDisagreeSource = static_cast<uint8_t>(reading.actorSource);
            }
        }
        if (rec.fill == 0) {
            ++rep.pawnsRefused;
            // Asked-and-declined is not the same event as asked-answered-and-dropped, and the second
            // is OUR fault. Counted apart so the log can say which of the two happened.
            if (engineMask != 0) ++rep.pawnsUntranslated;
            if (!rep.localRefusalKnown) {
                rep.localRefusalKnown = true;
                rep.localRefusal = static_cast<uint8_t>(reading.refusal);
                rep.localRefusedEngineMask = engineMask;
                rep.localActorSource = static_cast<uint8_t>(reading.actorSource);
                rep.localEchoComponent = reading.echoComponent;
                rep.localSourcesDisagree = reading.actorSourcesDisagree;
            }
            continue;   // present and not recorded; the frame set already said it was there
        }
        rec.sourceTimeUs = sourceUs;
        rec.frameIndex = frameIndex;
        rec.entity = presentIds[k];
        if (k == 0) {
            rep.localFillMask = rec.fill;
        }
        if (!sink(kRecSampledPlayer, &rec, sizeof rec, nullptr, 0, ctx)) {
            ++rep.sinkRefusals;
            continue;
        }
        ++rep.pawnsSampled;
    }
}

template <class A, class E>
void GatherAsSeen(A* asSeen, E* entity, const GatherConfig& cfg, uint64_t sourceUs,
                  uint32_t frameIndex, GatherSink sink, void* ctx, GatherReport& rep,
                  const AsSeenSample* pre = nullptr) {
    if (!asSeen || !cfg.streamEnabled[kStreamAsSeen]) {
        return;
    }
    AsSeenRec rec{};
    rec.sourceTimeUs = sourceUs;
    rec.frameIndex = frameIndex;
    // `pre` is the reading ReadPeerTime already took this tick. Defaulted to null so a caller that
    // has none -- W2-B's chain test drives this directly -- still gets its own read rather than an
    // empty record.
    if (pre) {
        rec.sample = *pre;
    } else if (!asSeen->Read(rec.sample)) {
        return;
    }
    uint8_t players[AsSeenFrame::kMaxPlayers * sizeof(AsSeenPerPlayer)];
    uint16_t n = 0;
    if (entity) {
        const size_t count = entity->PlayerCount();
        for (size_t i = 0; i < count && n < AsSeenFrame::kMaxPlayers; ++i) {
            AsSeenPerPlayer p{};
            if (asSeen->ReadPlayer(UserIndex(static_cast<uint16_t>(i)), p)) {
                memcpy(players + size_t(n) * sizeof(AsSeenPerPlayer), &p, sizeof p);
                ++n;
            }
        }
    }
    rec.playerCount = n;
    rep.asSeenPlayers = n;
    if (!sink(kRecAsSeen, &rec, sizeof rec, players, uint32_t(n) * sizeof(AsSeenPerPlayer), ctx)) {
        ++rep.sinkRefusals;
    }
}

// `out` is the SAME sample GatherAsSeen needs, handed forward rather than re-fetched:
// the tick asked the engine for the whole AsSeenFrame here, kept two fields of it, and then asked
// again a few phases later. One engine round trip per tick instead of two, and the two records are
// now built from ONE reading -- which is a fidelity property as well as a cost one, because the peer
// clock in the tick record and the as-seen frame beside it can no longer come from different
// instants. Returns whether the sample was read at all; `out` is untouched when it was not.
template <class A>
bool ReadPeerTime(A* asSeen, uint64_t& peerUs, uint32_t& valid, AsSeenSample* out = nullptr) {
    AsSeenSample s{};
    if (!asSeen || !asSeen->Read(s)) {
        return false;
    }
    peerUs = s.peerTimeUs;
    valid = s.valid;
    if (out) {
        *out = s;
    }
    return true;
}

}  // namespace capture
}  // namespace replay
