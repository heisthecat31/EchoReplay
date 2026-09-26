#include "playback/core/windowbuild.h"

#include <string.h>
#include <stdio.h>

#include "capture/capture_types.h"
#include "capture/keyframe.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/format.h"   // GameStateRecordDisk, the S_GAMESTATE on-disk record

#include <stdint.h>

#include <algorithm>
#include <new>
#include <vector>

namespace {
// The handle contract's "this session does not carry it". Same number as the registry's
// 0xFFFF (LOBBY.TEAM_NONE / NETVAR.SLOT_NONE), unrelated subject -- naming either fact here
// would be a false derivation.
constexpr uint16_t kHandleAbsent = UINT16_MAX;
}  // namespace

namespace replay {
namespace playback {

const char* IdKindName(IdKind k) {
    switch (k) {
        case IdKind::NetActorGuaranteed: return "net-actor-guaranteed";
        case IdKind::NetIdByTag: return "net-id-by-tag";
        case IdKind::UserIdByTag: return "user-id-by-tag";
        case IdKind::ComponentIdByTag: return "component-id-by-tag";
        default: return "unresolvable";
    }
}

IdKind ClassifyBodyIdentity(uint32_t fill, uint32_t tag) {
    if (!capture::BodyIdentityResolvable(fill)) {
        return IdKind::Unresolvable;
    }
    // ★ PRECEDENCE, NOT STYLE. The net-actor guarantee is checked FIRST because a file written
    // today carries kFillBodyIdNetActor with a placeholder idTag of 0; testing the tag first would
    // read every one of them as a user id and resolve none.
    if (fill & capture::kFillBodyIdNetActor) {
        return IdKind::NetActorGuaranteed;
    }
    if (fill & capture::kFillBodyIdTag) {
        if (tag >= kBodyIdTagNetIdMin) {
            return IdKind::NetIdByTag;
        }
        return tag == 1 ? IdKind::ComponentIdByTag : IdKind::UserIdByTag;
    }
    return IdKind::Unresolvable;
}

BlockVerdict VerifyBlockConsumed(uint32_t declared_bits, size_t bits_read, bool overrun) {
    BlockVerdict v;
    v.declared_bits = declared_bits;
    v.consumed_bits = (uint32_t)bits_read;
    v.overrun = overrun;
    if (overrun) {
        v.why = "the decoder read past the end of the block";
        return v;
    }
    if ((uint32_t)bits_read < declared_bits) {
        v.why = "the decoder consumed FEWER bits than the encoder produced: the tail of every record "
                "after the shortfall is unread, and a decode that stops short reports success";
        return v;
    }
    if ((uint32_t)bits_read > declared_bits) {
        v.why = "the decoder consumed MORE bits than the encoder produced: the stream is desynchronised";
        return v;
    }
    v.ok = true;
    return v;
}

namespace {

// ReencodeDiffers is the proof failing, not the read: the codec checks the padding before it
// re-encodes, so that verdict still means a whole record at the pinned offset.
bool Recoverable(capture::WireDecode r) {
    return r == capture::WireDecode::Ok || r == capture::WireDecode::ReencodeDiffers;
}

}  // namespace

bool RecoverWireVerbatim(capture::DecodedPlayer& d) {
    capture::PlayerFrame f;
    if (!d.body || !Recoverable(capture::DecodePlayerBody(d.body, d.bodyBytes, f))) {
        return false;
    }
    d.frame = f;
    d.verbatim = false;
    return true;
}

bool RecoverWireVerbatim(capture::DecodedBody& d) {
    capture::BodyFrame f;
    if (!d.body || !Recoverable(capture::DecodeBodyBody(d.body, d.bodyBytes, f))) {
        return false;
    }
    d.frame = f;
    d.verbatim = false;
    return true;
}

namespace {

// Opens a stream block: skips the 4-byte prefix, exposes the reader, and carries the declared bit
// length so the caller can prove the decode consumed exactly it. One place reads the prefix, so a
// second decode site cannot forget to.
struct BlockCursor {
    bitio::BitReader reader;
    uint32_t declared_bits = 0;
    bool usable = false;

    BlockCursor(const StreamView& sv)
        : reader(sv.bytes > capture::kBlockPrefixBytes ? sv.records + capture::kBlockPrefixBytes
                                                       : sv.records,
                 sv.bytes > capture::kBlockPrefixBytes ? sv.bytes - capture::kBlockPrefixBytes : 0) {
        if (sv.records && sv.bytes > capture::kBlockPrefixBytes) {
            memcpy(&declared_bits, sv.records, sizeof declared_bits);
            usable = true;
        }
    }

