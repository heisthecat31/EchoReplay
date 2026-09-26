// The real-data arm: every message of the only real capture on this machine, through the shipping
// parse, the shipping wire decode and the shipping storage codec.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS ARM ESTABLISHES AND WHAT IT CANNOT.
//
// ESTABLISHES  the DUV layout and its numbering behaviour, over all 14,795 messages rather than over an
//              example; and that the wire decode reproduces real engine-produced bodies bit for bit,
//              over every PlayerState and SimState body in the capture.
// CANNOT       say anything about COMPRESSION on real motion. Appendix D.2 is explicit and this file
//              repeats it: the capture is IDLE -- 5,520 PlayerState records carry one distinct payload
//              and 11,029 SimState records carry sixteen. A ratio measured on that data measures the
//              corpus. The A/B below is therefore reported per DISTINCT PAYLOAD and labelled, and the
//              shipping number waits for W3-E's real match. A previous attempt on this same corpus
//              reported >3000x and was rightly refused.
//
// ★ The corpus lives outside the repository. Its absence is NOT RUN naming the path, never a pass.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "capture/capture_facts.h"
#include "capture/duv.h"
#include "capture/manifestbuild.h"
#include "events/lobbytrack.h"
#include "engine/wiretap_facts.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/wirefields.h"

#include "zstd.h"
#include "../local_inputs.h"

using namespace replay;
using namespace replay::capture;

namespace {


// ★ CITED, not transcribed (S4.6 rule 3). Two of these are registry facts and W1-L already holds them
// in one place with a static_assert against facts_generated.h; writing them again here would be a
// second copy that nothing keeps in step -- exactly the duplication that let a stale stride of 520
// survive in three places after the true value was known (F10).
constexpr uint64_t kSymPlayerState = replay::wiretap_facts::kSymPlayerStateMsg;
constexpr uint64_t kSymDuv = replay::wiretap_facts::kSymDeltaUnreliableVars;
// SR15NetPhysicsSimStateEvent has no merged registry row yet, so it is cited from this packet's own
// single declaration -- W1-L's wiretap_facts.h pattern -- rather than written a second time here.
constexpr uint64_t kSymSimState = replay::capture_facts::kSymPhysicsSimStateEvent;

struct Msg {
    uint64_t sym = 0;
    uint64_t dst = 0;
    std::vector<uint8_t> body;
};

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseLine(const std::string& line, Msg& out) {
    const size_t symAt = line.find("sym=0x");
    if (symAt == std::string::npos) {
        return false;
    }
    out.sym = _strtoui64(line.c_str() + symAt + 6, nullptr, 16);
    const size_t dstAt = line.find("dst=0x");
    out.dst = dstAt == std::string::npos ? 0 : _strtoui64(line.c_str() + dstAt + 6, nullptr, 16);
    const size_t bodyAt = line.find("body=");
    if (bodyAt == std::string::npos) {
        return false;
    }
    const char* p = line.c_str() + bodyAt + 5;
    out.body.clear();
    while (p[0] && p[1]) {
        const int hi = HexVal(p[0]), lo = HexVal(p[1]);
        if (hi < 0 || lo < 0) break;
        out.body.push_back(static_cast<uint8_t>((hi << 4) | lo));
        p += 2;
    }
    return true;
}

size_t Zstd(const std::vector<uint8_t>& in) {
    if (in.empty()) {
        return 0;
    }
    std::vector<uint8_t> out(ZSTD_compressBound(in.size()));
    const size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
    return ZSTD_isError(n) ? 0 : n;
}

int NotRun(const char* why, const char* detail) {
    printf("CORPUS-RESULT status=NOT-RUN checked=0 reason=%s detail=%s\n", why, detail);
    return 2;
}

}  // namespace


