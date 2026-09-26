// W3-E's decode and marker checks.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT MAKES THESE CHECKS EVIDENCE RATHER THAN A ROUND TRIP.
//
// A decoder tested against its own output proves nothing: read a field at offset X, assert it equals
// what is at offset X. Every assertion below is therefore anchored to something the decoder did not
// produce:
//
//   1. THE PLATFORM STRING SPELLS THE ACCOUNT ID. The wire carries "DMO|0123456789abcdef" at body+24
//      and the u64 0x0123456789ABCDEF at body+8. Two fields, two offsets, one fact -- and this file
//      formats the integer and compares the text. Move either offset and they disagree.
//   2. TWO MESSAGES AGREE ON THE TEAM. SNSLobbyEntrantStatev2 body+102 says 1 on the later frames;
//      the SNSLobbyOwnerState JSON, a different message from a different sender, says
//      "user":[{"team":1,...}]. The check reads the number out of the JSON TEXT and compares.
//   3. THE ID-SPACE KEY IS ECHOED BY THE REQUEST. Not asserted here (the request bodies are not
//      fixtures) but recorded in MESSAGE_COVERAGE.md as the derivation.
//
// ★ AND EVERY CHECK IS SHOWN TO BE ABLE TO FAIL. The MUTATION cases below re-run the same comparisons
// against a deliberately shifted offset and assert they go red. A check nobody has watched fail is a
// check nobody has tested.
// ---------------------------------------------------------------------------------------------

#include <doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "events/events_facts.h"
#include "events/lobbymsg.h"
#include "capture/manifestbuild.h"
#include "events/lobbytrack.h"
#include "fixtures.h"
#include "foundation/demofile/demotypes.h"

using namespace replay::events;
namespace F = replay::events_facts;

namespace {

std::string Text(const StrView& s) { return std::string(s.p, s.n); }
std::string Text(const ByteView& b) {
    return std::string(reinterpret_cast<const char*>(b.p), b.n);
}

// "DMO|0123456789abcdef" -- the prefix is the XPlatformId platform code's name and the tail is the
// account id in lower-case hex, 16 digits, zero-padded.
std::string ExpectedPlatformString(uint64_t platformCode, uint64_t accountId) {
    const char* prefix = "???";
    switch (platformCode) {
        case 1: prefix = "STM"; break;
        case 2: prefix = "PSN"; break;
        case 3: prefix = "XBX"; break;
        case 4: prefix = "OVR-ORG"; break;
        case 5: prefix = "OVR"; break;
        case 6: prefix = "BOT"; break;
        case 7: prefix = "DMO"; break;
        case 8: prefix = "TEN"; break;
        default: break;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s|%016llx", prefix,
                  static_cast<unsigned long long>(accountId));
    return buf;
}

// The team the OwnerState JSON states, read out of the text. Returns 0xFFFF if the key is absent, so
// a parse that finds nothing cannot masquerade as team 0.
uint16_t TeamFromOwnerJson(const std::string& json) {
    const size_t user = json.find("\"user\"");
    if (user == std::string::npos) {
        return F::kTeamNone;
    }
    const size_t key = json.find("\"team\"", user);
    if (key == std::string::npos) {
        return F::kTeamNone;
    }
    const size_t colon = json.find(':', key);
    if (colon == std::string::npos) {
        return F::kTeamNone;
    }
    return static_cast<uint16_t>(std::strtoul(json.c_str() + colon + 1, nullptr, 10));
}

}  // namespace