    // Call after the record loop. Records the verdict into stats and returns whether it held.
    bool Verify(BuildStats& st) {
        const BlockVerdict v = VerifyBlockConsumed(declared_bits, reader.BitsRead(), reader.Overrun());
        if (v.ok) {
            ++st.blocks_verified;
        } else {
            ++st.blocks_bit_mismatch;
        }
        return v.ok;
    }
};

// ★ EVERY STREAM BLOCK CARRIES capture::kBlockPrefixBytes AHEAD OF THE PACKED RECORDS. A BitReader
// built on StreamView::records directly starts mid-header, and the first decode refuses -- which is
// exactly what the round-trip arm caught. The prefix is the codec's, so the constant is cited from
// streamcodec.h rather than written as a 4 here.

using capture::BodyFrame;
using capture::PlayerFrame;

// BodyFrame -> BodyState. The engine-facing struct is the subset the interpolator and the history node
// carry; the file's header words (hdr0/hdr2) and netId are identity and provenance, not pose, and they
// deliberately do not travel into the node.
// ...with one exception, and it is identity that the ENGINE consumes rather than provenance: for
// the disc the low dword of hdr0 is the recorded owner's user slot (all-ones = unowned), and
// PhysCI+0x1C is where the engine reads possession from. DIAG-DISC-HELD S1.1.
uint32_t OwnerSlotOf(const BodyFrame& f) { return WireClocks::SenderOf(f.hdr0); }

void ToBodyState(const BodyFrame& f, BodyState& s) {
    memcpy(s.com, f.com, sizeof s.com);
    memcpy(s.ori, f.ori, sizeof s.ori);
    memcpy(s.vel, f.vel, sizeof s.vel);
    memcpy(s.angVel, f.angVel, sizeof s.angVel);
    memcpy(s.lastHeldCom, f.lastHeldCom, sizeof s.lastHeldCom);
    s.bits11 = f.bits11;
    s.owner_slot = OwnerSlotOf(f);
}

// PlayerFrame -> PlayerState. The capture splits holdingontouserid into its {slot, generation} halves
// (chapterbuilder: `holdLSlot = holdL & 0xFFFF`, `holdLGen = holdL >> 16`); this is the exact inverse.
// It is written as the inverse of a cited site rather than as an independent guess at the packing,
// because a wrong reassembly here produces a grab id that is plausible and wrong.
void ToPlayerState(const PlayerFrame& f, PlayerState& s) {
    memcpy(s.hmdOri, f.hmdOri, sizeof s.hmdOri);
    memcpy(s.lHandOri, f.lHandOri, sizeof s.lHandOri);
    memcpy(s.rHandOri, f.rHandOri, sizeof s.rHandOri);
    memcpy(s.lHandOff, f.lHandOff, sizeof s.lHandOff);
    memcpy(s.rHandOff, f.rHandOff, sizeof s.rHandOff);
    memcpy(s.refVel, f.refVel, sizeof s.refVel);
    s.bendBack = f.bendBack;
    s.bendFwd = f.bendFwd;
    s.restPoseHeight = f.restPoseHeight;
    s.trigL = f.trigL;
    s.trigR = f.trigR;
    s.bits28 = f.bits28;
    s.holdL = (uint32_t)f.holdLSlot | ((uint32_t)f.holdLGen << 16);
    s.holdR = (uint32_t)f.holdRSlot | ((uint32_t)f.holdRGen << 16);
    s.anchor = f.anchor;
}

// ★ A WIRE ROW'S ID FIELD IS THE WHOLE TAGGED PAIR. SR15NetPhysicsSimState ends with the id at +0x48
// and its tag at +0x4C (streamcodec.h BodyFill), and the message packs those eight bytes as ONE
// 64-bit field -- so the high dword of a wire `idValue` is the tag and the low dword is what it tags.
// DecodeBodyBody stores the 64 bits whole and claims no tag, which is honest for a writer that did
// not interpret them and is why every wire row used to be refused as unresolvable. The real
// spectator capture agrees three ways: the player bodies read tag 0 with a low word equal to the
// owner slot in their own header, and the disc reads tag 1 under all three of its owners.
//
// Tag 0 is an SR15NetUserId {u16 slot, u16 generation}; tag 1 a component id. At or above the
// net-id threshold the qword is kept whole, because that is what FindNetActor is handed.
bool BodyKeyOfWire(const BodyFrame& f, BodyIdentity& id) {
    if ((f.fill & capture::kFillBodyIdValue) == 0) {
        return false;
    }
    id.tag = (uint32_t)(f.idValue >> 32);
    id.value = id.tag >= kBodyIdTagNetIdMin ? f.idValue : (f.idValue & UINT32_MAX);
    id.fill = capture::kFillBodyIdValue | capture::kFillBodyIdTag;
    return true;
}

// A sampled row: the capture's own fill bits say how `idValue` reads, or that nothing does.
bool BodyKeyOfSampled(const BodyFrame& f, uint16_t entity, BodyIdentity& id, BuildStats& st) {
    // ★ THE RULE, APPLIED BEFORE THE VALUE IS TOUCHED. An identity is resolvable only when the
    // value is present AND something says how to read it. Keying on idValue without that is the
    // defect W2-A's own comment names, and it is invisible in any test where the tags agree.
    if (capture::BodyIdentityResolvable(f.fill)) {
        id.value = f.idValue;
        id.tag = f.idTag;
        // Masked to the identity bits HERE, at the one boundary where a BodyFrame becomes a key.
        // The per-record value bits (transform, lastheld, bits11) are not identity and would
        // otherwise split a body into one track per fill combination it was captured with.
        id.fill = f.fill & (capture::kFillBodyIdValue | capture::kFillBodyIdTag |
                            capture::kFillBodyIdNetActor);
        return true;
    }
    if (f.idValue != 0) {
        // An id VALUE with nothing saying how to read it. There is no second name to fall back
        // on, so this record cannot become a key at all.
        ++st.body_identity_unresolvable;
        return false;
    }
    // SECOND HALF. `idValue == 0` with no identity fill is not a broken row: it
    // is the capture's honest statement that this body has no net-actor entry, which is exactly what
    // the RECORDING CLIENT'S OWN body looks like (gather.h). Dropping it here is why a solo replay
    // moved every body except the one the viewer looks out of.
    //
    // It is keyed on the capture's POOL HANDLE, which is a per-session diagnostic and is never
    // resolved as an id -- ClassifyBodyIdentity reads the FILL, and a zero fill is Unresolvable
    // however this value reads. The zero fill is also what keeps it apart from every wire key, whose
    // fill is never zero. The handle is used only to keep two locally simulated bodies (a host with
    // bots) as two tracks; merging them would hand BindBodyHandles one track where the file has two
    // subjects.
    id.value = entity;
    id.tag = 0;
    id.fill = 0;
    ++st.body_no_net_id;
    return true;
}

// TRACKMAP_MUTATE_DENSE_KEY restores the keying this packet replaced -- a wire row keyed like a
// sampled one, a wire player keyed on the stream's dense index -- so the collision case can be
// shown red against it.
#ifdef TRACKMAP_MUTATE_DENSE_KEY
constexpr bool kKeyWireByHeader = false;
#else
constexpr bool kKeyWireByHeader = true;
#endif

// Where a record lands on the capture axis. A sampled row is already there.
bool PlaceOnCaptureAxis(bool sampled, const WireClocks* clocks, bool has_header, uint64_t hdr0,
                        uint64_t stamp_us, uint64_t& out, StreamStats& ss) {
    if (sampled || clocks == nullptr) {
        out = stamp_us;
        return true;
    }
    bool clamped = false;
    if (!has_header || !clocks->ToCaptureUs(hdr0, stamp_us, out, &clamped)) {
        return false;
    }
    if (clamped) ++ss.clock_clamped;
    return true;
}

bool DecodeBodyStream(const StreamView& sv, bool sampled, const WireClocks* clocks, Window& out,
                      BuildStats& st) {
    if (!sv.records || !sv.bytes) {
        return true;
    }
    BlockCursor bc(sv);
    if (!bc.usable) {
        return true;
    }
    StreamStats& ss = sampled ? st.body_sampled : st.body_wire;
    bitio::BitReader& r = bc.reader;
    capture::BodyDecoder dec(sampled);
    uint8_t scratch[512];
    for (uint32_t i = 0; i < sv.recordCount; ++i) {
        capture::DecodedBody d;
        memset(&d, 0, sizeof d);
        if (!dec.Decode(r, d, scratch, sizeof scratch)) {
            ++st.decode_failures;
            return false;   // a half-read block desynchronises everything after it
        }
        ++ss.read;
        if (d.verbatim) {
            if (sampled || !RecoverWireVerbatim(d)) {
                ++st.verbatim_skipped;
                ++ss.dropped_verbatim;
                continue;
            }
            ++ss.recovered_verbatim;
        }
        BodyIdentity id;
        const bool keyed = (!sampled && kKeyWireByHeader)
                               ? BodyKeyOfWire(d.frame, id)
                               : BodyKeyOfSampled(d.frame, d.entity, id, st);
        if (!keyed) {
            ++ss.dropped_identity;
            continue;
        }
        uint64_t t_us = 0;
        if (!PlaceOnCaptureAxis(sampled, clocks, (d.frame.fill & capture::kFillBodyHeader) != 0,
                                d.frame.hdr0, d.stampUs, t_us, ss)) {
            ++ss.dropped_clock;
            continue;
        }
        st.body_fill_seen |= d.frame.fill;
        BodyState s;
        memset(&s, 0, sizeof s);
        ToBodyState(d.frame, s);
        if (out.AddBodySample(id, t_us, s)) {
            ++st.body_samples;
            ++ss.keyed;
        } else {
            ++ss.dropped_window;
        }
    }
    return bc.Verify(st);
}

bool DecodePlayerStream(const StreamView& sv, bool sampled, const WireClocks* clocks, Window& out,
                        BuildStats& st) {
    if (!sv.records || !sv.bytes) {
        return true;
    }
    BlockCursor bc(sv);
    if (!bc.usable) {
        return true;
    }
    StreamStats& ss = sampled ? st.player_sampled : st.player_wire;
    bitio::BitReader& r = bc.reader;
    capture::PlayerDecoder dec(sampled);
    uint8_t scratch[512];
    for (uint32_t i = 0; i < sv.recordCount; ++i) {
        capture::DecodedPlayer d;
        memset(&d, 0, sizeof d);
        if (!dec.Decode(r, d, scratch, sizeof scratch)) {
            ++st.decode_failures;
            return false;
        }
        ++ss.read;
        if (d.verbatim) {
            if (sampled || !RecoverWireVerbatim(d)) {
                ++st.verbatim_skipped;
                ++ss.dropped_verbatim;
                continue;
            }
            ++ss.recovered_verbatim;
        }
        const bool has_header = (d.frame.fill & capture::kFillHeader) != 0;
        PlayerIdentity who;
        who.sampled = sampled;
        if (!sampled && kKeyWireByHeader) {
            // The wire stream's entity is a first-seen index over the header word
            // (chapterbuilder.cpp SlotMap); the header's low dword is the sender's SR15NetUserId,
            // slot in the low word and generation above it. All-ones is the engine's "no user".
            const uint32_t user = (uint32_t)d.frame.hdr0;
            if (!has_header || user == UINT32_MAX) {
                ++ss.dropped_identity;
                continue;
            }
            who.user_slot = (uint16_t)user;
            who.generation = (uint16_t)(user >> 16);
        } else {
            // S_PLAYER_SAMPLED stores the user slot itself (gather.h GatherLocalPawns).
            who.user_slot = d.entity;
        }
        uint64_t t_us = 0;
        if (!PlaceOnCaptureAxis(sampled, clocks, has_header, d.frame.hdr0, d.stampUs, t_us, ss)) {
            ++ss.dropped_clock;
            continue;
        }
        st.player_fill_seen |= d.frame.fill;
        PlayerState s;
        memset(&s, 0, sizeof s);
        ToPlayerState(d.frame, s);
        if (out.AddPlayerSample(who, t_us, s)) {
            ++st.player_samples;
            ++ss.keyed;
        } else {
            ++ss.dropped_window;
        }
    }
    return bc.Verify(st);
}

}  // namespace

namespace {

// The recording client's own body, as the capture actually writes it: `idValue` 0 and NOT ONE
// identity fill bit, because gather.h sets the bits only when the net id is non-zero so that "a row
// that cannot name its subject says so rather than carrying a zero that looks like a key". That
// exact shape, and only it, is a local-pawn candidate. A track carrying a user id or a component id
// is a different, resolvable thing and stays out of this arm.
bool IsLocalPawnCandidate(const BodyTrack& tr) {
    return tr.id.fill == 0 && tr.id.tag == 0;
}

}  // namespace

const char* PawnClassName(PawnClass c) {
    switch (c) {
        case PawnClass::Local:  return "local";
        case PawnClass::Remote: return "remote";
        default: break;
    }
    return "unknown";
}

namespace {

// The contract carries no default member initialisers, and 0 is a VALID pool index at
// every handle field: a value-initialised PawnRef reads as "resolved to slot 0", which is the
// one wrong answer this whole file exists to prevent. Absence is written explicitly.
PawnRef EmptyPawn() {
    // `entity` starts UNKNOWN, not 0: entity id 0 is the local pawn's, so a zero here would be read
    // as the viewer's own pawn by a caller that never got a key at all.
    return PawnRef{PlayerHandle(kPawnAbsent), PhysHandle(kPawnAbsent), kEntityKeyUnknown,
                   PawnClass::Unknown, false};
}

// HOW FAR A SURFACE GOT, so two surfaces refusing for different reasons produce ONE honest word.
// A slot the remote pool never held and the local pool held without a body is better described by
// the second: the first says nothing was there, the second says something was and carried no body.
// Ranking them makes that choice a rule instead of an ordering accident in the code below.
int RefusalDepth(PawnResolve::Refusal x) {
    switch (x) {
        case PawnResolve::Refusal::NotAttempted:      return 0;
        case PawnResolve::Refusal::NoEntityService:   return 1;
        case PawnResolve::Refusal::NoPlayerCi:        return 2;
        case PawnResolve::Refusal::NoPhysCi:          return 3;
        case PawnResolve::Refusal::SlotCollision:     return 4;
        case PawnResolve::Refusal::PawnClassMismatch: return 5;
        default: break;
    }
    return 0;
}

PawnResolve::Refusal Deeper(PawnResolve::Refusal a, PawnResolve::Refusal b) {
    return RefusalDepth(b) > RefusalDepth(a) ? b : a;
}

// The local surface's own vocabulary, mapped onto the resolve's. Pool membership is what makes a
// CI exist, so "in the pool and no actor" is NoPhysCi -- a CI that carries no body -- while "not in
// the pool" is NoPlayerCi. An unproved FirstActorComponent is neither: it is a could-not-ask, and
// merging it into an absence is the exact defect this vocabulary exists to prevent.
PawnResolve::Refusal LocalRefusalWord(IEntity::LocalPawn x) {
    switch (x) {
        case IEntity::LocalPawn::Ok:              return PawnResolve::Refusal::None;
        case IEntity::LocalPawn::NoBroadcaster:   return PawnResolve::Refusal::NoPlayerCi;
        case IEntity::LocalPawn::SlotNotInPool:   return PawnResolve::Refusal::NoPlayerCi;
        case IEntity::LocalPawn::ActorNull:       return PawnResolve::Refusal::NoPhysCi;
        case IEntity::LocalPawn::NoPhysComponent: return PawnResolve::Refusal::NoPhysCi;
        case IEntity::LocalPawn::NoFindComponent: return PawnResolve::Refusal::NoEntityService;
        case IEntity::LocalPawn::NotAsked:        break;
    }
    return PawnResolve::Refusal::NotAttempted;
}

void CountResolveRefusal(BindResult& r, PawnResolve::Refusal x, bool awaiting) {
    switch (x) {
        case PawnResolve::Refusal::NoPlayerCi:        ++r.no_player_ci; break;
        case PawnResolve::Refusal::NoPhysCi:
            if (awaiting) ++r.awaiting_phys_ci; else ++r.no_phys_ci;
            break;
        case PawnResolve::Refusal::PawnClassMismatch: ++r.class_mismatch; break;
        case PawnResolve::Refusal::SlotCollision:     ++r.slot_collision; break;
        case PawnResolve::Refusal::NoEntityService:   ++r.no_entity_service; break;
        case PawnResolve::Refusal::NoModel:           ++r.no_model; break;
        default:                                      ++r.unresolvable; break;
    }
}

}  // namespace

void PawnResolver::SetSlotMap(const uint16_t* map, size_t n) {
    have_map_ = map != nullptr && n != 0;
    for (size_t i = 0; i < kSlots; ++i) {
        assigned_[i] = have_map_ && i < n && map[i] != kSlotUnassigned;
        map_[i] = assigned_[i] ? map[i] : static_cast<uint16_t>(i);
    }
    // A map arriving mid-load re-aims every cached answer at a different slot, so the table it
    // keyed goes with it.
    for (size_t i = 0; i < kSlots; ++i) table_[i] = Entry();
}

uint16_t PawnResolver::MappedSlot(uint16_t recorded) const {
    if (!have_map_ || recorded >= kSlots) return recorded;
    return map_[recorded];
}

void PawnResolver::Reset() {
    for (size_t i = 0; i < kSlots; ++i) table_[i] = Entry();
    epoch_ = 0;
    have_epoch_ = false;
    last_ = PawnResolve::Refusal::NotAttempted;
    stats_ = Stats();
}

bool PawnResolver::ResolvePawn(const IEntity& entity, uint16_t slot, PawnClass recorded,
                               PawnRef& out) {
    out = EmptyPawn();
    ++stats_.asked;
    last_ = PawnResolve::Refusal::NotAttempted;

    // A slot outside the engine's own user-slot table names no CI in either pool, which is exactly
    // what NoPlayerCi says. It is checked first because everything below indexes on it.
    if (slot >= kSlots) {
        last_ = PawnResolve::Refusal::NoPlayerCi;
        return false;
    }
    if (!entity.Available()) {
        last_ = PawnResolve::Refusal::NoEntityService;
        return false;
    }

    uint64_t epoch = 0;
    const IEntity::BindEpoch ep = entity.BindEpochNow(epoch);
    if (ep == IEntity::BindEpoch::Unbound) {
        // Tracked, and nothing is bound: every handle either pool could hand back belongs to a
        // game space that is gone.
        last_ = PawnResolve::Refusal::NoEntityService;
        return false;
    }
    const bool cacheable = ep == IEntity::BindEpoch::Ok;
    if (cacheable) {
        if (!have_epoch_ || epoch != epoch_) {
            for (size_t i = 0; i < kSlots; ++i) table_[i] = Entry();
            if (have_epoch_) ++stats_.epoch_changes;
            epoch_ = epoch;
            have_epoch_ = true;
        }
    } else {
        ++stats_.uncached;
    }

    // The surfaces are asked about the provisioned slot; the cache stays keyed on the recorded one.
    const uint16_t live = MappedSlot(slot);
    if (live >= kSlots) {
        last_ = PawnResolve::Refusal::NoPlayerCi;
        return false;
    }
    Entry& e = table_[slot];
    if (cacheable && e.used) {
        if (e.ok) {
            ++stats_.cache_hits;
            out = e.ref;
            last_ = e.refusal;
            return true;
        }
        if (e.attempts >= cap_) {
            // The cap, ARCH-23 section 8: the refusal latches for the rest of the epoch and no
            // surface is touched. It is still the SAME refusal word -- a cap does not change why
            // the pawn was absent, only how often we ask.
            ++stats_.latched;
            last_ = e.refusal;
            return false;
        }
    }

    ++stats_.surface_lookups;
    if (cacheable) {
        e.used = true;
        ++e.attempts;
    }

    // ---- the LOCAL surface: the one the capture read the row out of (F-B) --------------------
    PawnRef local = EmptyPawn();
    bool have_local = false;
    IEntity::LocalPawnRef lp;
    const IEntity::LocalPawn lr = entity.LocalPawnAt(UserIndex(live), lp);
    PawnResolve::Refusal local_why = LocalRefusalWord(lr);
    if (lp.duplicateInPool) {
        local_why = PawnResolve::Refusal::SlotCollision;
    } else if (lr == IEntity::LocalPawn::Ok) {
        if (lp.phys.v == kPawnAbsent) {
            local_why = PawnResolve::Refusal::NoPhysCi;
        } else {
            local.phys = lp.phys;
            local.model = lp.model;
            local.cls = PawnClass::Local;
            have_local = true;
        }
    }

    // ---- the REMOTE surface: PlayerOf scans CR15RemotePlayerCS (F-A) -------------------------
    PawnRef remote = EmptyPawn();
    bool have_remote = false;
    PawnResolve::Refusal remote_why = PawnResolve::Refusal::NoPlayerCi;
    const PlayerHandle ph = entity.PlayerOf(UserIndex(live));
    if (ph.v != kPawnAbsent) {
        const PhysHandle bh = entity.PhysOf(ph);
        if (bh.v == kPawnAbsent) {
            remote_why = PawnResolve::Refusal::NoPhysCi;
        } else {
            remote.player = ph;
            remote.phys = bh;
            remote.cls = PawnClass::Remote;
            bool visible = false;
            remote.model = entity.ModelVisible(UserIndex(live), visible);
            have_remote = true;
            remote_why = PawnResolve::Refusal::None;
        }
    }

    // ---- INV-PP2: the class the row was captured from decides which surface may answer --------
    const PawnClass want = recorded == PawnClass::Unknown ? PawnClass::Local : recorded;
    const bool want_local = want == PawnClass::Local;
    const bool preferred = want_local ? have_local : have_remote;
    const bool other = want_local ? have_remote : have_local;

    PawnResolve::Refusal why;
    if (preferred) {
        out = want_local ? local : remote;
        why = PawnResolve::Refusal::None;
    } else if (other && recorded == PawnClass::Unknown) {
        // No recorded class, so nothing is being crossed: the file cannot say which surface it read
        // and the other one is the only one that answered. The class that DID answer is reported.
        out = want_local ? remote : local;
        why = PawnResolve::Refusal::None;
    } else if (other) {
        // The file names a class and the other surface answered. Driving it would apply a row to a
        // CI of a class it was never read from, which is the one thing INV-PP2 forbids.
        why = PawnResolve::Refusal::PawnClassMismatch;
    } else {
        why = Deeper(local_why, remote_why);
        if (why == PawnResolve::Refusal::None) why = PawnResolve::Refusal::NoPlayerCi;
    }

    const bool ok = why == PawnResolve::Refusal::None;
    if (ok) {
        if (out.cls == PawnClass::Local) ++stats_.resolved_local; else ++stats_.resolved_remote;
        // THE KEY COMES OFF THE SURFACE THAT ANSWERED. The two surfaces reach the CI by different
        // routes and hand back different handles for one slot, so a key read off the other one
        // names a different entity with nothing in the answer to say so. Only the Ok word writes
        // the field: every u16 is a legal id, 0 included, so no value can stand for "not read".
        uint16_t key = 0;
        const IEntity::PawnSurface surface = out.cls == PawnClass::Local
                                                 ? IEntity::PawnSurface::Local
                                                 : IEntity::PawnSurface::Remote;
        if (entity.EntityKeyOf(UserIndex(live), surface, key) == IEntity::EntityKey::Ok) {
            out.entity = key;
        }
        // Resolved and invisible. Reported, never a refusal: a body that cannot be seen still has
        // to be driven, and a run whose pawn is invisible must be able to say that is what happened.
        if (!out.model) why = PawnResolve::Refusal::NoModel;
    }
    last_ = why;
    if (cacheable) {
        e.ok = ok;
        e.ref = out;
        e.refusal = why;
        // The wait, and it is what makes the next epoch ask again. The pass that found a CI with no
        // body did not prove the body absent, so it is not spent against the absence cap -- it is
        // spent against this counter, which caps at the same number. When that runs out the refusal
        // latches under its own name and the surface is left alone, which is the whole point of a
        // cap: `no phys ci` then means asked cap_ times and never answered.
#ifndef W2B_MUTATE_NO_PHYS_RETRY
        if (why == PawnResolve::Refusal::NoPhysCi && e.phys_waits < cap_) {
            ++e.phys_waits;
            if (e.attempts) --e.attempts;
        }
#endif
    }
    return ok;
}

// `ent=?` and `ent=0` are different sentences: 0 is the local pawn's id, so the digits and the
// no-key word cannot share a spelling on a line a launch quotes back.
void FormatEntityKey(uint32_t key, char* out, size_t cap) {
    if (key == kEntityKeyUnknown) {
        _snprintf_s(out, cap, _TRUNCATE, "?");
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%u", unsigned(key));
}

void FormatBindSummary(const BindResult& b, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    // Every refusal by name, and the CLASS of the surface that answered. A run that binds nothing
    // now says which of the five steps refused -- the one thing L3 #11 could not say -- and a run
    // that binds says which pool the pawn came out of, which is FACT-PP1 answered in the log
    // instead of inferred from whether the body moved.
    char ent[kEntityKeyTextMax] = {};
    FormatEntityKey(b.slot_entity, ent, sizeof ent);
    _snprintf_s(out, cap, _TRUNCATE,
                "%u bound (%u by slot %u->%u, class=%s, ent=%s), %u no player ci, %u no phys ci, "
                "%u awaiting physics ci, "
                "%u class mismatch, %u slot collision, %u no model, %u no entity service, "
                "%u no net actor, %u not net ids, %u unresolvable, %u local pawn ambiguous, "
                "%u slot already driven, %u slot unassigned",
                unsigned(b.bound), unsigned(b.bound_by_slot), unsigned(b.slot_recorded),
                unsigned(b.slot_resolved), PawnClassName(b.slot_class), ent,
                unsigned(b.no_player_ci), unsigned(b.no_phys_ci), unsigned(b.awaiting_phys_ci),
                unsigned(b.class_mismatch),
                unsigned(b.slot_collision), unsigned(b.no_model), unsigned(b.no_entity_service),
                unsigned(b.no_net_actor), unsigned(b.not_a_net_id), unsigned(b.unresolvable),
                unsigned(b.local_pawn_ambiguous), unsigned(b.slot_already_driven),
                unsigned(b.slot_unassigned));
    if (b.slot_unassigned) {
        const size_t used = strnlen(out, cap);
        _snprintf_s(out + used, cap - used, _TRUNCATE, " (recorded slot %u)",
                    unsigned(b.first_unassigned_slot));
    }

    // P3-MOTION. The pose-vouch discriminator, appended only when a remote pawn resolved by slot.
    // RUN-L3-47 greps `p3probe` on this line: `comp=present vouched=yes` with a nonzero `ci0` equal
    // to `handle` is Scenario A (the pose installs); `comp=absent vouched=no handle=65535` is
    // Scenario B (the receive-path wall, a safe no-op).
    if (b.p3_probed) {
        const size_t used = strnlen(out, cap);
        if (used < cap) {
            _snprintf_s(out + used, cap - used, _TRUNCATE,
                        " | p3probe slot=%u->%u bound=%u ci0=%u rev=%u comp=%s vouched=%s handle=%u",
                        unsigned(b.slot_recorded), unsigned(b.slot_resolved),
                        unsigned(b.p3_bound_handle), unsigned(b.p3_ci_entity_id),
                        unsigned(b.p3_rev_handle), b.p3_component_present ? "present" : "absent",
                        b.p3_vouched ? "yes" : "no", unsigned(b.p3_vouched_handle));
        }
    }
}

// DISC-BIND probes P1 and P4, and the identity check between them. Its own line: the bind summary
// already fills most of one. `ci+4` wants 0x0800 set (interpolation enabled) and 0x2000 clear (not
// paused) for UpdateTargetState to move the body.
bool FormatDiscBind(const BindResult& b, char* out, size_t cap) {
    if (!cap) return false;
    out[0] = 0;
    if (b.disc_verdict == BindResult::DiscVerdict::NotAsked) return false;
    const IEntity::ComponentIdReading& d = b.disc_reading;
    char flags[16] = "unread";
    if (d.ciFlagsRead) _snprintf_s(flags, sizeof flags, _TRUNCATE, "0x%08X", d.ciFlags);
    _snprintf_s(out, cap, _TRUNCATE,
                "%s: recorded cid=0x%08X live cid=0x%08X (%u live%s%s) bound cid=0x%08X "
                "map{pool=%u gen=%u} ids=%u count=%u ci+4=%s, physCs=%p playerPhysCs=%p %s "
                "frisbeeCs=%p shortcut=%s, %u bound %u refused",
                DiscVerdictName(b.disc_verdict), b.disc_recorded_id, b.disc_live_id,
                unsigned(b.disc_live_count), b.disc_oracle_unreadable ? ": " : "",
                b.disc_oracle_unreadable ? b.disc_oracle_unreadable : "", b.disc_bound_id,
                unsigned(d.poolIndex), unsigned(d.generation), unsigned(d.idCapacity),
                unsigned(d.liveCount), flags, b.disc_phys_cs, b.disc_player_phys_cs,
                (b.disc_phys_cs && b.disc_phys_cs == b.disc_player_phys_cs) ? "SAME-POOL"
                                                                           : "separate-pool",
                b.disc_frisbee_cs, b.disc_shortcut_agreed ? "agreed" : "disagreed",
                unsigned(b.bound_disc), unsigned(b.disc_refused));
    return true;
}

const char* DiscVerdictName(BindResult::DiscVerdict v) {
    switch (v) {
        case BindResult::DiscVerdict::NotAsked: return "not-asked";
        case BindResult::DiscVerdict::Agreed: return "agreed";
        case BindResult::DiscVerdict::ReaimedToLive: return "reaimed-to-live";
        case BindResult::DiscVerdict::OracleUnreadable: return "oracle-unreadable";
        case BindResult::DiscVerdict::Ambiguous: return "ambiguous";
        case BindResult::DiscVerdict::NoSuchComponent: return "no-such-component";
        case BindResult::DiscVerdict::HandleInUse: return "handle-in-use";
    }
    return "?";
}

namespace {

bool IsComponentTrack(const BodyTrack& tr) {
    return ClassifyBodyIdentity(tr.id.fill, tr.id.tag) == IdKind::ComponentIdByTag;
}

}  // namespace

// After every player body is bound, so a disc can be refused a body a player already drives.
//
// The live disc comes from the engine's own rules block, not from a component-system lookup, and
// the chain names exactly one. The recorded id is trusted only as far as that chain agrees with it;
// when it does not -- the normal case against a host whose disc is a different component -- the one
// component track of a window is re-aimed at the one live disc. Any other disagreement has no
// single answer and is refused, as is a session whose chain cannot be read or is not fresh.
void BindDiscTracks(IEntity& entity, void* netGame, Window& w, BindResult& r) {
    r.bound_disc = 0;
    r.disc_refused = 0;
    size_t component_tracks = 0;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        if (IsComponentTrack(w.BodyAt(i))) ++component_tracks;
    }
    if (component_tracks == 0) return;

    IEntity::DiscBind db;
    const bool chain_ok = entity.BindDisc(netGame, db);
    r.disc_phys_cs = db.physCs;
    r.disc_player_phys_cs = db.playerPhysCs;
    r.disc_frisbee_cs = db.frisbeeCs;
    r.disc_shortcut_agreed = db.shortcutAgreed;
    // The chain names exactly one disc, or none: it walks the engine's own single cached ref.
    r.disc_live_count = chain_ok ? 1u : 0u;
    r.disc_live_id = chain_ok ? db.physId : 0u;
    r.disc_oracle_unreadable = chain_ok ? nullptr : db.refused;

    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        if (!IsComponentTrack(tr)) continue;
        // BodyKeyOfWire keeps only the low dword for a tag-1 key: the SComponentID itself.
        const uint32_t recorded = static_cast<uint32_t>(tr.id.value);
        r.disc_recorded_id = recorded;
        r.disc_bound_id = 0;
        r.disc_reading = db.reading;

        if (!chain_ok) {
            r.disc_verdict = db.physPoolRefused ? BindResult::DiscVerdict::NoSuchComponent
                                                : BindResult::DiscVerdict::OracleUnreadable;
            ++r.disc_refused;
            continue;
        }
        // A live host's disc is a different component from the one the file recorded, so
        // disagreement is the NORMAL case and the one track binds to the one live disc. More than
        // one component track has no single answer and is refused rather than guessed at.
        const bool agreed = db.physId == recorded;
        if (!agreed && component_tracks != 1) {
            r.disc_verdict = BindResult::DiscVerdict::Ambiguous;
            ++r.disc_refused;
            continue;
        }
        // Two handles collide only if they index the SAME component system. The disc's is the
        // level space's instance whenever the two pointers differ, and then a player sharing its
        // number is a different body entirely.
        const bool shared_pool = db.physCs == db.playerPhysCs;
        bool in_use = false;
        for (size_t o = 0; o < w.BodyCount(); ++o) {
            const BodyTrack& other = w.BodyAt(o);
            in_use = in_use || (o != i && other.handle_valid && other.handle.v == db.handle.v &&
                                (shared_pool || other.handle.pool == db.handle.pool));
        }
        if (in_use) {
            r.disc_verdict = BindResult::DiscVerdict::HandleInUse;
            ++r.disc_refused;
            continue;
        }
        tr.handle = db.handle;
        tr.handle_valid = true;
        tr.claim_owner = true;
        tr.disc_component_id = db.physId;
        r.disc_bound_id = db.physId;
        r.disc_verdict = agreed ? BindResult::DiscVerdict::Agreed
                                : BindResult::DiscVerdict::ReaimedToLive;
        ++r.bound_disc;
    }
}

BindResult BindBodyHandles(IEntity& entity, Window& w, PawnResolver& resolver,
                           bool drive_disc, void* netGame) {
    BindResult r;

    // THE PAIRING IS COUNTED BEFORE ANY OF IT IS USED. Every S_PLAYER_SAMPLED row is keyed by a user
    // slot for local and remote alike (capture_types.h SampledPlayerRec::entity), and on the capture
    // side GatherLocalPawns is the only producer of that stream -- so a player track names a pawn
    // THIS RECORDING AUTHORED, which is precisely the set whose bodies carry no net id. One of each
    // is a solved pairing. Any other count is not, and taking the first pawn anyway is exactly the
    // class of silent mis-bind this file is organised against.
    //
    // Only the SAMPLED player tracks count: a wire player track is a remote player, whose body names
    // itself on the wire (UserIdByTag below) and needs no pairing.
    size_t candidates = 0;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        if (IsLocalPawnCandidate(w.BodyAt(i))) {
            ++candidates;
        }
    }
    size_t sampled_players = 0;
    uint16_t sampled_slot = kHandleAbsent;
    for (size_t i = 0; i < w.PlayerCount(); ++i) {
        if (w.PlayerAt(i).from_sampled) {
            ++sampled_players;
            sampled_slot = w.PlayerAt(i).user_slot;
        }
    }
    const bool slot_pairing_solved = candidates == 1 && sampled_players == 1;

