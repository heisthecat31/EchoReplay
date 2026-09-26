// W1-E JSONWALK -- the entrant profile CJson tree, serialised with the ENGINE'S OWN read accessors.
//
// The fake tree below is built to the accessor CONTRACT, not to the engine's node layout: nothing
// here reproduces a CJson node, because nothing in src/engine/entity_json.cpp reads one.  Every
// field arrives through a function pointer, so a fake that answers the same questions the engine's
// eight entry points answer exercises the emitter end to end on a host with no game.
//
// What the fake CANNOT prove is listed in ENG-W1E-JSONWALK S5: that a live entrant's profile holds
// only shapes this build's read API can express.  What it CAN prove is every byte of the encoding,
// every bound, and every refusal -- and that the eight sites the shipped path binds are the eight
// addresses the registry holds.

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include <new>
#include <string>
#include <vector>

// The DOCROOT cases at the bottom of this file build their fixtures out of REAL pages -- one
// committed, one only reserved -- so the committed-memory probe they hand the emitter is the
// same VirtualQuery answer identity_access binds, not a lookaside table that always agrees.
#include <windows.h>

#include "engine/entity_json.h"
#include "engine/identity_access.h"
#include "generated/facts_generated.h"

using namespace replay;
using entity_json::JsonRefusal;
using entity_json::Kind;

namespace {

// One node of the fake document.  `kind` is FIRST because EmitCanonicalJson probes four bytes at
// the node pointer before it hands the pointer to any accessor, so the probe and the tag coincide
// exactly as they do in the engine.
struct FakeNode {
    uint32_t                kind;
    const char*             key;
    const char*             str;
    int64_t                 num;
    int                     boolean;
    std::vector<FakeNode*>  kids;
    int64_t                 countOverride;  // < 0 means "answer kids.size()", as the engine would

    FakeNode(uint32_t k)
        : kind(k), key("k"), str(nullptr), num(0), boolean(-1), countOverride(-1) {}
};

// The one pointer the readable() probe refuses, so the unreadable-node and unreadable-string arms
// are reachable without a fixture that maps and unmaps real pages inside the emitter's own walk.
const void* gUnreadable = nullptr;

bool FakeReadable(const void* p, size_t) { return p != nullptr && p != gUnreadable; }

uint32_t FakeKind(void*, void* n) { return static_cast<FakeNode*>(n)->kind; }
const char* FakeName(void*, void* n) { return static_cast<FakeNode*>(n)->key; }

const char* FakeString(void*, void* n, const char* dflt) {
    FakeNode* f = static_cast<FakeNode*>(n);
    return f->str ? f->str : dflt;
}

int64_t FakeInt(void*, void* n, int64_t dflt) {
    FakeNode* f = static_cast<FakeNode*>(n);
    return f->kind == uint32_t(Kind::Int) ? f->num : dflt;
}

int FakeBool(void*, void* n, int dflt) {
    FakeNode* f = static_cast<FakeNode*>(n);
    return f->boolean < 0 ? dflt : f->boolean;
}

// The engine's IDENT.JSON_NEXT_ELEM: prev == null gives the first child, and a parent that is not
// the tag-0 container yields nothing at all.
void* FakeNextElem(void*, void* parent, void* prev) {
    FakeNode* p = static_cast<FakeNode*>(parent);
    if (p->kind != uint32_t(Kind::Object)) return nullptr;
    if (!prev) return p->kids.empty() ? nullptr : p->kids[0];
    for (size_t i = 0; i + 1 < p->kids.size(); ++i)
        if (p->kids[i] == prev) return p->kids[i + 1];
    return nullptr;
}

int64_t FakeCount(void*, void* n) {
    FakeNode* f = static_cast<FakeNode*>(n);
    if (f->countOverride >= 0) return f->countOverride;
    if (f->kind != uint32_t(Kind::Object) && f->kind != uint32_t(Kind::Array)) return 0;
    return int64_t(f->kids.size());
}

entity_json::CJsonReader MakeReader() {
    entity_json::CJsonReader r;
    r.json        = reinterpret_cast<void*>(uintptr_t(1));
    r.kind        = &FakeKind;
    r.name        = &FakeName;
    r.stringValue = &FakeString;
    r.intValue    = &FakeInt;
    r.boolValue   = &FakeBool;
    r.nextElem    = &FakeNextElem;
    r.elemCount   = &FakeCount;
    r.readable    = &FakeReadable;
    return r;
}

FakeNode* Obj(const char* key) {
    FakeNode* n = new FakeNode(uint32_t(Kind::Object));
    n->key = key;
    return n;
}
FakeNode* Arr(const char* key) {
    FakeNode* n = new FakeNode(uint32_t(Kind::Array));
    n->key = key;
    return n;
}
FakeNode* Str(const char* key, const char* v) {
    FakeNode* n = new FakeNode(uint32_t(Kind::String));
    n->key = key;
    n->str = v;
    return n;
}
FakeNode* Int(const char* key, int64_t v) {
    FakeNode* n = new FakeNode(uint32_t(Kind::Int));
    n->key = key;
    n->num = v;
    return n;
}
FakeNode* Boolean(const char* key, bool v) {
    FakeNode* n = new FakeNode(uint32_t(Kind::Bool));
    n->key = key;
    n->boolean = v ? 1 : 0;
    return n;
}

// Run the emitter over `root` with a generous buffer and hand back what it wrote.  The buffer is
// 0xCC-filled so a case that asserts an untouched buffer is asserting real bytes.
constexpr size_t kRunBuf = 2048;

struct Run {
    bool        ok;
    JsonRefusal why;
    std::string text;
    size_t      written;
    bool        truncated;
    uint8_t     buf[kRunBuf];
    std::string path;
};

Run Emit(FakeNode* root, size_t cap = kRunBuf,
         const entity_json::JsonEmitLimits* lim = nullptr) {
    Run r;
    memset(r.buf, 0xCC, sizeof r.buf);
    r.why = JsonRefusal::None;
    r.ok  = entity_json::EmitCanonicalJson(MakeReader(), root, r.buf, cap, r.written, r.truncated,
                                          &r.why, lim);
    r.text.assign(reinterpret_cast<const char*>(r.buf), r.ok ? r.written : 0);
    r.path = entity_json::LastRefusalPath();
    return r;
}

}  // namespace

