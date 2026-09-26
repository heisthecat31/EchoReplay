// W1-C's offline-bit lock. The two things worth testing hardest are the two that could be quietly
// wrong: the scanner that decides WHICH instructions clear the bit, and the stub harness the runtime
// self-test uses to prove it found the right ones.
//
// Both are exercised with an input that must pass AND an input that must fail. A scanner only ever seen
// finding things has not been shown capable of not finding them, and a harness that always answers
// "cleared" would report a working lock over a broken one.
//
// ---------------------------------------------------------------------------------------------------
// TWO RULES FOR WHOEVER ADDS THE NEXT TEST HERE, with the reasons, because a rule without its reason
// gets tidied away by the next person who finds it odd.
//
// ★ 1. NO COMMAS IN TEST NAMES. doctest treats a comma in --test-case as a filter SEPARATOR, so a name
// containing one can never be selected by its full name - the run then executes ZERO tests and still
// prints "Status: SUCCESS!" and exits 0. That happened here: a filtered run of the whole-token matcher
// reported success having run nothing, and was caught only because "62 skipped" looked wrong.
//
// ★★ This DEFEATS the staleness gate rather than being caught by it. A freshly built binary running no
// tests is still a meaningless green, and no freshness check can see it. Before quoting any filtered
// run, read the EXECUTED count, not the status line.
//
// ★ 2. WHEN A POLICY GAINS A LEGITIMATE "OFF" STATE, EVERY CHECK WRITTEN UNDER THE OLD POLICY BECOMES A
// CHECK THAT FIRES ON CORRECT BEHAVIOUR. `hooksLive == hooksRequested` was right while the blocker
// armed in every session; once live-online-capture became a state where NOT armed is correct, that
// assertion failed sessions for behaving as designed - the same shape as asserting `refused == 0` over
// a blocker whose whole job is to refuse. Sweep your own assertions whenever a policy changes here.
// ---------------------------------------------------------------------------------------------------

#include <doctest.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bootstrap/bootstrap.h"
#include "bootstrap/config.h"
#include "bootstrap/egress.h"
#include "bootstrap/offlinelock.h"
#include "generated/facts_generated.h"
#include "offlinelock_report.h"
#include "../local_inputs.h"

namespace {

using replay::offlinelock::Site;

constexpr uint8_t kRexW = 0x48;
constexpr uint8_t kRexWB = 0x49;
constexpr uint8_t kAndImm8 = 0x83;
constexpr uint8_t kAndImm32 = 0x81;

// The mask the engine's teardown uses: everything except the offline bit and the host-alt bit beside it.
// Computed from the fact rather than written down, so the registry stays the single source for the bit.
constexpr uint8_t kClearingMask =
    static_cast<uint8_t>(~static_cast<uint8_t>(facts::SESSION_OFFLINE_FLAG_BIT) & ~0x02u);
// The same mask with the one bit that matters put back. This is what the runtime control flips.
constexpr uint8_t kPreservingMask =
    static_cast<uint8_t>(kClearingMask | static_cast<uint8_t>(facts::SESSION_OFFLINE_FLAG_BIT));

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

// `REX.W[+B] 83 /4 [reg+disp32], imm8`
void AppendAndImm8(std::vector<uint8_t>& out, uint8_t reg, uint32_t disp, uint8_t imm) {
    out.push_back(reg >= 8 ? kRexWB : kRexW);
    out.push_back(kAndImm8);
    out.push_back(static_cast<uint8_t>(0xA0 | (reg & 7)));
    AppendU32(out, disp);
    out.push_back(imm);
}

// `REX.W 81 /4 [reg+disp32], imm32`
void AppendAndImm32(std::vector<uint8_t>& out, uint8_t reg, uint32_t disp, uint32_t imm) {
    out.push_back(reg >= 8 ? kRexWB : kRexW);
    out.push_back(kAndImm32);
    out.push_back(static_cast<uint8_t>(0xA0 | (reg & 7)));
    AppendU32(out, disp);
    AppendU32(out, imm);
}

constexpr uint8_t kRbx = 3;
constexpr uint8_t kRdi = 7;
constexpr uint8_t kR8 = 8;

}  // namespace

TEST_CASE("the scanner finds what clears the offline bit and passes over what preserves it") {
    std::vector<uint8_t> text;

    // 1. The real shape: an AND whose mask drops the offline bit. MUST be found.
    const size_t clearerAt = text.size();
    AppendAndImm8(text, kRbx, facts::SESSION_OFFLINE_FLAG_OFF, kClearingMask);

    // 2. ★ The negative that matters. The image really contains four of these - they clear only the
    //    online bit and leave the offline bit alone - and a scanner that flagged them would refuse a
    //    perfectly good build. MUST NOT be found.
    AppendAndImm8(text, kRdi, facts::SESSION_OFFLINE_FLAG_OFF, kPreservingMask);

    // 3. The same displacement through an extended register. MUST be found.
    const size_t extendedAt = text.size();
    AppendAndImm8(text, kR8, facts::SESSION_OFFLINE_FLAG_OFF, kClearingMask);

    // 4. A clearing mask at a DIFFERENT offset - some other field entirely. MUST NOT be found.
    AppendAndImm8(text, kRbx, facts::SESSION_OFFLINE_FLAG_OFF + 0x10u, kClearingMask);

    // 5. The imm32 encoding of the same operation. MUST be found.
    const size_t wideAt = text.size();
    AppendAndImm32(text, kRbx, facts::SESSION_OFFLINE_FLAG_OFF, 0xFFFFFF00u | kClearingMask);

    Site sites[8] = {};
    const uint64_t base = 0x140000000ull;
    const size_t found =
        replay::offlinelock::FindClearSites(text.data(), text.size(), base, sites, 8);

    REQUIRE(found == 3);
    CHECK(sites[0].va == base + clearerAt);
    CHECK(sites[0].baseReg == kRbx);
    CHECK(sites[0].mask == kClearingMask);
    CHECK(sites[0].length == 8);

    CHECK(sites[1].va == base + extendedAt);
    CHECK(sites[1].baseReg == kR8);

    CHECK(sites[2].va == base + wideAt);
    CHECK(sites[2].length == 11);

    // Every site it returned must genuinely fail to preserve the bit. This is the property the whole
    // scan exists to decide, asserted rather than assumed from the count.
    for (size_t i = 0; i < found; ++i) {
        CAPTURE(i);
        CHECK((sites[i].mask & facts::SESSION_OFFLINE_FLAG_BIT) == 0);
    }
}