    bool slot_driven[PawnResolver::kSlots] = {};

    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        tr.handle_valid = false;
        tr.bound_by_slot = false;
        tr.slot_remote = false;
        tr.claim_owner = false;
        tr.slot_entity = kEntityKeyUnknown;

        const IdKind kind = ClassifyBodyIdentity(tr.id.fill, tr.id.tag);
        if (kind == IdKind::Unresolvable || kind == IdKind::UserIdByTag) {
            uint16_t slot = kHandleAbsent;
            if (kind == IdKind::UserIdByTag) {
                // The wire named the body's player outright: SR15NetUserId, slot in the low word.
                slot = (uint16_t)tr.id.value;
            } else if (!IsLocalPawnCandidate(tr)) {
                ++r.unresolvable;
                continue;
            } else if (!slot_pairing_solved) {
                ++r.local_pawn_ambiguous;
                continue;
            } else {
                slot = sampled_slot;
            }
            // Second half. PlayerOf takes the user slot the file recorded and
            // PhysOf takes the PlayerCI it returns; 0xFFFF is absence at BOTH steps and neither of
            // them is a pool index. A slot this session does not carry is absent, not bound.
            if (slot == kHandleAbsent) {
                ++r.unresolvable;
                continue;
            }
#ifndef TRACKMAP_MUTATE_BIND_UNASSIGNED
            // A recorded player is never where the file says: the identity map would aim recorded
            // slot 0 at the viewer's own pawn. The locally authored body keeps the identity rule;
            // a wire body waits for the provisioner's assignment.
            if (kind == IdKind::UserIdByTag && !resolver.SlotAssigned(slot)) {
                if (r.slot_unassigned++ == 0) r.first_unassigned_slot = slot;
                continue;
            }
#endif
            if (slot < PawnResolver::kSlots && slot_driven[slot]) {
                ++r.slot_already_driven;
                continue;
            }
            // BOTH SURFACES, and the refusal says which one declined and how far it got. The file
            // carries no captured class today, so the recorded class is Unknown and the resolver
            // prefers the surface the capture reads (ARCH-23 F-B).
            PawnRef pawn = EmptyPawn();
            if (!resolver.ResolvePawn(entity, slot, PawnClass::Unknown, pawn)) {
                CountResolveRefusal(r, resolver.LastResolveRefusal(), resolver.AwaitingPhysCi(slot));
                continue;
            }
            if (resolver.LastResolveRefusal() == PawnResolve::Refusal::NoModel) ++r.no_model;
            r.slot_class = pawn.cls;
            r.slot_entity = pawn.entity;
            r.slot_recorded = slot;
            r.slot_resolved = resolver.MappedSlot(slot);
            tr.user_slot = slot;
            tr.slot_entity = pawn.entity;
            slot_driven[slot] = true;   // ResolvePawn refuses any slot past kSlots

            // P3-MOTION. pawn.phys is PhysOf(PlayerOf(slot)) -- the handle the pose-write gate
            // refuses for this remote entrant (DIAG-ENTRANT-MOTION-TRACK). Ask the engine's own
            // actor->component resolver for the handle it vouches for and bind THAT one; keep
            // pawn.phys when nothing vouches, so a still-refused handle no-ops exactly as today
            // rather than driving blind. The gate is never bypassed -- the RIGHT handle is made to
            // pass the EXISTING one. Only the remote surface takes this route: a local pawn's own
            // handle already vouches and is left untouched.
            PhysHandle bind = pawn.phys;
            if (pawn.cls == PawnClass::Remote) {
                const IEntity::PhysVouch pv = entity.VouchPhysForUser(UserIndex(r.slot_resolved));
                r.p3_probed = true;
                r.p3_bound_handle = pv.boundHandle;
                r.p3_ci_entity_id = pv.diag.ciEntityId;
                r.p3_rev_handle = pv.diag.revHandle;
                r.p3_component_present = pv.componentPresent;
                r.p3_vouched = pv.vouched;
                r.p3_vouched_handle = pv.vouchedHandle;
#if !defined(P3_MUTATE_BIND_UNVOUCHED_HANDLE)
                // The fix. Without this the mis-addressed pawn.phys is bound and the write refuses,
                // which is exactly RUN-L3-46.
                if (pv.vouched) bind = PhysHandle(pv.vouchedHandle);
#endif
                // The identity-proved key is refused for exactly the entrants the driven surface
                // exists for, so the pawn is named through the gate it is driven through.
                uint16_t key = 0;
                if (pawn.entity == kEntityKeyUnknown &&
                    entity.EntityKeyOfDriven(bind, key) == IEntity::EntityKey::Ok) {
                    r.slot_entity = key;
                    tr.slot_entity = key;
                }
            }
            tr.handle = bind;
            tr.handle_valid = true;
            tr.bound_by_slot = true;
            // The remote-replay drive surface: only a Remote-class slot pawn takes the bounds-only
            // install (DIAG-POSE-DECOUPLE 4b). A local pawn's own handle already vouches.
            tr.slot_remote = (pawn.cls == PawnClass::Remote);
            tr.synthetic_owner_set = false;
            ++r.bound;
            ++r.bound_by_slot;
            continue;
        }
        if (kind != IdKind::NetActorGuaranteed && kind != IdKind::NetIdByTag) {
            // A component id: a real identity, just not one the net-actor table answers. With the
            // disc drive on it is left to BindDiscTracks, below.
            if (!drive_disc) ++r.not_a_net_id;
            continue;
        }