TEST_CASE("W1-E JSONWALK 1: a nested document serialises to exact canonical bytes") {
    gUnreadable = nullptr;
    FakeNode* root = Obj("$");
    root->kids.push_back(Str("displayname", "Ada"));
    root->kids.push_back(Int("level", 42));
    FakeNode* unlocks = Obj("unlocks");
    unlocks->kids.push_back(Boolean("emote_wave", true));
    unlocks->kids.push_back(Boolean("decal_skull", false));
    root->kids.push_back(unlocks);
    root->kids.push_back(Arr("tags"));           // empty array IS the whole value
    root->kids.push_back(Obj("empty"));          // and so is an empty object
    root->kids.push_back(Int("negative", -7));

    Run r = Emit(root);
    REQUIRE(r.ok);
    CHECK(r.why == JsonRefusal::None);
    CHECK(r.truncated == false);

    // Key order is TREE order -- the order IDENT.JSON_NEXT_ELEM yields, which is the order the
    // engine's own unlocks walk sees.  No whitespace, no sorting, no trailing comma.
    const std::string expect =
        "{\"displayname\":\"Ada\",\"level\":42,"
        "\"unlocks\":{\"emote_wave\":true,\"decal_skull\":false},"
        "\"tags\":[],\"empty\":{},\"negative\":-7}";
    CHECK(r.text == expect);
    CHECK(r.written == expect.size());
    // Nothing past the document was touched: the byte after the last is still the 0xCC fill.
    CHECK(r.buf[expect.size()] == 0xCC);
}