TEST_CASE("the scanner reports how many it found, not how many fitted") {
    // A count clamped to the output capacity would turn "this image surprised us" into "everything is
    // as expected", which is the one reading that must never be produced by accident.
    std::vector<uint8_t> text;
    for (int i = 0; i < 5; ++i) {
        AppendAndImm8(text, kRbx, facts::SESSION_OFFLINE_FLAG_OFF, kClearingMask);
    }
    Site sites[2] = {};
    const size_t found =
        replay::offlinelock::FindClearSites(text.data(), text.size(), 0x140000000ull, sites, 2);
    CHECK(found == 5);
}

TEST_CASE("the scanner finds nothing in bytes that hold no such instruction") {
    // The green case has to be constructible too: if this found something, every count above is noise.
    std::vector<uint8_t> text(4096, 0x90);
    Site sites[8] = {};
    CHECK(replay::offlinelock::FindClearSites(text.data(), text.size(), 0x140000000ull, sites, 8) == 0);
    CHECK(replay::offlinelock::FindClearSites(nullptr, 0, 0x140000000ull, sites, 8) == 0);
}

TEST_CASE("the stub harness runs a located instruction and can report both outcomes") {
    // ★ This is the runtime self-test's instrument, and it is the reason the lock can claim it found
    // the right bytes rather than merely bytes matching a pattern. If both halves answered the same
    // way, the control would be decoration.
    std::vector<uint8_t> clearing;
    AppendAndImm8(clearing, kRbx, facts::SESSION_OFFLINE_FLAG_OFF, kClearingMask);

    std::vector<uint8_t> preserving;
    AppendAndImm8(preserving, kRbx, facts::SESSION_OFFLINE_FLAG_OFF, kPreservingMask);

    const uint64_t seed = ~0ull;
    uint64_t afterClearing = 0;
    uint64_t afterPreserving = 0;

    REQUIRE(replay::offlinelock::RunSiteOnScratch(clearing.data(), clearing.size(), kRbx, seed,
                                                  &afterClearing));
    REQUIRE(replay::offlinelock::RunSiteOnScratch(preserving.data(), preserving.size(), kRbx, seed,
                                                  &afterPreserving));

    CHECK((afterClearing & facts::SESSION_OFFLINE_FLAG_BIT) == 0);
    CHECK((afterPreserving & facts::SESSION_OFFLINE_FLAG_BIT) != 0);

    // And the two masks differ in exactly the one bit under discussion, so the control is isolating the
    // offline bit rather than some other change that happens to travel with it.
    CHECK((afterClearing ^ afterPreserving) == facts::SESSION_OFFLINE_FLAG_BIT);
}

TEST_CASE("the stub harness drives an extended base register as well as a low one") {
    // The image's teardown uses rbx today. Nothing guarantees the next build does, and a harness that
    // only handled rbx would silently mis-execute the site it was handed.
    std::vector<uint8_t> insn;
    AppendAndImm8(insn, kR8, facts::SESSION_OFFLINE_FLAG_OFF, kClearingMask);
    uint64_t after = 0;
    REQUIRE(replay::offlinelock::RunSiteOnScratch(insn.data(), insn.size(), kR8, ~0ull, &after));
    CHECK((after & facts::SESSION_OFFLINE_FLAG_BIT) == 0);
}

TEST_CASE("the stub harness refuses an input it cannot encode") {
    uint64_t after = 0;
    std::vector<uint8_t> insn;
    AppendAndImm8(insn, kRbx, facts::SESSION_OFFLINE_FLAG_OFF, kClearingMask);
    CHECK_FALSE(replay::offlinelock::RunSiteOnScratch(nullptr, 8, kRbx, 0, &after));
    CHECK_FALSE(replay::offlinelock::RunSiteOnScratch(insn.data(), 0, kRbx, 0, &after));
    CHECK_FALSE(replay::offlinelock::RunSiteOnScratch(insn.data(), insn.size(), 16, 0, &after));
    CHECK_FALSE(replay::offlinelock::RunSiteOnScratch(insn.data(), insn.size(), kRbx, 0, nullptr));
}