TEST_CASE("SNSLobbyEntrantStatev2 decodes, and the body cross-checks itself") {
    struct Case { const uint8_t* body; uint32_t bytes; uint16_t team; bool json; };
    const Case cases[] = {
        {fixtures::kEntrantState0, sizeof fixtures::kEntrantState0, F::kTeamNone, true},
        {fixtures::kEntrantState1, sizeof fixtures::kEntrantState1, F::kTeamNone, true},
        {fixtures::kEntrantState2, sizeof fixtures::kEntrantState2, 1, true},
        {fixtures::kEntrantState3, sizeof fixtures::kEntrantState3, 1, true},
    };
    for (const Case& c : cases) {
        EntrantState e{};
        REQUIRE(DecodeEntrantState(c.body, c.bytes, &e));
        CHECK(e.platformCode == 7);
        CHECK(e.accountId == 0x0123456789ABCDEFull);
        // (1) the string spells the integer: two offsets, one fact.
        CHECK(Text(e.platformStr) == ExpectedPlatformString(e.platformCode, e.accountId));
        CHECK(Text(e.displayName) == "EchoDev [DEMO]");
        CHECK(e.entrantIndex == 0);
        CHECK(e.team == c.team);
        CHECK(e.hasProfileJson == c.json);
        CHECK(Text(e.profileJson).find("\"bodytype\"") != std::string::npos);
    }
}

TEST_CASE("0xFFFF is a value and not an error, and it does not fit a byte") {
    EntrantState before{}, after{};
    REQUIRE(DecodeEntrantState(fixtures::kEntrantState0, sizeof fixtures::kEntrantState0, &before));
    REQUIRE(DecodeEntrantState(fixtures::kEntrantState3, sizeof fixtures::kEntrantState3, &after));

    CHECK(before.team == F::kTeamNone);
    CHECK(after.team == 1);

    // (2) a second message, from the other side of the connection, states the same team.
    ByteView json{};
    REQUIRE(DecodeJsonBody(fixtures::kOwnerState0, sizeof fixtures::kOwnerState0, &json));
    CHECK(TeamFromOwnerJson(Text(json)) == after.team);

    // The narrowing S5.4.1a describes, demonstrated rather than asserted: the sentinel truncated to a
    // byte is neither the sentinel nor an invalid team, which is why it fails silently.
    const uint8_t narrowed = static_cast<uint8_t>(before.team);
    CHECK(narrowed == 0xFF);
    // Widened back, the byte is 0x00FF -- not the sentinel, and a perfectly ordinary-looking team id.
    // That is the whole defect: nothing downstream can tell it from a real answer.
    CHECK(static_cast<uint16_t>(narrowed) != F::kTeamNone);
    CHECK(static_cast<uint16_t>(narrowed) != before.team);
}

TEST_CASE("the roster's own string fields are at least as wide as the engine's") {
    static_assert(sizeof(EntrantRow::displayName) > F::kEntrantStrLen, "INV-W1: name field too narrow");
    static_assert(sizeof(EntrantRow::platformStr) > F::kEntrantStrLen, "INV-W1: platform field too narrow");

    // A 36-character name is legal on the wire and carries no terminator. Built here rather than taken
    // from the corpus, because the corpus name is short and a short name cannot show a truncation.
    uint8_t body[F::kEntrantHeaderLen] = {};
    const char* full = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";  // exactly 36
    REQUIRE(static_cast<uint32_t>(std::strlen(full)) == F::kEntrantStrLen);
    std::memcpy(body + F::kEntrantDisplayNameOff, full, F::kEntrantStrLen);

    EntrantState e{};
    REQUIRE(DecodeEntrantState(body, sizeof body, &e));
    CHECK(e.displayName.n == F::kEntrantStrLen);
    CHECK(Text(e.displayName) == full);

    // Reported, not asserted: Appendix B.0's PlayerRecord is the destination this value has to survive,
    // and it is four bytes short. Raised to the orchestrator; an inverted assertion here would go red
    // on the day the field is widened, which is the wrong direction for a check to point.
    std::printf("INV-W1 note: engine display name is %u B; PlayerRecord::displayName is %zu B\n",
                F::kEntrantStrLen, sizeof(replay::PlayerRecord::displayName));
}