TEST_CASE("W1-E JSONWALK 2: RFC 8259 escaping, in keys as well as values") {
    gUnreadable = nullptr;
    FakeNode* root = Obj("$");
    root->kids.push_back(Str("quote\"and\\slash", "he said \"hi\"\\"));
    root->kids.push_back(Str("ctl", "a\bb\tc\nd\fe\rf"));
    root->kids.push_back(Str("low", "\x01\x1f"));
    root->kids.push_back(Str("utf8", "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));  // e-acute, euro, emoji
    root->kids.push_back(Str("solidus", "a/b"));   // RFC 8259 does NOT require escaping '/'

    Run r = Emit(root);
    REQUIRE(r.ok);
    const std::string expect =
        "{\"quote\\\"and\\\\slash\":\"he said \\\"hi\\\"\\\\\","
        "\"ctl\":\"a\\bb\\tc\\nd\\fe\\rf\","
        "\"low\":\"\\u0001\\u001f\","
        "\"utf8\":\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\","
        "\"solidus\":\"a/b\"}";
    CHECK(r.text == expect);
}

TEST_CASE("W1-E JSONWALK 3: bytes that are not UTF-8 are refused, not passed through") {
    gUnreadable = nullptr;
    // A lone continuation byte, a truncated three-byte sequence, an overlong two-byte encoding of
    // '/', a surrogate half, and a codepoint past U+10FFFF.  None of these has a canonical JSON
    // form, and passing them through would make a document no parser accepts.
    const char* bad[] = {"\x80", "\xE2\x82", "\xC0\xAF", "\xED\xA0\x80", "\xF5\x80\x80\x80"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        FakeNode* root = Obj("$");
        root->kids.push_back(Str("v", bad[i]));
        Run r = Emit(root);
        CAPTURE(i);
        CHECK(r.ok == false);
        CHECK(r.why == JsonRefusal::StringNotUtf8);
        CHECK(std::string(r.path) == "$.v");
    }
}

TEST_CASE("W1-E JSONWALK 4: the cap reports the document length and writes nothing") {
    gUnreadable = nullptr;
    FakeNode* root = Obj("$");
    root->kids.push_back(Str("a", "bcdef"));
    const std::string expect = "{\"a\":\"bcdef\"}";

    Run full = Emit(root);
    REQUIRE(full.ok);
    REQUIRE(full.text == expect);

    // One byte short: written is the DOCUMENT length so the caller can size its second call, the
    // flag says why, and every byte of the buffer still holds its fill.  A prefix of a JSON
    // document parses as nothing and would reach the demo file looking like a capture.
    Run cut = Emit(root, expect.size() - 1);
    CHECK(cut.ok == false);
    CHECK(cut.why == JsonRefusal::ShortBuffer);
    CHECK(cut.truncated == true);
    CHECK(cut.written == expect.size());
    for (size_t i = 0; i < sizeof cut.buf; ++i) CHECK(cut.buf[i] == 0xCC);

    // Exactly the document length fits, which pins the boundary from the other side.
    Run exact = Emit(root, expect.size());
    CHECK(exact.ok);
    CHECK(exact.truncated == false);
    CHECK(exact.text == expect);
    CHECK(exact.buf[expect.size()] == 0xCC);
}

TEST_CASE("W1-E JSONWALK 5: the depth and node bounds refuse by name and name the node") {
    gUnreadable = nullptr;
    FakeNode* root = Obj("$");
    FakeNode* a = Obj("a");
    FakeNode* b = Obj("b");
    FakeNode* c = Obj("c");
    root->kids.push_back(a);
    a->kids.push_back(b);
    b->kids.push_back(c);
    c->kids.push_back(Int("n", 1));

    entity_json::JsonEmitLimits deep = entity_json::DefaultLimits();
    CHECK(Emit(root, 2048, &deep).ok);

    entity_json::JsonEmitLimits shallow = deep;
    shallow.maxDepth = 3;   // root, a, b fit; c is one container too far
    Run d = Emit(root, 2048, &shallow);
    CHECK(d.ok == false);
    CHECK(d.why == JsonRefusal::DepthLimit);
    CHECK(std::string(d.path) == "$.a.b.c");

    entity_json::JsonEmitLimits few = deep;
    few.maxNodes = 3;
    Run n = Emit(root, 2048, &few);
    CHECK(n.ok == false);
    CHECK(n.why == JsonRefusal::NodeLimit);

    // A depth bound past the ceiling the cycle check can be exact over is refused, not clamped: a
    // silently shallower cycle check is the kind of quiet downgrade this subsystem exists to remove.
    entity_json::JsonEmitLimits huge = deep;
    huge.maxDepth = 65;
    Run h = Emit(root, 2048, &huge);
    CHECK(h.ok == false);
    CHECK(h.why == JsonRefusal::DepthLimit);
}

TEST_CASE("W1-E JSONWALK 6: a cycle is a named refusal, not a hang") {
    gUnreadable = nullptr;
    FakeNode* root = Obj("$");
    FakeNode* a = Obj("a");
    root->kids.push_back(a);
    a->kids.push_back(root);   // a's child IS its own grandparent

    Run r = Emit(root);
    CHECK(r.ok == false);
    CHECK(r.why == JsonRefusal::Cycle);
    CHECK(std::string(r.path) == "$.a.$");
}

TEST_CASE("W1-E JSONWALK 7: the four shapes this build's read API cannot express") {
    gUnreadable = nullptr;

    // A POPULATED array.  IDENT.JSON_NEXT_ELEM tests the parent's tag and iterates the tag-0
    // container only, so an array's elements are unreachable -- and emitting [] for a populated
    // one would be the confident wrong answer.
    {
        FakeNode* root = Obj("$");
        FakeNode* a = Arr("items");
        a->kids.push_back(Str("x", "one"));
        root->kids.push_back(a);
        Run r = Emit(root);
        CHECK(r.ok == false);
        CHECK(r.why == JsonRefusal::ArrayNotWalkable);
        CHECK(std::string(r.path) == "$.items");
    }
    // A non-integer number. RECORDER-3 F4 reverses the old refusal: there is still no float32
    // accessor to read the value exactly, but ONE such node used to throw the whole document away
    // -- with it every item id, key and jersey number the row carried. The node is emitted as
    // `null`, the walk finishes, and LastEmitDroppedNumber() is what the recorder flags the row on.
    {
        FakeNode* root = Obj("$");
        FakeNode* f = new FakeNode(uint32_t(Kind::Float));
        f->key = "ratio";
        FakeNode* keep = Str("name", "hontomn2");
        root->kids.push_back(f);
        root->kids.push_back(keep);
        Run r = Emit(root);
        CHECK(r.ok == true);
        CHECK(r.text == "{\"ratio\":null,\"name\":\"hontomn2\"}");
        CHECK(entity_json::LastEmitDroppedNumber() == true);
    }
    // ... and a document with no float leaves the flag clear, so the flag is evidence and not a
    // constant.
    {
        FakeNode* root = Obj("$");
        root->kids.push_back(Str("name", "hontomn2"));
        Run r = Emit(root);
        CHECK(r.ok == true);
        CHECK(entity_json::LastEmitDroppedNumber() == false);
    }
    // A tag IDENT.JSON_NODE_KIND does not name.  Tag 7 exists in the library and its switch has no
    // arm for it, so the accessor answers 0 -- writing `null` there would be a fabricated value.
    {
        FakeNode* root = Obj("$");
        FakeNode* u = new FakeNode(uint32_t(Kind::None));
        u->key = "mystery";
        root->kids.push_back(u);
        Run r = Emit(root);
        CHECK(r.ok == false);
        CHECK(r.why == JsonRefusal::UnknownNodeKind);
        CHECK(std::string(r.path) == "$.mystery");
    }
    // A Bool node whose accessor answers neither 1 nor 0 is the same class of unknown.
    {
        FakeNode* root = Obj("$");
        FakeNode* b = new FakeNode(uint32_t(Kind::Bool));
        b->key = "maybe";
        b->boolean = -1;   // the accessor hands back the caller's default
        root->kids.push_back(b);
        Run r = Emit(root);
        CHECK(r.ok == false);
        CHECK(r.why == JsonRefusal::UnknownNodeKind);
    }
}

TEST_CASE("W1-E JSONWALK 8: the walk and IDENT.JSON_ELEM_COUNT are cross-checked") {
    gUnreadable = nullptr;
    FakeNode* root = Obj("$");
    root->kids.push_back(Int("a", 1));
    root->kids.push_back(Int("b", 2));
    REQUIRE(Emit(root).ok);

    // The iterator and the count come from two different engine functions, so a sibling walk that
    // stopped early is a disagreement between them and not a shorter document.
    root->countOverride = 3;
    Run r = Emit(root);
    CHECK(r.ok == false);
    CHECK(r.why == JsonRefusal::ChildCountDisagrees);

    root->countOverride = 1;
    CHECK(Emit(root).why == JsonRefusal::ChildCountDisagrees);
}

TEST_CASE("W1-E JSONWALK 9: every pointer is probed before it is used") {
    FakeNode* root = Obj("$");
    FakeNode* deep = Obj("deep");
    FakeNode* leaf = Str("leaf", "v");
    deep->kids.push_back(leaf);
    root->kids.push_back(deep);

    gUnreadable = nullptr;
    REQUIRE(Emit(root).ok);

    gUnreadable = leaf;                       // the node itself is not in committed memory
    Run n = Emit(root);
    CHECK(n.ok == false);
    CHECK(n.why == JsonRefusal::NodeUnreadable);
    // The proof precedes the first ACCESSOR call on the child -- IDENT.JSON_NODE_NAME reads
    // node+0x28 -- so an unreadable child is refused before its key can be asked for.  The path
    // therefore names its PARENT and the address names the node itself, which is strictly more
    // than a key read out of a node that was never proven could have said.
    CHECK(std::string(n.path) == "$.deep");
    CHECK(entity_json::LastRefusalAt() == static_cast<const void*>(leaf));
    CHECK(entity_json::LastRefusalExtent() == entity_json::kNodeAccessedExtent);

    gUnreadable = leaf->str;                  // the node is readable, its value pointer is not
    Run s = Emit(root);
    CHECK(s.ok == false);
    CHECK(s.why == JsonRefusal::StringUnreadable);

    gUnreadable = nullptr;
    leaf->str = nullptr;                      // a String node whose accessor answers null
    Run z = Emit(root);
    CHECK(z.ok == false);
    CHECK(z.why == JsonRefusal::StringUnreadable);

    // An accessor table with a hole answers NoReader rather than calling through a null pointer.
    entity_json::CJsonReader half = MakeReader();
    half.elemCount = nullptr;
    size_t      w = 1;
    bool        t = true;
    JsonRefusal why = JsonRefusal::None;
    CHECK(entity_json::EmitCanonicalJson(half, root, nullptr, 0, w, t, &why) == false);
    CHECK(why == JsonRefusal::NoReader);
    CHECK(w == 0);
    CHECK(t == false);

    CHECK(entity_json::EmitCanonicalJson(MakeReader(), nullptr, nullptr, 0, w, t, &why) == false);
    CHECK(why == JsonRefusal::NoRoot);
}

TEST_CASE("W1-E JSONWALK 10: integers render at both ends of the range") {
    gUnreadable = nullptr;
    FakeNode* root = Obj("$");
    root->kids.push_back(Int("min", INT64_MIN));
    root->kids.push_back(Int("max", INT64_MAX));
    root->kids.push_back(Int("zero", 0));
    root->kids.push_back(Int("negone", -1));

    Run r = Emit(root);
    REQUIRE(r.ok);
    CHECK(r.text ==
          "{\"min\":-9223372036854775808,\"max\":9223372036854775807,\"zero\":0,\"negone\":-1}");
}

TEST_CASE("W1-E JSONWALK 11: every refusal has its own sentence, and none of them is 'unknown'") {
    const JsonRefusal all[] = {
        JsonRefusal::None,             JsonRefusal::NoReader,        JsonRefusal::NoRoot,
        JsonRefusal::NodeUnreadable,   JsonRefusal::KeyUnreadable,   JsonRefusal::StringUnreadable,
        JsonRefusal::StringTooLong,    JsonRefusal::StringNotUtf8,   JsonRefusal::DepthLimit,
        JsonRefusal::NodeLimit,        JsonRefusal::Cycle,           JsonRefusal::ArrayNotWalkable,
        JsonRefusal::NumberNotExact,   JsonRefusal::UnknownNodeKind, JsonRefusal::ChildCountDisagrees,
        JsonRefusal::TreeChanged,      JsonRefusal::ShortBuffer};
    const size_t n = sizeof all / sizeof *all;
    for (size_t i = 0; i < n; ++i) {
        const std::string a = entity_json::JsonRefusalText(all[i]);
        CAPTURE(i);
        CHECK(a != "unknown");
        if (all[i] != JsonRefusal::None) CHECK(a.size() > 8u);
        for (size_t j = i + 1; j < n; ++j) CHECK(a != std::string(entity_json::JsonRefusalText(all[j])));
    }
}

TEST_CASE("W1-E JSONWALK 12: the kind vocabulary is the registry's, re-read at its anchors") {
    // The emitter's enum is DECLARED here and GENERATED there, so the two are a real comparison.
    // Each generated value is the imm32 of its own arm in IDENT.JSON_NODE_KIND's switch, and
    // the W1-E_json anchor drop carries the instruction bytes the gate reads back out of
    // echovr.exe.
    CHECK(uint32_t(Kind::String) == facts::IDENT_JSON_KIND_STRING);
    CHECK(uint32_t(Kind::Int)    == facts::IDENT_JSON_KIND_INT);
    CHECK(uint32_t(Kind::Float)  == facts::IDENT_JSON_KIND_FLOAT);
    CHECK(uint32_t(Kind::Bool)   == facts::IDENT_JSON_KIND_BOOL);
    CHECK(uint32_t(Kind::Array)  == facts::IDENT_JSON_KIND_ARRAY);
    CHECK(uint32_t(Kind::Object) == facts::IDENT_JSON_KIND_OBJECT);
    CHECK(uint32_t(Kind::None)   == 0u);

    // The six are distinct: a switch whose arms collide would make two shapes indistinguishable.
    const uint32_t v[] = {facts::IDENT_JSON_KIND_STRING, facts::IDENT_JSON_KIND_INT,
                          facts::IDENT_JSON_KIND_FLOAT,  facts::IDENT_JSON_KIND_BOOL,
                          facts::IDENT_JSON_KIND_ARRAY,  facts::IDENT_JSON_KIND_OBJECT};
    for (size_t i = 0; i < 6; ++i) {
        CHECK(v[i] != 0u);
        for (size_t j = i + 1; j < 6; ++j) CHECK(v[i] != v[j]);
    }
}

TEST_CASE("W1-E JSONWALK 13: the shipped path binds all eight accessors, at the registry's addresses") {
    // Unit-green cannot see wiring.  This walks identity_access's OWN site table and asserts the
    // eight new rows are present and carry the eight registered addresses -- so a site dropped from
    // the table, or pointed at a different function, fails here rather than at a capture.
    struct Want { const char* name; uint64_t va; };
    const Want want[] = {
        {"JsonNodeKind",   facts::IDENT_JSON_NODE_KIND},
        {"JsonNodeName",   facts::IDENT_JSON_NODE_NAME},
        {"JsonNodeString", facts::IDENT_JSON_NODE_STRING},
        {"JsonNodeInt",    facts::IDENT_JSON_NODE_INT},
        {"JsonNodeBool",   facts::IDENT_JSON_NODE_BOOL},
        {"JsonNextElem",   facts::IDENT_JSON_NEXT_ELEM},
        {"JsonElemCount",  facts::IDENT_JSON_ELEM_COUNT},
        {"JsonFindNode",   facts::IDENT_JSON_FIND_NODE},
    };
    const size_t sites = identity_access::EngineSiteCount();
    for (size_t k = 0; k < sizeof want / sizeof *want; ++k) {
        bool found = false;
        for (size_t i = 0; i < sites; ++i) {
            if (std::string(identity_access::EngineSiteName(i)) != want[k].name) continue;
            found = true;
            CHECK(identity_access::EngineSiteVa(i) == want[k].va);
        }
        const std::string missing = want[k].name;
        CAPTURE(missing);
        CHECK(found);
    }

    // And the refusal words the mapping produces are all distinct sentences, so a capture log that
    // says why entrantJson is short says something different for each reason.
    const identity_access::Refusal words[] = {
        identity_access::Refusal::JsonNoRoot,          identity_access::Refusal::JsonNodeUnreadable,
        identity_access::Refusal::JsonDepthLimit,      identity_access::Refusal::JsonNodeLimit,
        identity_access::Refusal::JsonCycle,           identity_access::Refusal::JsonArrayNotWalkable,
        identity_access::Refusal::JsonNumberNotExact,  identity_access::Refusal::JsonUnknownKind,
        identity_access::Refusal::JsonChildCountDisagrees,
        identity_access::Refusal::JsonStringUnreadable,
        identity_access::Refusal::JsonStringNotUtf8,   identity_access::Refusal::JsonTreeChanged};
    const size_t nw = sizeof words / sizeof *words;
    for (size_t i = 0; i < nw; ++i) {
        const std::string a = identity_access::RefusalText(words[i]);
        CAPTURE(i);
        CHECK(a != "unknown");
        for (size_t j = i + 1; j < nw; ++j)
            CHECK(a != std::string(identity_access::RefusalText(words[j])));
    }
}

TEST_CASE("W1-E JSONWALK 14: a scalar document, and a key that is not a bare identifier") {
    gUnreadable = nullptr;
    FakeNode* scalar = Str("$", "just a string");
    Run r = Emit(scalar);
    REQUIRE(r.ok);
    CHECK(r.text == "\"just a string\"");

    FakeNode* root = Obj("$");
    root->kids.push_back(Str("", "empty key"));
    root->kids.push_back(Str("a b|c[0]", "punctuation"));
    Run o = Emit(root);
    REQUIRE(o.ok);
    CHECK(o.text == "{\"\":\"empty key\",\"a b|c[0]\":\"punctuation\"}");
}

namespace {

Run EmitAt(FakeNode* root, const char* path, size_t cap = kRunBuf,
           const entity_json::JsonEmitLimits* lim = nullptr) {
    Run r;
    memset(r.buf, 0xCC, sizeof r.buf);
    r.why = JsonRefusal::None;
    r.ok  = entity_json::EmitCanonicalJsonAtPath(MakeReader(), root, path, r.buf, cap, r.written,
                                                 r.truncated, &r.why, lim);
    r.text.assign(reinterpret_cast<const char*>(r.buf), r.ok ? r.written : 0);
    r.path = entity_json::LastRefusalPath();
    return r;
}

// { "a": { "b": {"x":1}, "z":9 }, "b": {"x":2} }
//
// The top-level "b" is a DECOY and it is what makes the path mutants red: a resolver that stops
// one level early answers with `a`, and one that restarts at the root for every segment answers
// with the top-level `b`.  Both are documents, both parse, and neither is {"x":1}.
FakeNode* NestedDecoyDoc() {
    FakeNode* inner = Obj("b");
    inner->kids.push_back(Int("x", 1));
    FakeNode* a = Obj("a");
    a->kids.push_back(inner);
    a->kids.push_back(Int("z", 9));
    FakeNode* topB = Obj("b");
    topB->kids.push_back(Int("x", 2));
    FakeNode* root = Obj("$");
    root->kids.push_back(a);
    root->kids.push_back(topB);
    return root;
}

// The S3 shape: unlocks is a CHILD of the profile document, an object of name -> bool.
FakeNode* ProfileDoc() {
    FakeNode* unlocks = Obj("unlocks");
    unlocks->kids.push_back(Boolean("emote_wave", true));
    unlocks->kids.push_back(Boolean("decal_skull", false));
    FakeNode* root = Obj("$");
    root->kids.push_back(Str("displayname", "Ada"));
    root->kids.push_back(unlocks);
    root->kids.push_back(Int("level", 42));
    return root;
}

}  // namespace

TEST_CASE("W1-E SUBTREE 15: the subtree bytes ARE the subtree of the root serialisation") {
    gUnreadable = nullptr;
    FakeNode* root = ProfileDoc();

    Run whole = Emit(root);
    REQUIRE(whole.ok);
    Run sub = EmitAt(root, "unlocks");
    REQUIRE(sub.ok);
    CHECK(sub.why == JsonRefusal::None);
    CHECK(sub.truncated == false);

    // Derived from the whole document, not retyped: the subtree is the bytes that follow the
    // key, so a subtree serialised from the ROOT instead of from the resolved node cannot match.
    const std::string key = "\"unlocks\":";
    const size_t at = whole.text.find(key);
    REQUIRE(at != std::string::npos);
    CHECK(whole.text.find(key, at + 1) == std::string::npos);
    CHECK(whole.text.compare(at + key.size(), sub.text.size(), sub.text) == 0);
    CHECK(sub.text == "{\"emote_wave\":true,\"decal_skull\":false}");
    CHECK(sub.written == sub.text.size());
    CHECK(sub.written < whole.written);
    CHECK(sub.buf[sub.written] == 0xCC);

    // A null path and an empty path are the whole document, which is why ProfileJsonBytes can be
    // this same call: the two walks cannot drift apart if there is only one of them.
    Run nullPath = EmitAt(root, nullptr);
    REQUIRE(nullPath.ok);
    CHECK(nullPath.text == whole.text);
    Run emptyPath = EmitAt(root, "");
    REQUIRE(emptyPath.ok);
    CHECK(emptyPath.text == whole.text);
}

TEST_CASE("W1-E SUBTREE 16: a nested path resolves the right LEVEL, past a same-named decoy") {
    gUnreadable = nullptr;
    FakeNode* root = NestedDecoyDoc();

    Run whole = Emit(root);
    REQUIRE(whole.ok);
    CHECK(whole.text == "{\"a\":{\"b\":{\"x\":1},\"z\":9},\"b\":{\"x\":2}}");

    Run deep = EmitAt(root, "a.b");
    REQUIRE(deep.ok);
    CHECK(deep.text == "{\"x\":1}");

    // The two documents the mutants would answer with instead, each named and each different.
    Run oneLevelShort = EmitAt(root, "a");
    REQUIRE(oneLevelShort.ok);
    CHECK(oneLevelShort.text == "{\"b\":{\"x\":1},\"z\":9}");
    CHECK(oneLevelShort.text != deep.text);
    Run fromRoot = EmitAt(root, "b");
    REQUIRE(fromRoot.ok);
    CHECK(fromRoot.text == "{\"x\":2}");
    CHECK(fromRoot.text != deep.text);
    CHECK(deep.text != whole.text);

    // A leaf is reachable too, and it is the value alone.
    Run leaf = EmitAt(root, "a.b.x");
    REQUIRE(leaf.ok);
    CHECK(leaf.text == "1");
}

TEST_CASE("W1-E SUBTREE 17: a key that is not there is NoSuchKey, naming the segment") {
    gUnreadable = nullptr;
    FakeNode* root = NestedDecoyDoc();

    Run missing = EmitAt(root, "unlocks");
    CHECK(missing.ok == false);
    CHECK(missing.why == JsonRefusal::NoSuchKey);
    CHECK(missing.path == "$.unlocks");
    CHECK(missing.written == 0);
    CHECK(missing.truncated == false);
    CHECK(missing.buf[0] == 0xCC);

    // The segment that failed is named, not the whole path and not the root.
    Run deepMissing = EmitAt(root, "a.q");
    CHECK(deepMissing.ok == false);
    CHECK(deepMissing.why == JsonRefusal::NoSuchKey);
    CHECK(deepMissing.path == "$.a.q");

    // A segment asked of something that has no keys is NoSuchKey as well -- never a silent
    // fall-through to the parent, which is the whole document wearing the subtree's name.
    Run pastLeaf = EmitAt(root, "a.z.deeper");
    CHECK(pastLeaf.ok == false);
    CHECK(pastLeaf.why == JsonRefusal::NoSuchKey);
    CHECK(pastLeaf.path == "$.a.z.deeper");

    // A key that itself contains '.' is not addressable through a dotted path, and that is a
    // refusal rather than some other node: entity_json.h states the cost, this pins it.
    FakeNode* dotted = Obj("$");
    dotted->kids.push_back(Int("a.b", 5));
    Run d = EmitAt(dotted, "a.b");
    CHECK(d.ok == false);
    CHECK(d.why == JsonRefusal::NoSuchKey);

    // The refusal has its own sentence, and it is not "unknown".
    const std::string text = entity_json::JsonRefusalText(JsonRefusal::NoSuchKey);
    CHECK(text != "unknown");
    CHECK(text != std::string(entity_json::JsonRefusalText(JsonRefusal::NoRoot)));
    CHECK(identity_access::RefusalText(identity_access::Refusal::JsonNoSuchKey) !=
          std::string("unknown"));
}

TEST_CASE("W1-E SUBTREE 18: the cap is the SUBTREE's length, both sides of the boundary") {
    gUnreadable = nullptr;
    FakeNode* root = ProfileDoc();

    Run whole = Emit(root);
    REQUIRE(whole.ok);
    Run sized = EmitAt(root, "unlocks");
    REQUIRE(sized.ok);
    const size_t len = sized.written;
    REQUIRE(len > 0);

    Run shortCall = EmitAt(root, "unlocks", len - 1);
    CHECK(shortCall.ok == false);
    CHECK(shortCall.why == JsonRefusal::ShortBuffer);
    CHECK(shortCall.truncated == true);
    // The length reported is the SUBTREE's, not the document's: a caller sizing its second call
    // from it asks for the block it wanted.  A subtree serialised from the root reports the
    // document length here and this is the assertion that says so.
    CHECK(shortCall.written == len);
    CHECK(shortCall.written != whole.written);
    // And nothing was written -- a JSON block cut at a byte boundary parses as nothing.
    for (size_t i = 0; i < kRunBuf; ++i) {
        CAPTURE(i);
        CHECK(shortCall.buf[i] == 0xCC);
    }

    Run exact = EmitAt(root, "unlocks", len);
    REQUIRE(exact.ok);
    CHECK(exact.truncated == false);
    CHECK(exact.written == len);
    CHECK(exact.buf[len] == 0xCC);
}

TEST_CASE("W1-E SUBTREE 19: the build-mismatch detail names EVERY site, not the first sixteen") {
    // This process is not echovr.exe, so every site rebases outside the module and BuildMatches
    // refuses -- which is what makes the detail observable here at all.
    REQUIRE(identity_access::BuildMatches() == false);
    const char* detail = identity_access::BuildMismatchDetail();
    REQUIRE(detail != nullptr);

    const size_t sites = identity_access::EngineSiteCount();
    REQUIRE(sites > 0);
    size_t named = 0;
    for (size_t i = 0; i < sites; ++i) {
        const std::string name = identity_access::EngineSiteName(i);
        CAPTURE(name);
        CHECK(identity_access::EngineSiteResolved(i) == nullptr);
        if (strstr(detail, name.c_str()) != nullptr) ++named;
        CHECK(strstr(detail, name.c_str()) != nullptr);
    }
    // Counted as well as checked: a detail that stopped early would still satisfy the sites it
    // did name, and the count is the one number a truncating buffer cannot fake.
    CHECK(named == sites);
}

// ---------------------------------------------------------------------------------------
// DOCUMENT-ROOT CASES.  Every case above this line hands the emitter a root
// NODE.  The shipped path cannot: on this build the document root is a DESCRIPTOR, and
// IDENT.JSON_FIND_NODE returns a CHILD COUNT -- handing that count over as the root is what
// refused every live entrant's profile at a bogus address like 0x11.  So these cases drive
// EmitDocumentJson through a stand-in built to THAT contract, over REAL pages -- one committed,
// one only reserved -- probed with the same VirtualQuery answer identity_access binds.

namespace {

// identity_access's ReadableProbe, rebuilt here rather than imported: the emitter under test must
// be answered about pages this file allocated itself, and a fake `readable` that simply returns
// false could not tell a reserved page from a wrong constant.
bool CommittedReadable(const void* p, size_t bytes) {
    if (!p || bytes == 0) return false;
    const uint8_t* cursor = static_cast<const uint8_t*>(p);
    const uint8_t* end    = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(cursor, &info, sizeof info) != sizeof info) return false;
        if (info.State != MEM_COMMIT) return false;
        if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        cursor = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

// The document handle the accessors take first.  It is a descriptor: it holds the root object's
// children and has no node hook, exactly as *(json+0) does on this build.
struct FakeDocument {
    std::vector<FakeNode*> children;
};

// IDENT.JSON_PATH_ELEM: the child iterator AT A PATH, prev == null yielding the first.  "" is the
// document root, the only path the shipped walk asks it for.
void* FakePathElem(void* json, const char* path, void* prev) {
    FakeDocument* d = static_cast<FakeDocument*>(json);
    if (!path || *path) return nullptr;
    if (!prev) return d->children.empty() ? nullptr : d->children[0];
    for (size_t i = 0; i + 1 < d->children.size(); ++i)
        if (d->children[i] == prev) return d->children[i + 1];
    return nullptr;
}

// IDENT.JSON_FIND_NODE, to its ACTUAL contract: a CHILD COUNT.  Never a node, never a pointer.
int64_t FakePathCount(void* json, const char* path) {
    FakeDocument* d = static_cast<FakeDocument*>(json);
    if (!path || *path) return 0;
    return int64_t(d->children.size());
}

entity_json::CJsonReader MakeDocumentReader(FakeDocument* doc) {
    entity_json::CJsonReader r = MakeReader();
    r.json      = doc;
    r.readable  = &CommittedReadable;
    r.pathElem  = &FakePathElem;
    r.pathCount = &FakePathCount;
    return r;
}

struct DocRun {
    bool        ok;
    JsonRefusal why;
    std::string text;
    size_t      written;
    bool        truncated;
    std::string path;
    const void* at;
    size_t      extent;
    uint8_t     buf[kRunBuf];
};

DocRun EmitDoc(FakeDocument* doc, const char* path = nullptr) {
    DocRun r;
    memset(r.buf, 0xCC, sizeof r.buf);
    r.why = JsonRefusal::None;
    r.ok  = entity_json::EmitDocumentJson(MakeDocumentReader(doc), path, r.buf, sizeof r.buf,
                                          r.written, r.truncated, &r.why);
    r.text.assign(reinterpret_cast<const char*>(r.buf), r.ok ? r.written : 0);
    r.path   = entity_json::LastRefusalPath();
    r.at     = entity_json::LastRefusalAt();
    r.extent = entity_json::LastRefusalExtent();
    return r;
}

// Two reserved pages with only the FIRST committed -- the boundary kNodeAccessedExtent is about.
struct PageEdge {
    uint8_t* base;
    size_t   page;
};

PageEdge MakePageEdge() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    PageEdge e;
    e.page = si.dwPageSize;
    e.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, e.page * 2, MEM_RESERVE, PAGE_NOACCESS));
    if (e.base && !VirtualAlloc(e.base, e.page, MEM_COMMIT, PAGE_READWRITE)) e.base = nullptr;
    return e;
}