TEST_CASE("the lock names its states and separates armed from exercised") {
    using replay::offlinelock::Exercise;
    using replay::offlinelock::LockState;
    CHECK(std::string(replay::offlinelock::LockStateName(LockState::NotAttempted)) == "not-attempted");
    CHECK(std::string(replay::offlinelock::LockStateName(LockState::Locked)) == "locked");
    CHECK(std::string(replay::offlinelock::LockStateName(LockState::RefusedNoSites)) ==
          "refused-no-sites");
    CHECK(std::string(replay::offlinelock::LockStateName(LockState::RefusedSelfTest)) ==
          "refused-self-test");

    // ★ The distinction the harness must never collapse: a lock that was never called on is not a lock
    // that held.
    CHECK(std::string(replay::offlinelock::ExerciseName(Exercise::NotExercised)) == "not-exercised");
    CHECK(std::string(replay::offlinelock::ExerciseName(Exercise::HeldUnderExercise)) ==
          "held-under-exercise");
    CHECK(std::string(replay::offlinelock::ExerciseName(Exercise::RanWithNothingToHold)) ==
          "ran-with-nothing-to-hold");
    CHECK(std::string(replay::offlinelock::ExerciseName(Exercise::Failed)) == "failed");

    replay::offlinelock::ResetForTest();
    CHECK(replay::offlinelock::Current().state == LockState::NotAttempted);
    CHECK(replay::offlinelock::Exercised() == Exercise::NotExercised);
    CHECK(replay::offlinelock::Current().calls == 0);
}

// ★ The one check that reaches the real binary. Everything above proves the scanner behaves as
// specified; this proves the specification matches the executable we actually ship against - which is
// the half a synthetic test can never reach.
TEST_CASE("the scanner finds exactly the expected clearing sites in the real echovr.exe") {
    const std::wstring exe = replay_test::FindEchoVrExeW();
    if (exe.empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE); the real-image scan did not run");
        return;
    }
    HANDLE file = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        MESSAGE("echovr.exe is not on this machine; the real-image scan did not run");
        return;
    }
    LARGE_INTEGER size{};
    REQUIRE(GetFileSizeEx(file, &size));
    std::vector<uint8_t> image(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, image.data(), static_cast<DWORD>(image.size()), &read, nullptr);
    CloseHandle(file);
    REQUIRE(ok);
    REQUIRE(read == image.size());

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    REQUIRE(dos->e_magic == IMAGE_DOS_SIGNATURE);
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    REQUIRE(nt->Signature == IMAGE_NT_SIGNATURE);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    const uint8_t* text = nullptr;
    size_t textSize = 0;
    uint64_t textVa = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            text = image.data() + sec[i].PointerToRawData;
            textSize = sec[i].SizeOfRawData;
            textVa = nt->OptionalHeader.ImageBase + sec[i].VirtualAddress;
        }
    }
    REQUIRE(text != nullptr);

    Site sites[8] = {};
    const size_t found = replay::offlinelock::FindClearSites(text, textSize, textVa, sites, 8);

    // Two, because the teardown has two exits and each clears the flags on its way out. A different
    // number means the image is not the one this lock was designed against.
    CHECK(found == 2);
    for (size_t i = 0; i < found && i < 8; ++i) {
        CAPTURE(i);
        CAPTURE(sites[i].va);
        CHECK((sites[i].mask & facts::SESSION_OFFLINE_FLAG_BIT) == 0);
        // The host-alt bit beside it is NOT ours to make permanent: the engine's host predicate reads
        // it, so the lock restores one bit and leaves this one exactly as the teardown left it.
        CHECK((sites[i].mask & 0x02u) == 0);
        CHECK(sites[i].length == 8);
    }
    // Both exits belong to one routine, which is what makes a single hook sufficient.
    if (found == 2) {
        CHECK(sites[0].baseReg == sites[1].baseReg);
        CHECK(sites[0].va != sites[1].va);
    }
}

// ---------------------------------------------------------------------------------------------------
// The artifact.
//
// BLOCK-1 shipped a done.json that no parser would accept, and it shipped because the probe DLL refuses
// to load outside echovr.exe, so nothing offline had ever produced the document a launch depends on.
// The builder is now a pure function and these cases are the parse that was missing.
// ---------------------------------------------------------------------------------------------------
namespace {

// Structural JSON validation: balanced containers, correctly terminated strings, no control characters
// loose in a string. Deliberately not a full parser - what BLOCK-1 produced failed on exactly these.
struct JsonScan {
    bool   ok;
    size_t depth;
};

JsonScan ValidateJson(const char* text, size_t len) {
    size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    bool sawTopLevel = false;
    for (size_t i = 0; i < len; ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            } else if (static_cast<unsigned char>(c) < 0x20u) {
                return {false, depth};   // a raw control character inside a string
            }
            continue;
        }
        switch (c) {
            case '"': inString = true; break;
            case '{':
            case '[': ++depth; sawTopLevel = true; break;
            case '}':
            case ']':
                if (depth == 0) return {false, depth};
                --depth;
                break;
            default: break;
        }
    }
    return {depth == 0 && !inString && sawTopLevel, depth};
}