TEST_CASE("every decoder refuses a body that does not fit its framing") {
    EntrantState e{};
    CHECK_FALSE(DecodeEntrantState(fixtures::kEntrantState0, F::kEntrantHeaderLen - 1, &e));
    CHECK(DecodeEntrantState(fixtures::kEntrantState0, F::kEntrantHeaderLen, &e));
    CHECK_FALSE(e.hasProfileJson);   // header only: absent, not defaulted

    uint64_t slot = 0;
    // The engine's own ListenProxy drops a body under 16 bytes, so accepting 8 here would decode
    // something the client never saw.
    CHECK_FALSE(DecodeEntrantRemoved(fixtures::kEntrantRemoved, 8, &slot));
    CHECK(DecodeEntrantRemoved(fixtures::kEntrantRemoved, 16, &slot));
    CHECK(slot == 1);

    EntrantTeamChanged t{};
    CHECK_FALSE(DecodeEntrantTeamChanged(fixtures::kTeamChanged0, 15, &t));
    REQUIRE(DecodeEntrantTeamChanged(fixtures::kTeamChanged0, 16, &t));
    CHECK(t.entrantIndex == 0);
    CHECK(t.team == 1);

    NetIdSpace s{};
    CHECK_FALSE(DecodeNetIds(fixtures::kNetIds0, sizeof fixtures::kNetIds0 - 1, &s));
    CHECK_FALSE(DecodeNetIds(fixtures::kNetIds0, 7, &s));
}

TEST_CASE("SR15NetIdsResponse carries TWO id spaces and the slot only means something inside one") {
    NetIdSpace a{}, b{};
    REQUIRE(DecodeNetIds(fixtures::kNetIds0, sizeof fixtures::kNetIds0, &a));
    REQUIRE(DecodeNetIds(fixtures::kNetIds1, sizeof fixtures::kNetIds1, &b));

    CHECK(a.space != b.space);
    CHECK(a.count == 21);
    CHECK(b.count == 27);
    CHECK(sizeof fixtures::kNetIds0 == F::kNetIdsHeaderLen + a.count * F::kNetIdsEntryLen);
    CHECK(sizeof fixtures::kNetIds1 == F::kNetIdsHeaderLen + b.count * F::kNetIdsEntryLen);

    // Contiguous, disjoint, and together covering 0..47 exactly once.
    bool covered[48] = {};
    for (uint16_t i = 0; i < a.count; ++i) {
        const uint16_t slot = NetSlotAt(a, i);
        REQUIRE(slot < 48);
        CHECK_FALSE(covered[slot]);
        covered[slot] = true;
    }
    for (uint16_t i = 0; i < b.count; ++i) {
        const uint16_t slot = NetSlotAt(b, i);
        REQUIRE(slot < 48);
        CHECK_FALSE(covered[slot]);
        covered[slot] = true;
    }
    for (bool c : covered) {
        CHECK(c);
    }
    CHECK(NetSlotAt(a, 0) == 27);
    CHECK(NetSlotAt(b, 0) == 0);

    // ★ THE ESTABLISHED NEGATIVE, KEPT AND MADE PRECISE. The u64 column is not a per-actor identity:
    // in one space its 27 entries hold four distinct values, in runs of ten and fifteen. In the OTHER
    // space all 21 are distinct -- which is why the negative has to be stated per space rather than
    // per message, and why nothing may key an actor on this column.
    auto distinct = [](const NetIdSpace& s) {
        uint16_t d = 0;
        for (uint16_t i = 0; i < s.count; ++i) {
            bool dup = false;
            for (uint16_t j = 0; j < i && !dup; ++j) {
                dup = NetIdAt(s, j) == NetIdAt(s, i);
            }
            if (!dup) ++d;
        }
        return d;
    };
    CHECK(distinct(a) == 21);
    CHECK(distinct(b) == 4);
}