        const PhysHandle h = entity.PhysOfNetId(tr.id.value);
        // 0xFFFF is the contract's "this session does not carry that id". Treating it as a handle
        // would index slot 65535 of a pool with tens of entries.
        if (h.v == kHandleAbsent) {
            ++r.no_net_actor;
            continue;
        }
        tr.handle = h;
        tr.handle_valid = true;
        ++r.bound;
    }
    if (drive_disc) {
        BindDiscTracks(entity, netGame, w, r);
        r.bound += r.bound_disc;
    }
    return r;
}

BindResult BindBodyHandles(IEntity& entity, Window& w) {
    PawnResolver resolver;
    return BindBodyHandles(entity, w, resolver);
}

BindResult BindBodyHandles(IEntity& entity, Window& w, const uint16_t* slot_map, size_t n,
                           bool drive_disc, void* netGame) {
    PawnResolver resolver;
    resolver.SetSlotMap(slot_map, n);
    return BindBodyHandles(entity, w, resolver, drive_disc, netGame);
}

size_t ChapterForTime(IDemoReader& reader, int64_t t_us) {
    const uint64_t t = t_us < 0 ? 0u : (uint64_t)t_us;
    return reader.FindChapter(t, kSectionCore);
}

// ★ Neighbours are found by WALKING, not by i-1 / i+1. MANIFEST, ASSEEN, VOIP and INDEX chapters sit
// between the CORE ones in the file, so arithmetic on the index lands on a different section and the
// window would silently resident-ise a chapter with no samples in it.
size_t PrevCoreChapter(IDemoReader& reader, size_t i) {
    for (size_t k = i; k > 0; --k) {
        if (reader.ChapterAt(k - 1).section == kSectionCore) {
            return k - 1;
        }
    }
    return i;
}

size_t NextCoreChapter(IDemoReader& reader, size_t i) {
    const size_t n = reader.ChapterCount();
    for (size_t k = i + 1; k < n; ++k) {
        if (reader.ChapterAt(k).section == kSectionCore) {
            return k;
        }
    }
    return i;
}

// Row i of n, placed strictly inside (begin, begin+span). `k <= d` always, so `(span / d) * k` cannot
// exceed `span` and the remainder term is bounded by d*d -- neither multiplication can overflow, for
// any span a chapter index can express. Exactly floor(span * k / d), so the sequence is monotone.
uint64_t SpreadAt(uint64_t begin, uint64_t span, uint32_t i, uint32_t n) {
    const uint64_t d = uint64_t(n) + 1;
    const uint64_t k = uint64_t(i) + 1;
    return begin + (span / d) * k + ((span % d) * k) / d;
}