w1c::EgressView SampleEgress(uint32_t dests) {
    w1c::EgressView e{};
    e.reported = true;
    std::snprintf(e.state, sizeof e.state, "%s", "armed");
    std::snprintf(e.hooks, sizeof e.hooks, "%s", "\"connect\", \"sendto\"");
    e.armedAtMs = 157;
    e.hooksLive = 16;
    e.hooksRequested = 16;
    e.refused = 47;
    e.allowedLocal = 21;
    e.distinct = dests;
    for (uint32_t i = 0; i < dests && i < w1c::kMaxDestinations; ++i) {
        std::snprintf(e.dest[i].text, sizeof e.dest[i].text, "host%u.example.net:443", i);
        std::snprintf(e.dest[i].via, sizeof e.dest[i].via, "%s", "connect");
        e.dest[i].count = i + 1;
        e.dest[i].local = false;
        ++e.destCount;
    }
    return e;
}

w1c::Reads SampleReads(size_t series) {
    w1c::Reads r{};
    r.clockSeen = true;
    r.clockPtr = 0x1234567890ull;
    r.minElapsedRead = true;
    r.minElapsedUs = 1666;
    r.maxElapsedRead = true;
    r.maxElapsedUs = facts::CLOCK_MAXELAPSED_US;
    r.firstSeenAtMs = 4200;
    r.deltaSeen = true;
    r.deltaMinUs = 11000;
    r.deltaMaxUs = 17000;
    r.deltaLastUs = 13888;
    r.deltaSamples = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a sample count, not that offset
    r.gameSeen = true;
    r.gameClockSeen = true;
    for (size_t i = 0; i < series && i < w1c::kMaxSeries; ++i) {
        r.series[i].atMs = 1000 * (i + 1);
        r.series[i].value = 1.0f - static_cast<float>(i) * 0.01f;
    }
    r.seriesLen = series < w1c::kMaxSeries ? series : w1c::kMaxSeries;

    // Two read sites with OPPOSITE outcomes, because the whole point of the record is that a refused
    // read and a clean read of zero are different facts.
    r.moduleBase = 0x7FF612340000ull;
    r.imageBase = 0x140000000ull;
    std::snprintf(r.sites[0].name, sizeof r.sites[0].name, "%s", "clock.global");
    r.sites[0].preferredVa = facts::CLOCK_GLOBAL_PTR;
    r.sites[0].absoluteVa = 0x7FF6143400E8ull;
    r.sites[0].state = 0x1000;
    r.sites[0].protect = 0x04;
    r.sites[0].value = 0x1234;
    r.sites[0].attempts = 3;
    r.sites[0].outcome = w1c::ReadOutcome::ReadOk;
    std::snprintf(r.sites[1].name, sizeof r.sites[1].name, "%s", "game.singleton");
    r.sites[1].preferredVa = facts::GAME_SINGLETON_VA;
    r.sites[1].absoluteVa = 0x7FF614340478ull;
    r.sites[1].outcome = w1c::ReadOutcome::NotCommitted;
    r.sites[1].attempts = 600;
    r.siteCount = 2;
    return r;
}

replay::offlinelock::Report SampleReport() {
    replay::offlinelock::Report lk{};
    lk.state = replay::offlinelock::LockState::Locked;
    lk.sitesFound = 2;
    lk.sitesExpected = 2;
    lk.sitesRecorded = 2;
    lk.functionVa = 0x1401042B0ull;
    lk.scanMs = 12;
    lk.scanBytes = 23859143;
    lk.selfTestRan = true;
    lk.selfTestClearedByOriginal = true;
    lk.selfTestHeldWhenPreserved = true;
    lk.installedHere = true;
    lk.instrumentShared = true;
    return lk;
}

}  // namespace

TEST_CASE("the offline-local probe's artifact is valid JSON in every configuration it can reach") {
    // The size the probe actually ships, so this exercises the configuration that will run rather
    // than a smaller one that would pass while the real one truncated.
    static char body[32768];
    for (const size_t series : {size_t{0}, size_t{1}, size_t{7}, w1c::kMaxSeries}) {
        CAPTURE(series);
        const size_t len = w1c::BuildDoneJson(body, sizeof body, "W1-C-offlinelock", "PASS",
                                              "the offline bit cannot be cleared", 7, 0,
                                              SampleReport(),
                                              replay::offlinelock::Exercise::HeldUnderExercise,
                                              SampleReads(series),
                                              SampleEgress(static_cast<uint32_t>(series)));
        REQUIRE(len > 0);
        REQUIRE(len < sizeof body);
        const JsonScan scan = ValidateJson(body, len);
        CHECK(scan.ok);
        const std::string doc(body, len);
        CHECK(doc.find("\"truncated\": false") != std::string::npos);
        CHECK(doc.find("\"exercise\": \"held-under-exercise\"") != std::string::npos);
        CHECK(doc.find("\"minElapsedUs\": 1666") != std::string::npos);
        CHECK(doc.back() == '\n');
    }
}

TEST_CASE("a document that does not fit still closes, and says it was cut") {
    // ★ The red case. A size limit must cost a FIELD, never the document - that is the whole lesson of
    // BLOCK-1, and a claim nothing exercises is a claim nobody has checked.
    char small[700];
    const size_t len = w1c::BuildDoneJson(small, sizeof small, "W1-C-offlinelock", "PASS",
                                          "a note long enough to crowd the buffer on its own, which is "
                                          "exactly the case that used to cut the array mid-token and "
                                          "leave the harness with nothing it could parse at all",
                                          7, 0, SampleReport(),
                                          replay::offlinelock::Exercise::HeldUnderExercise,
                                          SampleReads(w1c::kMaxSeries),
                                          SampleEgress(w1c::kMaxDestinations));
    REQUIRE(len > 0);
    REQUIRE(len <= sizeof small);
    const JsonScan scan = ValidateJson(small, len);
    CHECK(scan.ok);
    const std::string doc(small, len);
    CHECK(doc.find("\"truncated\": true") != std::string::npos);
    CHECK(doc.back() == '\n');
}