// A node whose LAST byte is the last byte of its allocation.  Every offset the engine reads at
// fixed positions (node+0x08, +0x18, +0x28) is inside it; the next page is not memory.
FakeNode* NodeAtCommittedEdge(const PageEdge& e, uint32_t kind) {
    uintptr_t at = reinterpret_cast<uintptr_t>(e.base) + e.page - sizeof(FakeNode);
    at &= ~uintptr_t(0xF);
    return new (reinterpret_cast<void*>(at)) FakeNode(kind);
}

// A string body in an allocation of its own, so the node proof and the string proof cannot be
// the same proof.
const char* OutOfLineString(const char* text) {
    const size_t n = strlen(text) + 1;
    char* p = static_cast<char*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE));
    if (p) memcpy(p, text, n);
    return p;
}

}  // namespace

TEST_CASE("W1-E DOCROOT 20: a node at the edge of its allocation, its string in another one") {
    PageEdge edge = MakePageEdge();
    REQUIRE(edge.base != nullptr);

    FakeNode* name = NodeAtCommittedEdge(edge, uint32_t(Kind::String));
    name->key = "name";
    name->str = OutOfLineString("Ghost");
    REQUIRE(name->str != nullptr);

    // The fixture IS the claim, so it is asserted rather than assumed: the engine's own fixed
    // reads fit inside the allocation, a page-wide proof of the same node does not, and the
    // string is not in the node's allocation at all.
    CHECK(CommittedReadable(name, entity_json::kNodeAccessedExtent));
    CHECK_FALSE(CommittedReadable(name, edge.page));
    CHECK(reinterpret_cast<uintptr_t>(name) + entity_json::kNodeAccessedExtent <=
          reinterpret_cast<uintptr_t>(edge.base) + edge.page);
    const uintptr_t strAt = reinterpret_cast<uintptr_t>(name->str);
    CHECK((strAt < reinterpret_cast<uintptr_t>(edge.base) ||
           strAt >= reinterpret_cast<uintptr_t>(edge.base) + 2 * edge.page));

    FakeNode* profile = Obj("profile");
    profile->kids.push_back(name);

    FakeDocument doc;
    doc.children.push_back(profile);

    DocRun r = EmitDoc(&doc);
    CHECK(r.ok);
    CHECK(r.why == JsonRefusal::None);
    CHECK(r.text == "{\"profile\":{\"name\":\"Ghost\"}}");
    CHECK(r.written == r.text.size());
    CHECK_FALSE(r.truncated);
}