namespace {

// Per-key tallies for the spread, sized once and kept off the heap: window build may not allocate.
// A count of zero marks an empty slot, which a tallied key can never have.
struct KeyTally {
    static const size_t kSlots = 512;   // the encoder's name dictionary holds 128; this is 4x headroom
    uint64_t key[kSlots] = {};
    uint32_t count[kSlots] = {};
    uint32_t seen[kSlots] = {};

    // kNotFound only when the table is full, which no real chapter reaches.
    static const size_t kNotFound = (size_t)-1;
    size_t Slot(uint64_t k, bool insert) {
        size_t at = size_t((k * 0x9E3779B97F4A7C15ull) >> 55) & (kSlots - 1);
        for (size_t probe = 0; probe < kSlots; ++probe) {
            if (count[at] == 0) {
                if (!insert) return kNotFound;
                key[at] = k;
                return at;
            }
            if (key[at] == k) return at;
            at = (at + 1) & (kSlots - 1);
        }
        return kNotFound;
    }
};

// ★★★ THE BLOCK IS COUNT-PREFIXED, AND NOTHING EVER READ THE COUNT.
//
// chapterbuilder.cpp:604-616 writes `u16 count` and then {u16 slot, u64 name, u64 raw} x count. Both
// walks in this file started at byte 0 and took the count for the first record's slot, so every name
// came out two bytes adrift -- `B2996AD484F1D1E8` read as `6AD484F1D1E80000` -- and NO keyframe seed
// has ever matched a real symbol. It was invisible because a seed nobody can look up is inert: the
// resolver answered from the S_NETVAR rows instead, which is why only the keys with no rows in the
// resident chapters (the match clock at T0) ever showed the fault.
//
// The declared count and the block's own length are both honoured, and the smaller wins: a truncated
// block must not be walked past its bytes, and a length that would allow more records than the
// writer declared must not invent them.
struct NetVarKeyBlock {
    static const uint32_t kStride = 2 + 8 + 8;

    NetVarKeyBlock(const uint8_t* data, uint32_t len) {
        if (data == nullptr || len < 2) {
            return;
        }
        uint16_t declared = 0;
        memcpy(&declared, data, 2);
        const uint32_t fits = (len - 2) / kStride;
        at_ = data + 2;
        left_ = declared < fits ? declared : fits;
    }

    bool Next(uint16_t& slot, uint64_t& name, uint64_t& value) {
        if (left_ == 0) {
            return false;
        }
        memcpy(&slot, at_, 2);
        memcpy(&name, at_ + 2, 8);
        memcpy(&value, at_ + 10, 8);
        at_ += kStride;
        --left_;
        return true;
    }

private:
    const uint8_t* at_ = nullptr;
    uint32_t left_ = 0;
};

}  // namespace

