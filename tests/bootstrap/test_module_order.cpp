// DIAG-SEAT-ORDER's invariant, asserted instead of assumed.
//
// Two orderings in the module registry are load-bearing, and both are checked here:
//   - residue-check inits between capture and playback. It is the sole writer of the
//     ReplayServices pointer playback's Init reads back through ArmPlaybackResidue
//     (residue_module.cpp:95-96 -> playback_module.cpp:2253), so after playback it is useless.
//   - capture's recording phase is published from the callback it registers EARLY, not from its
//     modreg tick, which runs after every early callback (src/engine/tick.cpp:247-252). That is
//     what lets a same-frame reader of CaptureIsRecording() see the arming edge. (Playback's seat
//     dropped its capture term at 9a8779a; capture's own phase consumers are what this carries.)
//
// The order is driven through the registry with the real constants. Where the two calls sit inside
// their functions is a fact about translation units this suite does not link, so it is read from
// the sources -- the same route test_config.cpp already uses for capture's stream table.

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <doctest.h>

#include "bootstrap/modreg.h"
#include "bootstrap/module_order.h"
#include "bootstrap/services.h"

namespace {

std::vector<std::string> g_initOrder;

bool CaptureInit(replay::ReplayServices*) {
    g_initOrder.push_back("capture");
    return true;
}
bool PlaybackInit(replay::ReplayServices*) {
    g_initOrder.push_back("playback");
    return true;
}
bool ResidueInit(replay::ReplayServices*) {
    g_initOrder.push_back("residue-check");
    return true;
}

std::string ReadRepoFile(const char* rel) {
    const std::string path = std::string(REPLAY_REPO_ROOT) + "/" + rel;
    std::ifstream in(path.c_str(), std::ios::binary);
    REQUIRE_MESSAGE(in.is_open(), path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t                   start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string  line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        lines.push_back(line);
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return lines;
}

// The needle must name exactly one site. A second one would mean this check is speaking about a
// call it did not look at, which is the failure mode that makes a source check worthless.
size_t SoleLineWith(const std::vector<std::string>& lines, const std::string& needle) {
    size_t at = lines.size();
    size_t hits = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(needle) != std::string::npos) {
            ++hits;
            at = i;
        }
    }
    REQUIRE_MESSAGE(hits == 1, needle);
    return at;
}

bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == ':';
}

// The nearest definition that opens at column 0 above `at`. Every function in these two files is
// written that way inside its namespace, and a function body's own lines are all indented, so the
// first unindented `name(...) {` above a statement is the function that statement runs in.
std::string EnclosingFunctionName(const std::vector<std::string>& lines, size_t at) {
    for (size_t i = at + 1; i-- > 0;) {
        const std::string& l = lines[i];
        if (l.empty()) {
            continue;
        }
        const char c0 = l[0];
        if (c0 == ' ' || c0 == '\t' || c0 == '#' || c0 == '/' || c0 == '}' || c0 == '{') {
            continue;
        }
        if (l[l.size() - 1] != '{') {
            continue;
        }
        const size_t open = l.find('(');
        if (open == std::string::npos) {
            continue;
        }
        size_t start = open;
        while (start > 0 && IsIdentChar(l[start - 1])) {
            --start;
        }
        std::string name = l.substr(start, open - start);
        const size_t colons = name.rfind(':');
        return colons == std::string::npos ? name : name.substr(colons + 1);
    }
    return std::string();
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

// The comma-separated arguments on the REPLAY_MODULE line, unqualified: name, order, init, tick.
// The macro's fifth argument wraps onto the next line at both sites and no check here needs it.
std::vector<std::string> ModuleArgs(const std::string& line) {
    const size_t open = line.find('(');
    REQUIRE(open != std::string::npos);
    std::vector<std::string> args;
    std::string              current;
    for (size_t i = open + 1; i < line.size(); ++i) {
        if (line[i] == ',') {
            args.push_back(Trim(current));
            current.clear();
        } else {
            current += line[i];
        }
    }
    const std::string tail = Trim(current);
    if (!tail.empty()) {
        args.push_back(tail);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        const size_t colons = args[i].rfind(':');
        if (colons != std::string::npos) {
            args[i] = args[i].substr(colons + 1);
        }
    }
    return args;
}

const char* const kCaptureSrc = "src/capture/capture.cpp";
const char* const kPlaybackSrc = "src/playback/core/playback_module.cpp";
const char* const kResidueSrc = "src/playback/residue/residue_module.cpp";

}  // namespace

