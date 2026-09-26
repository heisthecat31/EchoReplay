// W3-E's real-corpus arm.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS EXISTS BESIDE test_events.cpp. The committed fixtures are thirteen bodies. Thirteen
// bodies can show that a framing WORKS; they cannot show that it always works, and "the decoder
// handled every example I chose for it" is the shape of a result that holds until the first real file.
// This arm runs the same decoders over every message in the reference capture -- 97,779 of them -- and
// reports what refused. A framing that is right on a sample and wrong on the population fails here.
//
// ★ AND IT CARRIES THE NEGATIVE. The one claim in MESSAGE_COVERAGE.md that cannot be shown from a
// sample is the negative one: that no value in the SR15NetIdsResponse id column appears anywhere in a
// SimStateEvent, PlayerStateMsg or ComponentEvent body. A negative needs the whole population and it
// needs a stated scope, so this scans every byte offset of every such body and prints how many it
// examined. A zero with no denominator beside it is not a result.
//
// ★ ITS CORPUS LIVES OUTSIDE THE REPOSITORY, so its absence is NOT RUN naming the path it looked for.
// An absent corpus reported as a pass is how a check stops being one.
// ---------------------------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "events/events_facts.h"
#include "events/lobbymsg.h"
#include "events/lobbytrack.h"
#include "../local_inputs.h"

using namespace replay::events;
namespace F = replay::events_facts;