// ★ WHAT A FILE WITHOUT PER-MESSAGE STAMPS GETS INSTEAD, AND WHY IT IS NOT A RECORDED FACT.
//
// A capture written before the fifth netvar schema field says only "these n rows for this key arrived
// somewhere inside this chapter". Leaving them all on the chapter's beginning is what made the HUD
// clock step 2-4 s at a time AND lead the playhead, because a tie resolves to its LAST member -- the
// value at the chapter's END (DIAG-HUD-CLOCK-STAIRCASE). Even spacing is the only placement the file
// supports: it is monotone, it is right on average for a stream the tap delivers at a steady rate,
// and it is bounded strictly inside the chapter so it can neither move this chapter's keyframe
// snapshot (which stays on tBegin) nor reach the next chapter's.
//
// It is an INTERPOLATION. BuildStats::netvar_rows_spread is how many rows carry one, and the load
// says so once in the log.
void SpreadRowsOverChapter(KeyedRecord* rows, size_t n, uint64_t tBeginUs, uint64_t tEndUs,
                           SpreadBound* bound) {
    if (n == 0 || tEndUs <= tBeginUs) {
        return;
    }
    const uint64_t span = tEndUs - tBeginUs;
    KeyTally tally;
    for (size_t i = 0; i < n; ++i) {
        const size_t at = tally.Slot(rows[i].key, true);
        if (at != KeyTally::kNotFound) {
            ++tally.count[at];
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t at = tally.Slot(rows[i].key, false);
        if (at == KeyTally::kNotFound) {
            continue;   // table full: the row keeps tBegin, which is where it already is
        }
        rows[i].t_us = SpreadAt(tBeginUs, span, tally.seen[at], tally.count[at]);
        ++tally.seen[at];
    }
    if (bound == nullptr) {
        return;
    }
    // ★★ THE ERROR IS PER KEY, AND THE OLD NUMBER IGNORED THE ROW COUNT (DIAG-CLOCK-BIND-2 B6.3).
    // `span / 2` is the bound for a key with ONE row. A key the tap delivers 724 times in a 4 s
    // chapter is placed every 5.5 ms, and reporting +/-2.0 s for it made the spread the prime
    // suspect for a 2 s clock jump it could not possibly have caused. The worst over the keys
    // actually present is the honest headline; the one-row keys are counted beside it, because they
    // are the ones the headline is about.
    for (size_t at = 0; at < KeyTally::kSlots; ++at) {
        if (tally.count[at] == 0) {
            continue;
        }
        ++bound->keys;
        if (tally.count[at] == 1) {
            ++bound->one_row_keys;
        }
        const uint32_t err = NetvarSpreadWorstUs(span, tally.count[at]);
        if (err > bound->worst_us) {
            bound->worst_us = err;
            bound->sparsest_rows = tally.count[at];
        }
    }
}

uint32_t NetvarSpreadWorstUs(uint64_t spanUs, uint32_t rowsForKey) {
    // SpreadAt lands row k of n at begin + span*(k+1)/(n+1), so consecutive placements are one
    // interval apart and no row is further from its true instant than that interval. n == 1 gives
    // span/2, which is the midpoint case this function used to report for every key.
    const uint64_t e = spanUs / (uint64_t(rowsForKey) + 1u);
    return e > UINT32_MAX ? UINT32_MAX : uint32_t(e);
}

bool NetvarStampsPresent(const Manifest& m) {
    if (m.schemas == nullptr) {
        return false;
    }
    for (uint16_t i = 0; i < m.schemaCount; ++i) {
        if (m.schemas[i].id == capture::kSchemaNetvar) {
            return m.schemas[i].fieldCount >= 5;
        }
    }
    return false;   // a file that declares no netvar schema declares no stamp either
}

namespace {

// The accumulating state of a build. It exists so one chapter and three chapters go through the SAME
// decode -- a second path that read a chapter differently is exactly the fork this packet's whole
// design is organised against.
struct BuildCtx {
    static const size_t kMaxTicks = 24576;    // three 2 s chapters at 120 Hz, with room
    static const size_t kMaxSeeds = 256;
    static const size_t kMaxRows = 24576;

    uint64_t ticks[kMaxTicks];
    KeyedRecord seeds[kMaxSeeds];
    KeyedRecord rows[kMaxRows];
    size_t tick_count = 0, seed_count = 0, row_count = 0;
    bool have_seeds = false;
    // (name, slot) as read off the wire, staged here because the resolver is keyed by name alone and
    // the slot is what picks the engine's update record.
    uint64_t slot_names[kMaxSeeds] = {};
    uint16_t slot_vals[kMaxSeeds] = {};
    size_t slot_count = 0;
    void NoteSlot(uint64_t name, uint16_t slot) {
        for (size_t i = 0; i < slot_count; ++i) {
            if (slot_names[i] == name) return;
        }
        if (slot_count < kMaxSeeds) {
            slot_names[slot_count] = name;
            slot_vals[slot_count] = slot;
            ++slot_count;
        }
    }                  // only the FIRST chapter's keyframe seeds the resolver
    uint64_t outer_begin = 0, outer_end = 0;
    bool span_set = false;
    const WireClocks* clocks = nullptr;
    const NetVarPrimer* primer = nullptr;
    // ★ THE KEYFRAME STAGING BUFFER LIVES HERE AND NOT IN THE FUNCTION, because the function is now
    // entered from two threads. It was a `static uint8_t blob[64 * 1024]` inside DecodeChapterInto,
    // which is the one piece of per-decode state a per-call ctx would have left shared -- two builds
    // in flight would have read each other's keyframe bytes with nothing failing anywhere. Not in
    // Reset() for the same reason the arrays are not: it is written before it is read, and
    // `blob_len` is what bounds it.
    uint8_t blob[64 * 1024];

    // ★★ RESET THE COUNTERS, NEVER THE BUFFERS. `ctx = BuildCtx()` reads as the obvious way to
    // clear this and it constructs an ~800 KB temporary ON THE STACK to do it -- which overflowed the
    // stack the first time the range build ran. The arrays do not need clearing: every element is
    // written before it is read, and the counts are what bound them.
    void Reset() {
        // ★ slot_count IS A COUNTER, and it was the one this missed. `ctx` is a function static, so
        // an unreset slot table made the FIRST key->slot pair this process ever saw permanent: a
        // second file loaded into the same session kept the first file's netvar record slots, and
        // netvarhud writes -- or refuses -- against that slot. Found by test_disc_motion.cpp
        // building windows from the owner's capture before test_netvarstamp.cpp builds its own.
        tick_count = seed_count = row_count = slot_count = 0;
        have_seeds = false;
        outer_begin = outer_end = 0;
        span_set = false;
        clocks = nullptr;
        primer = nullptr;
    }
};

// One chapter into the window. Everything borrowed from the reader is copied before this returns, so
// the caller may decode the next chapter immediately -- which is what keeps the three-chapter build
// inside the reader's depth-2 guarantee.
// Every stream this file decodes is fetched through here, so `asked` is built by the decode itself.
// A separate hand-maintained list of "streams we handle" would be a second source of truth that is
// right until somebody adds or removes a decode and forgets it -- which is exactly how S_WORLD stayed
// in the table above after the stream it names stopped existing.
const StreamView* Take(const DecodedChapter& dc, uint16_t id, uint32_t& asked) {
    if (id < 32) {
        asked |= 1u << id;
    }
    return dc.Find(static_cast<StreamId>(id));
}

bool DecodeChapterInto(IDemoReader& reader, size_t index, Window& out, BuildStats& st, BuildCtx& ctx) {
    DecodedChapter dc;
    if (!reader.DecodeChapter(index, dc)) {
        st.refusal = "DecodeChapter refused";
        return false;
    }
    ++st.chapters;
    uint32_t asked = 0;   // every id Take() is called with, for the leftover walk at the end

    if (!ctx.span_set) {
        ctx.outer_begin = dc.key.tBeginUs;
        ctx.outer_end = dc.key.tEndUs;
        ctx.span_set = true;
    } else {
        if (dc.key.tBeginUs < ctx.outer_begin) ctx.outer_begin = dc.key.tBeginUs;
        if (dc.key.tEndUs > ctx.outer_end) ctx.outer_end = dc.key.tEndUs;
    }

    // ---- ticks: the transport's step source -------------------------------------------------
    if (const StreamView* sv = Take(dc, kStreamTick, asked)) {
        BlockCursor bc(*sv);
        if (bc.usable) {
            bitio::BitReader& r = bc.reader;
            capture::TickDecoder dec;
            uint32_t read = 0;
            for (uint32_t i = 0; i < sv->recordCount && ctx.tick_count < BuildCtx::kMaxTicks; ++i) {
                capture::TickRec rec;
                memset(&rec, 0, sizeof rec);
                if (!dec.Decode(r, rec)) {
                    ++st.decode_failures;
                    st.refusal = "S_TICK decode refused";
                    return false;
                }
                // sourceTimeUs is the replay time axis (S5.4.3) -- never simTimeUs, which is the local
                // render clock and does not index anything.
                ctx.ticks[ctx.tick_count++] = rec.sourceTimeUs;
                ++read;
            }
            if (read == sv->recordCount && !bc.Verify(st)) {
                st.refusal = "S_TICK consumed a different number of bits than the encoder produced";
                return false;
            }
        }
    }

    // ---- bodies and players ------------------------------------------------------------------
    // * THIS TABLE PICKS THE CODEC, WHICH IS WHY A RETIRED ID CANNOT SIT IN IT.
    //
    // S_WORLD (stream 10) was listed here as a sampled body stream. It is retired -- producer, schema
    // entry, keyframe block write and config row all went with it -- but the row survived, and a row
    // here is not an inert name: DecodeBodyStream takes its codec from the `sampled` flag BESIDE the
    // id, never from the block's own schema. WorldRec ordered its fields (sourceTimeUs, idValue,
    // idTag, fill, localSlot, pad, frameIndex, state); SampledBodyRec orders them (sourceTimeUs,
    // frameIndex, idValue, idTag, fill, handle, pad, state). So on a capture recorded before the
    // retirement -- and those exist -- this handed S_WORLD's bits to the wrong decoder, which is
    // either a bit-length mismatch that refuses the whole window or bodies that were never recorded.
    //
    // ** THE ID IS NOT REUSED AND NOT RENUMBERED (DESIGN S5.4.0 INV-STREAM-ID): retiring a stream
    // frees its NAME and never its NUMBER, because that number is the only thing that says what an
    // already-recorded block means. Stream 10 is simply not decoded here any more, and a chapter that
    // carries one is counted and NAMED by CountUndecodedStreams rather than silently ignored.
    struct { uint16_t id; bool sampled; bool body; } kStreams[] = {
        {kStreamBodyWire, false, true},
        {kStreamBodySampled, true, true},
        {kStreamPlayerWire, false, false},
        {kStreamPlayerSampled, true, false},
    };
    for (size_t i = 0; i < sizeof kStreams / sizeof kStreams[0]; ++i) {
        const StreamView* sv = Take(dc, kStreams[i].id, asked);
        if (!sv) {
            continue;
        }
        const bool ok = kStreams[i].body
                            ? DecodeBodyStream(*sv, kStreams[i].sampled, ctx.clocks, out, st)
                            : DecodePlayerStream(*sv, kStreams[i].sampled, ctx.clocks, out, st);
        if (!ok) {
            // Either a record refused, or the block's declared bit length and the bits consumed
            // disagree. The second is the quieter failure and the reason the check exists: every
            // record "decoded", and the tail of each was never read.
            st.refusal = st.blocks_bit_mismatch
                             ? "a stream block consumed a different number of bits than the encoder "
                               "produced -- the decode was short or desynchronised"
                             : "a stream block did not decode";
            return false;
        }
    }

    // ---- netvars: the keyframe, then this chapter's writes ------------------------------------
    if (const StreamView* sv = Take(dc, kStreamKey, asked)) {
        BlockCursor bc(*sv);
        if (bc.usable) {
            bitio::BitReader& r = bc.reader;
            uint8_t* blob = ctx.blob;
            uint16_t blob_len = 0;
            if (capture::DecodeRawRecord(r, blob, sizeof ctx.blob, blob_len) && blob_len) {
                capture::KeyframeReader kr;
                if (kr.Open(blob, blob_len)) {
                    st.keyframe_present = true;
                    uint16_t block_id = 0;
                    const uint8_t* data = nullptr;
                    uint32_t len = 0;
                    while (kr.Next(block_id, data, len)) {
                        if (block_id == capture::kKeyBlockAudioPins) {
                            // Decoded by W2-H's own decoder, so this file does not become a second
                            // source of truth about the layout. A block that does not decode is not
                            // pins: it stays absent and is counted with the other unread blocks.
                            if (len <= sizeof st.audio_pins_raw) {
                                memcpy(st.audio_pins_raw, data, len);
                                st.audio_pins_bytes = len;
                                st.audio_pins_present = true;
                            } else {
                                ++st.key_blocks_unknown;
                            }
                            continue;
                        }
                        if (block_id != capture::kKeyBlockNetvar) {
                            // Skipped by length -- and COUNTED. A block capture writes and playback
                            // never reads is exactly what the counted-skip rule exists to surface.
                            ++st.key_blocks_unknown;
                            continue;
                        }
                        // u16 count, then {u16 slot, u64 name, u64 raw SValue} x count.
                        NetVarKeyBlock block(data, len);
                        uint64_t name = 0, value = 0;
                        uint16_t slot = 0;
                        while (block.Next(slot, name, value)) {
                            ctx.NoteSlot(name, slot);
                            // ★ The FIRST chapter's keyframe seeds the resolver; a later chapter's is
                            // a full snapshot at ITS start, which is exactly a set of writes at that
                            // time. Treating it as a seed instead would back-date it to the window's
                            // beginning and answer the wrong value for the whole first chapter.
                            if (!ctx.have_seeds) {
                                if (ctx.seed_count < BuildCtx::kMaxSeeds) {
                                    ctx.seeds[ctx.seed_count].t_us = dc.key.tBeginUs;
                                    ctx.seeds[ctx.seed_count].key = name;
                                    ctx.seeds[ctx.seed_count].value = value;
                                    ++ctx.seed_count;
                                }
                            } else if (ctx.row_count < BuildCtx::kMaxRows) {
                                ctx.rows[ctx.row_count].t_us = dc.key.tBeginUs;
                                ctx.rows[ctx.row_count].key = name;
                                ctx.rows[ctx.row_count].value = value;
                                ++ctx.row_count;
                            }
                        }
                    }
                }
            }
            // S_KEY is one raw record, so an exact consume is as checkable here as on a packed
            // stream -- and a keyframe that decodes short is the worst one to get wrong, because it
            // is the seek target every chapter opens with.
            if (!bc.Verify(st)) {
                st.refusal = "S_KEY consumed a different number of bits than the encoder produced";
                return false;
            }
        }
    }
    ctx.have_seeds = true;

    if (const StreamView* sv = Take(dc, kStreamNetvar, asked)) {
        BlockCursor bc(*sv);
        if (bc.usable) {
            bitio::BitReader& r = bc.reader;
            // The FILE says whether its messages carry a time, through the field count its own
            // manifest recorded for the netvar schema. Never inferred from the bits: a four-field
            // block read as five desynchronises at the first message and every row after it is
            // garbage that decodes without complaint.
            const bool stamped = NetvarStampsPresent(reader.GetManifest());
            st.netvar_stamps_present = stamped;
            capture::NetvarDecoder dec(stamped);
            const size_t first_row = ctx.row_count;
            for (uint32_t i = 0; i < sv->recordCount; ++i) {
                capture::NetvarMessage m;
                memset(&m, 0, sizeof m);
                if (!dec.Decode(r, m)) {
                    ++st.decode_failures;
                    st.refusal = "S_NETVAR decode refused";
                    return false;
                }
                // Clamped into the chapter it was filed in. The capture places the stamp there by
                // construction, so this only catches a file whose index and records disagree -- and
                // the alternative to catching it is a row outside the window's span, which
                // KeyedResolver::Build would then flatten back into the tie this packet removes.
                uint64_t t_us = dc.key.tBeginUs;
                if (stamped) {
                    t_us = m.stampUs;
                    if (t_us < dc.key.tBeginUs) t_us = dc.key.tBeginUs;
                    else if (t_us > dc.key.tEndUs) t_us = dc.key.tEndUs;
                }
                for (uint16_t v = 0; v < m.varCount; ++v) {
                    if (ctx.row_count >= BuildCtx::kMaxRows) {
                        // Counted, then refused below -- never silently dropped. A truncated netvar
                        // track answers a plausible wrong value for every key past the cut, which is
                        // the same failure the chapter-begin stamp caused and just as invisible.
                        ++st.netvar_rows_over_budget;
                        continue;
                    }
                    ctx.rows[ctx.row_count].t_us = t_us;
                    ctx.rows[ctx.row_count].key = m.rows[v].name;
                    ctx.rows[ctx.row_count].value = m.rows[v].value;
                    ctx.NoteSlot(m.rows[v].name, m.slot);
                    ++ctx.row_count;
                }
            }
            // Runs whatever the row budget did. It used to be skipped once the budget filled, so a
            // full window silently switched off the one check that proves the block was read whole.
            if (!bc.Verify(st)) {
                st.refusal = "S_NETVAR consumed a different number of bits than the encoder produced";
                return false;
            }
            if (st.netvar_rows_over_budget) {
                st.refusal = "the netvar row budget filled; refused rather than truncated";
                return false;
            }
            if (!stamped) {
                SpreadBound sb;
                SpreadRowsOverChapter(ctx.rows + first_row, ctx.row_count - first_row,
                                      dc.key.tBeginUs, dc.key.tEndUs, &sb);
                st.netvar_rows_spread += uint32_t(ctx.row_count - first_row);
                const uint64_t span = dc.key.tEndUs > dc.key.tBeginUs
                                          ? dc.key.tEndUs - dc.key.tBeginUs : 0u;
                if (span > st.netvar_spread_span_us) {
                    st.netvar_spread_span_us = span > UINT32_MAX ? UINT32_MAX : uint32_t(span);
                }
                // The worst is taken over KEYS, not over chapter spans: the widest chapter is not
                // necessarily the one holding the sparsest key.
                if (sb.worst_us > st.netvar_spread_worst_us) {
                    st.netvar_spread_worst_us = sb.worst_us;
                    st.netvar_spread_sparsest_rows = sb.sparsest_rows;
                }
                st.netvar_spread_one_row_keys += sb.one_row_keys;
            }
        }
    }

    // ---- game state: the match clock ------------------------------------
    // S_GAMESTATE is raw fixed-width records (chapterbuilder.cpp AppendRaw), so the block is a plain
    // array of GameStateRecordDisk -- read it as bytes, never through a bit codec it was not written
    // with. Only gameTimer-bearing samples feed the clock track: a sample whose clock read did not
    // answer sets no kGsGameTimer bit and is skipped rather than inserted as a zero. The scoreboard
    // scores ride the netvar stream above, not this one.
    if (const StreamView* sv = Take(dc, kStreamGameState, asked)) {
        const size_t rec = sizeof(GameStateRecordDisk);
        if (sv->bytes % rec != 0) {
            // The writer refuses to emit a partial record, so a non-whole block is corruption, not a
            // short read -- refused by name like every other stream here rather than decoded halfway.
            st.refusal = "an S_GAMESTATE block is not a whole number of records";
            return false;
        }
        if (sv->records != nullptr) {
            const size_t n = sv->bytes / rec;
            for (size_t i = 0; i < n; ++i) {
                GameStateRecordDisk d;
                memcpy(&d, sv->records + i * rec, rec);
                if ((d.present & kGsGameTimer) == 0) {
                    continue;
                }
                if (out.AddGameStateClock(d.captureTimeUs, d.gameTimer)) {
                    ++st.gamestate_clock_samples;
                }
            }
        }
    }

    // Events are present in the file and not decoded here. Counted so the absence is visible; the
    // decoder, when it lands, must read its type table out of the keyframe rather than a C array.
    if (const StreamView* sv = Take(dc, kStreamEvent, asked)) {
        st.events_undecoded += sv->recordCount;
    }

    st.streams_asked |= asked;
    CountUndecodedStreams(dc, asked, st);
    return true;
}

// Everything that must happen once, however many chapters went in.
bool FinishBuild(Window& out, BuildStats& st, BuildCtx& ctx, uint64_t inner_begin,
                 uint64_t inner_end) {
    // ★★★ NO RESIDENT ROW IS NOT NO VALUE. A key this window does not carry still HAD a value at
    // every instant inside it -- the one the recording opened on, which the file states in its very
    // first keyframe that mentions the key. Seeded at the window's own beginning so ValueAt answers
    // everywhere inside it, and only when the resident chapters said nothing: a key with a seed or a
    // row of its own is answered by the file's own data at this time, which is always better.
    if (ctx.primer != nullptr && ctx.primer->count != 0) {
        // Slot(k, true) reserves the bucket; the COUNT is what marks it occupied, so it has to be
        // bumped here exactly as the spread's tally bumps it.
        KeyTally present;
        for (size_t i = 0; i < ctx.seed_count; ++i) {
            const size_t at = present.Slot(ctx.seeds[i].key, true);
            if (at != KeyTally::kNotFound) ++present.count[at];
        }
        for (size_t i = 0; i < ctx.row_count; ++i) {
            const size_t at = present.Slot(ctx.rows[i].key, true);
            if (at != KeyTally::kNotFound) ++present.count[at];
        }
        for (size_t p = 0; p < ctx.primer->count; ++p) {
            const uint64_t key = ctx.primer->key[p];
            if (present.Slot(key, false) != KeyTally::kNotFound) {
                continue;
            }
            if (ctx.seed_count >= BuildCtx::kMaxSeeds) {
                break;
            }
            ctx.seeds[ctx.seed_count].t_us = ctx.outer_begin;
            ctx.seeds[ctx.seed_count].key = key;
            ctx.seeds[ctx.seed_count].value = ctx.primer->value[p];
            ++ctx.seed_count;
            // Without its slot the resolver answers and netvarhud.cpp still refuses the key.
            ctx.NoteSlot(key, ctx.primer->slot[p]);
            ++st.netvar_seeds_primed;
        }
    }
    st.ticks = (uint32_t)ctx.tick_count;
    st.netvar_seeds = (uint32_t)ctx.seed_count;
    st.netvar_rows = (uint32_t)ctx.row_count;
    out.SetTicks(ctx.ticks, ctx.tick_count);

    for (size_t i = 0; i < ctx.slot_count; ++i) {
        out.NoteNetVarSlot(ctx.slot_names[i], ctx.slot_vals[i]);
    }
    if (!out.NetVars().Build(ctx.seeds, ctx.seed_count, ctx.rows, ctx.row_count)) {
        st.refusal = "the netvar key set exceeded the resolver's capacity; refused rather than dropped";
        return false;
    }

    // ★ Regroup the interleaved staging into per-track contiguous runs. Without this the sampler
    // walks a track whose samples are not adjacent, which is not a slow answer -- it is a wrong one.
    out.Finalize();

    // ★ A short window is REFUSED, not returned. AddBodySample failing is not a soft condition: it
    // means the file had samples this window could not hold, and sampling the remainder would render a
    // plausible, wrong replay with nothing reporting the difference.
    if (out.DroppedSamples()) {
        st.refusal = "the window could not hold every sample in the range; refused rather than truncated";
        return false;
    }

    out.SetCoverage(ctx.outer_begin, ctx.outer_end, inner_begin, inner_end);
    out.InvalidateCursors();
    return true;
}

}  // namespace

// The opaque box the header promises. Defined here so BuildCtx's ~860 KB stays private to this file
// and a caller can only ever own one by pointer -- which is also what stops it being put on a stack.
struct BuildScratch {
    BuildCtx ctx;
};

BuildScratch* CreateBuildScratch() { return new (std::nothrow) BuildScratch(); }

void DestroyBuildScratch(BuildScratch* s) { delete s; }

void ScanNetVarPrimer(IDemoReader& reader, NetVarPrimer& out) {
    out = NetVarPrimer();
    static uint8_t blob[64 * 1024];
    for (size_t i = 0; i < reader.ChapterCount() && out.count < NetVarPrimer::kMax; ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) {
            continue;
        }
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) {
            continue;   // a chapter this walk cannot read is one fewer source, never a refusal
        }
        ++out.chapters;
        const StreamView* sv = dc.Find(static_cast<StreamId>(kStreamKey));
        if (sv == nullptr) {
            continue;
        }
        BlockCursor bc(*sv);
        if (!bc.usable) {
            continue;
        }
        uint16_t blob_len = 0;
        if (!capture::DecodeRawRecord(bc.reader, blob, sizeof blob, blob_len) || blob_len == 0) {
            continue;
        }
        capture::KeyframeReader kr;
        if (!kr.Open(blob, blob_len)) {
            continue;
        }
        ++out.keyframes;
        uint16_t block_id = 0;
        const uint8_t* data = nullptr;
        uint32_t len = 0;
        while (kr.Next(block_id, data, len)) {
            if (block_id != capture::kKeyBlockNetvar) {
                continue;
            }
            NetVarKeyBlock block(data, len);
            uint64_t name = 0, value = 0;
            uint16_t slot = 0;
            while (block.Next(slot, name, value)) {
                // FIRST wins: a later chapter's snapshot is a later value, and the question this
                // answers is what the key opened on.
                if (out.IndexOf(name) != NetVarPrimer::kMax) {
                    continue;
                }
                if (out.count >= NetVarPrimer::kMax) {
                    break;
                }
                out.key[out.count] = name;
                out.value[out.count] = value;
                out.slot[out.count] = slot;
                ++out.count;
            }
        }
    }
}