TEST_CASE("SR15NetReliableStatUpdate and TeamStatUpdate frame exactly") {
    StatUpdate s{};
    REQUIRE(DecodeStatUpdate(fixtures::kStatUpdate0, sizeof fixtures::kStatUpdate0, &s));
    CHECK(s.entrantIndex == 0);
    // The layout is closed; the CONTENT is empty because the reference match is solo. That is S13.6.1's
    // U6, and an empty object is the honest observation rather than a decode failure.
    CHECK(Text(s.json) == "{}");

    TeamStatUpdate t0{}, t1{};
    REQUIRE(DecodeTeamStatUpdate(fixtures::kTeamStat0, sizeof fixtures::kTeamStat0, &t0));
    REQUIRE(DecodeTeamStatUpdate(fixtures::kTeamStat1, sizeof fixtures::kTeamStat1, &t1));
    CHECK(t0.team == 0);
    CHECK(t1.team == 1);
    CHECK(t0.rowCount == 12);
    CHECK(t1.rowCount == 12);
    CHECK(sizeof fixtures::kTeamStat0 == F::kTeamStatHeaderLen + t0.rowCount * F::kTeamStatRowLen);
    // Two bodies emitted as a pair carry the SAME stat names in the same order -- which is what makes
    // the first eight bytes of a row a name rather than a value.
    for (uint16_t i = 0; i < t0.rowCount; ++i) {
        CHECK(TeamStatNameAt(t0, i) == TeamStatNameAt(t1, i));
        CHECK(TeamStatNameAt(t0, i) != 0);
    }
    CHECK_FALSE(DecodeTeamStatUpdate(fixtures::kTeamStat0, sizeof fixtures::kTeamStat0 - 1, &t0));
}

TEST_CASE("JOIN / TEAM_CHANGE / LEAVE are edges, and a repeated body is not an edge") {
    LobbyTracker tr;
    tr.Reset();
    replay::capture::MarkRec marks[8];
    uint32_t joins = 0, teams = 0, leaves = 0, total = 0;
    auto feed = [&](LobbyKind k, const uint8_t* b, uint32_t n, uint64_t t) {
        const uint32_t got = tr.Note(k, b, n, t, marks, 8);
        for (uint32_t i = 0; i < got; ++i) {
            switch (marks[i].kind) {
                case replay::capture::kMarkJoin: ++joins; break;
                case replay::capture::kMarkTeamChange: ++teams; break;
                case replay::capture::kMarkLeave: ++leaves; break;
                default: break;
            }
        }
        total += got;
    };

    feed(LobbyKind::EntrantState, fixtures::kEntrantState0, sizeof fixtures::kEntrantState0, 1000);
    CHECK(joins == 1);
    CHECK(total == 1);

    // ★ THE CHECK THAT IS NOT SHORTER THAN ITS SUBJECT. The client re-sends this body about every
    // 20 ms; over a five-minute match that is roughly 15,000 copies. One more JOIN out of any of them
    // is a timeline nobody can read, so the duplicate is driven hard rather than sampled once.
    for (int i = 0; i < 15000; ++i) {
        feed(LobbyKind::EntrantState, fixtures::kEntrantState1, sizeof fixtures::kEntrantState1,
             2000 + static_cast<uint64_t>(i) * 20);
    }
    CHECK(joins == 1);
    CHECK(total == 1);
    CHECK(tr.GetStats().bodiesSeen == 15001);

    // Team none -> team 1 is one edge, and repeating the post-change body adds nothing.
    feed(LobbyKind::EntrantState, fixtures::kEntrantState2, sizeof fixtures::kEntrantState2, 400000);
    feed(LobbyKind::EntrantState, fixtures::kEntrantState3, sizeof fixtures::kEntrantState3, 400020);
    CHECK(teams == 1);
    CHECK(joins == 1);

    REQUIRE(tr.EntrantCount() == 1);
    const EntrantRow& row = tr.EntrantAt(0);
    CHECK(std::string(row.displayName) == "EchoDev [DEMO]");
    CHECK(row.team == 1);
    CHECK(row.active);
    CHECK(row.accountId == 0x0123456789ABCDEFull);

    // The marker payloads carry what a browser needs without the roster.
    tr.Note(LobbyKind::EntrantRemoved, fixtures::kEntrantRemoved, 16, 500000, marks, 8);
    // slot 1, and this entrant holds slot 0: a LEAVE for a slot nobody holds must not evict somebody.
    CHECK(tr.GetStats().removedUnknownSlot == 1);
    CHECK(tr.EntrantAt(0).active);

    uint8_t removeSlot0[16] = {};
    const uint32_t got = tr.Note(LobbyKind::EntrantRemoved, removeSlot0, 16, 500100, marks, 8);
    REQUIRE(got == 1);
    CHECK(marks[0].kind == replay::capture::kMarkLeave);
    CHECK(marks[0].index == 0);
    CHECK(marks[0].payload == 0x0123456789ABCDEFull);
    CHECK(marks[0].tUs == 500100);
    CHECK_FALSE(tr.EntrantAt(0).active);
}