TEST_CASE("refused destinations are named in the artifact, not just counted") {
    // ★ The whole point of the change. A count says the offline path is contained; only the list says
    // what it was reaching for, and layer 1 cannot be extended from a number.
    static char body[32768];
    const size_t len = w1c::BuildDoneJson(body, sizeof body, "W1-C-offlinelock", "PASS", "note", 7, 0,
                                          SampleReport(),
                                          replay::offlinelock::Exercise::HeldUnderExercise,
                                          SampleReads(2), SampleEgress(3));
    REQUIRE(len > 0);
    const std::string doc(body, len);
    CHECK(ValidateJson(body, len).ok);
    CHECK(doc.find("\"checked\": 7") != std::string::npos);
    CHECK(doc.find("\"refused\": 47") != std::string::npos);
    CHECK(doc.find("\"destinationsEmitted\": 3") != std::string::npos);
    for (const char* host : {"host0.example.net:443", "host1.example.net:443",
                             "host2.example.net:443"}) {
        CAPTURE(host);
        CHECK(doc.find(host) != std::string::npos);
    }
    CHECK(doc.find("\"via\": \"connect\"") != std::string::npos);
}

TEST_CASE("a refused read and a clean read of zero are different words in the artifact") {
    // ★ Three launches reported four booleans that could not tell "we would not touch that address"
    // from "the engine had not built it yet". The record now says which, and this pins that it does.
    static char body[32768];
    const size_t len = w1c::BuildDoneJson(body, sizeof body, "W1-C-offlinelock", "PASS", "note", 7, 0,
                                          SampleReport(),
                                          replay::offlinelock::Exercise::HeldUnderExercise,
                                          SampleReads(1), SampleEgress(1));
    REQUIRE(len > 0);
    CHECK(ValidateJson(body, len).ok);
    const std::string doc(body, len);
    CHECK(doc.find("\"relocated\": true") != std::string::npos);
    CHECK(doc.find("\"outcome\": \"read-ok\"") != std::string::npos);
    CHECK(doc.find("\"outcome\": \"not-committed\"") != std::string::npos);
    CHECK(doc.find("\"name\": \"clock.global\"") != std::string::npos);
    CHECK(doc.find("\"name\": \"game.singleton\"") != std::string::npos);
    // The address actually dereferenced travels beside the one the registry states. Derived from the
    // same constant rather than written out in decimal: a hand-converted number is a second source that
    // can disagree with the first, and this file has already had one do exactly that.
    const std::string expectAbs = "\"absoluteVa\": " + std::to_string(0x7FF6143400E8ull);
    CHECK(doc.find(expectAbs) != std::string::npos);
    const std::string expectPref =
        "\"preferredVa\": " + std::to_string(static_cast<uint64_t>(facts::CLOCK_GLOBAL_PTR));
    CHECK(doc.find(expectPref) != std::string::npos);

    // Every outcome must have a distinct name, or the record collapses the way the booleans did.
    const w1c::ReadOutcome all[] = {
        w1c::ReadOutcome::NotAttempted, w1c::ReadOutcome::QueryFailed,
        w1c::ReadOutcome::NotCommitted, w1c::ReadOutcome::NotReadable,
        w1c::ReadOutcome::GuardPage,    w1c::ReadOutcome::ReadNull,
        w1c::ReadOutcome::ReadOk};
    std::vector<std::string> names;
    for (const w1c::ReadOutcome o : all) {
        names.push_back(w1c::ReadOutcomeName(o));
    }
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());
}

TEST_CASE("a destination list that does not fit keeps the ones that did") {
    // The red case for the rewind. Losing the TAIL of the list must not cost the destinations that fit
    // NOR the egress block around them - the first version of this code discarded both.
    // Self-calibrating: measure the document with an EMPTY destination list, then allow room for a few
    // entries but nowhere near 128. Guessing a byte count here would make the test rot the moment a
    // field is added, and a version of it that guessed wrong dropped the whole egress member instead of
    // the tail - which is a different, also-correct behaviour, and not the one under test.
    static char big[32768];
    const size_t baseLen = w1c::BuildDoneJson(big, sizeof big, "W1-C-offlinelock", "PASS", "note", 7, 0,
                                              SampleReport(),
                                              replay::offlinelock::Exercise::HeldUnderExercise,
                                              SampleReads(0), SampleEgress(0));
    REQUIRE(baseLen > 0);

    std::vector<char> small(baseLen + 400); // facts-ok: AUDIO.LISTENER_COUNT_OFF a buffer length, not that offset
    const size_t len = w1c::BuildDoneJson(small.data(), small.size(), "W1-C-offlinelock", "PASS",
                                          "note", 7, 0, SampleReport(),
                                          replay::offlinelock::Exercise::HeldUnderExercise,
                                          SampleReads(0),
                                          SampleEgress(w1c::kMaxDestinations));
    REQUIRE(len > 0);
    REQUIRE(len <= small.size());
    const std::string doc(small.data(), len);
    CHECK(ValidateJson(small.data(), len).ok);
    CHECK(doc.find("\"truncated\": true") != std::string::npos);
    // The egress member survived the cut, and it says how many of the 128 actually made it.
    CHECK(doc.find("\"refused\": 47") != std::string::npos);
    CHECK(doc.find("\"distinct\": 128") != std::string::npos);
    CHECK(doc.find("host0.example.net:443") != std::string::npos);
    const size_t at = doc.find("\"destinationsEmitted\": ");
    REQUIRE(at != std::string::npos);
    const int emitted = std::atoi(doc.c_str() + at + std::strlen("\"destinationsEmitted\": "));
    CHECK(emitted > 0);
    CHECK(emitted < static_cast<int>(w1c::kMaxDestinations));
}

