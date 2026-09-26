#include "events/lobbymsg.h"

#include <string.h>

#include "events/events_facts.h"

namespace replay {
namespace events {
namespace {

using namespace replay::events_facts;

// Every read goes through these. The bodies arrive from the wire with no alignment guarantee, and a
// message one byte shorter than its framing is the ordinary case rather than the exotic one -- the
// engine's own ListenProxy drops undersized bodies, so we will see them.
uint64_t U64(const uint8_t* p, uint32_t off) {
    uint64_t v;
    memcpy(&v, p + off, sizeof v);
    return v;
}
uint32_t U32(const uint8_t* p, uint32_t off) {
    uint32_t v;
    memcpy(&v, p + off, sizeof v);
    return v;
}
uint16_t U16(const uint8_t* p, uint32_t off) {
    uint16_t v;
    memcpy(&v, p + off, sizeof v);
    return v;
}

StrView Fixed(const uint8_t* p, uint32_t off, uint32_t cap) {
    const char* s = reinterpret_cast<const char*>(p + off);
    uint32_t n = 0;
    while (n < cap && s[n] != '\0') {
        ++n;
    }
    return StrView{s, n};
}

// The JSON bodies are [tag byte][UTF-8 JSON][NUL]. The tag is skipped rather than checked: the corpus
// carries 0x25 on both owner states, 0xC0 on the copy embedded in a batch event, and a working host
// sends 0x00 -- three values and no reading, so asserting one would fail on real traffic.
//
// The open brace is located within a two-byte window rather than assumed at [1], so a body that turns
// out to carry a two-byte tag still decodes and a body carrying no JSON at all is refused instead of
// yielding a span that starts inside binary.
bool JsonSpan(const uint8_t* p, uint32_t bytes, uint32_t from, ByteView* out) {
    for (uint32_t at = from; at < from + 2 && at < bytes; ++at) {
        if (p[at] != '{' && p[at] != '[') {
            continue;
        }
        for (uint32_t end = at; end < bytes; ++end) {
            if (p[end] == '\0') {
                *out = ByteView{p + at, end - at};
                return true;
            }
        }
        return false;  // unterminated: refuse, rather than hand back a run into whatever follows
    }
    return false;
}

}  // namespace

bool DecodeEntrantState(const void* body, uint32_t bytes, EntrantState* out) {
    if (body == nullptr || out == nullptr || bytes < kEntrantHeaderLen) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    EntrantState e{};
    e.platformCode = U64(p, kEntrantPlatformCodeOff);
    e.accountId = U64(p, kEntrantAccountIdOff);
    e.sessionId = U64(p, kEntrantSessionIdOff);
    e.platformStr = Fixed(p, kEntrantPlatformStrOff, kEntrantStrLen);
    e.displayName = Fixed(p, kEntrantDisplayNameOff, kEntrantStrLen);
    e.marker = U16(p, kEntrantMarkerOff);
    e.entrantIndex = U16(p, kEntrantIndexOff);
    e.generation = U16(p, kEntrantGenerationOff);
    e.team = U16(p, kEntrantTeamOff);
    e.hasProfileJson = JsonSpan(p, bytes, kEntrantHeaderLen, &e.profileJson);
    *out = e;
    return true;
}

bool DecodeEntrantRemoved(const void* body, uint32_t bytes, uint64_t* slotOut) {
    if (body == nullptr || slotOut == nullptr || bytes < kRemovedLen) {
        return false;
    }
    *slotOut = U64(static_cast<const uint8_t*>(body), kRemovedSlotOff);
    return true;
}

bool DecodeEntrantTeamChanged(const void* body, uint32_t bytes, EntrantTeamChanged* out) {
    if (body == nullptr || out == nullptr || bytes < kTeamChangedLen) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    out->entrantIndex = U64(p, kTeamChangedIndexOff);
    out->team = U16(p, kTeamChangedTeamOff);
    return true;
}

bool DecodeJsonBody(const void* body, uint32_t bytes, ByteView* out) {
    if (body == nullptr || out == nullptr) {
        return false;
    }
    return JsonSpan(static_cast<const uint8_t*>(body), bytes, kJsonTagLen - 1, out);
}

bool DecodeNetIds(const void* body, uint32_t bytes, NetIdSpace* out) {
    if (body == nullptr || out == nullptr || bytes < kNetIdsHeaderLen) {
        return false;
    }
    const uint32_t payload = bytes - kNetIdsHeaderLen;
    if (payload % kNetIdsEntryLen != 0) {
        return false;
    }
    const uint32_t n = payload / kNetIdsEntryLen;
    if (n > 0xFFFFu) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    out->space = U64(p, kNetIdsSpaceOff);
    out->count = static_cast<uint16_t>(n);
    out->ids = p + kNetIdsHeaderLen;
    out->slots = p + kNetIdsHeaderLen + n * 8u;
    return true;
}

uint64_t NetIdAt(const NetIdSpace& s, uint16_t i) { return U64(s.ids, i * 8u); }
uint16_t NetSlotAt(const NetIdSpace& s, uint16_t i) { return U16(s.slots, i * 2u); }

bool DecodeStatUpdate(const void* body, uint32_t bytes, StatUpdate* out) {
    if (body == nullptr || out == nullptr || bytes < kStatJsonOff) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    out->entrantIndex = U32(p, kStatEntrantOff);
    if (!JsonSpan(p, bytes, kStatJsonOff, &out->json)) {
        out->json = ByteView{nullptr, 0};
    }
    return true;
}

bool DecodeTeamStatUpdate(const void* body, uint32_t bytes, TeamStatUpdate* out) {
    if (body == nullptr || out == nullptr || bytes < kTeamStatHeaderLen) {
        return false;
    }
    const uint32_t payload = bytes - kTeamStatHeaderLen;
    if (payload % kTeamStatRowLen != 0) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    out->team = p[kTeamStatTeamOff];
    out->rowCount = static_cast<uint16_t>(payload / kTeamStatRowLen);
    out->rows = p + kTeamStatHeaderLen;
    return true;
}

uint64_t TeamStatNameAt(const TeamStatUpdate& t, uint16_t i) {
    return U64(t.rows, i * kTeamStatRowLen);
}

bool DecodeProfileFromServer(const void* body, uint32_t bytes, ProfileFromServer* out) {
    if (body == nullptr || out == nullptr || bytes < kProfileJsonOff) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    out->platformCode = U64(p, kProfilePlatformCodeOff);
    out->accountId = U64(p, kProfileAccountIdOff);
    if (!JsonSpan(p, bytes, kProfileJsonOff, &out->json)) {
        out->json = ByteView{nullptr, 0};
    }
    return true;
}

}  // namespace events
}  // namespace replay
