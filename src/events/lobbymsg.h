#pragma once

// W3-E. Decoders for the lobby and identity messages S5.4.1 names, over bodies W2-A already stores
// verbatim on S_EVENT.
//
// ---------------------------------------------------------------------------------------------
// ★ THIS PACKET ADDS A READING, NOT A CAPTURE PATH. Every byte these functions look at is already in
// the file. S13.6.1's closing argument is that "what is missing is a DECODER, not data" -- so this
// header is deliberately a set of pure functions over (pointer, length) with no engine access, no
// global state and no allocation. They run identically inside the capture worker and inside a tool
// pointed at a .echodemo recorded months ago.
//
// ★ EVERY DECODER REFUSES RATHER THAN GUESSES. A body whose length does not fit the framing returns
// false and fills nothing. There is no partial result, because a partially-filled identity is exactly
// the failure S5.4.1a describes: a field that looks valid and is not. A caller that gets false has an
// undecoded body -- which is a state the format already represents, since the bytes are still there.
//
// ★ AND NO DECODER OWNS ITS INPUT. Every string and blob below borrows the body. Copy out what has to
// outlive it; the tracker in lobbytrack.h does exactly that and says so.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

namespace replay {
namespace events {

// A borrowed run of bytes. Not demofile's Blob: that one is part of the frozen manifest contract and
// this one is a decode result with a different lifetime, so they are kept apart on purpose.
struct ByteView {
    const uint8_t* p;
    uint32_t n;
};

// A borrowed NUL-padded fixed-width field, already trimmed. `n` is the length up to the first NUL, so
// an empty name is n == 0 -- which is a real state on the wire (the host forwards a nameless
// placeholder before the entrant's own push arrives) and must not read as "we failed to decode".
struct StrView {
    const char* p;
    uint32_t n;
};

// ---- SNSLobbyEntrantStatev2 -------------------------------------------------------------------
struct EntrantState {
    uint64_t platformCode;   // XPlatformId lo: 1 STM, 2 PSN, 3 XBX, 4 OVR-ORG, 5 OVR, 6 BOT, 7 DMO, 8 TEN
    uint64_t accountId;      // XPlatformId hi
    uint64_t sessionId;      // body+16; carried, never interpreted
    StrView platformStr;     // "DMO|0123456789abcdef" and the like
    StrView displayName;
    uint16_t marker;
    uint16_t entrantIndex;   // the GLOBAL lobby slot; re-stamped by the host on every forward
    uint16_t generation;
    uint16_t team;           // 0xFFFF is NO TEAM, and it is a value rather than an error
    ByteView profileJson;    // body+104 up to the NUL, empty on a keepalive push
    bool hasProfileJson;
};

// False unless the body carries the whole 104-byte fixed header. A shorter body is a message this
// decoder does not understand, not an entrant with default fields.
bool DecodeEntrantState(const void* body, uint32_t bytes, EntrantState* out);

// ---- SNSLobbyEntrantRemoved -------------------------------------------------------------------
// Refuses below 16 bytes, because the client's own ListenProxy refuses below 16 bytes: accepting an
// 8-byte body here would decode something the engine itself never delivered to its callback.
bool DecodeEntrantRemoved(const void* body, uint32_t bytes, uint64_t* slotOut);

// ---- SNSLobbyEntrantTeamChanged ---------------------------------------------------------------
struct EntrantTeamChanged {
    uint64_t entrantIndex;
    uint16_t team;
};
bool DecodeEntrantTeamChanged(const void* body, uint32_t bytes, EntrantTeamChanged* out);

// ---- SNSLobbyOwnerState -----------------------------------------------------------------------
// The JSON only. Its FIELDS (gametype, starttime, level, team[], user[]) are left to the caller: this
// packet does not ship a JSON parser and does not need one for markers, and the manifest already
// stores the message whole. Returns the [ '{' .. NUL ) span.
bool DecodeJsonBody(const void* body, uint32_t bytes, ByteView* out);

// ---- SR15NetIdsResponse -----------------------------------------------------------------------
// ★★ TWO SPACES, AND THE SLOT IS ONLY MEANINGFUL INSIDE ONE. The 8-byte header is the id-space key --
// proved by the SR15NetIdsRequest whose entire body is that key, and by the key's low u32 appearing at
// body+4 of every SimStateEvent in the matching direction. A decoder that dropped the header would
// hand back two slot ranges with no way to tell which space a slot came from.
//
// ★ AND THE ID COLUMN IS NOT AN ACTOR IDENTITY. In the client space its 27 entries hold four distinct
// values (runs of 10 and 15); in the server space its 21 entries are all distinct. That asymmetry is
// why this struct exposes the column as `ids` and never as anything named for an actor -- and why no
// consumer may key on it. Across the whole corpus not one of these values appears anywhere in a
// SimStateEvent, PlayerStateMsg or ComponentEvent body, at any byte offset. See MESSAGE_COVERAGE.md.
struct NetIdSpace {
    uint64_t space;
    uint16_t count;
    const uint8_t* ids;    // count little-endian u64, unaligned
    const uint8_t* slots;  // count little-endian u16, unaligned
};
bool DecodeNetIds(const void* body, uint32_t bytes, NetIdSpace* out);
uint64_t NetIdAt(const NetIdSpace&, uint16_t i);
uint16_t NetSlotAt(const NetIdSpace&, uint16_t i);

// ---- SR15NetReliableStatUpdate ----------------------------------------------------------------
struct StatUpdate {
    uint32_t entrantIndex;
    ByteView json;
};
bool DecodeStatUpdate(const void* body, uint32_t bytes, StatUpdate* out);

// ---- SR15NetReliableTeamStatUpdate ------------------------------------------------------------
// The row's trailing 12 bytes are handed back undivided. Every one of the 456 rows in the corpus has
// them all zero, so nothing can discriminate a {u64 value, u32 flags} split from any other; naming
// fields there would be a guess wearing a struct.
struct TeamStatUpdate {
    uint8_t team;
    uint16_t rowCount;
    const uint8_t* rows;  // rowCount * 20 bytes
};
bool DecodeTeamStatUpdate(const void* body, uint32_t bytes, TeamStatUpdate* out);
uint64_t TeamStatNameAt(const TeamStatUpdate&, uint16_t i);

// ---- SNSRefreshProfileFromServer --------------------------------------------------------------
struct ProfileFromServer {
    uint64_t platformCode;
    uint64_t accountId;
    ByteView json;
};
bool DecodeProfileFromServer(const void* body, uint32_t bytes, ProfileFromServer* out);

}  // namespace events
}  // namespace replay