TEST_CASE("the validator this file trusts can tell bad JSON from good") {
    // Without this, every "valid JSON" assertion above is only a statement about a function that might
    // return true unconditionally.
    const char* good = "{ \"a\": [1, 2], \"b\": \"x\" }";
    CHECK(ValidateJson(good, std::strlen(good)).ok);

    // The exact shape BLOCK-1 shipped: an array cut mid-token, so a container never closes.
    const char* cut = "{ \"hooks\": [\"InternetConnectA\", \"I], \"hooksLive\": 16 }";
    CHECK_FALSE(ValidateJson(cut, std::strlen(cut)).ok);

    const char* unbalanced = "{ \"a\": [1, 2 }";
    CHECK_FALSE(ValidateJson(unbalanced, std::strlen(unbalanced)).ok);
    const char* dangling = "{ \"a\": \"unterminated";
    CHECK_FALSE(ValidateJson(dangling, std::strlen(dangling)).ok);
    const char* rawControl = "{ \"a\": \"x\ty\" }";
    CHECK_FALSE(ValidateJson(rawControl, std::strlen(rawControl)).ok);
}

TEST_CASE("the egress switch is a tri-state whose default lets the session mode decide") {
    // ★ Two defects sit behind this one line. It first read `inSweep || GetBool(key, false)`, so a
    // shipped viewer phoned out freely and only the harness ever saw a refusal - 82 destinations in one
    // 151 s session. Defaulting it to armed then broke the owner's PRIMARY use case: this plugin
    // captures online matches, and there the traffic is the subject of the recording.
    //
    // A bool cannot express "no opinion", and IConfig::GetBool ignores a caller's default for a declared
    // key - so an absent key and a false one are indistinguishable. A tri-state string can say it.
    replay::config::ResetForTest();
    const replay::IConfig* cfg = replay::config::Interface();
    REQUIRE(cfg->Has("bootstrap.block_egress"));
    CHECK(std::string(cfg->GetString("bootstrap.block_egress", "auto")) == "auto");
}

TEST_CASE("every reason the blocker is or is not armed has its own word") {
    using replay::egress::ArmTrigger;
    // "Armed", "armed only because this is a test run", and "not armed because this is a capture and
    // the network is what we are recording" printed identically before. That is how a default which
    // could not hold in production survived every launch this project has taken.
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::ReplayMode)) == "replay-mode");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::SweepOverride)) == "sweep-override");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::ConfigForcedOn)) ==
          "config-forced-on");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::ModeUnknown)) == "mode-unknown");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::OfflineHint)) == "offline-hint");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::ReplayHint)) == "replay-hint");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::OfflineSession)) ==
          "offline-session");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::LiveOnlineCapture)) ==
          "live-online-capture");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::DisabledByConfig)) ==
          "disabled-by-config");
    CHECK(std::string(replay::egress::ArmTriggerName(ArmTrigger::Unspecified)) == "unspecified");

    std::vector<std::string> names;
    for (const ArmTrigger t : {ArmTrigger::Unspecified, ArmTrigger::ReplayMode,
                               ArmTrigger::SweepOverride, ArmTrigger::ConfigForcedOn,
                               ArmTrigger::ModeUnknown, ArmTrigger::OfflineHint,
                               ArmTrigger::ReplayHint, ArmTrigger::OfflineSession,
                               ArmTrigger::LiveOnlineCapture,
                               ArmTrigger::DisabledByConfig}) {
        names.push_back(replay::egress::ArmTriggerName(t));
    }
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());

    // RecordNotArmed states the reason without arming anything, so a run that deliberately leaves
    // egress open still says so rather than reporting an absent trigger.
    replay::egress::ResetForTest();
    replay::egress::RecordNotArmed(ArmTrigger::DisabledByConfig);
    CHECK(replay::egress::Current().trigger == ArmTrigger::DisabledByConfig);
    CHECK(replay::egress::Current().state == replay::egress::ArmState::NotAttempted);
    CHECK(replay::egress::Current().hooksLive == 0);
    replay::egress::ResetForTest();
}

