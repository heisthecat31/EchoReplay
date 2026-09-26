#include "doctest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "import/import_facts.h"

using namespace replay::import;

// The facts guard (DESIGN-FORMATS 11.1): every value P1 measured is pinned to the published
// measurement file, so a hand edit to import_facts.h -- or to the file -- fails here.

namespace {

std::string ReadFactsFile() {
    FILE* f = fopen(REPLAY_NATIVE_FACTS_JSON, "rb");
    if (!f) return {};
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

// The object that follows `"id": {` inside "facts". The file is written flat on purpose: one object
// per fact, no nesting inside it.
std::string FactObject(const std::string& json, const char* id) {
    const size_t facts = json.find("\"facts\"");
    if (facts == std::string::npos) return {};
    const std::string key = std::string("\"") + id + "\": {";
    const size_t at = json.find(key, facts);
    if (at == std::string::npos) return {};
    const size_t end = json.find('}', at);
    return end == std::string::npos ? std::string() : json.substr(at + key.size(), end - at - key.size());
}

// The raw token after `"field": `: a number, a bare word, a quoted string, or a bracketed list.
std::string Field(const std::string& obj, const char* field) {
    const std::string key = std::string("\"") + field + "\": ";
    const size_t at = obj.find(key);
    if (at == std::string::npos) return {};
    size_t p = at + key.size();
    if (obj[p] == '"') return obj.substr(p + 1, obj.find('"', p + 1) - p - 1);
    const size_t end = obj[p] == '[' ? obj.find(']', p) + 1 : obj.find_first_of(",}", p);
    return obj.substr(p, end - p);
}

std::vector<double> Numbers(const std::string& token) {
    std::vector<double> out;
    const char* p = token.c_str();
    while (*p) {
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char* end = nullptr;
            out.push_back(strtod(p, &end));
            p = end;
        } else {
            ++p;
        }
    }
    return out;
}

const char* OriName(OriSource s) {
    switch (s) {
        case OriSource::HeadBasis: return "HeadBasis";
        case OriSource::BodyBasis: return "BodyBasis";
        case OriSource::SameAsHmd: return "SameAsHmd";
    }
    return "?";
}

const char* OwnerName(DiscOwnerMeaning m) {
    return m == DiscOwnerMeaning::InHand ? "InHand" : "Possession";
}

struct Pinned {
    const std::string json = ReadFactsFile();

    // Present in the file as measured, and marked measured in the header.
    template <class T>
    std::string Obj(const Fact<T>& f) {
        const std::string obj = FactObject(json, f.id);
        CHECK_MESSAGE(!obj.empty(), f.id, " is missing from native_facts.json");
        CHECK_MESSAGE(Field(obj, "status") == "measured", f.id);
        CHECK_MESSAGE(f.status == FactStatus::kMeasured, f.id, " is pinned but import_facts.h says unmeasured");
        return obj;
    }
    template <class T>
    void Number(const Fact<T>& f, double tolerance = 0.0) {
        const std::vector<double> v = Numbers(Field(Obj(f), "value"));
        REQUIRE_MESSAGE(v.size() == 1, f.id);
        CHECK_MESSAGE(fabs(double(f.value) - v[0]) <= tolerance, f.id, " header ", double(f.value), " file ", v[0]);
    }
    void Vector(const Fact<Vec3f>& f) {
        const std::vector<double> v = Numbers(Field(Obj(f), "value"));
        REQUIRE_MESSAGE(v.size() == 3, f.id);
        CHECK_MESSAGE(float(v[0]) == f.value.x, f.id);
        CHECK_MESSAGE(float(v[1]) == f.value.y, f.id);
        CHECK_MESSAGE(float(v[2]) == f.value.z, f.id);
    }
    void Bool(const Fact<bool>& f) {
        CHECK_MESSAGE(Field(Obj(f), "value") == (f.value ? "true" : "false"), f.id);
    }
    void Word(const Fact<OriSource>& f) { CHECK_MESSAGE(Field(Obj(f), "value") == OriName(f.value), f.id); }
    void Word(const Fact<DiscOwnerMeaning>& f) {
        CHECK_MESSAGE(Field(Obj(f), "value") == OwnerName(f.value), f.id);
    }
};

}  // namespace