TEST_CASE("capture initialises before playback, from the named orders") {
    CHECK(replay::kModuleOrderCapture < replay::kModuleOrderPlayback);

    replay::modreg::ResetForTest();
    g_initOrder.clear();

    // Appended in reverse so the sequence can only come from the order field.
    const replay::ReplayModule playback{"playback", replay::kModuleOrderPlayback, PlaybackInit, nullptr,
                                        nullptr};
    const replay::ReplayModule residue{"residue-check", replay::kModuleOrderResidueCheck, ResidueInit,
                                       nullptr, nullptr};
    const replay::ReplayModule capture{"capture", replay::kModuleOrderCapture, CaptureInit, nullptr,
                                       nullptr};
    replay::modreg::RegisterForTest(playback);
    replay::modreg::RegisterForTest(residue);
    replay::modreg::RegisterForTest(capture);
    REQUIRE(replay::modreg::Count() == 3);

    replay::ReplayServices services{};
    CHECK(replay::modreg::InitAll(&services) == 3);
    REQUIRE(g_initOrder.size() == 3);
    // Each module calls ITick::RegisterEarly from inside its own init, and tick.cpp appends without
    // reordering -- so this is the early table's index order.
    CHECK(g_initOrder[0] == "capture");
    // Between the two: after playback, the pointer it lands would arrive too late to be read.
    CHECK(g_initOrder[1] == "residue-check");
    CHECK(g_initOrder[2] == "playback");

    replay::modreg::ResetForTest();
    g_initOrder.clear();
}

TEST_CASE("both module sites carry the named order, not a literal") {
    const std::vector<std::string> cap = Lines(ReadRepoFile(kCaptureSrc));
    const std::vector<std::string> pb = Lines(ReadRepoFile(kPlaybackSrc));

    const std::vector<std::string> capArgs = ModuleArgs(cap[SoleLineWith(cap, "REPLAY_MODULE(\"capture\"")]);
    const std::vector<std::string> pbArgs = ModuleArgs(pb[SoleLineWith(pb, "REPLAY_MODULE(\"playback\"")]);
    REQUIRE(capArgs.size() >= 4);
    REQUIRE(pbArgs.size() >= 4);

    CHECK(capArgs[1] == "kModuleOrderCapture");
    CHECK(pbArgs[1] == "kModuleOrderPlayback");

    const std::vector<std::string> res = Lines(ReadRepoFile(kResidueSrc));
    const std::vector<std::string> resArgs =
        ModuleArgs(res[SoleLineWith(res, "REPLAY_MODULE(\"residue-check\"")]);
    REQUIRE(resArgs.size() >= 2);
    CHECK(resArgs[1] == "kModuleOrderResidueCheck");

    // Playback's modreg tick is null on purpose; its frame runs entirely from the early table, so
    // the init order above is what separates it from capture's publication.
    CHECK(pbArgs[3] == "nullptr");
}

TEST_CASE("capture publishes its recording phase from its early callback, not from its modreg tick") {
    const std::vector<std::string> cap = Lines(ReadRepoFile(kCaptureSrc));
    const std::vector<std::string> pb = Lines(ReadRepoFile(kPlaybackSrc));

    const std::vector<std::string> capArgs = ModuleArgs(cap[SoleLineWith(cap, "REPLAY_MODULE(\"capture\"")]);
    REQUIRE(capArgs.size() >= 4);
    const std::string capInitName = capArgs[2];
    const std::string capTickName = capArgs[3];

    const size_t      capEarlyLine = SoleLineWith(cap, "RegisterEarly(&");
    const std::string capEarlyFn = Trim(cap[capEarlyLine].substr(
        cap[capEarlyLine].find("RegisterEarly(&") + 15,
        cap[capEarlyLine].find(',', cap[capEarlyLine].find("RegisterEarly(&")) -
            (cap[capEarlyLine].find("RegisterEarly(&") + 15)));

    // The distinction the invariant rests on: the early callback is a different function from the
    // modreg tick. If they were the same name every check below would pass vacuously.
    REQUIRE(capEarlyFn != capTickName);
    // ... and it is registered from capture's own init, which is what makes the order field decide
    // its index in the early table.
    CHECK(EnclosingFunctionName(cap, capEarlyLine) == capInitName);

    // THE RATCHET: the arming publication must sit in the early callback. Moved into the modreg
    // tick it would run after every early callback, including playback's seat pump, and the
    // recorder would be seated for the arming frame.
    const size_t armLine = SoleLineWith(cap, "SetPhase(Phase::Arming)");
    CHECK(EnclosingFunctionName(cap, armLine) == capEarlyFn);

    // Playback likewise drives its frame from an early callback registered in its own init.
    const size_t pbEarlyLine = SoleLineWith(pb, "RegisterEarly(&");
    const std::vector<std::string> pbArgs = ModuleArgs(pb[SoleLineWith(pb, "REPLAY_MODULE(\"playback\"")]);
    REQUIRE(pbArgs.size() >= 4);
    CHECK(EnclosingFunctionName(pb, pbEarlyLine) == pbArgs[2]);
}
