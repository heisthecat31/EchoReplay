#pragma once

// W3-E. Where every offset this packet decodes comes from, in one place.
//
// Mirrors the W3-E fact drop and the W3-E anchor drop, which W0-2 merges. Once merged,
// configure with -DEVENTS_FACTS_MERGED and every value below becomes an assertion against the
// generated registry instead of a second copy of it (the pattern wiretap_facts.h established).
//
// ---------------------------------------------------------------------------------------------
// PROVENANCE, and it is two independent sources per row rather than one.
//
//   [wire]  the freshcap corpus (a private capture, arena.bin) -- a real retail
//           Echo Arena host + client, 34,791 datagrams, decoded with 0 MAC failures. Message bodies
//           read out byte for byte; the fixtures in tests/events/fixtures.h are those exact bytes.
//   [lib]   libr15.so, which retains symbols, read through echo-gs's recorded RE of the client-side
//           callbacks. Named by function and address in each row below.
//
// A row backed by only ONE of the two is marked so. Nothing here is transcribed from prose.
//
// ★ NONE OF THESE ARE ENGINE ADDRESSES. Every value is an offset INTO A MESSAGE BODY the wiretap
// hands us -- a buffer in our own address space with an explicit length. There is no rebase to get
// wrong and no image to read; the DYNAMICBASE hazard that has bitten four packets cannot reach this
// file, and no buildguard::VerifySite call belongs here. The two lobby-object offsets at the bottom
// are the exception and they are NOT dereferenced by this packet -- they are recorded because a
// second source now corroborates W0-2's PC_CANDIDATE rows.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

