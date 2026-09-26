#include "identity/dresser.h"

#include <stdio.h>
#include <string.h>

#include "engine/identity_access_facts.h"

namespace replay {
namespace identity {

using identity_access::Refusal;

namespace {

// Team ids at or above this have no alignment at all -- CR15NetGame::Alignment returns
// 0xFFFF for them -- which is DESIGN 9.5 E-I6's spectator and moderator entrants.  The limit
// is IDENT.ALIGN_TEAM_LIMIT, taken from the registry rather than repeated here.
constexpr uint32_t kAlignedTeamCount = identity_facts::kALIGN_TEAM_LIMIT;
constexpr uint16_t kNoAlignment = 0xFFFF;

// The engine's no-user state, authored once.  See dresser.h's VacateSlot comment for where each
// value comes from; the two that are not literals are taken from the registry, not repeated.
//
// A document rather than nothing: SetProfileJson refuses an empty blob (Refusal::EmptyProfile), and
// the leaf that has to change is the one UserDisplayName reads.
constexpr char kVacantProfileJson[] = "{\"displayname\":\"\"}";

// The first team id the engine's own alignment table does not cover, so CR15NetGame::Alignment
// answers "none" for it without depending on our override having landed.
constexpr uint8_t kVacantTeam = uint8_t(identity_facts::kALIGN_TEAM_LIMIT);

// The recorded name always fits the engine's inline slot-name buffer, as a compile-time fact
// rather than a hope: 33 bytes terminated against IDENT.SLOTNAME_FIELD_SIZE (36).  It used to
// read 44, which was the distance to the generation u16 rather than the buffer the engine itself
// writes; the margin is smaller than it looked but the assert still holds.  SetSlotName refuses a
// longer name -- it is public and other callers are not bounded by this struct -- and vacating
// authors the mirror at IDENT.SLOTNAME_MIRROR_OFF too, so a vacated slot is byte-identical to an
// engine-emptied one.  Dress cannot hit the refusal.
static_assert(sizeof(PlayerRecord::displayName) + 1 <= identity_access::kSlotNameCap,
              "a recorded display name no longer fits IDENT.SLOTNAME_FIELD_OFF's buffer");

void Copy(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    size_t n = 0;
    while (src && src[n] && n + 1 < cap) { dst[n] = src[n]; ++n; }
    dst[n] = '\0';
}

// Step 1's own failure path.  The provision refusal is carried by NAME and the identity_access
// refusal is left at None on purpose: nothing in that vocabulary ran, and picking a word out of it
// -- LastRefusal still holds whatever the previous call left -- is how a provisioning failure came
// to be reported as an empty profile.
bool FailProvision(DressResult& out, IIdentity& id) {
    out.ok = false;
    out.stage = "entrant";
    out.provisionRefusal = id.LastProvisionRefusal();
    out.refusal = Refusal::None;
    Copy(out.detail, sizeof out.detail, "not provisioned: ");
    Copy(out.detail + strlen(out.detail), sizeof out.detail - strlen(out.detail),
         PawnProvision::RefusalText(out.provisionRefusal));
    return false;
}

bool Fail(DressResult& out, const char* stage, IIdentity& id) {
    out.ok = false;
    out.stage = stage;
    out.refusal = identity_access::LastRefusal(&id);
    // A STEP THAT FAILED WITH NO REASON IS ITS OWN REPORT.  `None` here is not "nothing went
    // wrong" -- the step said no -- and printing its text made every such failure read
    // "stage=entrant reason=None: none", which is the line that hid the wrapper defect for a
    // whole session.  It gets a word of its own instead.
    if (out.refusal == Refusal::None) out.refusal = Refusal::ReasonNotRecorded;
    Copy(out.detail, sizeof out.detail, identity_access::LastDetail(&id));
    if (out.detail[0] == '\0')
        Copy(out.detail, sizeof out.detail, identity_access::RefusalText(out.refusal));
    return false;
}

// Which team the capture recorded for this row, if it recorded one at all.  The 16-bit field is
// the recorded one; the byte is an older capture's only copy of it, and both carry the same
// absent sentinel.
bool RecordedTeam(const PlayerRecord& rec, uint16_t& team) {
    if (rec.teamId16 != kPlayerNoTeam16) { team = rec.teamId16; return true; }
    // The byte field can only carry the sentinel's low half, which is the only absence it can
    // express; masked rather than cast so the truncation is the statement and not a warning.
    if (rec.teamId != uint8_t(kPlayerNoTeam16 & 0xFFu)) { team = uint16_t(rec.teamId); return true; }
    return false;
}

// A row that was never a playing entrant.  CR15NetGame::Alignment answers "none" at or above
// IDENT.ALIGN_TEAM_LIMIT, and the engine's add-entrant handler enables an actor only for a
// participating role -- so an admitted spectator burns one of sixteen engine slots, gets no actor
// and no physics CI, never leaves the provisioner's Awaiting state, and is retried until the
// deferral bound gives up on it.  The recorder's own row is one of these: the live viewer IS that
// person, so admitting it seats a second copy of whoever is watching.
bool IsSpectatorRow(const PlayerRecord& rec) {
    uint16_t team = 0;
    return RecordedTeam(rec, team) && team >= kAlignedTeamCount;
}

// No position holds a real item id.  kPlayerFillLoadout is not trusted for this: a real capture
// sets it on a row whose twenty ids are all the no-model sentinel.
bool NoRecordedLoadout(const Symbol64 slots[20]) {
    for (uint32_t i = 0; i < 20; ++i)
        if (slots[i] != 0 && slots[i] != identity_access::kUnrecordedItemId) return false;
    return true;
}

// The document of last resort.  The engine parses a remote entrant's profile out of the join
// reply; offline there is no sender, and four of five rows of a real capture carry no entrantJson
// and no identity block either.  Every step after the attach -- the scalar setters, the loadout
// apply, the rendered name -- refuses EmptyProfile against the empty node the engine leaves there,
// so a document with just the recorded name in it is the difference between a jersey number and
// `00`.  Returns the bytes written, excluding the terminator.
uint32_t AuthorMinimalProfile(const char* name, char* out, size_t cap) {
    static const char kHead[] = "{\"displayname\":\"";
    static const char kTail[] = "\"}";
    size_t n = 0;
    for (const char* p = kHead; *p && n + 1 < cap; ++p) out[n++] = *p;
    for (const char* p = name; p && *p && n + 7 < cap; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = char(c); }
        else if (c < 0x20) {
            static const char kHex[] = "0123456789abcdef";
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = kHex[(c >> 4) & 0xF]; out[n++] = kHex[c & 0xF];
        } else out[n++] = char(c);
    }
    for (const char* p = kTail; *p && n + 1 < cap; ++p) out[n++] = *p;
    out[n] = '\0';
    return uint32_t(n);
}

}  // namespace

ItemDiff DiffItems(const Symbol64 want[20], const Symbol64 got[20]) {
    ItemDiff d{};
    if (!want || !got) return d;
    for (uint32_t i = 0; i < 20; ++i) {
        if (want[i] == 0) continue;    // the recording equipped nothing in this position
        // ...and neither does the no-model sentinel, which ApplyLoadout refuses to write.  Scored
        // as a comparison it matches ITSELF -- the engine reads back what was already there -- and
        // reports `20/20 items match` for an entrant wearing nothing.
        if (want[i] == identity_access::kUnrecordedItemId) continue;
        ++d.compared;
        if (got[i] == want[i]) ++d.matching;
        else if (got[i] != 0)  ++d.remapped;
        else                   ++d.absent;
    }
    return d;
}

DressAction NextDressAction(uint32_t deferredNow, uint32_t deferredBefore, bool firstPass,
                            uint64_t now_ms, uint64_t boundMs, uint64_t& since_ms) {
    if (deferredNow == 0) {
        since_ms = 0;
        return DressAction::Settled;
    }
    if (firstPass || since_ms == 0 || deferredNow < deferredBefore) {
        since_ms = now_ms;
        return DressAction::KeepWaiting;
    }
    return now_ms - since_ms >= boundMs ? DressAction::GiveUp : DressAction::KeepWaiting;
}

bool Verdict(const DressResult& r) {
    // A row the capture recorded no loadout for is judged on its NAME alone.  Requiring a compared
    // item would fail it for something it never had, exactly as requiring a team would.
    if (r.loadoutRefusal == Refusal::LoadoutNotRecorded) return r.nameMatches;
    // A slot the ENGINE holds no loadout instance for is judged the same way, and for the same
    // reason: there is no container to write the twenty ids into, so requiring a matched item
    // would fail the player for something no step of this ladder can provide.  The instance is
    // built by the engine's parse of a loadout DOCUMENT, and the subject capture recorded none
    // for four of its five rows (`profile refused` in the identity block).  What the ladder CAN
    // put on such a player -- the name, both jersey numbers, the team, the colour -- has already
    // been written when this is reached, and the outfit is named as refused on its own line.
#ifndef NAMES4_MUTATE_NO_INSTANCE_IS_A_FAILURE
    if (r.loadoutRefusal == Refusal::NoLoadoutInstance) return r.nameMatches;
#endif
    return r.items.absent == 0 && r.items.compared > 0 && r.nameMatches;
}

// A team the recording never captured is not a team the engine refused, and the
// difference decides whether the player keeps its loadout.  B.0 wrote the low byte of
// kPlayerNoTeam16 into the 8-bit field for "absent" -- that is the only absence it could express --
// and the 16-bit field carries the same sentinel, so either form is the record saying it has no
// team.  A step with no input costs itself and nothing after it: the twenty loadout ids, both
// numbers, the colour and the chassis still go on.
bool ApplyTeamStep(IIdentity& id, const PlayerRecord& rec, UserIndex index, DressResult& out) {
#ifndef W2P1_MUTATE_TEAM_NOT_RECORDED_ABORTS_DRESS
    if (rec.teamId == uint8_t(kPlayerNoTeam16) || rec.teamId16 == kPlayerNoTeam16) {
        out.teamRefusal = identity_access::Refusal::TeamNotRecorded;
        out.wrote.team = false;
        return true;
    }
#endif
    if (!id.SetTeam(index, rec.teamId, rec.teamSlot)) return false;
    out.wrote.team = true;
    return true;
}

bool Dress(IIdentity& id, const PlayerRecord& rec, DressResult& out, const DressEnv& env,
           uint16_t adoptEngineSlot) {
    memset(&out, 0, sizeof out);
    out.stage = "start";
    out.userIndex = kNotDressed;
    out.slotResolved = kNotDressed;
    out.numberWanted = rec.loadoutNumber;
    out.numberBodyWanted = rec.loadoutNumberBody;
    out.alignmentWanted = rec.alignment;

    // NOT A PLAYER.  Refused before the admission, because admitting one is the damage: it takes an
    // engine slot the engine will never finish, and the pass then waits on it for the whole bound.
    // DressRoster filters these out before it gets here; this is the same rule for a direct caller.
#ifndef W2P1_MUTATE_SPECTATOR_ADMITTED
    if (IsSpectatorRow(rec)) {
        out.spectator = true;
        out.stage = "spectator";
        out.refusal = Refusal::SpectatorRow;
        Copy(out.detail, sizeof out.detail, identity_access::RefusalText(out.refusal));
        return false;
    }
#endif

    const UserSlot recorded(rec.userSlot);
    UserIndex index(0);

    // PlayerRecord::displayName is a fixed array that the format pads but does not guarantee
    // to terminate, and both the engine write and the comparison below are C-string calls.
    // Terminating it once here is the only place that has to know.
    char want[sizeof rec.displayName + 1];
    memcpy(want, rec.displayName, sizeof rec.displayName);
    want[sizeof rec.displayName] = '\0';

    // 1. ADMIT the recorded player, then look up what was admitted.  ARCH-23 S7.2: this step used
    //    to be a lookup wearing an adding verb, and that is the whole of L3 #11 -- there was no
    //    entrant to dress, so the five predicates refused on the profile of somebody who had never
    //    been admitted, and the log said the profile was empty.  Provision is idempotent per slot
    //    per load on the provider that owns the state; here a refusal is reported by name and the
    //    slot is left alone, because a dresser that guessed which slot its earlier pass had been
    //    given would write this player's face onto whoever the engine actually has there.
    // The RECORDED slot is the entrant discriminator the descriptor's synthetic xplatform id
    // is built on: it is unique per entrant within the capture and it is known before the
    // admission, which the live index is not.
    //    ADOPT FIRST.  An entrant this session already provisioned for this capture slot is still
    //    in the engine -- the release refuses on this build -- so admitting another one is what put
    //    two rows on the rail for one player.  ResolveReplayUser is a pure lookup with the same
    //    five predicates the dress needs, so a slot it answers for is an entrant this ladder can
    //    dress; anything else falls through to the admission below.
    if (adoptEngineSlot < RosterMapping::kSlots &&
        id.ResolveReplayUser(UserSlot(adoptEngineSlot), index)) {
        out.adopted = true;
    }
    else if (!id.ProvisionReplayUser(recorded, JoinDescFromRecord(rec, recorded.v), index))
        return FailProvision(out, id);

    // WHERE it landed, asked of the engine rather than assumed.  The recorded slot is what the file
    // says and is never a live index (W2-P1's contract, ★): the viewer's own user holds slot 0, so
    // the admission is routinely placed elsewhere and everything addressing the engine's user from
    // here on keys on this.
    const UserSlot resolved = id.SlotOf(index);
    if (resolved.v >= RosterMapping::kSlots) {
        out.ok = false;
        out.stage = "entrant";
        out.refusal = Refusal::SlotOutOfRange;
        Copy(out.detail, sizeof out.detail,
             "provisioned, but the engine does not place that user index in any slot the user-slot "
             "table covers, so there is nothing to dress");
        return false;
    }
    out.slotResolved = resolved.v;

    // 1a. THE PROFILE DOCUMENT, AND IT IS THE FIRST WRITE.  On a real join the login service's
    //     entrant-state reply carries it and the engine parses it into the entrant record; offline
    //     nothing sends one, so the record holds an empty node and every step below -- the five
    //     predicates included -- answers EmptyProfile.  That is the `loadouts=?(ProfileNotAttached)`
    //     the census printed for every slot, all session, and the reason no name or number ever
    //     landed.  Three sources, in the order of how much they know: the capture's own entrant
    //     profile; the identity block's loadout document (four of five rows of a real capture have
    //     no entrant profile at all); and, failing both, a document carrying the recorded name,
    //     which is enough to unlock the slot name, the team and the twenty ids.
    Blob doc = rec.entrantJson;
    if ((doc.p == nullptr || doc.n == 0) && env.recordedDocument != nullptr) {
        Blob fromBlock{};
        if (env.recordedDocument(rec.userSlot, fromBlock) && fromBlock.p && fromBlock.n)
            doc = fromBlock;
    }
    char minimal[288];
    if (doc.p == nullptr || doc.n == 0) {
        out.profileRefusal = Refusal::ProfileNotRecorded;
        const uint32_t n = AuthorMinimalProfile(want, minimal, sizeof minimal);
        doc = Blob{reinterpret_cast<const uint8_t*>(minimal), n};
    }
    Refusal attached = identity_access::AttachProfile(&id, index, doc);
    if (attached != Refusal::None && attached != Refusal::NoNetGame &&
        doc.p != reinterpret_cast<const uint8_t*>(minimal)) {
        // A COSMETIC SOURCE COSTS ITSELF (the review m3).  The recorded document is bytes from
        // the file and the engine's parser is entitled to refuse them -- a name that is not valid
        // UTF-8 is enough -- and four of the five recorded rows have no document at all.  Losing
        // the whole ladder over it would cost this player its slot name, its team and its loadout
        // too, so the authored `{"displayname":...}` is tried in its place and, failing that, the
        // step refuses by name like every other step that cannot do its job.
        out.profileRefusal = attached;
        const uint32_t n = AuthorMinimalProfile(want, minimal, sizeof minimal);
        doc = Blob{reinterpret_cast<const uint8_t*>(minimal), n};
        attached = identity_access::AttachProfile(&id, index, doc);
    }
    if (attached != Refusal::None && attached != Refusal::NoNetGame) {
        if (out.profileRefusal == Refusal::None) out.profileRefusal = attached;
        Copy(out.detail, sizeof out.detail, identity_access::RefusalText(attached));
    } else if (attached == Refusal::NoNetGame) {
        // NO ENGINE BEHIND THIS IDENTITY AT ALL.  Not this step's refusal to claim: every step
        // below reaches the same session and names itself, and claiming it here would rename all
        // of them "attach".  It is also what keeps the ladder drivable with no game present.
        if (out.profileRefusal == Refusal::None) out.profileRefusal = attached;
    } else {
        // The slot now carries THIS replay's document, so the next load owes it a vacate whatever
        // the rest of the ladder does.
        out.wrote.profile = true;
    }

    // The five predicates, on the RESOLVED slot: the entrant has to exist and be OUTSIDE the replay
    // band, because a player inside it is renamed "Replayer %u" whatever the profile says.  This is
    // also the admission's postcondition -- an insert that "succeeded" and left the slot unusable
    // refuses here rather than three steps later, wearing another step's name.
    if (!id.ResolveReplayUser(resolved, index)) return Fail(out, "entrant", id);

    // The one place the live index is knowable.  It is recorded here rather than at the end
    // because the steps below can fail on cosmetics without unmaking the identity: an entrant
    // wearing the wrong chassis is still that entrant, and its body still has to move.
    if (index.v != kNotDressed) {
        out.indexResolved = true;
        out.userIndex = index.v;
    }

    // 2. The profile FIRST.  ApplyLoadoutItems returns without a word on an empty CJson, and
    //    UserDisplayName reads `displayname` out of the same document, so every later step
    //    depends on this one having happened.
    //     A row with no recorded entrant profile has no scalars to carry -- the document attached
    //     above is all this player was recorded as -- so the step costs itself and nothing after
    //     it, exactly as a missing team does.
    if (rec.entrantJson.p == nullptr || rec.entrantJson.n == 0) {
        out.profileRefusal = Refusal::ProfileNotRecorded;
    } else if (!id.SetProfileJson(resolved, rec.entrantJson)) {
        return Fail(out, "profile", id);
    } else {
        // ONLY A WRITE OWES A VACATE (the review m1).  Set unconditionally, a row whose attach
        // answered NoNetGame and whose scalars refused ProfileNotRecorded still joined the residue,
        // and the NEXT load printed the "`displayname` is emptied" Warn for a slot this replay
        // never touched.  The attach's own success arm above sets it for the document it stored.
        out.wrote.profile = true;
    }
    out.profileScalars = identity_access::CarriedScalarCount(&id);
    out.uncarriedArrays = identity_access::UncarriedArrayCount(&id);
    Copy(out.firstUncarried, sizeof out.firstUncarried, identity_access::FirstUncarriedArray(&id));

    // 2a. The OTHER name source, unconditionally.  UserDisplayName reads the profile only when
    //     bit 27 of the mode flags is clear, and `-noovr` -- which the default entry strategy
    //     supplies -- sets that bit for the whole session.  On that launch the profile write
    //     above cannot be seen at all and the slot table is the only source the engine reads.
    //     Writing both, rather than branching on the bit, makes every reachable path agree.
    const Refusal slotName = identity_access::SetSlotName(&id, resolved, want);
    if (slotName != Refusal::None) {
        out.refusal = slotName;
        out.stage = "slotname";
        Copy(out.detail, sizeof out.detail, identity_access::RefusalText(slotName));
        out.ok = false;
        return false;
    }
    out.wrote.slotName = true;

    // 3. Team before loadout: the alignment the item gate compares against is derived from
    //    the team bits, so applying the loadout first would apply it against the old side.
    if (!ApplyTeamStep(id, rec, index, out)) return Fail(out, "team", id);

    // 4. The twenty ids and both numbers.  A row whose every id is the no-model sentinel has no
    //    loadout to apply: the engine's catalogue cannot resolve ~0ull, so every position would
    //    come back "[NETGAME] Unknown item" and a read-back would score 20/20 against a row we
    //    never recorded.  The step refuses by name and costs itself only -- the name, the team and
    //    the colour still go on -- exactly as a missing team does.
    if (!env.applyLoadout) {
        // playback.dress_loadout off: names, numbers and team still go on, the outfit does not.
        // The one step that writes into the engine's own per-entrant instance, switchable on its
        // own so a launch can tell a crash in it from a crash anywhere else in the ladder.
        out.loadoutRefusal = Refusal::LoadoutStepOff;
    } else if (NoRecordedLoadout(rec.loadoutSlots)) {
        out.loadoutRefusal = Refusal::LoadoutNotRecorded;
    } else if (!id.ApplyLoadout(index, rec.loadoutSlots, rec.loadoutNumber,
                                rec.loadoutNumberBody)) {
        // ShortInstance is a partial success worth continuing through: the items that fitted
        // ARE applied, and the read-back below is how many of them survived.  Everything else
        // stopped before the engine was touched.
        const Refusal loadoutWhy = identity_access::LastRefusal(&id);
        // NoLoadoutInstance is a partial success too, and NAMES-4 is why: ApplyLoadout writes
        // both jersey numbers into the engine's per-slot loadout HEADER before it looks for an
        // instance, so this refusal means "the outfit did not go on", not "nothing went on".
        // Costing the whole ladder over it took the colour and the read-back down with it and
        // left the roster with no dressed players at all.
        if (loadoutWhy == Refusal::NoLoadoutInstance) {
            out.loadoutRefusal = loadoutWhy;
            // The numbers ARE on the engine, so this slot owes the next load a vacate exactly as
            // a full apply would -- VacateSlot's ApplyLoadout(none, 0, 0) clears them the same way.
            out.wrote.loadout = true;
        } else if (loadoutWhy != Refusal::ShortInstance) {
            return Fail(out, "loadout", id);
        } else {
            out.stage = "loadout-short";
            out.wrote.loadout = true;
        }
    } else {
        out.wrote.loadout = true;
    }
    // ShortInstance included: the items that fitted ARE on the engine, so they are ours to vacate.
    // A step that had no input wrote nothing, so it owes nothing.

    // 5. Colour.  Alignment is team-derived, so this both stores the per-player override and
    //    forces the unconditional re-run -- a recolour without the re-run applies nothing.
    //
    //    Teams 2 and 4 (spectator, moderator) have NO alignment: CR15NetGame::Alignment
    //    returns 0xFFFF for any team id at or above IDENT.ALIGN_TEAM_LIMIT, and every
    //    alignment-gated slot is skipped for them.  Forcing an override here would give a
    //    spectator a team colour it never had, so the step is skipped rather than applied.
    //
    //    PlayerRecord::alignment is a uint8_t and the engine's alignment is a uint16_t whose
    //    "none" value is 0xFFFF, which a byte cannot hold.  That is why the no-alignment case
    //    is decided from the TEAM here and not from the recorded alignment byte.
    if (rec.teamId < kAlignedTeamCount) {
        if (!id.OverrideAlignment(index, rec.alignment)) return Fail(out, "alignment", id);
        out.wrote.alignment = true;
    } else {
        out.alignmentWanted = kNoAlignment;
    }

    // Every write this dress owes the slot has now been issued.  Whatever the read-back below
    // concludes, running the ladder again would only repeat them, so this is where the retry's
    // latch closes.
    out.applied = true;

    // 6. Read back what the engine actually holds.  This is the whole point: every step above
    //    can succeed and still leave the wrong avatar on screen.
    Symbol64 applied[20] = {};
    if (!id.ReadAppliedItems(index, applied)) return Fail(out, "readback", id);
    out.items = DiffItems(rec.loadoutSlots, applied);

    // The name is checked by ASKING THE ENGINE WHAT IT RENDERS -- the same call, on the same
    // object, that the client prints from -- and not by reading back either of the two sources
    // we just wrote.  UserDisplayName has three gates and the profile is only the third, so a
    // read-back of the profile confirms our own write landed on the one source the shipped
    // `-noovr` launch never consults, and reports "name ok" while every player renders
    // "Unknown".  What is compared here is indifferent to which gate answered.
    out.nameResolved =
        identity_access::ResolvedDisplayName(&id, index, out.nameShown, sizeof out.nameShown);
    out.nameMatches = out.nameResolved && strcmp(out.nameShown, want) == 0;
    if (!out.nameResolved) {
        out.refusal = Refusal::NameNotResolved;
        Copy(out.detail, sizeof out.detail, identity_access::RefusalText(out.refusal));
    } else if (!out.nameMatches) {
        // A name that resolved to the WRONG string is not a refusal -- every step succeeded and
        // the engine answered -- so it needs its own detail or the failure log reads blank.
        Copy(out.detail, sizeof out.detail, "the engine renders '");
        Copy(out.detail + strlen(out.detail), sizeof out.detail - strlen(out.detail), out.nameShown);
        Copy(out.detail + strlen(out.detail), sizeof out.detail - strlen(out.detail),
             "', not the recorded name");
    }

    out.ok = Verdict(out);
    if (strcmp(out.stage, "loadout-short") != 0) out.stage = out.ok ? "done" : "readback";
    if (out.ok) out.refusal = Refusal::None;
    return out.ok;
}

namespace {

// The report half of DressAndReport, split out so a RETRY pass can decide whether this player's
// outcome is news before the lines are written rather than after.
void ReportDress(IIdentity& id, ILog& log, const PlayerRecord& rec,
                 const DressResult& out, bool ok) {
    if (!ok) {
        log.Write(LogLevel::Error, "identity",
                  "player '%s' slot %u NOT dressed: stage=%s reason=%s",
                  rec.displayName, unsigned(rec.userSlot), out.stage, out.detail);
    } else {
        // A row whose outfit refused by name is still DRESSED -- the name, the numbers, the team
        // and the colour are on -- but the line has to say so, or a reader counts an avatar we
        // never put clothes on as a match.
        log.Write(LogLevel::Info, "identity",
                  "player '%s' slot %u -> %u dressed: %u/%u items match, %u remapped, "
                  "numbers %u/%u, alignment %u, name ok%s%s",
                  rec.displayName, unsigned(rec.userSlot), unsigned(out.slotResolved),
                  out.items.matching, out.items.compared, out.items.remapped,
                  unsigned(out.numberWanted), unsigned(out.numberBodyWanted),
                  unsigned(out.alignmentWanted),
                  out.loadoutRefusal == Refusal::None ? "" : " -- OUTFIT NOT APPLIED: ",
                  out.loadoutRefusal == Refusal::None
                      ? "" : identity_access::RefusalText(out.loadoutRefusal));
    }

    // The name the engine RENDERS, quoted, whenever it is not the recorded one.  This is the
    // line that would have shown "Unknown" for every player while the old check said "name ok":
    // a bare MISMATCH does not distinguish a wrong name from no name path at all.
    if (!out.nameMatches) {
        log.Write(LogLevel::Error, "identity",
                  "player '%s' slot %u renders as '%s'%s -- UserDisplayName does not return the "
                  "recorded name, so this player's name is wrong on screen",
                  rec.displayName, unsigned(rec.userSlot), out.nameShown,
                  out.nameResolved ? "" : " (UserDisplayName could not be called)");
    }

    // Always, on the pass path too.  An item the engine dropped and a profile field we could
    // not carry are exactly the two things that otherwise leave no trace at all.
    if (out.items.absent) {
        log.Write(LogLevel::Error, "identity",
                  "player '%s': %u of %u recorded items are NOT applied -- the avatar is wrong",
                  rec.displayName, out.items.absent, out.items.compared);
    }
    // WHAT THE ENGINE NOW HOLDS, on every pass, dressed or not.  LAUNCH-SHIPDEFAULTS was read as a
    // total name failure off the engine's own entrant dumps -- which it prints from the add-entrant
    // handler, before the ladder has run -- so the next launch gets the same four values at a
    // moment this code chooses.  `Loadouts` of 0 with a number set is the expected NAMES-4 shape.
    if (out.indexResolved) {
        identity_access::EngineIdentity eng{};
        if (identity_access::ReadEngineIdentity(&id, UserIndex(out.userIndex), eng)) {
            char instances[16], team[24];
            if (eng.instancesRead) snprintf(instances, sizeof instances, "%llu",
                                            (unsigned long long)eng.instances);
            else                   snprintf(instances, sizeof instances, "unread");
            if (eng.teamRead) snprintf(team, sizeof team, "%u/%u",
                                       unsigned(eng.teamId), unsigned(eng.teamSlot));
            else              snprintf(team, sizeof team, "unread");
            log.Write(LogLevel::Info, "identity",
                      "player '%s' slot %u -> index %u: ENGINE HOLDS name='%s'%s number=%u/%u%s "
                      "loadouts=%s team=%s",
                      rec.displayName, unsigned(rec.userSlot), unsigned(out.userIndex),
                      eng.name, eng.nameRead ? "" : " (UserDisplayName could not be called)",
                      unsigned(eng.number), unsigned(eng.numberBody),
                      eng.numbersRead ? "" : " (unread)", instances, team);
        }
    }

    if (out.uncarriedArrays) {
        log.Write(LogLevel::Warn, "identity",
                  "player '%s': %u profile array(s) could not be carried, first '%s'. "
                  "`unlocks` is one of these; see docs for what that does and does not affect",
                  rec.displayName, out.uncarriedArrays, out.firstUncarried);
    }
}

}  // namespace

bool DressAndReport(IIdentity& id, ILog& log, const PlayerRecord& rec, DressResult& out) {
    const bool ok = Dress(id, rec, out);
    ReportDress(id, log, rec, out, ok);
    return ok;
}

VacateOutcome VacateSlot(IIdentity& id, ILog& log, uint16_t userSlot,
                         const RosterMapping::ResidueEntry& e) {
    // The RESOLVED slot, never the recorded one: `userSlot` is this residue table's key -- what the
    // capture file called the player -- and the engine put the entrant wherever it had room.  Every
    // write below addresses the engine's user, so keyed on the recorded slot a vacate would strip
    // the name and profile of whoever holds that slot now, which in the L3 #11 shape is the viewer.
    const UserSlot slot(e.slotResolved);

    // The index is RE-RESOLVED, never taken from the residue: the entry's index belonged to the
    // game space that dressed it, and a re-bind between the two loads makes it a number from
    // somewhere else.  The slot is ours and stays valid; this is the one lookup that turns it back
    // into something the engine will accept.
    const UserIndex index = id.IndexOf(slot);
    if (index.v == kNotDressed) {
        log.Write(LogLevel::Info, "identity",
                  "residue slot %u: the entrant no longer exists in engine slot %u, so there "
                  "is nothing wearing the previous replay's identity; nothing to vacate",
                  unsigned(userSlot), unsigned(e.slotResolved));
        return VacateOutcome::StaleSlot;
    }

    // Every step is attempted even after one refuses.  Dress stops at the first refusal because its
    // later steps depend on its earlier ones; here they do not, and clearing four of five fields
    // leaves less of the previous player on screen than clearing none.  The outcome is still
    // Refused, and the reason named is the first one.
    const char* refusedStep = nullptr;
    Refusal     refusedWhy  = Refusal::None;
    auto note = [&](const char* step, Refusal why) {
        if (refusedStep) return;
        refusedStep = step;
        refusedWhy  = why;
    };

    if (e.wrote.profile) {
        const Blob neutral{reinterpret_cast<const uint8_t*>(kVacantProfileJson),
                           uint32_t(sizeof kVacantProfileJson - 1)};
        if (!id.SetProfileJson(slot, neutral)) note("profile", identity_access::LastRefusal(&id));
    }
    if (e.wrote.slotName) {
        const Refusal r = identity_access::SetSlotName(&id, slot, "");
        if (r != Refusal::None) note("slotname", r);
    }
    if (e.wrote.team) {
        if (!id.SetTeam(index, kVacantTeam, 0)) note("team", identity_access::LastRefusal(&id));
    }
    if (e.wrote.loadout) {
        const Symbol64 none[20] = {};
        if (!id.ApplyLoadout(index, none, 0, 0)) {
            // Same partial success the dress accepts: the instance the engine holds is shorter than
            // twenty and every entry it does hold was cleared.
            const Refusal r = identity_access::LastRefusal(&id);
            if (r != Refusal::ShortInstance) note("loadout", r);
        }
    }
    if (e.wrote.alignment) {
        if (!id.OverrideAlignment(index, kNoAlignment))
            note("alignment", identity_access::LastRefusal(&id));
    }

    if (refusedStep) {
        const char* why = identity_access::RefusalText(refusedWhy);
        if (refusedWhy == Refusal::None) why = "the write was refused and no reason was recorded";
        log.Write(LogLevel::Error, "identity",
                  "residue slot %u NOT vacated (vacate_refused: %s at step %s in engine slot "
                  "%u) -- it is still wearing the previous replay's name, loadout or colours",
                  unsigned(userSlot), why, refusedStep, unsigned(e.slotResolved));
        return VacateOutcome::Refused;
    }

    log.Write(LogLevel::Info, "identity",
              "residue slot %u vacated: this replay does not use it, so the previous "
              "replay's identity was overwritten with the engine's no-user state in engine slot %u "
              "(index %u)",
              unsigned(userSlot), unsigned(e.slotResolved), unsigned(index.v));
    if (e.wrote.profile) {
        // Said out loud, every time, because it is the one part of the vacate that is partial.  The
        // engine's CJson has typed setters and no remove, so the leaf the client renders is emptied
        // and the leaves behind it are not.
        log.Write(LogLevel::Warn, "identity",
                  "residue slot %u: `displayname` is emptied, but the previous replay's other "
                  "profile scalars (stats, level) cannot be removed -- the engine's CJson exposes "
                  "no remove -- so they persist until an entrant document replaces them",
                  unsigned(userSlot));
    }
    return VacateOutcome::Vacated;
}

bool DressRoster(IIdentity& id, ILog& log, const PlayerRecord* players, size_t count,
                 RosterReport& out, RosterMapping& map, DressPass pass, const DressEnv& env) {
    out = RosterReport{};
    map.Clear();
    if (!players) count = 0;
    // THE DENOMINATOR IS PLAYERS, NOT ROWS.  A spectator row is never admitted and never dressed,
    // so counting it here would leave the roster permanently short of its own total and the pass
    // waiting on an entrant that cannot exist.
    for (size_t i = 0; i < count; ++i) {
        if (IsSpectatorRow(players[i])) ++out.spectators;
    }
    out.players = (uint32_t)count - out.spectators;
    const bool first = pass == DressPass::First;
    // A retry reports only what CHANGED, and the roster summary is part of that: a wait that lasts
    // seconds would otherwise print three unchanging lines a few times a second.
    bool news = first;

    // The residue as it stands BEFORE this roster adds its own entries: the slots earlier loads in
    // this session dressed and could not release.  Taken by copy, because the dress loop below
    // writes into the live residue and the obligation is owed to the state we came in with.
    RosterMapping::ResidueEntry prior[RosterMapping::kSlots];
    bool claimed[RosterMapping::kSlots] = {};    // named by THIS manifest
    bool redressed[RosterMapping::kSlots] = {};  // ...and this pass wrote at least one field into it
    for (size_t i = 0; i < RosterMapping::kSlots; ++i) {
        prior[i] = map.ResidueAt(i);
        if (prior[i].used) ++out.residue;
    }

    // The claim is manifest MEMBERSHIP, and it is taken before a single field is written, so it
    // cannot depend on how the dress below goes.  W2-B re-dresses the same manifest after an entity
    // re-bind: a player whose step 1 refuses on that second pass writes nothing, and keyed on
    // what the pass wrote it would be vacated -- the currently playing replay's own player losing
    // its name, loadout and colour.  The manifest names the slot; that is what makes it not residue.
    for (size_t i = 0; i < count; ++i)
        if (players[i].userSlot < RosterMapping::kSlots) claimed[players[i].userSlot] = true;

    for (size_t i = 0; i < count; ++i) {
        const uint16_t slot = players[i].userSlot;
        const bool inTable = slot < RosterMapping::kSlots;

        // NOT ADMITTED AND NOT DRESSED, at the one place that would have admitted it: Dress's own
        // step 1 IS the admission.  The engine gives a spectator no actor and no physics CI, so an
        // admitted one sits in the provisioner's Awaiting state for the life of the session and the
        // retry spends its whole bound on it.  Said out loud once per row, by team id, because
        // "five in the file, three on the ice" is a statement a reader has to be able to check.
        if (IsSpectatorRow(players[i])) {
            uint16_t team = 0;
            (void)RecordedTeam(players[i], team);
            if (first) {
                log.Write(LogLevel::Info, "identity",
                          "player '%s' slot %u is a spectator (team %u): not admitted and not "
                          "dressed -- the engine gives such an entrant no body, and the live "
                          "viewer is the spectator",
                          players[i].displayName, unsigned(slot), unsigned(team));
            }
            continue;
        }

        // ALREADY DRESSED ON THIS LOAD.  The ladder writes the profile, the slot name, the team,
        // twenty item ids, both numbers and the colour, and re-applying the loadout re-runs the
        // engine's own apply against the actor.  Doing that a few times a second for the rest of
        // the file is the one way this retry could make things worse, so a slot the ladder has
        // already run is skipped -- and its earlier verdict is carried forward, so `dressed` and
        // `failed` still sum to `players` on every pass.
        if (inTable && map.LatchAt(slot).applied) {
            const RosterMapping::DressLatch done = map.LatchAt(slot);
            ++out.alreadyApplied;
            if (done.verdict) ++out.dressed; else ++out.failed;
            redressed[slot] = true;
            if (done.userIndex != kNotDressed) map.Set(slot, UserIndex(done.userIndex));
            continue;
        }

        DressResult r;
        // The engine slot an earlier load in this session left an entrant in for THIS capture
        // slot.  Recorded for every provisioned entrant, written or not, and it survives Unload --
        // which is what makes the second load re-bind rather than admit a second body.
        // FIRST PASS ONLY.  `prior` is re-read on every pass, so from the second pass on it holds
        // THIS load's own entries and adopting from it would turn the retry into a re-bind of the
        // admission it is retrying.  The pre-load residue is only knowable on the first pass, and
        // that is the only pass that can have a previous load's entrant to adopt.
        const uint16_t adopt = (first && inTable) ? prior[slot].slotResolved : kNotDressed;
        const bool ok = Dress(id, players[i], r, env, adopt);
        if (r.adopted) ++out.adopted;

        // A DEFERRAL is an admission that is outstanding, not one the engine decided: the same set
        // the provisioner leaves queued rather than latching Failed.  It is what keeps the roster
        // pending, and it is deliberately still counted in `failed` too -- a deferred player is not
        // dressed, and the two summary lines have always meant that.
        const bool deferring = !ok && PawnProvision::IsDeferral(r.provisionRefusal);
        if (deferring && out.deferred < RosterMapping::kSlots) {
            out.deferredSlots[out.deferred].userSlot = slot;
            out.deferredSlots[out.deferred].why = r.provisionRefusal;
            ++out.deferred;
        }
        // THE OTHER WAY OUT OF THE DEFERRED SET, and it used to be the silent one: not dressed and
        // not waiting either.  Its refusal is carried by name so the summary can print who and why
        // without dressing the roster a second time to find out.
        if (!ok && !deferring && out.stuck < RosterMapping::kSlots) {
            out.stuckSlots[out.stuck].userSlot = slot;
            out.stuckSlots[out.stuck].why = r.refusal;
            ++out.stuck;
        }

        const RosterMapping::Outcome now = deferring ? RosterMapping::Outcome::Deferred
                                           : r.applied ? RosterMapping::Outcome::Applied
                                                       : RosterMapping::Outcome::Failed;
        const bool changed = inTable && map.LatchAt(slot).last != now;
        if (first || changed) {
            ReportDress(id, log, players[i], r, ok);
            news = true;
        }
        map.NoteDressPass(slot, now, ok, r.slotResolved, r.userIndex);

        if (ok) {
            // ONE LINE PER DRESSED PLAYER, next to which the engine's own entrant dump is read.
            // Everything the launch has to check is on it: which slot the row landed in, the name
            // and number that should now render, the team, and how many of the twenty positions
            // carried a recorded id.  The rest are SET to the engine's empty id, never written as
            // the sentinel -- nothing is removed from the actor, and the engine logs one
            // "[NETGAME] Unknown item" per such position when it applies the instance.
            uint32_t recorded = 0;
            for (size_t k = 0; k < 20; ++k)
                if (players[i].loadoutSlots[k] != identity_access::kUnrecordedItemId) ++recorded;
            uint16_t team = 0;
            (void)RecordedTeam(players[i], team);
            log.Write(LogLevel::Info, "identity",
                      "DRESSED slot %u -> engine slot %u (index %u): name '%s' number %u/%u "
                      "team %u items %u recorded / %u set to the engine's no-item%s%s",
                      unsigned(slot), unsigned(r.slotResolved), unsigned(r.userIndex),
                      players[i].displayName, unsigned(players[i].loadoutNumber),
                      unsigned(players[i].loadoutNumberBody), unsigned(team), recorded,
                      20u - recorded,
                      r.wrote.loadout ? "" : " (loadout step did not write)",
                      r.adopted ? " [adopted: re-bound to this session's existing entrant]" : "");
            ++out.dressed;
        }
        else    ++out.failed;
        const IdentityFields& w = r.wrote;
        const bool wroteAny = w.profile || w.slotName || w.team || w.loadout || w.alignment;
        if (wroteAny || r.slotResolved != kNotDressed) {
            // A slot that was written into now carries THIS replay's identity, so it joins the
            // residue -- which is what makes the NEXT load's obligation reach it.  A slot that only
            // RESOLVED is recorded too, without the obligation: Unload releases the entrant by the
            // engine slot this is the session's only record of.
            if (wroteAny && players[i].userSlot < RosterMapping::kSlots)
                redressed[players[i].userSlot] = true;
            map.NoteDressed(players[i].userSlot, r.slotResolved, r.userIndex, w);
        }
        if (!r.indexResolved) continue;
        if (!map.Set(players[i].userSlot, UserIndex(r.userIndex))) {
            // Only reachable for a slot past the engine's own table, which step 1 refuses --
            // so if this ever prints, the two bounds have drifted apart and that is the news.
            log.Write(LogLevel::Error, "identity",
                      "player '%s' resolved to index %u but slot %u is outside the user-slot table; "
                      "its body cannot be published",
                      players[i].displayName, unsigned(r.userIndex), unsigned(players[i].userSlot));
        }
    }
    // THE VACATE OBLIGATION.  Every slot in `residue u this roster` now reaches a defined state:
    // the ones this manifest NAMES are its own, and the ones it does not name are undressed here.  A
    // slot left alone would play the new replay wearing the old one's name, loadout and colours --
    // and ReleaseReplayUser cannot take it back on this build, so LOAD is where it gets taken back.
    //
    // It belongs to the LOAD, and the load's first pass discharged it: every slot it vacated was
    // dropped from the residue there.  A retry is about this manifest's own players, so it neither
    // re-walks the obligation nor re-prints its outcome.
    for (uint16_t slot = 0; first && slot < RosterMapping::kSlots; ++slot) {
        if (!prior[slot].used) continue;
        if (claimed[slot]) {
            if (!redressed[slot]) {
                // Owned by the loaded manifest, so it is NOT vacated -- and not clean either: it
                // still wears whatever an earlier pass put there.  Its own number and its own line,
                // because "not vacated because the replay owns it" and "not vacated because the
                // write refused" are two different reports.
                ++out.dressRefused;
                log.Write(LogLevel::Error, "identity",
                          "residue slot %u is named by THIS manifest but its dress wrote nothing on "
                          "this pass (dress_refused) -- it is NOT vacated, because the loaded replay "
                          "owns the slot, and it still carries what an earlier pass wrote",
                          unsigned(slot));
            }
            continue;
        }
        switch (VacateSlot(id, log, slot, prior[slot])) {
            case VacateOutcome::Vacated:   ++out.vacated;   map.DropResidue(slot); break;
            case VacateOutcome::StaleSlot: ++out.staleSlot; map.DropResidue(slot); break;
            // KEPT: the slot still carries the previous identity, so the next load owes it the same
            // obligation.  Dropping it here would turn one refusal into permanent silence.
            case VacateOutcome::Refused:   ++out.vacateRefused; break;
        }
    }

    const bool complete = out.dressed == out.players;
    if (!news) return complete;
    log.Write(complete ? LogLevel::Info : LogLevel::Error, "identity",
              // The deferred count always prints, including zero: "0 deferred" after a failure is
              // a decision the engine made, and a missing number is not the same answer.
              "roster: %u/%u players dressed, %u NOT dressed, %u deferred (retrying), "
              "%u already dressed on an earlier pass of this load, %u spectator row(s) skipped",
              out.dressed, out.players, out.failed, out.deferred, out.alreadyApplied,
              out.spectators);
    // Named, together, in one line: a reader who sees "3 NOT dressed" and no deferrals has to be
    // able to read WHICH three and WHY without correlating five per-player lines by hand.
    for (uint32_t i = 0; i < out.stuck; ++i) {
        log.Write(LogLevel::Error, "identity",
                  "roster: slot %u stopped waiting and is NOT dressed: %s",
                  unsigned(out.stuckSlots[i].userSlot),
                  identity_access::RefusalText(out.stuckSlots[i].why));
    }
    // Separate from the line above on purpose.  Dressing is about cosmetics and this is about
    // ownership: a roster can be 0/2 dressed and still 2/2 mapped, and in that state every body
    // publishes wearing the wrong skin -- which is a completely different bug report from a world
    // that does not move.
    log.Write(map.Mapped() == out.players ? LogLevel::Info : LogLevel::Error, "identity",
              "roster: %u/%u players mapped to a live user index", map.Mapped(), out.players);
    // A third line, and a third subject: not what this replay wears, but what the LAST one left
    // behind.  It prints whenever there was a residue at all, including the all-clean case, because
    // "0 vacated" after a second load means something different from the line never appearing.
    if (out.residue || out.adopted) {
        log.Write((out.vacateRefused || out.dressRefused) ? LogLevel::Error : LogLevel::Info,
                  "identity",
                  "roster: %u residue slot(s) from an earlier load, %u vacated, %u stale-slot, "
                  "%u vacate-refused, %u dress-refused, %u adopted (re-bound, not re-admitted)",
                  out.residue, out.vacated, out.staleSlot, out.vacateRefused, out.dressRefused,
                  out.adopted);
    }
    return complete;
}

namespace {

// One pass per frame per slot, and eight of them.  The leave is fed from the stage-3 tail, so eight
// passes is eight engine updates -- long enough to ride out a re-entrant frame or a phase the
// handler will not act in, short enough that a slot the engine genuinely will not free is reported
// inside a tenth of a second rather than holding the next load's dress pass off the roster.
constexpr uint32_t kLeaveAttempts = 8;

struct LeaveFeed {
    bool     live[RosterMapping::kSlots] = {};
    uint16_t captureSlot[RosterMapping::kSlots] = {};
    uint32_t tries[RosterMapping::kSlots] = {};
    LeaveReport totals{};
};
LeaveFeed g_leave;
bool g_leaveOnUnload = true;
bool g_leaveReverse = false;
bool g_leaveOnePerPass = false;

}  // namespace

void SetLeaveOnUnload(bool on) { g_leaveOnUnload = on; }
bool LeaveOnUnload() { return g_leaveOnUnload; }

void SetLeaveOrder(bool reverse, bool onePerPass) {
    g_leaveReverse = reverse;
    g_leaveOnePerPass = onePerPass;
}
bool LeaveReverseOrder() { return g_leaveReverse; }
bool LeaveOnePerPass() { return g_leaveOnePerPass; }

void RequestRosterLeave(const RosterMapping& map, LeaveReport& out) {
    out = LeaveReport{};
    for (size_t slot = 0; slot < RosterMapping::kSlots; ++slot) {
        // The mapping is keyed on the CAPTURE slot; the engine holds the entrant in the slot the
        // dress resolved.  Handing back the recorded number would hand back somebody else.
        const uint16_t engineSlot = map.ResidueAt(slot).slotResolved;
        if (engineSlot == kNotDressed || engineSlot >= RosterMapping::kSlots) continue;
        // Keyed on the ENGINE slot, so two capture slots that resolved to one entrant -- which
        // adoption can produce across loads -- queue one leave and not two.  A second removal for a
        // slot the first one freed would land on whatever the engine seats there next.
        if (!g_leave.live[engineSlot]) ++out.queued;
        g_leave.live[engineSlot] = true;
        g_leave.captureSlot[engineSlot] = uint16_t(slot);
        g_leave.tries[engineSlot] = 0;
    }
    g_leave.totals = LeaveReport{};
    g_leave.totals.queued = out.queued;
}

bool RosterLeavePending() {
    for (size_t i = 0; i < RosterMapping::kSlots; ++i) if (g_leave.live[i]) return true;
    return false;
}

void ForgetRosterLeave() { g_leave = LeaveFeed{}; }

const LeaveReport& RosterLeaveTotals() { return g_leave.totals; }

void ServiceRosterLeave(IIdentity& id, ILog& log, RosterMapping& map) {
    bool leftOne = false;
    for (uint16_t step = 0; step < RosterMapping::kSlots; ++step) {
        // The order is the A/B, and only the order: the same slots, the same call, walked from the
        // top down instead of the bottom up when the owner asks for it.
        const uint16_t engineSlot =
            g_leaveReverse ? uint16_t(RosterMapping::kSlots - 1 - step) : step;
        if (!g_leave.live[engineSlot]) continue;
        if (leftOne) break;   // one per pass: the rest go on the next engine update
        ++g_leave.tries[engineSlot];
        if (id.ReleaseReplayUser(UserSlot(engineSlot))) {
            g_leave.live[engineSlot] = false;
            leftOne = g_leaveOnePerPass;
            ++g_leave.totals.left;
            // The residue goes with the entrant: the slot is free, so the next load admits a fresh
            // one for it rather than adopting a record of something that is no longer there.
            map.DropResidue(g_leave.captureSlot[engineSlot]);
            log.Write(LogLevel::Info, "identity",
                      "LEFT: engine slot %u (capture slot %u) -- the entrant this session admitted "
                      "was removed through the engine's own removing-entrant path and the slot is "
                      "free",
                      unsigned(engineSlot), unsigned(g_leave.captureSlot[engineSlot]));
            continue;
        }
        const identity_access::Refusal why = identity_access::LastRefusal(&id);
        if (g_leave.tries[engineSlot] < kLeaveAttempts) continue;
        // ★ LOUD, NAMED, AND THE RESIDUE IS KEPT.  A slot the engine would not free still carries
        // this replay's identity, so the next load must still adopt it -- which is what keeps a
        // refused leave from turning back into the duplicated roster it replaced.
        g_leave.live[engineSlot] = false;
        ++g_leave.totals.refused;
        log.Write(LogLevel::Error, "identity",
                  "engine slot %u (capture slot %u) did NOT leave after %u passes: %s. It still "
                  "carries this replay's identity; the next load will re-bind to it instead of "
                  "admitting a second entrant for the same player",
                  unsigned(engineSlot), unsigned(g_leave.captureSlot[engineSlot]),
                  unsigned(kLeaveAttempts), identity_access::RefusalText(why));
    }
    if (!RosterLeavePending() && g_leave.totals.queued != 0) {
        log.Write(g_leave.totals.refused ? LogLevel::Error : LogLevel::Info, "identity",
                  "roster: %u/%u replay entrant(s) left the engine, %u refused",
                  g_leave.totals.left, g_leave.totals.queued, g_leave.totals.refused);
        g_leave.totals.queued = 0;   // reported once; the counts stay readable until the next unload
    }
}

void ReleaseRoster(IIdentity& /*id*/, ILog& log, RosterMapping& map, ReleaseReport& out) {
    out = ReleaseReport{};
    out.mapped = map.Mapped();
    // ★ QUEUED, NOT CALLED.  The engine's removing-entrant handler has to be driven from the engine
    // thread with a netGame in hand and this call has neither, so what leaves here is a list.  See
    // the leave-feed comment in dresser.h.
    LeaveReport leave;
    if (g_leaveOnUnload) {
        RequestRosterLeave(map, leave);
    } else {
        // The switch exists so a build whose leave misbehaves in the engine can still be shipped:
        // with no queue the residue stands and the next load ADOPTS it, which is what every build
        // before the leave path did.
        ForgetRosterLeave();
    }
    // Cleared whatever the engine says later.  Holding indices from a session we have finished with
    // is how a second load inherits the first one's mapping and publishes onto the wrong entrants;
    // an unreleased engine record is a leak, a stale mapping is a wrong replay.  The RESIDUE is not
    // cleared with it: it is what the leave feed drops per slot as each entrant actually goes, and
    // what the next load adopts for any slot that refused.
    map.Clear();
    if (out.mapped || leave.queued) {
        if (g_leaveOnUnload) {
            log.Write(LogLevel::Info, "identity",
                      "roster: %u replay entrant(s) asked to leave the engine (%u slot(s) were "
                      "mapped); the removals are fed from the engine's own update and reported as "
                      "they land",
                      leave.queued, out.mapped);
        } else {
            log.Write(LogLevel::Warn, "identity",
                      "roster: playback.leave_on_unload is off, so the %u mapped replay entrant(s) "
                      "stay seated; the next load re-binds to them instead of admitting fresh ones",
                      out.mapped);
        }
    }

    out.residue = map.ResidueCount();
}

}  // namespace identity
}  // namespace replay