namespace {


struct Record {
    uint64_t symbol;
    std::vector<uint8_t> body;
};

bool HexNibble(char c, uint8_t* out) {
    if (c >= '0' && c <= '9') { *out = static_cast<uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = static_cast<uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = static_cast<uint8_t>(c - 'A' + 10); return true; }
    return false;
}

// One decoded line looks like:
//   sym=0x2068C0F03B4B8F25 id=4 dst=0xFFFF... frame=0 size=233 body=257b2267...
// A line that does not parse cleanly is COUNTED rather than skipped: a parser that quietly drops input
// would make every downstream total an undercount that looks like a result.
bool ParseLine(const std::string& line, Record* out, bool* wasMessageLine) {
    *wasMessageLine = false;
    const size_t at = line.find("sym=0x");
    if (at == std::string::npos) {
        return true;
    }
    *wasMessageLine = true;
    uint64_t sym = 0;
    for (int i = 0; i < 16; ++i) {
        uint8_t n = 0;
        if (at + 6 + i >= line.size() || !HexNibble(line[at + 6 + i], &n)) {
            return false;
        }
        sym = (sym << 4) | n;
    }
    const size_t b = line.find("body=");
    if (b == std::string::npos) {
        return false;
    }
    const char* p = line.c_str() + b + 5;
    out->symbol = sym;
    out->body.clear();
    while (p[0] != '\0' && p[1] != '\0') {
        uint8_t hi = 0, lo = 0;
        if (!HexNibble(p[0], &hi) || !HexNibble(p[1], &lo)) {
            break;
        }
        out->body.push_back(static_cast<uint8_t>((hi << 4) | lo));
        p += 2;
    }
    return true;
}

// Symbols, from the same CMatSym::TypeId computation tests/wiretap validates. Written here as
// constants because this executable does not map echovr.exe; test_matsym.cpp is what proves each one,
// and MESSAGE_COVERAGE.md records the derivation.
constexpr uint64_t kSymEntrantState  = 0xEB6218834B3FA648ull;
constexpr uint64_t kSymEntrantTeam   = 0xCEB8759C36184B82ull;
constexpr uint64_t kSymOwnerState    = 0x2068C0F03B4B8F25ull;
constexpr uint64_t kSymNetIdsResp    = 0x8F8AF081AF21CBE7ull;
constexpr uint64_t kSymNetIdsReq     = 0xC4FE7B57CAAB27E2ull;
constexpr uint64_t kSymStatUpdate    = 0x5D84B0C2D4BB6CFCull;
constexpr uint64_t kSymTeamStat      = 0x7BA444F7D419B1A2ull;
constexpr uint64_t kSymSimState      = 0x8F62ADBA23F15FE0ull;
constexpr uint64_t kSymPlayerState   = 0xC6BB60E8D9E9A3D3ull;
constexpr uint64_t kSymComponentEvt  = 0x6E54D0D298BFFB17ull;

int failures = 0;
void Fail(const char* what) {
    std::printf("FAIL  %s\n", what);
    ++failures;
}
void Pass(const char* what, unsigned long n) {
    std::printf("ok    %-58s  n=%lu\n", what, n);
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "";
    if (path.empty()) {
        char env[512]{};
        size_t envLen = 0;
        getenv_s(&envLen, env, sizeof env, "REPLAY_EVENT_CORPUS");
        path = envLen > 1 ? env : "";
    }
    if (path.empty()) {
        const std::string freshcap = replay_test::OptionalInputA("REPLAY_FRESHCAP_DIR");
        if (!freshcap.empty()) {
            path = freshcap + "\\arena_decoded.txt";
        }
    }

    std::ifstream in(path.c_str());
    if (!in) {
        std::printf("NOT RUN  the reference corpus is not present at %s\n", path.c_str());
        std::printf("NOT RUN  set REPLAY_EVENT_CORPUS or REPLAY_FRESHCAP_DIR, or pass the path; absence is not a pass\n");
        return 2;
    }

    std::map<uint64_t, std::vector<Record>> byType;
    unsigned long lines = 0, messageLines = 0, unparsed = 0;
    std::string line;
    Record rec;
    while (std::getline(in, line)) {
        ++lines;
        bool isMessage = false;
        if (!ParseLine(line, &rec, &isMessage)) {
            ++unparsed;
            continue;
        }
        if (!isMessage) {
            continue;
        }
        ++messageLines;
        switch (rec.symbol) {
            case kSymEntrantState: case kSymEntrantTeam: case kSymOwnerState:
            case kSymNetIdsResp: case kSymNetIdsReq: case kSymStatUpdate:
            case kSymTeamStat: case kSymSimState: case kSymPlayerState:
            case kSymComponentEvt:
                byType[rec.symbol].push_back(rec);
                break;
            default:
                break;
        }
    }
    std::printf("corpus %s: %lu lines, %lu message records, %lu unparsed\n",
                path.c_str(), lines, messageLines, unparsed);
    if (unparsed != 0) {
        Fail("lines carrying sym= that did not parse");
    }
    if (messageLines == 0) {
        Fail("no message records at all: the corpus format is not what this reader expects");
        return 1;
    }

    // ---- every instance of every type this packet decodes must decode ----------------------
    unsigned long n = 0;
    for (const Record& r : byType[kSymEntrantState]) {
        EntrantState e{};
        if (!DecodeEntrantState(r.body.data(), static_cast<uint32_t>(r.body.size()), &e)) {
            Fail("an SNSLobbyEntrantStatev2 body refused");
            break;
        }
        // The self-cross-check from test_events.cpp, applied to every instance rather than four.
        char want[64];
        std::snprintf(want, sizeof want, "DMO|%016llx",
                      static_cast<unsigned long long>(e.accountId));
        if (e.platformCode != 7 || std::string(e.platformStr.p, e.platformStr.n) != want) {
            Fail("an entrant body's platform string does not spell its account id");
            break;
        }
        ++n;
    }
    Pass("SNSLobbyEntrantStatev2 decoded and self-consistent", n);

    n = 0;
    std::set<uint64_t> spaces;
    std::vector<uint64_t> idColumn;
    for (const Record& r : byType[kSymNetIdsResp]) {
        NetIdSpace s{};
        if (!DecodeNetIds(r.body.data(), static_cast<uint32_t>(r.body.size()), &s)) {
            Fail("an SR15NetIdsResponse body refused the 8 + n*10 framing");
            break;
        }
        spaces.insert(s.space);
        for (uint16_t i = 0; i < s.count; ++i) {
            idColumn.push_back(NetIdAt(s, i));
        }
        ++n;
    }
    Pass("SR15NetIdsResponse framed as 8 + n*10", n);

    // ★ The header is an id-space key, and the request proves it: every SR15NetIdsRequest body is
    // exactly one response header. This is the derivation MESSAGE_COVERAGE.md cites, run rather than
    // asserted -- and it is what makes "there are two spaces" a finding instead of a reading.
    n = 0;
    for (const Record& q : byType[kSymNetIdsReq]) {
        if (q.body.size() != F::kNetIdsHeaderLen) {
            Fail("an SR15NetIdsRequest body is not exactly one response header long");
            break;
        }
        uint64_t key = 0;
        std::memcpy(&key, q.body.data(), sizeof key);
        if (spaces.find(key) == spaces.end()) {
            Fail("an SR15NetIdsRequest asked for a space no response header carries");
            break;
        }
        ++n;
    }
    if (n != spaces.size()) {
        Fail("the request and response id-space sets are not the same size");
    }
    Pass("every SR15NetIdsRequest body is a response's id-space key", n);

    n = 0;
    for (const Record& r : byType[kSymTeamStat]) {
        TeamStatUpdate t{};
        if (!DecodeTeamStatUpdate(r.body.data(), static_cast<uint32_t>(r.body.size()), &t) ||
            t.rowCount == 0) {
            Fail("an SR15NetReliableTeamStatUpdate body refused the 2 + n*20 framing");
            break;
        }
        ++n;
    }
    Pass("SR15NetReliableTeamStatUpdate framed as 2 + n*20", n);

    n = 0;
    for (const Record& r : byType[kSymStatUpdate]) {
        StatUpdate s{};
        if (!DecodeStatUpdate(r.body.data(), static_cast<uint32_t>(r.body.size()), &s)) {
            Fail("an SR15NetReliableStatUpdate body refused");
            break;
        }
        ++n;
    }
    Pass("SR15NetReliableStatUpdate decoded", n);

    n = 0;
    for (const Record& r : byType[kSymOwnerState]) {
        ByteView j{};
        if (!DecodeJsonBody(r.body.data(), static_cast<uint32_t>(r.body.size()), &j) || j.n == 0) {
            Fail("an SNSLobbyOwnerState body carried no NUL-terminated JSON");
            break;
        }
        ++n;
    }
    Pass("SNSLobbyOwnerState JSON located", n);

    // ---- the roster, driven by the whole corpus in order ------------------------------------
    {
        LobbyTracker tr;
        tr.Reset();
        replay::capture::MarkRec marks[8];
        struct Feed { uint64_t sym; LobbyKind kind; };
        const Feed feeds[] = {
            {kSymEntrantState, LobbyKind::EntrantState},
            {kSymEntrantTeam, LobbyKind::EntrantTeamChanged},
            {kSymNetIdsResp, LobbyKind::NetIds},
        };
        unsigned long fed = 0, emitted = 0;
        for (const Feed& f : feeds) {
            for (const Record& r : byType[f.sym]) {
                emitted += tr.Note(f.kind, r.body.data(), static_cast<uint32_t>(r.body.size()),
                                   0, marks, 8);
                ++fed;
            }
        }
        std::printf("roster: %u entrant(s) from %lu bodies, %lu marker(s), %u id space(s)\n",
                    tr.EntrantCount(), fed, emitted, tr.NetIdSpaceCount());
        if (tr.GetStats().bodiesRefused != 0) {
            Fail("the tracker refused a body the decoders accepted");
        }
        if (tr.GetStats().overflow != 0) {
            Fail("the roster overflowed");
        }
        // The reference capture is one human against AI fills, so exactly one entrant is the right
        // answer -- and it is the answer that a per-body JOIN would NOT have produced.
        if (tr.EntrantCount() != 1) {
            Fail("the reference capture has one human entrant and the roster disagrees");
        }
        if (tr.NetIdSpaceCount() != 2) {
            Fail("the reference capture carries two id spaces and the tracker disagrees");
        }
        Pass("roster derived over the whole corpus", fed);
    }

    // ---- the negative, with its denominator --------------------------------------------------
    {
        std::set<uint64_t> ids(idColumn.begin(), idColumn.end());
        unsigned long bodies = 0, offsets = 0, hits = 0;
        const uint64_t scanned[] = {kSymSimState, kSymPlayerState, kSymComponentEvt};
        for (uint64_t sym : scanned) {
            for (const Record& r : byType[sym]) {
                ++bodies;
                if (r.body.size() < 8) {
                    continue;
                }
                for (size_t off = 0; off + 8 <= r.body.size(); ++off) {
                    uint64_t v = 0;
                    std::memcpy(&v, r.body.data() + off, sizeof v);
                    ++offsets;
                    if (ids.find(v) != ids.end()) {
                        ++hits;
                    }
                }
            }
        }
        std::printf("negative: %lu distinct id-column values sought at %lu qword offsets "
                    "across %lu SimState/PlayerState/ComponentEvent bodies -> %lu hit(s)\n",
                    static_cast<unsigned long>(ids.size()), offsets, bodies, hits);
        if (bodies == 0 || offsets == 0) {
            Fail("the negative scan examined nothing, which is not a negative result");
        }
        // ★ The probe is proved before its zero is trusted: a value that IS in the set must be found by
        // the same scan. Without this, a broken comparison and a true negative look identical.
        if (!ids.empty()) {
            const uint64_t known = *ids.begin();
            std::vector<uint8_t> plant(16, 0);
            std::memcpy(plant.data() + 4, &known, sizeof known);
            bool found = false;
            for (size_t off = 0; off + 8 <= plant.size(); ++off) {
                uint64_t v = 0;
                std::memcpy(&v, plant.data() + off, sizeof v);
                if (ids.find(v) != ids.end()) {
                    found = true;
                }
            }
            if (!found) {
                Fail("the negative scan cannot find a value that is present: the probe is broken");
            } else {
                Pass("the negative scan finds a planted id-column value", 1);
            }
        }
        if (hits != 0) {
            Fail("an id-column value DOES appear in a per-frame body: the negative is withdrawn");
        }
    }

    std::printf("%s\n", failures == 0 ? "events_corpus: all arms green" : "events_corpus: FAILURES");
    return failures == 0 ? 0 : 1;
}