namespace replay::events_facts {

// ---- SNSLobbyEntrantStatev2 (0xEB6218834B3FA648) --------------------------------------------
// [wire] 4 bodies, 360/364/368/400 B.  [lib] CNSLobby::EntrantStateCB @26361920.
//
// The fixed header is 104 bytes and the profile JSON follows it. EntrantStateCB reads the display
// name as a 36-byte NUL-padded CSysString (`CSysString::Compare(slot+60, body+60, .., 0x24u)`), which
// is what fixes both 36-byte string fields: the platform string ends exactly where the name begins.
inline constexpr uint32_t kEntrantPlatformCodeOff = 0;    // u64, XPlatformId lo (DMO=7)
inline constexpr uint32_t kEntrantAccountIdOff    = 8;    // u64, XPlatformId hi
inline constexpr uint32_t kEntrantSessionIdOff    = 16;   // u64, [wire] only; meaning unestablished
inline constexpr uint32_t kEntrantPlatformStrOff  = 24;   // char[36], NUL-padded "DMO|<hex account>"
inline constexpr uint32_t kEntrantDisplayNameOff  = 60;   // char[36], NUL-padded
inline constexpr uint32_t kEntrantStrLen          = 36;   // 0x24, the CSysString::Compare length
inline constexpr uint32_t kEntrantMarkerOff       = 96;   // u16; low 2 bits drive the slot +136 flags
inline constexpr uint32_t kEntrantIndexOff        = 98;   // u16 GLOBAL entrant index, re-stamped
inline constexpr uint32_t kEntrantGenerationOff   = 100;  // u16 genidx; must match the slot's +140
inline constexpr uint32_t kEntrantTeamOff         = 102;  // u16 team; 0xFFFF is NO TEAM
inline constexpr uint32_t kEntrantHeaderLen       = 104;  // the JSON profile starts here

// ★ 0xFFFF is a real value on the wire, not an error code: the first two entrant bodies in the corpus
// carry it and the two after team assignment carry 1. A uint8_t destination truncates it to 0xFF,
// which is neither the sentinel nor a valid team -- the same defect S5.4.1a found in PlayerRecord.
inline constexpr uint16_t kTeamNone = 0xFFFFu;

// ---- SNSLobbyEntrantRemoved (0x39156563932117D6) --------------------------------------------
// [lib] EntrantRemovedCB @0x1927C38 reads the slot as **(u64**)(a2+16) and resets lobby+864+216*slot.
// [lib] CBroadcaster::ListenProxy<CNSLobby,SNSLobbyEntrantRemoved> @26442044 SILENTLY DROPS a body
//       shorter than 16 bytes -- so the wire body is [u64 slot][u64 pad] and a decoder that accepted
//       8 bytes would accept something the client itself discards.
// NOT in the freshcap corpus: nobody left that match. [lib] only, and MESSAGE_COVERAGE.md says so.
inline constexpr uint32_t kRemovedSlotOff = 0;   // u64
inline constexpr uint32_t kRemovedLen     = 16;

// ---- SNSLobbyEntrantTeamChanged (0xCEB8759C36184B82) ----------------------------------------
// [wire] one 16-byte body, pkt #15: 00*8 | 01 00 | ff ff | 00*4 -- entrant 0 to team 1, and the
//        SNSLobbyOwnerState two packets later independently says "user":[{"team":1,"teamslot":0}].
// [lib] EntrantTeamChangedCB reads **(a2+16) = entrant index (u64) and *(*(a2+16)+8) = team (u16),
//       then writes lobby+864+216*entrant+142.
inline constexpr uint32_t kTeamChangedIndexOff = 0;   // u64
inline constexpr uint32_t kTeamChangedTeamOff  = 8;   // u16
inline constexpr uint32_t kTeamChangedLen      = 16;

// ---- SNSLobbyOwnerState (0x2068C0F03B4B8F25) ------------------------------------------------
// [wire] two bodies, 233 and 241 B. Both are [u8 tag][UTF-8 JSON][NUL][trailer].
// [lib] the JSON is produced by CJson::EncodeToBroadcast, which is also what SNSRefreshProfileFromServer
//       uses -- the same one-byte-then-text-then-NUL shape.
//
// ★ The tag byte is NOT decoded. The two corpus bodies carry 0x25 and 0x25; a third instance embedded
// in a batch event carries 0xC0, and echo-gs's host sends 0x00 and the retail client accepts it. Three
// values, no reading -- so it is skipped, not interpreted, and the JSON is located by scanning to the
// first '{' within the tag window rather than by asserting a value nothing establishes.
inline constexpr uint32_t kJsonTagLen = 1;

// ---- SR15NetIdsResponse (0x8F8AF081AF21CBE7) ------------------------------------------------
// [wire] two bodies, 218 B and 278 B, both exactly 8 + n*10 with n = 21 and 27.
//
// ★★ THE HEADER IS AN ID-SPACE KEY, NOT PADDING, and that is what this packet adds. The 8 bytes are
// echoed verbatim as the whole body of the SR15NetIdsRequest that asked for the message (2 requests,
// 8 B each, byte-identical to the two responses' headers), and the LOW u32 of each key appears at
// body+4 of every SR15NetPhysicsSimStateEvent travelling in the matching direction. So there are TWO
// id spaces and a slot number is only meaningful inside one of them.
inline constexpr uint32_t kNetIdsSpaceOff  = 0;   // u64 id-space key
inline constexpr uint32_t kNetIdsHeaderLen = 8;
inline constexpr uint32_t kNetIdsEntryLen  = 10;  // one u64 in the id column + one u16 in the slot column

// ---- SR15NetReliableStatUpdate (0x5D84B0C2D4BB6CFC) -----------------------------------------
// [wire] 19 bodies, all 7 B: 00 00 00 00 7b 7d 00 = [u32 entrant][JSON "{}"][NUL]. The framing is
// closed; the CONTENT is empty because the corpus is a solo match, which is exactly S13.6.1's U6.
inline constexpr uint32_t kStatEntrantOff = 0;   // u32
inline constexpr uint32_t kStatJsonOff    = 4;

// ---- SR15NetReliableTeamStatUpdate (0x7BA444F7D419B1A2) -------------------------------------
// [wire] 38 bodies, every one 242 B = 2 + 12*20, emitted in team-0/team-1 pairs.
// The 20-byte row opens with a CSymbol64 stat name; the remaining 12 bytes are ALL ZERO in every one
// of the 456 rows in the corpus, so this packet stores the row and does not split it. A split nothing
// can discriminate is a guess with a struct around it.
inline constexpr uint32_t kTeamStatTeamOff   = 0;   // u8
inline constexpr uint32_t kTeamStatHeaderLen = 2;
inline constexpr uint32_t kTeamStatRowLen    = 20;  // u64 CSymbol64 name + 12 undecoded bytes

// ---- SNSRefreshProfileFromServer (0x06877B856E8D785F) ---------------------------------------
// [lib] SNSRefreshProfileFromServer::Send @19346932 writes the 16-byte SNSUserID then
//       CJson::EncodeToBroadcast(profile). RefreshProfileFromServerCB @19280556 compares that id
//       against the recipient's own and DROPS a profile belonging to anyone else.
// NOT in the freshcap corpus -- see MESSAGE_COVERAGE.md for the scope line, which matters: the corpus
// tees one UDP socket, and absence there is not absence from the broadcaster.
inline constexpr uint32_t kProfilePlatformCodeOff = 0;   // u64
inline constexpr uint32_t kProfileAccountIdOff    = 8;   // u64
inline constexpr uint32_t kProfileJsonOff         = 16;

// ---- the lobby member table ------------------------------------------------------------------
// NOT DEREFERENCED BY THIS PACKET. Recorded because the W0-2_lobby fact drop carries
// LOBBY.MEMBER_ARRAY_OFF 0x360 and LOBBY.MEMBER_STRIDE 0xD8 as PC_CANDIDATE with the note that no site
// in echovr.exe has been attributed to them -- and [lib] attributes both: EntrantRemovedCB @0x1927C38
// resets `lobby+864 + 216*slot` and EntrantTeam @26375476 reads `slot+142`. 864 == 0x360 and
// 216 == 0xD8. That is a second source for two rows that had none, and the per-slot field offsets
// below come with it.
inline constexpr uint32_t kLobbyMemberArrayOff = 864;   // 0x360
inline constexpr uint32_t kLobbyMemberStride   = 216;   // 0xD8
inline constexpr uint32_t kLobbySlotNameOff    = 60;    // char[36], the stored display name
inline constexpr uint32_t kLobbySlotFlagsOff   = 136;   // u16, fed from the body+96 marker's low bits
inline constexpr uint32_t kLobbySlotGenOff     = 140;   // u16, compared against body+100
inline constexpr uint32_t kLobbySlotTeamOff    = 142;   // u16, written by EntrantTeamChangedCB

}  // namespace replay::events_facts