TEST_CASE("a capture session that later begins playback can still arm") {
    // ★ The bug this pins is one I nearly shipped. RecordNotArmed originally set `attempted`, "so a
    // later Arm() cannot contradict the decision already recorded" - which is exactly wrong once the
    // mode decides: a capture session that opens a replay must arm at that point, and the flag would
    // have refused it in silence. Not-armed-yet and already-decided are different states.
    using replay::egress::ArmTrigger;
    replay::egress::ResetForTest();
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    replay::egress::RecordNotArmed(ArmTrigger::LiveOnlineCapture);
    REQUIRE(replay::egress::Current().state == replay::egress::ArmState::NotAttempted);
    CHECK(replay::egress::Current().trigger == ArmTrigger::LiveOnlineCapture);

    // Playback begins. The blocker must take, and the report must now say why it is armed.
    const replay::egress::ArmState later = replay::egress::Arm(ArmTrigger::ReplayMode);
    CHECK(later == replay::egress::ArmState::Armed);
    CHECK(replay::egress::Current().state == replay::egress::ArmState::Armed);
    CHECK(replay::egress::Current().trigger == ArmTrigger::ReplayMode);
    CHECK(replay::egress::Current().hooksLive > 0);

    WSACleanup();
    replay::egress::ResetForTest();
}

TEST_CASE("the switch match is whole-token, so it cannot arm on a switch that is not there") {
    // ★ This decides whether the network blocker arms. A substring match would see -offline inside
    // -offlinefoo and arm for a reason that is not present; a match that required start-of-string would
    // miss it in a real command line. Both directions are checked because both are reachable.
    using replay::bootstrap::HasSwitch;

    // Present, in the positions a real command line puts it.
    CHECK(HasSwitch(L"-offline", L"-offline"));
    CHECK(HasSwitch(L"echovr.exe -offline -windowed", L"-offline"));
    CHECK(HasSwitch(L"-windowed -offline", L"-offline"));
    CHECK(HasSwitch(L"-level mpl_arena_a -offline -gametype echo_arena", L"-offline"));
    CHECK(HasSwitch(L"-offline=1", L"-offline"));
    CHECK(HasSwitch(L"\"-offline\"", L"-offline"));
    CHECK(HasSwitch(L"-OFFLINE", L"-offline"));            // the parser is case-insensitive
    CHECK(HasSwitch(L"-Offline -x", L"-offline"));

    // ★ Absent, including the near-misses a substring match would get wrong.
    CHECK_FALSE(HasSwitch(L"-offlinefoo", L"-offline"));
    CHECK_FALSE(HasSwitch(L"-x-offline", L"-offline"));
    CHECK_FALSE(HasSwitch(L"-nooffline", L"-offline"));
    CHECK_FALSE(HasSwitch(L"-windowed -level mpl_arena_a", L"-offline"));
    CHECK_FALSE(HasSwitch(L"", L"-offline"));

    // The real profile that produced 82 refusals: it carries -offline and NOT -replayviewer, which is
    // why keying on -replayviewer classified the session that most needed the blocker as not needing it.
    const wchar_t* offlineLocal =
        L"echovr.exe -region usc -offline -windowed -level mpl_arena_a -gametype echo_arena "
        L"-loginhost 127.0.0.1 -confighost 127.0.0.1";
    CHECK(HasSwitch(offlineLocal, L"-offline"));
    CHECK_FALSE(HasSwitch(offlineLocal, L"-replayviewer"));

    // Null and empty inputs answer rather than fault.
    CHECK_FALSE(HasSwitch(nullptr, L"-offline"));
    CHECK_FALSE(HasSwitch(L"-offline", nullptr));
    CHECK_FALSE(HasSwitch(L"-offline", L""));
}

TEST_CASE("an arm taken on a hint is releasable, and one taken on a fact is not") {
    // ★ -offline is not evidence that the session is offline. It is absent from stock echovr.exe and
    // exists only because rlay.dll patches it in, so "plugin loaded, -offline passed, rlay absent" is a
    // session that goes ONLINE while the switch says otherwise. Arming on that hint is right - unknown
    // fails closed - but it MUST be given up when the state contradicts it, or the plugin cuts a live
    // match off from its server. What must never be given up is an arm taken on an observed fact.
    using replay::egress::ArmState;
    using replay::egress::ArmTrigger;

    CHECK(replay::egress::TriggerIsReleasable(ArmTrigger::OfflineHint));
    CHECK(replay::egress::TriggerIsReleasable(ArmTrigger::ReplayHint));
    CHECK(replay::egress::TriggerIsReleasable(ArmTrigger::ModeUnknown));
    CHECK_FALSE(replay::egress::TriggerIsReleasable(ArmTrigger::OfflineSession));
    CHECK_FALSE(replay::egress::TriggerIsReleasable(ArmTrigger::ReplayMode));
    CHECK_FALSE(replay::egress::TriggerIsReleasable(ArmTrigger::SweepOverride));
    CHECK_FALSE(replay::egress::TriggerIsReleasable(ArmTrigger::ConfigForcedOn));

    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    SUBCASE("a hint arm is released, and the blocker stops refusing without unhooking") {
        replay::egress::ResetForTest();
        REQUIRE(replay::egress::Arm(ArmTrigger::OfflineHint) == ArmState::Armed);
        CHECK(replay::egress::Enforcing());

        // Enforcing: a non-loopback destination is refused.
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(s != INVALID_SOCKET);
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(80);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
        CHECK(connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof remote) == SOCKET_ERROR);
        CHECK(WSAGetLastError() == WSAENETUNREACH);
        closesocket(s);
        const uint64_t refusedWhileEnforcing = replay::egress::Current().refusedRemote;
        CHECK(refusedWhileEnforcing >= 1);

        // The state contradicts the hint: this is a live online match after all.
        CHECK(replay::egress::Release(ArmTrigger::LiveOnlineCapture));
        CHECK_FALSE(replay::egress::Enforcing());
        CHECK(replay::egress::Current().released);
        CHECK(replay::egress::Current().releaseReason == ArmTrigger::LiveOnlineCapture);

        // ★ The hooks are STILL installed - the report still says armed - but nothing is refused now.
        CHECK(replay::egress::Current().state == ArmState::Armed);
        CHECK(replay::egress::Current().hooksLive > 0);

        // ★ And what it already refused is not rewritten by the release.
        CHECK(replay::egress::Current().refusedBeforeRelease == refusedWhileEnforcing);
        CHECK(replay::egress::Current().refusedRemote >= refusedWhileEnforcing);

        // Idempotent.
        CHECK(replay::egress::Release(ArmTrigger::LiveOnlineCapture));
        replay::egress::ResetForTest();
    }

    SUBCASE("an arm taken on observed state refuses to be released") {
        replay::egress::ResetForTest();
        REQUIRE(replay::egress::Arm(ArmTrigger::SweepOverride) == ArmState::Armed);
        CHECK(replay::egress::Enforcing());
        CHECK_FALSE(replay::egress::Release(ArmTrigger::LiveOnlineCapture));
        CHECK(replay::egress::Enforcing());           // still guarded
        CHECK_FALSE(replay::egress::Current().released);
        replay::egress::ResetForTest();
    }

    SUBCASE("releasing something that never armed is not a release") {
        replay::egress::ResetForTest();
        CHECK_FALSE(replay::egress::Release(ArmTrigger::LiveOnlineCapture));
        CHECK_FALSE(replay::egress::Current().released);
    }

    WSACleanup();
    replay::egress::ResetForTest();
}