TEST_CASE("a team change is keyed on the slot and does not invent a roster row") {
    LobbyTracker tr;
    tr.Reset();
    replay::capture::MarkRec marks[4];

    // The team-changed message arrives BEFORE any entrant state in the real capture (pkt #15 precedes
    // the owner state at #17). There is no account to attribute it to yet, so the marker is emitted
    // against the slot and the roster stays empty -- an identity guessed from a slot is the failure
    // this is written to prevent.
    const uint32_t got = tr.Note(LobbyKind::EntrantTeamChanged, fixtures::kTeamChanged0, 16, 10, marks, 4);
    REQUIRE(got == 1);
    CHECK(marks[0].kind == replay::capture::kMarkTeamChange);
    CHECK(marks[0].index == 0);
    CHECK(LobbyTracker::PrevTeam(marks[0].payload) == F::kTeamNone);
    CHECK(LobbyTracker::NowTeam(marks[0].payload) == 1);
    CHECK(tr.EntrantCount() == 0);
}

TEST_CASE("the two id spaces are tracked apart, and a re-sent space replaces rather than accumulates") {
    LobbyTracker tr;
    tr.Reset();
    replay::capture::MarkRec marks[4];
    tr.Note(LobbyKind::NetIds, fixtures::kNetIds0, sizeof fixtures::kNetIds0, 1, marks, 4);
    tr.Note(LobbyKind::NetIds, fixtures::kNetIds1, sizeof fixtures::kNetIds1, 2, marks, 4);
    CHECK(tr.NetIdSpaceCount() == 2);
    CHECK(tr.GetStats().netIdEntries == 48);

    tr.Note(LobbyKind::NetIds, fixtures::kNetIds0, sizeof fixtures::kNetIds0, 3, marks, 4);
    CHECK(tr.NetIdSpaceCount() == 2);
    CHECK(tr.GetStats().netIdEntries == 48);

    bool sawUnique = false, sawRepeated = false;
    for (uint16_t i = 0; i < tr.NetIdSpaceCount(); ++i) {
        const auto& r = tr.NetIdSpaceAt(i);
        CHECK(r.contiguous);
        if (r.distinctIds == r.count) sawUnique = true;
        if (r.distinctIds < r.count) sawRepeated = true;
    }
    CHECK(sawUnique);
    CHECK(sawRepeated);
}

// ---------------------------------------------------------------------------------------------
// THE MUTATIONS. Each one re-runs a check above against a deliberately wrong constant and asserts the
// comparison goes the other way. Without these, a cross-check that happened to hold for an unrelated
// reason would read as a pass.
// ---------------------------------------------------------------------------------------------