#ifdef EVENTS_FACTS_MERGED
#include "facts_generated.h"
// Only the ids this packet actually files in the W3-E fact drop are asserted here. An assertion
// against an id nobody contributes is not a check, it is a build break waiting for whoever notices.
// The two SNSLobbyEntrantRemoved constants and the two lobby-object offsets are absent on purpose --
// that file's header says why for each.
namespace replay::events_facts {
static_assert(kEntrantPlatformCodeOff == facts::LOBBY_ENTRANT_PLATFORM_OFF);
static_assert(kEntrantAccountIdOff == facts::LOBBY_ENTRANT_ACCOUNT_OFF);
static_assert(kEntrantPlatformStrOff == facts::LOBBY_ENTRANT_PLATFORMSTR_OFF);
static_assert(kEntrantDisplayNameOff == facts::LOBBY_ENTRANT_NAME_OFF);
static_assert(kEntrantStrLen == facts::LOBBY_ENTRANT_STR_LEN);
static_assert(kEntrantMarkerOff == facts::LOBBY_ENTRANT_MARKER_OFF);
static_assert(kEntrantIndexOff == facts::LOBBY_ENTRANT_INDEX_OFF);
static_assert(kEntrantGenerationOff == facts::LOBBY_ENTRANT_GEN_OFF);
static_assert(kEntrantTeamOff == facts::LOBBY_ENTRANT_TEAM_OFF);
static_assert(kEntrantHeaderLen == facts::LOBBY_ENTRANT_HEADER_LEN);
static_assert(kTeamNone == facts::LOBBY_TEAM_NONE);
static_assert(kTeamChangedIndexOff == facts::LOBBY_TEAMCHANGED_INDEX_OFF);
static_assert(kTeamChangedTeamOff == facts::LOBBY_TEAMCHANGED_TEAM_OFF);
static_assert(kTeamChangedLen == facts::LOBBY_TEAMCHANGED_LEN);
static_assert(kNetIdsHeaderLen == facts::NETIDS_HEADER_LEN);
static_assert(kNetIdsEntryLen == facts::NETIDS_ENTRY_LEN);
static_assert(kStatJsonOff == facts::STAT_RELIABLE_JSON_OFF);
static_assert(kTeamStatHeaderLen == facts::STAT_TEAM_HEADER_LEN);
static_assert(kTeamStatRowLen == facts::STAT_TEAM_ROW_LEN);
}  // namespace replay::events_facts
#endif