TEST_CASE("a -offline the engine will ignore is not a credible hint") {
    // ★★ The failure this prevents: plugin installed, -offline passed, rlay.dll absent. The engine
    // never parses -offline - it is not in stock echovr.exe at all, as ASCII or UTF-16 - so the session
    // goes ONLINE, and a blocker armed on the switch would cut the player's own match off from its
    // server.
    //
    // ★ Judged BEFORE arming, not corrected after. Arming on the hint and releasing when the session
    // state disagrees is a deadlock: the arm blocks the login and matchmaking traffic that produces the
    // state, so the observation that would release it can never happen. Neither input here needs a
    // socket, so this predicate has no such circularity.
    using replay::bootstrap::OfflineHintIsCredible;

    const wchar_t* withOffline =
        L"echovr.exe -region usc -offline -windowed -level mpl_arena_a -gametype echo_arena";
    const wchar_t* withoutOffline = L"echovr.exe -region usc -windowed -level mpl_arena_a";

    // Credible only when BOTH the switch is present and the module that implements it is loaded.
    CHECK(OfflineHintIsCredible(withOffline, true));
    CHECK_FALSE(OfflineHintIsCredible(withOffline, false));    // ★ the case that must not arm
    CHECK_FALSE(OfflineHintIsCredible(withoutOffline, true));
    CHECK_FALSE(OfflineHintIsCredible(withoutOffline, false));

    // The near-misses the whole-token matcher exists for still do not become credible.
    CHECK_FALSE(OfflineHintIsCredible(L"echovr.exe -offlinefoo", true));
    CHECK_FALSE(OfflineHintIsCredible(L"echovr.exe -nooffline", true));

    // Null and empty answer rather than fault.
    CHECK_FALSE(OfflineHintIsCredible(nullptr, true));
    CHECK_FALSE(OfflineHintIsCredible(L"", true));
}

TEST_CASE("the late rlay re-check can arm a session the first look left unguarded") {
    // ★★★ The sequence this pins is the COMMON one, not an edge case. Plugins load alphabetically, so
    // echoreplay.dll is reached before rlay.dll: the first look finds no rlay, disbelieves a -offline
    // that is in fact credible, and records live-online-capture. If that decision were final the
    // session would run unguarded, which is the fail-open direction on the one guard that matters.
    //
    // What makes the second look possible is that RecordNotArmed does not mark the blocker attempted.
    // If it did, the re-check would call Arm and be refused in silence.
    using replay::egress::ArmState;
    using replay::egress::ArmTrigger;
    replay::egress::ResetForTest();
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    // First look: rlay absent, so the switch is not believed.
    CHECK_FALSE(replay::bootstrap::OfflineHintIsCredible(L"echovr.exe -offline", false));
    replay::egress::RecordNotArmed(ArmTrigger::LiveOnlineCapture);
    REQUIRE(replay::egress::Current().state == ArmState::NotAttempted);
    CHECK_FALSE(replay::egress::Enforcing());

    // rlay appears. The same switch is now credible, and the blocker arms late.
    CHECK(replay::bootstrap::OfflineHintIsCredible(L"echovr.exe -offline", true));
    CHECK(replay::egress::Arm(ArmTrigger::OfflineHint) == ArmState::Armed);
    CHECK(replay::egress::Enforcing());
    CHECK(replay::egress::Current().trigger == ArmTrigger::OfflineHint);

    // ★ And it is still a HINT, so a later observation of live-online state can still give it up.
    CHECK(replay::egress::TriggerIsReleasable(replay::egress::Current().trigger));
    CHECK(replay::egress::Release(ArmTrigger::LiveOnlineCapture));
    CHECK_FALSE(replay::egress::Enforcing());

    WSACleanup();
    replay::egress::ResetForTest();
}