bool BuildWindow(IDemoReader& reader, size_t chapter_index, Window& out, BuildStats& st,
                 const WireClocks* clocks) {
    st = BuildStats();
    out.Clear();
    static BuildCtx ctx;
    ctx.Reset();
    ctx.clocks = clocks;
    st.first_chapter = st.center_chapter = st.last_chapter = chapter_index;
    if (!DecodeChapterInto(reader, chapter_index, out, st, ctx)) {
        return false;
    }
    // A single-chapter window's inner span IS its outer span: there is no neighbour to slide into, so
    // the rebuild trigger is the edge of the only chapter present.
    return FinishBuild(out, st, ctx, ctx.outer_begin, ctx.outer_end);
}

void CountUndecodedStreams(const DecodedChapter& dc, uint32_t asked, BuildStats& st) {
    for (uint16_t i = 0; i < dc.streamCount; ++i) {
        const uint16_t id = dc.streams[i].id;
        if (id < 32 && (asked & (1u << id))) {
            continue;
        }
        ++st.streams_undecoded;
        if (st.first_undecoded_stream == nullptr) {
            st.first_undecoded_stream = StreamName(id);
        }
    }
}

namespace {

// The one body both entry points run. The ONLY difference between a window decoded on the tick and
// one decoded by the prefetch worker is which BuildCtx it is handed -- which is what makes
// byte-identity between the two paths a property of the code rather than a hope.
bool BuildWindowRangeWith(IDemoReader& reader, size_t center_chapter, Window& out, BuildStats& st,
                          const WireClocks* clocks, const NetVarPrimer* primer, BuildCtx& ctx) {
    st = BuildStats();
    out.Clear();
    ctx.Reset();
    ctx.clocks = clocks;
    ctx.primer = primer;

    const size_t first = PrevCoreChapter(reader, center_chapter);
    const size_t last = NextCoreChapter(reader, center_chapter);
    st.first_chapter = first;
    st.center_chapter = center_chapter;
    st.last_chapter = last;

    // ★ Ascending order matters twice. Per track, samples must arrive non-decreasing in time -- the
    // cursor's precondition. And the FIRST keyframe seen becomes the resolver's seed, so it has to be
    // the earliest one in the range or every netvar query before the centre resolves to a later value.
    uint64_t inner_begin = 0, inner_end = 0;
    size_t previous = (size_t)-1;
    const size_t order[3] = {first, center_chapter, last};
    for (size_t k = 0; k < 3; ++k) {
        if (order[k] == previous) {
            continue;   // at either end of the file the neighbour IS the centre; decode it once
        }
        previous = order[k];
        if (!DecodeChapterInto(reader, order[k], out, st, ctx)) {
            return false;
        }
        if (order[k] == center_chapter) {
            const ChapterKey ck = reader.ChapterAt(center_chapter);
            inner_begin = ck.tBeginUs;
            inner_end = ck.tEndUs;
        }
    }
    out.SetFileEdges(PrevCoreChapter(reader, first) == first, NextCoreChapter(reader, last) == last);
    return FinishBuild(out, st, ctx, inner_begin, inner_end);
}

}  // namespace

bool BuildWindowRange(IDemoReader& reader, size_t center_chapter, Window& out, BuildStats& st,
                      const WireClocks* clocks, const NetVarPrimer* primer) {
    // ★ THE TICK THREAD'S OWN SCRATCH, and it stays a function static deliberately. The stack copy
    // this replaced overflowed, and a per-call heap allocation on the tick path is the cost this
    // packet exists to remove. A second thread passes its own through the overload below; nothing
    // else may enter this one.
    static BuildCtx ctx;
    return BuildWindowRangeWith(reader, center_chapter, out, st, clocks, primer, ctx);
}

bool BuildWindowRange(IDemoReader& reader, size_t center_chapter, Window& out, BuildStats& st,
                      const WireClocks* clocks, const NetVarPrimer* primer, BuildScratch& scratch) {
    return BuildWindowRangeWith(reader, center_chapter, out, st, clocks, primer, scratch.ctx);
}

bool WireClocks::ToCaptureUs(uint64_t hdr0, uint64_t stamp_us, uint64_t& out, bool* clamped) const {
    const uint32_t who = SenderOf(hdr0);
    for (size_t i = 0; i < count; ++i) {
        if (sender[i] != who) {
            continue;
        }
        const int64_t t = (int64_t)stamp_us - offset_us[i];
        out = t > 0 ? (uint64_t)t : 0u;
        if (clamped) *clamped = t < 0;
        return true;
    }
    return false;
}

namespace {

// One chapter's first and last stamp per sender, by value rather than by position in the block.
struct ChapterSenders {
    uint32_t sender[WireClocks::kMaxSenders];
    uint64_t first[WireClocks::kMaxSenders];
    uint64_t last[WireClocks::kMaxSenders];
    size_t count = 0;
    uint32_t overflowed = 0;

    void Note(uint64_t hdr0, uint64_t stamp_us) {
        const uint32_t who = WireClocks::SenderOf(hdr0);
        for (size_t i = 0; i < count; ++i) {
            if (sender[i] == who) {
                if (stamp_us < first[i]) first[i] = stamp_us;
                if (stamp_us > last[i]) last[i] = stamp_us;
                return;
            }
        }
        if (count >= WireClocks::kMaxSenders) {
            ++overflowed;
            return;
        }
        sender[count] = who;
        first[count] = last[count] = stamp_us;
        ++count;
    }
};

// The component-id bodies' owned/unowned changes as the scan meets them, in file order per body and
// stream; placed on the capture axis once every chapter has lent its bounds to the clocks.
struct HolderRun {
    struct Raw {
        uint64_t id_value, hdr0, stamp;
        bool sampled, owned;
    };
    struct Last {
        uint64_t id_value;
        bool sampled, owned;
    };
    static constexpr size_t kBodies = 8;
    std::vector<Raw> raw;
    Last last[kBodies] = {};
    size_t bodies = 0;
    bool incomplete = false;   // more edges or bodies than kept, or a block did not decode

    void Note(const capture::DecodedBody& d, const BodyIdentity& id, bool sampled) {
        if (ClassifyBodyIdentity(id.fill, id.tag) != IdKind::ComponentIdByTag) return;
        const bool owned = OwnerSlotOf(d.frame) != kBodyOwnerNone;
        size_t at = 0;
        while (at < bodies && (last[at].id_value != id.value || last[at].sampled != sampled)) ++at;
        if (at == bodies) {
            if (bodies == kBodies) {
                incomplete = true;
                return;
            }
            last[bodies++] = {id.value, sampled, !owned};
        }
        if (last[at].owned == owned) return;
        if (raw.size() >= TrackScan::kMaxHolderEdges) {
            incomplete = true;
            return;
        }
        last[at].owned = owned;
        raw.push_back({id.value, d.frame.hdr0, d.stampUs, sampled, owned});
    }

