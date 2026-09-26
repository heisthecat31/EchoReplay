// The one predicate every engine-writing path asks. What is worth testing here is not the two-line
// Classify -- it is the ENUMERATION: a feature added to the header and forgotten in the switch, or a
// verdict that lets one feature through, is exactly the defect this module exists to prevent.
//
// So the cases below sweep Feature::Count rather than naming features one at a time. A new
// enumerator is covered the moment it is declared, and cannot be added without its name.
//
// No commas in test case names: doctest reads a comma in --test-case as a filter separator.

#include <doctest.h>

#include <string.h>

#include <set>
#include <string>

#include "bootstrap/livesafety.h"

using replay::livesafety::Feature;
using replay::livesafety::Verdict;
namespace ls = replay::livesafety;

namespace {

// Every feature, as the sweep cases want it.
struct EachFeature {
    template <typename Fn>
    static void Do(Fn fn) {
        for (uint8_t i = 0; i < static_cast<uint8_t>(Feature::Count); ++i) {
            fn(static_cast<Feature>(i));
        }
    }
};

}  // namespace

TEST_CASE("the verdict table is the two boot facts and nothing else") {
    CHECK(ls::Classify(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true) == Verdict::Contained);
    CHECK(ls::Classify(false, false) == Verdict::LockNotHeld);
    CHECK(ls::Classify(true, false) == Verdict::LiveOnlineExempt);
    // The exemption wins even with the lock somehow held: a live public match is never written to.
    CHECK(ls::Classify(true, true) == Verdict::LiveOnlineExempt);
}

TEST_CASE("every feature has its own name and no feature is missing from the switch") {
    // FeatureName returns "?" for an enumerator the switch forgot, so this is the forgotten-case
    // detector as well as the uniqueness check the startup block depends on.
    std::set<std::string> seen;
    EachFeature::Do([&](Feature f) {
        const char* name = ls::FeatureName(f);
        REQUIRE(name != nullptr);
        CHECK(strcmp(name, "?") != 0);
        CHECK(name[0] != '\0');
        CHECK(seen.insert(name).second);
    });
    CHECK(seen.size() == static_cast<size_t>(Feature::Count));
}

TEST_CASE("an uncontained process refuses every feature there is") {
    // The zero-engine-writes claim, stated over the whole enumeration rather than over the list of
    // features someone remembered. This is what a newly added Feature inherits for free.
    for (int arm = 0; arm < 2; ++arm) {
        const bool liveExempt = (arm == 0);
        ls::SetForTest(liveExempt, /*offlineLockHeld=*/!liveExempt ? false : true);
        CHECK(ls::Current() != Verdict::Contained);
        EachFeature::Do([](Feature f) { CHECK_FALSE(ls::Allowed(f)); });
    }

    // NotDecided -- a process that has not classified itself -- refuses too. That is the state a
    // freshly loaded DLL is in, and the state this very test binary is in without an override.
    ls::ResetForTest();
    CHECK(ls::Current() == Verdict::LockNotHeld);
    EachFeature::Do([](Feature f) { CHECK_FALSE(ls::Allowed(f)); });
}

TEST_CASE("the control: a contained process allows every feature there is") {
    // Without this arm every case above would pass against a build whose Allowed() returned false
    // unconditionally -- which is not a safe plugin, it is a dead one.
    ls::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    CHECK(ls::Current() == Verdict::Contained);
    EachFeature::Do([](Feature f) { CHECK(ls::Allowed(f)); });
    ls::ResetForTest();
}

TEST_CASE("every verdict carries a name and a reason a reader can act on") {
    const Verdict all[] = {Verdict::Contained, Verdict::LiveOnlineExempt, Verdict::LockNotHeld,
                           Verdict::NotDecided};
    for (Verdict v : all) {
        CHECK(strcmp(ls::VerdictName(v), "?") != 0);
        CHECK(strlen(ls::VerdictReason(v)) > 20);
    }
}

// The startup block prints one `=inert(...)` row per Feature enumerator, and
// the owner's first-minute check reads each row as "that path asks this gate". Three rows --
// GripWrites, Dress and PreRoll -- asserted a gate that had no call site anywhere in src/. The
// paths were protected, but by a neighbouring gate one hop upstream, which is not what the row
// says. This walks the tree AS TEXT rather than keeping a list of sources here, because a list
// here is the second declaration that drifts -- the first draft of this test kept one and it was
// already wrong about three features.
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::string ReadLiveSafetyRepoFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Every `Feature::Name` spelled in livesafety.h's enum, in declaration order.
constexpr char kNewline = (char)10;  // a newline, spelled so no editor can eat the escape

std::vector<std::string> EnumeratorNames(const std::string& header) {
    std::vector<std::string> out;
    const size_t open = header.find("enum class Feature");
    if (open == std::string::npos) return out;
    const size_t brace = header.find('{', open);
    const size_t close = header.find("};", brace);
    if (brace == std::string::npos || close == std::string::npos) return out;
    size_t i = brace + 1;
    while (i < close) {
        while (i < close && (isspace((unsigned char)header[i]) || header[i] == ',')) ++i;
        if (i + 1 < close && header[i] == '/' && header[i + 1] == '/') {
            i = header.find(kNewline, i);
            if (i == std::string::npos) break;
            continue;
        }
        size_t j = i;
        while (j < close && (isalnum((unsigned char)header[j]) || header[j] == '_')) ++j;
        if (j > i) out.push_back(header.substr(i, j - i));
        i = header.find(kNewline, j);
        if (i == std::string::npos) break;
    }
    return out;
}

// `Allowed(Feature::X)` or `Allowed(livesafety::Feature::X)`, and NOT the `case Feature::X:` of the
// switch that gives the row its name -- naming a feature is not asking it.
bool AsksFeature(const std::string& text, const std::string& name) {
    const std::string needle = "Feature::" + name;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        const size_t end = at + needle.size();
        if (end < text.size() && (isalnum((unsigned char)text[end]) || text[end] == '_')) continue;
        const size_t from = at > 48 ? at - 48 : 0;
        const std::string before = text.substr(from, at - from);
        if (before.find("Allowed(") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("live-safety: every printed gate row has a call site that asks it") {
    const std::filesystem::path src = std::filesystem::path(REPLAY_REPO_ROOT) / "src";
    REQUIRE(std::filesystem::is_directory(src));
    const std::string header = ReadLiveSafetyRepoFile(src / "bootstrap" / "livesafety.h");
    REQUIRE_MESSAGE(!header.empty(), "cannot read src/bootstrap/livesafety.h");
    const std::vector<std::string> names = EnumeratorNames(header);
    REQUIRE(names.size() >= 20u);
    REQUIRE(names.back() == "Count");  // the terminator, not a gate

    // Whole tree, header excluded: the declaration is not a call site. livesafety.cpp IS included,
    // because HttpRoutesAllowed asks the gate there and forwards it -- an `Allowed(` is an ask
    // wherever it is written, and the `case Feature::X:` of FeatureName is not one.
    std::string all;
    for (const auto& e : std::filesystem::recursive_directory_iterator(src)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".inc") continue;
        if (e.path().filename() == "livesafety.h") continue;
        all += ReadLiveSafetyRepoFile(e.path());
        all += kNewline;
    }
    REQUIRE(all.size() > 100000u);

    for (const std::string& name : names) {
        if (name == "Count") continue;
        INFO("live-safety row '" << name << "' is printed at startup");
        CHECK_MESSAGE(AsksFeature(all, name),
                      "this gate is printed as a row but no Allowed() asks it: either wire it at "
                      "the write site it names or stop printing the row");
    }
}