TEST_CASE("MUTATION: the checks above discriminate, and here is each one failing") {
    const uint8_t* body = fixtures::kEntrantState3;

    auto readU64 = [&](uint32_t off) {
        uint64_t v;
        std::memcpy(&v, body + off, sizeof v);
        return v;
    };
    auto readStr = [&](uint32_t off) {
        const char* s = reinterpret_cast<const char*>(body + off);
        uint32_t n = 0;
        while (n < F::kEntrantStrLen && s[n] != '\0') ++n;
        return std::string(s, n);
    };
    auto readU16 = [&](uint32_t off) {
        uint16_t v;
        std::memcpy(&v, body + off, sizeof v);
        return v;
    };

    // The account id at the right offset spells the platform string; one qword either side does not.
    CHECK(ExpectedPlatformString(7, readU64(F::kEntrantAccountIdOff)) ==
          readStr(F::kEntrantPlatformStrOff));
    CHECK(ExpectedPlatformString(7, readU64(F::kEntrantAccountIdOff + 8)) !=
          readStr(F::kEntrantPlatformStrOff));
    CHECK(ExpectedPlatformString(7, readU64(F::kEntrantAccountIdOff - 8)) !=
          readStr(F::kEntrantPlatformStrOff));

    // The name offset is pinned by the string, not by a plausible-looking run of bytes.
    CHECK(readStr(F::kEntrantDisplayNameOff) == "EchoDev [DEMO]");
    CHECK(readStr(F::kEntrantDisplayNameOff + 1) != "EchoDev [DEMO]");
    CHECK(readStr(F::kEntrantDisplayNameOff - 1) != "EchoDev [DEMO]");

    // The team offset agrees with the OwnerState JSON; its neighbours do not.
    ByteView json{};
    REQUIRE(DecodeJsonBody(fixtures::kOwnerState0, sizeof fixtures::kOwnerState0, &json));
    const uint16_t stated = TeamFromOwnerJson(Text(json));
    CHECK(readU16(F::kEntrantTeamOff) == stated);
    CHECK(readU16(F::kEntrantTeamOff - 2) != stated);
    CHECK(readU16(F::kEntrantTeamOff + 2) != stated);

    // The net-id framing is 8 + n*10 and nothing else divides both real bodies. 8+n*8 leaves a
    // remainder on one of them, which is what rules the obvious alternative out.
    const uint32_t n0 = sizeof fixtures::kNetIds0, n1 = sizeof fixtures::kNetIds1;
    CHECK((n0 - F::kNetIdsHeaderLen) % F::kNetIdsEntryLen == 0);
    CHECK((n1 - F::kNetIdsHeaderLen) % F::kNetIdsEntryLen == 0);
    const bool eightFits = (n0 - F::kNetIdsHeaderLen) % 8 == 0 && (n1 - F::kNetIdsHeaderLen) % 8 == 0;
    const bool twelveFits = (n0 - F::kNetIdsHeaderLen) % 12 == 0 && (n1 - F::kNetIdsHeaderLen) % 12 == 0;
    CHECK_FALSE(eightFits);
    CHECK_FALSE(twelveFits);
}

// ---------------------------------------------------------------------------------------------
// THE SEAM. W3-E owns no file in src/capture/, so the integration is handed over rather than landed.
// This case compiles and links the exact expressions that hand-over contains, against W2-A's REAL tap
// table -- so a name that does not match, or a MarkRec that is not the type S_MARK takes, is a red
// gate here instead of a surprise in somebody else's tree.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the tap table resolves to exactly the kinds this packet decodes") {
    using replay::capture::TapTable;
    using replay::capture::TapTypeCount;

    int decodable = 0;
    LobbyTracker tr;
    tr.Reset();
    for (uint16_t i = 0; i < TapTypeCount(); ++i) {
        const LobbyKind kind = LobbyKindForName(TapTable()[i].structName);
        if (kind == LobbyKind::Other) {
            continue;
        }
        ++decodable;
        // The integration feeds MarkRec straight into AppendMark; if that type ever diverges this
        // stops compiling, which is the point.
        replay::capture::MarkRec marks[8];
        tr.Note(kind, fixtures::kEntrantState0, 0, 0, marks, 8);   // 0 bytes: must refuse, not crash
    }
    // Eight names in W2-A's table are ones this packet decodes. A rename on either side drops this.
    CHECK(decodable == 8);
    CHECK(tr.GetStats().marksEmitted == 0);
    CHECK(tr.GetStats().bodiesRefused > 0);

    // And the mapping is by NAME, so a name that is not in the table resolves to Other rather than to
    // whatever happens to sit at some index.
    CHECK(LobbyKindForName("SR15NetPlayerStateMsg") == LobbyKind::Other);
    CHECK(LobbyKindForName(nullptr) == LobbyKind::Other);
    CHECK(LobbyKindForName("SNSLobbyEntrantStatev2") == LobbyKind::EntrantState);
}