TEST_CASE("import_facts.h carries exactly what native_facts.json measured") {
    Pinned pin;
    REQUIRE_MESSAGE(!pin.json.empty(), "cannot read ", REPLAY_NATIVE_FACTS_JSON);

    const EmitFacts& e = kEmitFacts;
    pin.Bool(e.worldAxesMatchEngine);
    pin.Vector(e.comFromHead);
    pin.Word(e.hmdOri);
    pin.Word(e.bodyOri);
    pin.Bool(e.handAxesDirect);
    pin.Word(e.discOwner);
    // float literals in the header, decimal in the file: one quantum of the text is the tolerance
    pin.Number(e.bendBack, 1e-5);
    pin.Number(e.bendFwd, 1e-5);
    pin.Number(e.restPoseHeight, 1e-5);
    pin.Number(e.discIdValue);

    const InferFacts& f = kInferFacts;
    pin.Number(f.geoBitLeft);
    pin.Number(f.geoBitRight);
    pin.Number(f.idleWord);
    pin.Number(f.stunDurationMedianS, 1e-6);
    pin.Number(f.boostSpanS, 1e-6);
    pin.Vector(f.thrusterAxis);
}

TEST_CASE("facts P1 did not measure are absent from its file") {
    Pinned pin;
    REQUIRE(!pin.json.empty());
    const EmitFacts& e = kEmitFacts;
    // O-1, O-3 and O-4 belong to P4 and are read from the engine, not from captures; P4 has O-1.
    CHECK(e.gametypeId.status == FactStatus::kUnmeasured);
    CHECK(e.defaultLoadout.status == FactStatus::kUnmeasured);
    CHECK(e.goalTypes.status == FactStatus::kMeasured);
    CHECK(FactObject(pin.json, e.gametypeId.id).empty());
    CHECK(FactObject(pin.json, e.defaultLoadout.id).empty());
    CHECK(FactObject(pin.json, e.goalTypes.id).empty());
}

TEST_CASE("the reader takes the flat fact shape the file is written in") {
    const std::string json = "{\"facts\": {\n  \"X\": {\"status\": \"measured\", \"value\": [0.0, 0.0, 1.0]}\n}}";
    CHECK(Numbers(Field(FactObject(json, "X"), "value")) == std::vector<double>{0.0, 0.0, 1.0});
    CHECK(Field(FactObject(json, "X"), "status") == "measured");
    CHECK(FactObject(json, "Y").empty());
    CHECK(FactObject("{\"X\": {\"value\": 1}}", "X").empty());  // outside "facts" is not a fact
}

// P7's verdict (DESIGN-FORMATS 6.4): a gated rule's `enabled` is data from the held-out validation,
// not a hand edit. The "p7" section holds one flat object per gated rule id.
TEST_CASE("each gated rule's enabled flag is P7's published verdict") {
    const std::string json = ReadFactsFile();
    REQUIRE(!json.empty());
    const size_t p7 = json.find("\"p7\"");
    REQUIRE(p7 != std::string::npos);
    int gated = 0;
    for (const RuleGate& g : kInferFacts.rules) {
        CAPTURE(g.id);
        const std::string key = std::string("\"") + g.id + "\": {";
        const size_t at = json.find(key, p7);
        if (!g.gated) {
            CHECK(g.enabled);
            CHECK(at == std::string::npos);
            continue;
        }
        ++gated;
        REQUIRE(at != std::string::npos);
        const std::string obj = json.substr(at + key.size(), json.find('}', at) - at - key.size());
        CHECK(Field(obj, "enabled") == (g.enabled ? "true" : "false"));
    }
    CHECK(gated == 5);
}