// ---------------------------------------------------------------------------------------------
// Every registered tap name must be a name the binary actually contains.
//
// CMatSym::TypeId is TOTAL over strings: every name hashes to something, so Listen() succeeds for a
// typo, ActiveCount() counts the tap, and it never fires. B.14 names that failure and says it is
// indistinguishable from a quiet match -- and it cost this capture every ownership transfer until
// W3-E found it. A symbol table with no invalid input cannot report a typo, so the check has to come
// from outside the hasher: the string itself, in the image.
//
// ★ AND IT MUST MATCH A WHOLE NAME, NOT A SUBSTRING. The dead name occurred TWICE in echovr.exe, both
// times as a prefix of a real one, so a substring test reports it PRESENT -- a check that would have
// passed the exact defect it exists to catch. The control below proves the instrument on that name
// before any verdict is read from it.
// ---------------------------------------------------------------------------------------------
namespace {

bool WholeNameInImage(const std::vector<uint8_t>& img, const char* name) {
    std::string needle(name);
    needle.push_back('\0');
    if (needle.size() > img.size()) {
        return false;
    }
    const uint8_t* p = img.data();
    const size_t n = img.size() - needle.size();
    for (size_t i = 0; i <= n; ++i) {
        if (memcmp(p + i, needle.data(), needle.size()) == 0) {
            return true;
        }
    }
    return false;
}

bool SubstringInImage(const std::vector<uint8_t>& img, const char* name) {
    const size_t len = strlen(name);
    if (len > img.size()) {
        return false;
    }
    const uint8_t* p = img.data();
    for (size_t i = 0; i + len <= img.size(); ++i) {
        if (memcmp(p + i, name, len) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int TapNameArm(uint64_t& checked, uint64_t& failed) {
    const std::string exePath = replay_test::FindEchoVrExeA();
    FILE* f = nullptr;
    if (exePath.empty() || fopen_s(&f, exePath.c_str(), "rb") != 0 || !f) {
        printf("\nTAP-NAMES NOT-RUN: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> img(static_cast<size_t>(size));
    const size_t got = fread(img.data(), 1, img.size(), f);
    fclose(f);
    if (got != img.size()) {
        printf("\nTAP-NAMES NOT-RUN: short read of echovr.exe\n");
        return 2;
    }

    // THE CONTROL, FIRST. The name that shipped broken must be reported ABSENT as a whole name and
    // PRESENT as a substring. If those two ever agree, this instrument cannot tell a real name from a
    // prefix of one and its verdict below means nothing.
    const char* dead = "SR15NetTouchOwnershipTransfer";
    const bool deadWhole = WholeNameInImage(img, dead);
    const bool deadSub = SubstringInImage(img, dead);
    printf("\ntap-name control (%s): whole=%d substring=%d\n", dead, int(deadWhole), int(deadSub));
    ++checked;
    if (deadWhole || !deadSub) {
        printf("FAIL: the control did not behave, so this arm cannot discriminate a name from a "
               "prefix of one and its result is not evidence\n");
        ++failed;
        return 1;
    }

    uint16_t bad = 0;
    for (uint16_t i = 0; i < TapTypeCount(); ++i) {
        const char* name = TapTable()[i].structName;
        if (strcmp(name, "ReplayIdentitySnapshot") == 0) {
            continue;   // ours, never a wire message, correctly not in the image
        }
        ++checked;
        if (!WholeNameInImage(img, name)) {
            printf("FAIL: tap name %s is NOT a whole name in echovr.exe -- it would hash, register, "
                   "count, and never fire\n", name);
            ++failed;
            ++bad;
        }
    }
    printf("tap names: %u registered, %u absent from the image\n", TapTypeCount(), bad);
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// The OTHER half of the dead-name defect, in the opposite direction.
//
// TapNameArm answers "does every name I register exist in the image?". It cannot answer "does every
// name a CONSUMER decodes actually get registered?" -- and a decoder keyed on a name no tap registers
// is the identical failure: the lookup is total, it returns Other or its equivalent for a name nobody
// produces, and the feature is simply dead. LobbyKindForName recognises eight struct names. If one of
// them is not in kTaps, no body of that kind ever reaches the tracker, the roster silently loses a
// column, and nothing anywhere disagrees.
//
// The direction that matters is decoder -> tap, so this walks the TAP TABLE and collects which kinds
// are reachable from a registered name, then requires every kind to be reachable. Written the other
// way round -- iterating a list of names copied out of lobbytrack.cpp -- it would be checking this
// file's copy of the list against itself.
// ---------------------------------------------------------------------------------------------
int LobbyKindArm(uint64_t& checked, uint64_t& failed) {
    using events::LobbyKind;

    // ★ THE PIN. This arm can only require the kinds it knows the names of, so it must fail when the
    // enum grows rather than keep passing over a stale set. TeamStatUpdate is the last enumerator
    // today; if a ninth kind is added, this stops compiling into a true statement and forces a look
    // at whether the new kind's struct name is a registered tap.
    ++checked;
    if (static_cast<uint8_t>(LobbyKind::TeamStatUpdate) != 8) {
        printf("FAIL: LobbyKind gained or lost an enumerator, so the set of kinds this arm requires "
               "is stale -- add the new kind's struct name to kTaps and update this pin\n");
        ++failed;
        return 1;
    }

    // ★ THE CONTROL, FIRST. A mapping that returned a real kind for everything would make the sweep
    // below pass no matter what kTaps contains. A registered tap that is NOT a lobby message must map
    // to Other, and a known lobby name must not.
    ++checked;
    const bool negativeMapsToOther = events::LobbyKindForName("SR15NetSetOwner") == LobbyKind::Other;
    const bool positiveMapsToKind =
        events::LobbyKindForName("SNSLobbyEntrantStatev2") == LobbyKind::EntrantState;
    printf("\nlobby-kind control: non-lobby name -> Other = %d, lobby name -> its kind = %d\n",
           int(negativeMapsToOther), int(positiveMapsToKind));
    if (!negativeMapsToOther || !positiveMapsToKind) {
        printf("FAIL: LobbyKindForName does not discriminate, so the sweep below is not evidence\n");
        ++failed;
        return 1;
    }

    bool reachable[9] = {false, false, false, false, false, false, false, false, false};
    for (uint16_t i = 0; i < TapTypeCount(); ++i) {
        const LobbyKind k = events::LobbyKindForName(TapTable()[i].structName);
        if (k != LobbyKind::Other) {
            reachable[static_cast<uint8_t>(k)] = true;
        }
    }

    static const char* kKindNames[9] = {"Other",         "EntrantState",  "EntrantRemoved",
                                        "TeamChanged",   "OwnerState",    "NetIds",
                                        "ProfileServer", "StatUpdate",    "TeamStatUpdate"};
    uint16_t unreachable = 0;
    for (uint8_t k = 1; k <= 8; ++k) {
        ++checked;
        if (!reachable[k]) {
            printf("FAIL: LobbyKind::%s can be decoded but NO registered tap produces it -- the tap "
                   "table is missing that struct name and the kind is dead\n", kKindNames[k]);
            ++failed;
            ++unreachable;
        }
    }
    printf("lobby kinds: 8 decodable, %u unreachable from the tap table\n", unreachable);
    return unreachable ? 1 : 0;
}

int main(int argc, char** argv) {
    // The corpus is a private capture: argv[1], or arena_decoded.txt in REPLAY_FRESHCAP_DIR.
    std::string corpus = argc > 1 ? argv[1] : replay_test::OptionalInputA("REPLAY_FRESHCAP_DIR");
    if (argc <= 1 && !corpus.empty()) {
        corpus += "\\arena_decoded.txt";
    }
    const char* path = corpus.empty() ? "<REPLAY_FRESHCAP_DIR unset>" : corpus.c_str();
    FILE* f = nullptr;
    if (corpus.empty() || fopen_s(&f, path, "rb") != 0 || !f) {
        return NotRun("corpus-absent", path);
    }

    std::vector<Msg> player, body, duv;
    std::map<uint64_t, uint64_t> symbolCensus;
    {
        std::string line;
        char buf[1 << 16];
        while (fgets(buf, sizeof buf, f)) {
            line = buf;
            Msg m;
            if (!ParseLine(line, m)) continue;
            ++symbolCensus[m.sym];
            if (m.sym == kSymPlayerState) player.push_back(std::move(m));
            else if (m.sym == kSymSimState) body.push_back(std::move(m));
            else if (m.sym == kSymDuv) duv.push_back(std::move(m));
        }
        fclose(f);
    }
    printf("corpus           : %s\n", path);
    printf("messages         : %zu PlayerState, %zu SimState, %zu DUV\n", player.size(), body.size(),
           duv.size());
    if (player.empty() || body.empty() || duv.empty()) {
        return NotRun("corpus-empty", path);
    }

    uint64_t checked = 0, failed = 0;
    auto check = [&](bool ok, const char* what) {
        ++checked;
        if (!ok) {
            ++failed;
            printf("FAIL: %s\n", what);
        }
    };

    // ---- every symbol on the wire has a stated disposition -------------------------------------
    //
    // ★ THE QUESTION THIS ANSWERS: is there a replication channel the capture does not see? The tap
    // table says what we listen to; it cannot say what was SAID. Only the wire can, and a symbol that
    // appears in a real capture and in no table here is either a message we are silently dropping or
    // one nobody has classified -- and those two look identical from inside the plugin.
    //
    // This is what retired S_WORLD. The design carved that stream out by SUBJECT ("world actors")
    // while every other stream is carved out by MECHANISM -- physics SimState, DUV, tapped message --
    // and a subject-based stream in a mechanism-based format is necessarily redundant or empty. The
    // census below is the evidence for the "necessarily": over 97,779 records of a real Arena match,
    // every one of the 28 distinct symbols is either captured or is transport, handshake, or the
    // request/ack half of a pair whose payload half is captured. There is no third channel for a
    // world actor's state to arrive on.
    //
    // Dispositions are deliberately coarse and each row states WHY, so that adding a row is an act of
    // classification rather than of silencing. A symbol not listed fails the arm.
    {
        enum Disp : uint8_t { kCaptured, kTransport, kPairedHalf, kHandshake };
        struct Row { uint64_t sym; const char* name; Disp disp; };
        static const Row kWire[] = {
            // captured: tapped and stored, verbatim or decoded
            {0xC6BB60E8D9E9A3D3ull, "SR15NetPlayerStateMsg", kCaptured},
            {0x8F62ADBA23F15FE0ull, "SR15NetPhysicsSimStateEvent", kCaptured},
            {0x2DED551BABC46DBFull, "SR15NetReplicatedDeltaUnreliableVars", kCaptured},
            {0x6E54D0D298BFFB17ull, "SR15NetComponentEvent", kCaptured},
            {0x6D9A367EE21A5642ull, "SR15NetTouchStateEvent", kCaptured},
            {0xC57EC38913BE8582ull, "SR15NetServerFrameMsg", kCaptured},
            {0x5D84B0C2D4BB6CFCull, "SR15NetReliableStatUpdate", kCaptured},
            {0x7BA444F7D419B1A2ull, "SR15NetReliableTeamStatUpdate", kCaptured},
            {0x2068C0F03B4B8F25ull, "SNSLobbyOwnerState", kCaptured},
            {0xEB6218834B3FA648ull, "SNSLobbyEntrantStatev2", kCaptured},
            {0xCEB8759C36184B82ull, "SNSLobbyEntrantTeamChanged", kCaptured},
            {0x8F8AF081AF21CBE7ull, "SR15NetIdsResponse", kCaptured},
            {0xA52712EC66B2DB48ull, "SR15NetCurrentLoadoutResponse", kCaptured},
            {0x71C59625B07711B9ull, "SR15NetPlayerEquipmentUpdate", kCaptured},
            {0x80AF66BF1E67B474ull, "SR15NetLoadoutNumberNotification", kCaptured},
            {0xE9A4B52CBA8B27E7ull, "SR15NetBalanceSettingsMassUpdate", kCaptured},
            {0xFA563AE00EBFF2BAull, "SR15NetServerInfo", kCaptured},
            {0x03E01D2BD46A86C0ull, "SNSLobbyUpdatePings", kCaptured},

            // transport: the delivery layer, below CBroadcaster. Carries no replicated state, and the
            // RTT it measures is already recorded per player on S_ASSEEN (AsSeenPerPlayer::rttMs).
            {0x67232CA3535DF81Cull, "SR15NetClientPingResponseMsg", kTransport},
            {0xF85FD7296F268B1Bull, "SR15NetUserPingEstMsg", kTransport},
            {0x014C925361833BF6ull, "SBroadcasterPingUpdate", kTransport},
            {0x2CD38A848B5B9C73ull, "SBroadcasterReliableBatchEvent", kTransport},

            // paired half: an ack or request whose PAYLOAD half is captured above. Storing it would
            // duplicate state we already hold, and its own numbering is not our completeness oracle --
            // DUV's pkt_seq / rowset_seq is, because that is what the engine numbers.
            {0xD0AEAD470B6648B8ull, "SR15NetReplicatedDeltaAck", kPairedHalf},
            {0xC4FE7B57CAAB27E2ull, "SR15NetIdsRequest", kPairedHalf},
            {0xC66CA34BB55FDC28ull, "SR15NetCurrentLoadoutRequest", kPairedHalf},

            // handshake: one-shot session establishment, before any match state exists
            {0xA026B3042ADD4E14ull, "SR15NetReplicatedClientReady", kHandshake},
            {0x65B2FEBA429E1B02ull, "SNSLobbyJoinComplete", kHandshake},
            {0x10EC6147D112E1E4ull, "SR15NetBalanceSettingsClientReady", kHandshake},
        };

        uint32_t classified = 0, unclassified = 0, capturedKinds = 0;
        for (const auto& kv : symbolCensus) {
            const Row* hit = nullptr;
            for (const Row& r : kWire) {
                if (r.sym == kv.first) { hit = &r; break; }
            }
            ++checked;
            if (hit == nullptr) {
                printf("FAIL: symbol %016llX appears %llu times on the wire and no row classifies it "
                       "-- it is either a message the capture silently drops or one nobody has "
                       "classified, and those look identical from inside the plugin\n",
                       (unsigned long long)kv.first, (unsigned long long)kv.second);
                ++failed;
                ++unclassified;
                continue;
            }
            ++classified;
            if (hit->disp == kCaptured) ++capturedKinds;
        }

        // ★ The arm must not pass on an empty or truncated census. Zero unclassified over zero symbols
        // is the vacuous pass, and a corpus that parsed nothing would produce exactly that.
        ++checked;
        if (symbolCensus.size() < 20) {
            printf("FAIL: only %zu distinct symbols in the census; the corpus did not parse and this "
                   "arm's clean result is not evidence\n", symbolCensus.size());
            ++failed;
        }
        printf("\nwire symbols: %zu distinct, %u classified, %u unclassified (%u captured kinds)\n",
               symbolCensus.size(), classified, unclassified, capturedKinds);
    }

    // ---- the DUV layout and its numbering, over every message ---------------------------------
    {
        uint64_t sizeFormulaHolds = 0, headerOnly = 0, pktEqualsRowset = 0, res12One = 0,
                 res20Zero = 0, malformed = 0;
        std::map<int64_t, uint64_t> deltas;
        std::set<uint16_t> slots;
        DuvSequence seq;
        uint32_t last = 0;
        bool first = true;
        for (const Msg& m : duv) {
            const DuvMessage d = DuvParse(m.body.data(), m.body.size());
            if (d.status == DuvStatus::TooShort) {
                ++malformed;
                continue;
            }
            slots.insert(d.slot);
            if (d.reserved12 == 1) ++res12One;
            if (d.reserved20 == 0) ++res20Zero;
            if (d.hasRowset) {
                if (m.body.size() == size_t(30) + 16 * size_t(d.varCount)) ++sizeFormulaHolds;
                if (d.rowsetSeq == d.pktSeq) ++pktEqualsRowset;
            } else {
                ++headerOnly;
            }
            if (!first) {
                deltas[int64_t(int32_t(d.pktSeq - last))] += 1;
            }
            last = d.pktSeq;
            first = false;
            DuvSequence::Gap g{};
            seq.Note(d, &g);
        }
        printf("\nDUV, measured over every message in the capture:\n");
        printf("  parsed                       : %zu (%llu malformed)\n", duv.size(),
               static_cast<unsigned long long>(malformed));
        printf("  size == 30 + 16*var_count    : %llu\n",
               static_cast<unsigned long long>(sizeFormulaHolds));
        printf("  header-only (24 bytes)       : %llu\n",
               static_cast<unsigned long long>(headerOnly));
        printf("  pkt_seq == rowset_seq        : %llu\n",
               static_cast<unsigned long long>(pktEqualsRowset));
        printf("  reserved[12:16) == 1         : %llu\n", static_cast<unsigned long long>(res12One));
        printf("  reserved[20:24) == 0         : %llu\n", static_cast<unsigned long long>(res20Zero));
        printf("  slots seen                   : ");
        for (uint16_t s : slots) printf("%u ", s);
        printf("\n  pkt_seq deltas               : ");
        for (const auto& kv : deltas) {
            printf("%+lld x%llu  ", static_cast<long long>(kv.first),
                   static_cast<unsigned long long>(kv.second));
        }
        printf("\n  tracker: delivered=%u observed=%u missing=%u dup=%u regressions=%u\n",
               seq.Delivered(), seq.Observed(), seq.Missing(), seq.Duplicates(), seq.Regressions());

        // The claims duv.h makes, asserted against the data rather than restated.
        check(malformed == 0, "a DUV message did not parse");
        check(sizeFormulaHolds + headerOnly == duv.size(),
              "a DUV message matched neither the rowset formula nor the header-only shape");
        check(pktEqualsRowset + headerOnly == duv.size(),
              "pkt_seq and rowset_seq disagreed somewhere");
        check(res12One == duv.size() - malformed, "reserved[12:16) was not 1 everywhere");
        check(res20Zero == duv.size() - malformed, "reserved[20:24) was not 0 everywhere");
        // ★ The load-bearing one: the numbering steps by 0 or 1 and never backwards. If this ever
        // fails, S_NETVAR's completeness has no source and the file must say so instead of claiming it.
        for (const auto& kv : deltas) {
            check(kv.first == 0 || kv.first == 1, "a pkt_seq delta was not 0 or 1");
        }
        check(seq.Regressions() == 0, "the DUV sequence went backwards");
        check(seq.Missing() == 0, "the capture itself lost DUV packets");
    }

    // ---- the wire decode, over every body ------------------------------------------------------
    auto sweepWire = [&](const std::vector<Msg>& msgs, bool isPlayer) {
        uint64_t ok = 0, byReason[5] = {};
        // ★ Distinctness is measured over the RECORD, not over the message. The header carries the
        // source stamp, which differs on every message and would make an idle capture look like 5,520
        // distinct payloads -- the exact overstatement tests/o1/corpus/arena_idle.tsv exists to stop,
        // and one this tool made on its first run.
        const size_t recordAt = (isPlayer ? kPlayerRecordStartBits : kBodyRecordStartBits) / 8;
        std::set<std::string> distinct;
        for (const Msg& m : msgs) {
            if (m.body.size() > recordAt) {
                distinct.insert(std::string(reinterpret_cast<const char*>(m.body.data()) + recordAt,
                                            m.body.size() - recordAt));
            }
            WireDecode d;
            if (isPlayer) {
                PlayerFrame pf{};
                d = DecodePlayerBody(m.body.data(), m.body.size(), pf);
            } else {
                BodyFrame bf{};
                d = DecodeBodyBody(m.body.data(), m.body.size(), bf);
            }
            if (d == WireDecode::Ok) ++ok;
            else ++byReason[static_cast<uint32_t>(d)];
        }
        printf("\n%s bodies: %zu messages, %zu DISTINCT payloads\n",
               isPlayer ? "PlayerState" : "SimState", msgs.size(), distinct.size());
        printf("  decode proved by re-encode : %llu\n", static_cast<unsigned long long>(ok));
        for (uint32_t i = 1; i < 5; ++i) {
            if (byReason[i]) {
                printf("  refused (%s): %llu\n", WireDecodeName(static_cast<WireDecode>(i)),
                       static_cast<unsigned long long>(byReason[i]));
            }
        }
        // ★ Coverage is stated in DISTINCT PAYLOADS. Record counts on an idle capture are the exact
        // error tests/o1/corpus/arena_idle.tsv exists to avoid.
        check(ok == msgs.size(), isPlayer ? "a real PlayerState body did not decode-and-prove"
                                          : "a real SimState body did not decode-and-prove");
        return distinct.size();
    };
    const size_t playerDistinct = sweepWire(player, true);
    const size_t bodyDistinct = sweepWire(body, false);

    // ---- S5.6's A/B, on the only data there is, labelled for what it is ------------------------
    //
    // Arm B is the SAMPLED schema over the identical frames: same delta coding, same per-field change
    // flags, values at natural width instead of the engine's quantisation grid. That is exactly the
    // one variable S5.6 step 5 asks about, and holding everything else fixed is what makes it an A/B
    // rather than two measurements.
    {
        std::vector<uint8_t> packedA(1u << 22), packedB(1u << 22);
        bitio::BitWriter wa(packedA.data(), packedA.size());
        bitio::BitWriter wb(packedB.data(), packedB.size());
        BodyEncoder ea(false), eb(true);
        uint64_t raw = 0, records = 0;
        uint64_t stamp = 1;
        for (const Msg& m : body) {
            BodyFrame bf{};
            if (DecodeBodyBody(m.body.data(), m.body.size(), bf) != WireDecode::Ok) continue;
            BodyFrame sampled = bf;
            sampled.hasHeader = false;
            if (!ea.Encode(wa, 0, stamp, bf) || !eb.Encode(wb, 0, stamp, sampled)) break;
            raw += m.body.size();
            ++records;
            stamp += 22727;
        }
        packedA.resize((wa.BitsWritten() + 7) / 8);
        packedB.resize((wb.BitsWritten() + 7) / 8);
        const size_t za = Zstd(packedA), zb = Zstd(packedB);
        printf("\nS5.6 step-5 A/B on SimState, %llu records (%zu distinct payloads):\n",
               static_cast<unsigned long long>(records), bodyDistinct);
        printf("  raw wire bodies              : %llu B\n", static_cast<unsigned long long>(raw));
        printf("  (4+5) delta + bit-pack       : %zu B\n", packedA.size());
        printf("  (4+5+6) + zstd-3             : %zu B\n", za);
        printf("  (4)   delta at natural width : %zu B\n", packedB.size());
        printf("  (4+6) + zstd-3               : %zu B\n", zb);
        if (zb && za) {
            printf("  bit-packing buys             : %.2fx after zstd\n",
                   double(zb) / double(za));
        }
        printf("  ★ THIS IS NOT THE SHIPPING NUMBER. The capture is idle: %zu distinct payloads over\n"
               "    %llu records, so both arms are measuring the corpus. The A/B that decides whether\n"
               "    step 5 stays is W3-E's real match, and this run is the method, not the answer.\n",
               bodyDistinct, static_cast<unsigned long long>(records));
        check(records > 0, "the A/B encoded nothing");
        check(wa.Refused() == false, "the wire-class encoder refused a real record");
        check(wb.Refused() == false, "the natural-width encoder refused a real record");
    }
    (void)playerDistinct;

    TapNameArm(checked, failed);
    LobbyKindArm(checked, failed);

    printf("\nCORPUS-RESULT status=%s checked=%llu failed=%llu\n", failed ? "FAIL" : "PASS",
           static_cast<unsigned long long>(checked), static_cast<unsigned long long>(failed));
    return failed ? 1 : 0;
}