    void Place(const WireClocks& clocks, TrackScan& out) const {
        out.holder_edges_incomplete = incomplete;
        out.holder_edges.reserve(raw.size());
        StreamStats unused;
        for (const Raw& r : raw) {
            uint64_t t = 0;
            if (!PlaceOnCaptureAxis(r.sampled, &clocks, true, r.hdr0, r.stamp, t, unused)) {
                ++out.holder_edges_unplaced;
                continue;
            }
            out.holder_edges.push_back({r.id_value, (int64_t)t, r.owned});
        }
        std::stable_sort(out.holder_edges.begin(), out.holder_edges.end(),
                         [](const TrackScan::HolderEdge& a, const TrackScan::HolderEdge& b) {
                             return a.id_value != b.id_value ? a.id_value < b.id_value
                                                             : a.t_us < b.t_us;
                         });
    }
};

// The sampled body stream carries no wire clock, so only the holder edges are read out of it.
bool ScanSampledHolders(const StreamView& sv, HolderRun& holders) {
    BlockCursor bc(sv);
    if (!bc.usable) return true;
    capture::BodyDecoder dec(true);
    uint8_t scratch[512];
    BuildStats unused;
    for (uint32_t i = 0; i < sv.recordCount; ++i) {
        capture::DecodedBody d;
        memset(&d, 0, sizeof d);
        if (!dec.Decode(bc.reader, d, scratch, sizeof scratch)) return false;
        BodyIdentity id;
        if (!d.verbatim && BodyKeyOfSampled(d.frame, d.entity, id, unused)) {
            holders.Note(d, id, true);
        }
    }
    return true;
}

bool ScanWireStamps(const StreamView& sv, bool body, ChapterSenders& seen, TrackScan& out,
                    HolderRun& holders) {
    BlockCursor bc(sv);
    if (!bc.usable) {
        return true;
    }
    capture::BodyDecoder bodies(false);
    capture::PlayerDecoder players(false);
    uint8_t scratch[512];
    for (uint32_t i = 0; i < sv.recordCount; ++i) {
        uint64_t hdr0 = 0, stamp = 0;
        bool usable = false;
        if (body) {
            capture::DecodedBody d;
            memset(&d, 0, sizeof d);
            if (!bodies.Decode(bc.reader, d, scratch, sizeof scratch)) return false;
            if (d.verbatim) RecoverWireVerbatim(d);
            usable = !d.verbatim && (d.frame.fill & capture::kFillBodyHeader) != 0;
            hdr0 = d.frame.hdr0;
            stamp = d.stampUs;
            BodyIdentity id;
            if (usable && BodyKeyOfWire(d.frame, id)) holders.Note(d, id, false);
        } else {
            capture::DecodedPlayer d;
            memset(&d, 0, sizeof d);
            if (!players.Decode(bc.reader, d, scratch, sizeof scratch)) return false;
            if (d.verbatim) RecoverWireVerbatim(d);
            usable = !d.verbatim && (d.frame.fill & capture::kFillHeader) != 0;
            hdr0 = d.frame.hdr0;
            stamp = d.stampUs;
        }
        if (usable) {
            seen.Note(hdr0, stamp);
            ++out.wire_records;
        }
    }
    return true;
}

uint32_t RecordsIn(const DecodedChapter& dc, StreamId id) {
    const StreamView* sv = dc.Find(id);
    return sv ? sv->recordCount : 0u;
}

// One chapter's two bounds on one sender's offset.
struct ChapterBound {
    size_t sender_index;
    int64_t early_us, late_us;
};

// The bounds of WireClocks, from every chapter's pair. Load time only.
void SettleClocks(const std::vector<ChapterBound>& bounds, WireClocks& c) {
    std::vector<int64_t> early, late;
    for (size_t i = 0; i < c.count; ++i) {
        early.clear();
        late.clear();
        for (const ChapterBound& b : bounds) {
            if (b.sender_index == i) {
                early.push_back(b.early_us);
                late.push_back(b.late_us);
            }
        }
        std::sort(early.begin(), early.end());
        std::sort(late.begin(), late.end());
        const size_t n = late.size();
#ifdef TRACKMAP_MUTATE_UNTRIMMED_CLOCK
        const size_t trim = 0;
#else
        const size_t trim = WireClocks::TrimFor(n);
#endif
        // From the trusted rank back out toward the extreme, for as long as each chapter stands
        // within one step of the last. A sound file reaches its extreme; a bad stamp stands alone.
        size_t hi = n - 1 - trim, lo = trim;
        while (hi + 1 < n && late[hi + 1] - late[hi] <= WireClocks::kOutlierStepUs) ++hi;
        while (lo > 0 && early[lo] - early[lo - 1] <= WireClocks::kOutlierStepUs) --lo;
        c.chapters[i] = (uint32_t)n;
        c.ignored[i] = (uint32_t)((n - 1 - hi) + lo);
        c.raw_early_us[i] = early.front();
        c.raw_late_us[i] = late.back();
        c.early_us[i] = early[lo];
        c.late_us[i] = late[hi];
        // The late bound is the least-delayed record's, and is the estimate whenever the sender
        // was seen across a chapter boundary. Otherwise the two only bracket it.
        c.offset_us[i] = c.late_us[i] >= c.early_us[i]
                             ? c.late_us[i]
                             : c.late_us[i] + (c.early_us[i] - c.late_us[i]) / 2;
    }
}

// The first and last tick times in one decoded chapter, folded into the scan. Ticks are
// non-decreasing within a chapter and chapters arrive in order, so the running min/max over the
// file is the recorded span. A chapter with no S_TICK block contributes nothing.
bool ScanTickSpan(const DecodedChapter& dc, TrackScan& out) {
    const StreamView* sv = dc.Find(kStreamTick);
    if (sv == nullptr || sv->recordCount == 0) return true;
    BlockCursor bc(*sv);
    if (!bc.usable) return true;
    capture::TickDecoder dec;
    for (uint32_t i = 0; i < sv->recordCount; ++i) {
        capture::TickRec rec;
        memset(&rec, 0, sizeof rec);
        if (!dec.Decode(bc.reader, rec)) return false;
        const int64_t t = (int64_t)rec.sourceTimeUs;
        if (!out.have_tick_span) {
            out.have_tick_span = true;
            out.first_tick_us = t;
            out.last_tick_us = t;
        } else {
            if (t < out.first_tick_us) out.first_tick_us = t;
            if (t > out.last_tick_us) out.last_tick_us = t;
        }
    }
    return true;
}

}  // namespace

bool ScanTrackSources(IDemoReader& reader, TrackScan& out, const char** why) {
    out = TrackScan();
    WireClocks& c = out.clocks;
    std::vector<ChapterBound> bounds;
    HolderRun holders;
    const size_t n = reader.ChapterCount();
    for (size_t i = 0; i < n; ++i) {
        const ChapterKey ck = reader.ChapterAt(i);
        if (ck.section != kSectionCore) {
            continue;
        }
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) {
            if (why) *why = "DecodeChapter refused during the track scan";
            return false;
        }
        const uint32_t bodies = RecordsIn(dc, kStreamBodyWire) + RecordsIn(dc, kStreamBodySampled);
        const uint32_t players = RecordsIn(dc, kStreamPlayerWire) + RecordsIn(dc, kStreamPlayerSampled);
        const uint32_t ticks = RecordsIn(dc, kStreamTick);
        if (bodies > out.max_body_records) out.max_body_records = bodies;
        if (players > out.max_player_records) out.max_player_records = players;
        if (ticks > out.max_ticks) out.max_ticks = ticks;
        if (!ScanTickSpan(dc, out)) {
            if (why) *why = "S_TICK did not decode during the track scan";
            return false;
        }

        ChapterSenders seen;
        const StreamView* bw = dc.Find(kStreamBodyWire);
        const StreamView* pw = dc.Find(kStreamPlayerWire);
        if ((bw && !ScanWireStamps(*bw, true, seen, out, holders)) ||
            (pw && !ScanWireStamps(*pw, false, seen, out, holders))) {
            if (why) *why = "a wire stream block did not decode during the track scan";
            return false;
        }
        const StreamView* bs = dc.Find(kStreamBodySampled);
        // The window refuses such a chapter on its own; the clocks never needed this stream.
        if (bs && !ScanSampledHolders(*bs, holders)) holders.incomplete = true;
        c.senders_overflowed += seen.overflowed;
        for (size_t k = 0; k < seen.count; ++k) {
            size_t at = 0;
            while (at < c.count && c.sender[at] != seen.sender[k]) ++at;
            if (at == c.count) {
                if (c.count >= WireClocks::kMaxSenders) {
                    ++c.senders_overflowed;
                    continue;
                }
                c.sender[c.count++] = seen.sender[k];
            }
            bounds.push_back({at, (int64_t)seen.first[k] - (int64_t)ck.tBeginUs,
                              (int64_t)seen.last[k] - (int64_t)ck.tEndUs});
        }
    }
    SettleClocks(bounds, c);
    holders.Place(c, out);
    return true;
}

void FormatWireClock(const WireClocks& c, size_t i, const char* sender_name, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (i >= c.count) return;
    _snprintf_s(out, cap, _TRUNCATE,
                "wire clock: sender %s stamps %lld us ahead of the capture axis (latency spread "
                "%lld us; per-chapter offset spread %lld us over %u chapters, %u chapter bounds "
                "ignored as outliers)",
                sender_name, (long long)c.offset_us[i], (long long)(c.late_us[i] - c.early_us[i]),
                (long long)c.SpreadUs(i), unsigned(c.chapters[i]), unsigned(c.ignored[i]));
    if (c.SpreadUs(i) > WireClocks::kSpreadWarnUs) {
        const size_t used = strnlen(out, cap);
        _snprintf_s(out + used, cap - used, _TRUNCATE,
                    " -- the chapters disagree by more than %lld us: a mis-stamped record or a "
                    "clock step",
                    (long long)WireClocks::kSpreadWarnUs);
    }
}

WindowCaps CapsForScan(const WindowCaps& base, const TrackScan& scan) {
    WindowCaps c = base;
    const size_t bodies = (size_t)scan.max_body_records * kWindowChapters;
    const size_t players = (size_t)scan.max_player_records * kWindowChapters;
    const size_t ticks = (size_t)scan.max_ticks * kWindowChapters;
    if (bodies > c.max_body_samples) c.max_body_samples = bodies;
    if (players > c.max_player_samples) c.max_player_samples = players;
    if (ticks > c.max_ticks) c.max_ticks = ticks;
    return c;
}

void FormatBuildSummary(const BuildStats& st, size_t part, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    const struct { const char* name; const StreamStats* s; } rows[] = {
        {"body_wire", &st.body_wire},
        {"body_sampled", &st.body_sampled},
        {"player_wire", &st.player_wire},
        {"player_sampled", &st.player_sampled},
    };
    if (part >= kBuildSummaryParts) return;
    if (part + 1 == kBuildSummaryParts) {
        _snprintf_s(out, cap, _TRUNCATE,
                    "netvar rows=%u seeds=%u stamps=%s spread=%u over chapters up to %.3f s "
                    "(worst placement error +/-%.3f s on a key with %u row(s); %u one-row key(s))",
                    unsigned(st.netvar_rows), unsigned(st.netvar_seeds),
                    st.netvar_stamps_present ? "file" : "none",
                    unsigned(st.netvar_rows_spread), double(st.netvar_spread_span_us) / 1e6,
                    double(st.netvar_spread_worst_us) / 1e6,
                    unsigned(st.netvar_spread_sparsest_rows),
                    unsigned(st.netvar_spread_one_row_keys));
        return;
    }
    for (size_t i = part * 2; i < part * 2 + 2; ++i) {
        const size_t used = strnlen(out, cap);
        if (used + 1 >= cap) return;
        const StreamStats& s = *rows[i].s;
        _snprintf_s(out + used, cap - used, _TRUNCATE,
                    "%s%s read=%u keyed=%u recovered=%u drop(verbatim=%u identity=%u clock=%u "
                    "window=%u) clamped=%u",
                    (i % 2) ? " | " : "", rows[i].name, unsigned(s.read), unsigned(s.keyed),
                    unsigned(s.recovered_verbatim), unsigned(s.dropped_verbatim),
                    unsigned(s.dropped_identity), unsigned(s.dropped_clock),
                    unsigned(s.dropped_window), unsigned(s.clock_clamped));
    }
}

}  // namespace playback
}  // namespace replay