TEST_CASE("W1-E DOCROOT 21: a node in a reserved page refuses, and names WHICH node") {
    PageEdge edge = MakePageEdge();
    REQUIRE(edge.base != nullptr);

    // The second page of the reservation was never committed.  Nothing is constructed there --
    // this is a node pointer the engine's own accessors would fault on, which is the fault the
    // proof exists to get in front of.
    FakeNode* ghost = reinterpret_cast<FakeNode*>(edge.base + edge.page);
    REQUIRE_FALSE(CommittedReadable(ghost, entity_json::kNodeAccessedExtent));

    FakeNode* profile = Obj("profile");
    profile->kids.push_back(ghost);

    FakeDocument doc;
    doc.children.push_back(profile);

    DocRun r = EmitDoc(&doc);
    CHECK_FALSE(r.ok);
    CHECK(r.why == JsonRefusal::NodeUnreadable);
    CHECK(r.at == static_cast<const void*>(ghost));
    CHECK(r.extent == entity_json::kNodeAccessedExtent);
    CHECK(r.path.find("profile") != std::string::npos);
    CHECK(r.written == 0);
    CHECK(r.buf[0] == 0xCC);
}

TEST_CASE("W1-E DOCROOT 22: the root comes from the child ITERATOR, never from the count") {
    FakeDocument doc;
    doc.children.push_back(Str("displayname", "Ghost"));
    doc.children.push_back(Int("level", 50));
    doc.children.push_back(Boolean("moderator", false));
    FakeNode* unlocks = Obj("unlocks");
    unlocks->kids.push_back(Str("emote", "tada"));
    doc.children.push_back(unlocks);

    entity_json::CJsonReader r = MakeDocumentReader(&doc);

    // IDENT.JSON_FIND_NODE's answer for this document, to its real contract: 4, a count.
    const int64_t count = r.pathCount(r.json, "");
    CHECK(count == 4);
    CHECK(count == int64_t(doc.children.size()));
    // And what that answer is as a POINTER: not memory at all.  0x4 handed to the walker as the
    // root is the whole defect -- it came back NodeUnreadable with the count as its address.
    CHECK_FALSE(CommittedReadable(reinterpret_cast<const void*>(static_cast<uintptr_t>(count)),
                                  entity_json::kNodeAccessedExtent));

    DocRun whole = EmitDoc(&doc);
    CHECK(whole.ok);
    CHECK(whole.why == JsonRefusal::None);
    CHECK(whole.text ==
          "{\"displayname\":\"Ghost\",\"level\":50,\"moderator\":false,\"unlocks\":{\"emote\":\"tada\"}}");

    // A path resolves its first hop at the DOCUMENT level -- through the same iterator -- and
    // then walks with the per-node accessors, so a subtree is the whole walk entered one step in.
    DocRun sub = EmitDoc(&doc, "unlocks");
    CHECK(sub.ok);
    CHECK(sub.text == "{\"emote\":\"tada\"}");
    CHECK(whole.text.find(sub.text) != std::string::npos);

    DocRun leaf = EmitDoc(&doc, "level");
    CHECK(leaf.ok);
    CHECK(leaf.text == "50");

    DocRun missing = EmitDoc(&doc, "nosuch");
    CHECK_FALSE(missing.ok);
    CHECK(missing.why == JsonRefusal::NoSuchKey);
}
