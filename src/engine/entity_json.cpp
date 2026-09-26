#include "engine/entity_json.h"

#include <string.h>

#include "generated/facts_generated.h"

namespace replay {
namespace entity_json {

// One side declared, one side generated from the registry: the enum this emitter branches on is
// the switch IDENT.JSON_NODE_KIND actually contains, and a registry change stops the build rather
// than quietly re-labelling a node.
static_assert(uint32_t(Kind::String) == facts::IDENT_JSON_KIND_STRING, "kind vocabulary drifted");
static_assert(uint32_t(Kind::Int)    == facts::IDENT_JSON_KIND_INT,    "kind vocabulary drifted");
static_assert(uint32_t(Kind::Float)  == facts::IDENT_JSON_KIND_FLOAT,  "kind vocabulary drifted");
static_assert(uint32_t(Kind::Bool)   == facts::IDENT_JSON_KIND_BOOL,   "kind vocabulary drifted");
static_assert(uint32_t(Kind::Array)  == facts::IDENT_JSON_KIND_ARRAY,  "kind vocabulary drifted");
static_assert(uint32_t(Kind::Object) == facts::IDENT_JSON_KIND_OBJECT, "kind vocabulary drifted");

}  // namespace entity_json
}  // namespace replay

namespace replay {
namespace entity_json {
namespace {

// The deepest nesting a cycle check can be exact over: every ancestor on the path is held so a
// child that IS one of its own ancestors is caught by identity rather than by the node budget.
// A caller asking for more than this is refused rather than quietly given a shallower check.
constexpr uint32_t kDepthCeiling = 64;

constexpr size_t kPathBytes = 240;

thread_local char        gLastPath[kPathBytes] = {0};
thread_local const void* gLastAt     = nullptr;
thread_local size_t      gLastExtent = 0;
// RECORDER-3 F4.  The owner's ruling: a float may never refuse the whole document.  The walk emits
// `null` for it -- the same four bytes in the sizing and the emitting pass, so the two still agree
// -- and raises this, which the recorder records beside the document as "a value was dropped".
thread_local bool        gLastLossyNumber = false;

// The second half of a node proof.  `readable` answers from VirtualQuery, which describes the
// region as it was when it was asked; this touches the bytes themselves, so a page that went away
// in between -- and a reader whose probe simply says yes -- is caught here instead of killing the
// process.  POD locals only: an object needing unwinding may not share a frame with __try.
bool TouchSpan(const void* at, size_t bytes) {
    if (!at || !bytes) return false;
#if defined(_MSC_VER)
    __try {
        const volatile unsigned char* p = static_cast<const volatile unsigned char*>(at);
        volatile unsigned char sink = p[0];
        sink = p[bytes - 1];
        (void)sink;
        return true;
    } __except (1) {
        return false;
    }
#else
    return true;
#endif
}

struct Walk {
    const CJsonReader* r;
    uint8_t*    out;
    size_t      cap;
    size_t      len;
    bool        write;
    uint32_t    maxDepth;
    uint32_t    maxNodes;
    uint32_t    nodes;
    uint32_t    depth;
    void*       ancestors[kDepthCeiling];
    char        path[kPathBytes];
    size_t      pathLen;
    JsonRefusal fail;
};

bool Refuse(Walk& w, JsonRefusal r) {
    w.fail = r;
    // Only a node proof has an address to name; every other word would otherwise inherit the
    // last node the walk happened to prove, which reads as evidence and is not.
    if (r != JsonRefusal::NodeUnreadable) { gLastAt = nullptr; gLastExtent = 0; }
    size_t n = w.pathLen < kPathBytes - 1 ? w.pathLen : kPathBytes - 1;
    memcpy(gLastPath, w.path, n);
    gLastPath[n] = '\0';
    return false;
}

void Put(Walk& w, char c) {
    if (w.write && w.len < w.cap) w.out[w.len] = uint8_t(c);
    ++w.len;
}

void PutRaw(Walk& w, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) Put(w, s[i]);
}

// The path segment is for the refusal message only.  It is truncated rather than grown: a
// refusal that could not name its node still names its refusal, and a heap here would be a
// second failure mode inside the one that reports failures.
size_t PushPath(Walk& w, const char* key) {
    const size_t save = w.pathLen;
    if (w.pathLen + 1 < kPathBytes) w.path[w.pathLen++] = '.';
    for (size_t i = 0; key[i] && w.pathLen + 1 < kPathBytes; ++i) w.path[w.pathLen++] = key[i];
    w.path[w.pathLen] = '\0';
    return save;
}

void PopPath(Walk& w, size_t save) {
    w.pathLen = save;
    w.path[w.pathLen] = '\0';
}

const char kHex[] = "0123456789abcdef";

// One bounded scan that finds the terminator, probes committed memory at every page it crosses
// and refuses a byte string no canonical JSON encodes.  A key and a value take the same route.
bool PutJsonString(Walk& w, const char* s) {
    if (!s || !w.r->readable(s, 1)) return Refuse(w, JsonRefusal::StringUnreadable);

    size_t n = 0;
    while (n < kMaxStringBytes) {
        const char* p = s + n;
        if ((reinterpret_cast<uintptr_t>(p) & 0xFFFu) == 0 && !w.r->readable(p, 1))
            return Refuse(w, JsonRefusal::StringUnreadable);
        if (!*p) break;
        ++n;
    }
    if (n == kMaxStringBytes) return Refuse(w, JsonRefusal::StringTooLong);

    Put(w, '"');
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80u) {
            switch (c) {
                case '"':  PutRaw(w, "\\\"", 2); break;
                case '\\': PutRaw(w, "\\\\", 2); break;
                case 0x08: PutRaw(w, "\\b", 2);  break;
                case 0x09: PutRaw(w, "\\t", 2);  break;
                case 0x0A: PutRaw(w, "\\n", 2);  break;
                case 0x0C: PutRaw(w, "\\f", 2);  break;
                case 0x0D: PutRaw(w, "\\r", 2);  break;
                default:
                    if (c < 0x20u) {
                        PutRaw(w, "\\u00", 4);
                        Put(w, kHex[c >> 4]);
                        Put(w, kHex[c & 0xF]);
                    } else {
                        Put(w, char(c));
                    }
            }
            ++i;
            continue;
        }

        // Multi-byte UTF-8, validated before it is copied: length, continuation bytes, the
        // overlong encodings and the surrogate range.  An invalid sequence has no canonical
        // JSON form, and passing it through would produce a document no parser accepts.
        uint32_t cp = 0;
        size_t   need = 0;
        if (c >= 0xC2u && c <= 0xDFu)      { cp = c & 0x1Fu; need = 1; }
        else if (c >= 0xE0u && c <= 0xEFu) { cp = c & 0x0Fu; need = 2; }
        else if (c >= 0xF0u && c <= 0xF4u) { cp = c & 0x07u; need = 3; }
        else return Refuse(w, JsonRefusal::StringNotUtf8);

        if (i + need >= n) return Refuse(w, JsonRefusal::StringNotUtf8);
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) return Refuse(w, JsonRefusal::StringNotUtf8);
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (need == 2 && cp < 0x800u) return Refuse(w, JsonRefusal::StringNotUtf8);
        if (need == 3 && cp < 0x10000u) return Refuse(w, JsonRefusal::StringNotUtf8);
        if (cp > 0x10FFFFu) return Refuse(w, JsonRefusal::StringNotUtf8);
        if (cp >= 0xD800u && cp <= 0xDFFFu) return Refuse(w, JsonRefusal::StringNotUtf8);

        for (size_t k = 0; k <= need; ++k) Put(w, s[i + k]);
        i += need + 1;
    }
    Put(w, '"');
    return true;
}

void PutInt(Walk& w, int64_t v) {
    char     buf[24];
    size_t   n = 0;
    uint64_t mag = v < 0 ? (~static_cast<uint64_t>(v) + 1u) : static_cast<uint64_t>(v);
    do {
        buf[n++] = char('0' + (mag % 10u));
        mag /= 10u;
    } while (mag);
    if (v < 0) Put(w, '-');
    while (n) Put(w, buf[--n]);
}

bool EmitNode(Walk& w, void* node);

// The exact bytes the engine's own accessors read at fixed offsets from a node -- proven, then
// touched.  Never a page: a node whose neighbouring page is not committed is one the engine reads
// happily, and refusing it would be OUR bound, not the engine's.
bool ProveNode(Walk& w, void* node) {
    gLastAt     = node;
    gLastExtent = kNodeAccessedExtent;
    return w.r->readable(node, kNodeAccessedExtent) && TouchSpan(node, kNodeAccessedExtent);
}

// One object body, whether it is a node in the tree or the DOCUMENT ROOT -- which has no node
// pointer on this build and is reached through pathElem/pathCount instead.  One loop, so a rule
// added to the tree walk can never be missing from the root.
bool EmitObjectBody(Walk& w, void* node, bool document) {
    if (w.depth >= w.maxDepth) return Refuse(w, JsonRefusal::DepthLimit);
    // The document root has no identity to hold; no child can be null (EmitNode refuses one), so
    // the null parked here can never collide with a real ancestor.
    w.ancestors[w.depth++] = node;
    Put(w, '{');
    int64_t walked = 0;
    void*   prev   = nullptr;
    for (void* child = document ? w.r->pathElem(w.r->json, "", nullptr)
                                : w.r->nextElem(w.r->json, node, nullptr);
         child;
         child = document ? w.r->pathElem(w.r->json, "", prev)
                          : w.r->nextElem(w.r->json, node, prev)) {
        // The child is proven BEFORE it is handed to an accessor.  IDENT.JSON_NODE_NAME reads
        // node+0x28, so asking for the key first would dereference a node this walk has not
        // proven -- putting the guard after the read it exists to get in front of.  The key is
        // not knowable yet, so the refusal names the PARENT's path and LastRefusalAt() names
        // the node.
        if (!ProveNode(w, child)) return Refuse(w, JsonRefusal::NodeUnreadable);
        if (walked) Put(w, ',');
        const char* key = w.r->name(w.r->json, child);
        if (!key) return Refuse(w, JsonRefusal::KeyUnreadable);
        const size_t save = PushPath(w, key);
        if (!PutJsonString(w, key)) return false;
        Put(w, ':');
        if (!EmitNode(w, child)) return false;
        PopPath(w, save);
        ++walked;
        prev = child;
    }
    Put(w, '}');
    --w.depth;
    // Two different engine functions reach the child set -- the iterator walks the container, the
    // count reads its size field -- so a walk that stopped early is a disagreement between them
    // and not a shorter document.
    const int64_t counted = document ? w.r->pathCount(w.r->json, "")
                                     : w.r->elemCount(w.r->json, node);
    if (counted != walked) return Refuse(w, JsonRefusal::ChildCountDisagrees);
    return true;
}

bool EmitNode(Walk& w, void* node) {
    if (!node || !ProveNode(w, node)) return Refuse(w, JsonRefusal::NodeUnreadable);
    if (++w.nodes > w.maxNodes) return Refuse(w, JsonRefusal::NodeLimit);
    for (uint32_t i = 0; i < w.depth; ++i)
        if (w.ancestors[i] == node) return Refuse(w, JsonRefusal::Cycle);

    switch (static_cast<Kind>(w.r->kind(w.r->json, node))) {
        case Kind::Object:
            return EmitObjectBody(w, node, false);
        case Kind::Array:
            // IDENT.JSON_NEXT_ELEM iterates the tag-0 container only, so an array's elements
            // are unreachable through this build's read API.  Empty IS the whole document.
            if (w.r->elemCount(w.r->json, node) == 0) { PutRaw(w, "[]", 2); return true; }
            return Refuse(w, JsonRefusal::ArrayNotWalkable);
        case Kind::String:
            return PutJsonString(w, w.r->stringValue(w.r->json, node, nullptr));
        case Kind::Int:
            PutInt(w, w.r->intValue(w.r->json, node, 0));
            return true;
        case Kind::Bool: {
            const int b = w.r->boolValue(w.r->json, node, -1);
            if (b == 1) { PutRaw(w, "true", 4);  return true; }
            if (b == 0) { PutRaw(w, "false", 5); return true; }
            return Refuse(w, JsonRefusal::UnknownNodeKind);
        }
        case Kind::Float:
            // Not a refusal any more.  There is no float32 accessor bound on this build, so the
            // number itself cannot be read without fabricating it; `null` says "this key existed
            // and its value is not here", and the other 99% of the document reaches the file.
            gLastLossyNumber = true;
            PutRaw(w, "null", 4);
            return true;
        case Kind::None:
        default:
            return Refuse(w, JsonRefusal::UnknownNodeKind);
    }
}

void Begin(Walk& w, const CJsonReader& r, const JsonEmitLimits& lim, uint8_t* out, size_t cap,
           bool write) {
    memset(&w, 0, sizeof w);
    w.r        = &r;
    w.out      = out;
    w.cap      = cap;
    w.write    = write;
    w.maxDepth = lim.maxDepth;
    w.maxNodes = lim.maxNodes;
    w.path[0]  = '$';
    w.pathLen  = 1;
}

bool Why(JsonRefusal* why, JsonRefusal r) {
    if (why) *why = r;
    return r == JsonRefusal::None;
}

// One child key against one path segment.  The key is engine memory that may be neither
// terminated nor fully committed, so the read is bounded by the segment and probes at every page
// it crosses -- the same discipline PutJsonString uses for the bytes it copies.
bool KeyEquals(const CJsonReader& r, const char* key, const char* seg, size_t segLen,
               bool& unreadable) {
    for (size_t i = 0; i <= segLen; ++i) {
        const char* p = key + i;
        if ((reinterpret_cast<uintptr_t>(p) & 0xFFFu) == 0 && !r.readable(p, 1)) {
            unreadable = true;
            return false;
        }
        if (i == segLen) return *p == '\0';
        if (*p != seg[i]) return false;
    }
    return false;
}

}  // namespace

void* FindByKeyPathFrom(const CJsonReader& r, void* root, bool document,
                        const char* path, JsonRefusal* why);

const char* JsonRefusalText(JsonRefusal r) {
    switch (r) {
        case JsonRefusal::None:             return "none";
        case JsonRefusal::NoReader:         return "the CJson accessor table is incomplete, so no node can be read";
        case JsonRefusal::NoRoot:           return "the document has no root node";
        case JsonRefusal::NodeUnreadable:   return "a node pointer is not in committed memory";
        case JsonRefusal::KeyUnreadable:    return "a child of an object has no readable key";
        case JsonRefusal::StringUnreadable: return "a string value's pointer is null or not in committed memory";
        case JsonRefusal::StringTooLong:    return "a key or string value ran past the scan bound with no terminator";
        case JsonRefusal::StringNotUtf8:    return "a key or string value is not valid UTF-8, so no canonical JSON encodes it";
        case JsonRefusal::DepthLimit:       return "the tree is deeper than the caller's depth bound";
        case JsonRefusal::NodeLimit:        return "the tree holds more nodes than the caller's node bound";
        case JsonRefusal::Cycle:            return "a node is one of its own ancestors, so the document does not terminate";
        case JsonRefusal::ArrayNotWalkable: return "a populated array -- IDENT.JSON_NEXT_ELEM iterates objects only, so this build cannot reach its elements";
        case JsonRefusal::NumberNotExact:   return "a non-integer number -- the only per-node accessor on this build returns float32, which would re-quantise it";
        case JsonRefusal::UnknownNodeKind:  return "IDENT.JSON_NODE_KIND does not name this node's tag, and a guessed value would be a fabricated one";
        case JsonRefusal::ChildCountDisagrees: return "the child walk and IDENT.JSON_ELEM_COUNT do not agree on how many children a container has";
        case JsonRefusal::TreeChanged:      return "the sizing pass and the emitting pass produced different lengths -- the tree moved under the walk";
        case JsonRefusal::ShortBuffer:      return "the caller's buffer is shorter than the document; nothing was written and the full length was reported";
        case JsonRefusal::NoSuchKey:        return "a segment of the key path names no child of its parent, so there is no subtree to serialise";
    }
    return "unknown";
}

const char* LastRefusalPath() { return gLastPath; }
const void* LastRefusalAt()     { return gLastAt; }
size_t      LastRefusalExtent() { return gLastExtent; }
bool        LastEmitDroppedNumber() { return gLastLossyNumber; }

JsonEmitLimits DefaultLimits() {
    JsonEmitLimits l;
    l.maxDepth = 32;
    l.maxNodes = 4096;
    return l;
}

bool EmitCanonicalJson(const CJsonReader& r, void* root, void* out, size_t cap, size_t& written,
                       bool& truncated, JsonRefusal* why, const JsonEmitLimits* limits) {
    written   = 0;
    truncated = false;
    gLastPath[0] = '\0';
    gLastAt      = nullptr;
    gLastExtent  = 0;
    gLastLossyNumber = false;

    if (!r.kind || !r.name || !r.stringValue || !r.intValue || !r.boolValue || !r.nextElem ||
        !r.elemCount || !r.readable)
        return Why(why, JsonRefusal::NoReader);
    if (!root) return Why(why, JsonRefusal::NoRoot);

    const JsonEmitLimits lim = limits ? *limits : DefaultLimits();
    if (lim.maxDepth == 0 || lim.maxDepth > kDepthCeiling || lim.maxNodes == 0)
        return Why(why, JsonRefusal::DepthLimit);

    Walk sizing;
    Begin(sizing, r, lim, nullptr, 0, false);
    if (!EmitNode(sizing, root)) return Why(why, sizing.fail);

    written = sizing.len;
    if (sizing.len > cap || (!out && sizing.len)) {
        truncated = sizing.len > cap;
        return Why(why, JsonRefusal::ShortBuffer);
    }

    Walk emitting;
    Begin(emitting, r, lim, static_cast<uint8_t*>(out), cap, true);
    if (!EmitNode(emitting, root)) return Why(why, emitting.fail);
    if (emitting.len != sizing.len) return Why(why, JsonRefusal::TreeChanged);

    return Why(why, JsonRefusal::None);
}

void* FindByKeyPath(const CJsonReader& r, void* root, const char* path, JsonRefusal* why) {
    return FindByKeyPathFrom(r, root, false, path, why);
}

void* FindByKeyPathFrom(const CJsonReader& r, void* root, bool document,
                        const char* path, JsonRefusal* why) {
    gLastPath[0] = '$';
    gLastPath[1] = '\0';

    if (!r.kind || !r.name || !r.nextElem || !r.readable ||
        (document && (!r.pathElem || !r.pathCount))) {
        Why(why, JsonRefusal::NoReader);
        return nullptr;
    }
    if (!root && !document) {
        Why(why, JsonRefusal::NoRoot);
        return nullptr;
    }
    if (!path || !*path) {
        Why(why, JsonRefusal::None);
        return root;
    }

    void*  node    = root;
    size_t pathLen = 1;
    for (const char* seg = path;;) {
        size_t segLen = 0;
        while (seg[segLen] && seg[segLen] != '.') ++segLen;

        if (pathLen + 1 < kPathBytes) gLastPath[pathLen++] = '.';
        for (size_t i = 0; i < segLen && pathLen + 1 < kPathBytes; ++i)
            gLastPath[pathLen++] = seg[i];
        gLastPath[pathLen] = '\0';

        const bool atDocument = document && node == nullptr;
        if (!atDocument && (!r.readable(node, kNodeAccessedExtent) ||
                            !TouchSpan(node, kNodeAccessedExtent))) {
            gLastAt     = node;
            gLastExtent = kNodeAccessedExtent;
            Why(why, JsonRefusal::NodeUnreadable);
            return nullptr;
        }
        // Only an object HAS keys.  A segment asked of a leaf, of an array (whose elements this
        // build cannot reach at all), or of a tag the accessor does not name is a key that is not
        // there -- not a node to serialise, and not a silent fall-through to the parent.
        if (!atDocument && static_cast<Kind>(r.kind(r.json, node)) != Kind::Object) {
            Why(why, JsonRefusal::NoSuchKey);
            return nullptr;
        }

        void* found = nullptr;
        void* prev  = nullptr;
        for (void* child = atDocument ? r.pathElem(r.json, "", nullptr)
                                      : r.nextElem(r.json, node, nullptr);
             child;
             child = atDocument ? r.pathElem(r.json, "", prev)
                                : r.nextElem(r.json, node, prev)) {
            // Same order as the emitter's: the child is proven before IDENT.JSON_NODE_NAME
            // dereferences it.
            if (!r.readable(child, kNodeAccessedExtent) || !TouchSpan(child, kNodeAccessedExtent)) {
                gLastAt     = child;
                gLastExtent = kNodeAccessedExtent;
                Why(why, JsonRefusal::NodeUnreadable);
                return nullptr;
            }
            const char* key = r.name(r.json, child);
            if (!key || !r.readable(key, 1)) {
                Why(why, JsonRefusal::KeyUnreadable);
                return nullptr;
            }
            bool unreadable = false;
            if (KeyEquals(r, key, seg, segLen, unreadable)) {
                found = child;
                break;
            }
            if (unreadable) {
                Why(why, JsonRefusal::KeyUnreadable);
                return nullptr;
            }
            prev = child;
        }
        if (!found) {
            Why(why, JsonRefusal::NoSuchKey);
            return nullptr;
        }
        node = found;

        if (!seg[segLen]) break;
        seg += segLen + 1;
    }

    Why(why, JsonRefusal::None);
    return node;
}

bool EmitDocumentJson(const CJsonReader& r, const char* path, void* out, size_t cap,
                      size_t& written, bool& truncated, JsonRefusal* why,
                      const JsonEmitLimits* limits) {
    written   = 0;
    truncated = false;
    gLastLossyNumber = false;

    if (!r.pathElem || !r.pathCount) return Why(why, JsonRefusal::NoReader);

    // A non-empty path still names a NODE, and from the first hop the per-node accessors take
    // over -- so a subtree and the whole document are the same walk, entered one step apart.
    if (path && *path) {
        JsonRefusal found = JsonRefusal::None;
        void*       node  = FindByKeyPathFrom(r, nullptr, true, path, &found);
        if (!node) return Why(why, found);
        return EmitCanonicalJson(r, node, out, cap, written, truncated, why, limits);
    }

    if (!r.kind || !r.name || !r.stringValue || !r.intValue || !r.boolValue || !r.nextElem ||
        !r.elemCount || !r.readable)
        return Why(why, JsonRefusal::NoReader);

    const JsonEmitLimits lim = limits ? *limits : DefaultLimits();
    if (lim.maxDepth == 0 || lim.maxDepth > kDepthCeiling || lim.maxNodes == 0)
        return Why(why, JsonRefusal::DepthLimit);

    gLastPath[0] = '$';
    gLastPath[1] = '\0';
    gLastAt      = nullptr;
    gLastExtent  = 0;

    Walk sizing;
    Begin(sizing, r, lim, nullptr, 0, false);
    if (!EmitObjectBody(sizing, nullptr, true)) return Why(why, sizing.fail);

    written = sizing.len;
    if (sizing.len > cap || (!out && sizing.len)) {
        truncated = sizing.len > cap;
        return Why(why, JsonRefusal::ShortBuffer);
    }

    Walk emitting;
    Begin(emitting, r, lim, static_cast<uint8_t*>(out), cap, true);
    if (!EmitObjectBody(emitting, nullptr, true)) return Why(why, emitting.fail);
    if (emitting.len != sizing.len) return Why(why, JsonRefusal::TreeChanged);

    return Why(why, JsonRefusal::None);
}

bool EmitCanonicalJsonAtPath(const CJsonReader& r, void* root, const char* path, void* out,
                             size_t cap, size_t& written, bool& truncated, JsonRefusal* why,
                             const JsonEmitLimits* limits) {
    written   = 0;
    truncated = false;

    JsonRefusal found = JsonRefusal::None;
    void*       node  = FindByKeyPath(r, root, path, &found);
    if (!node) return Why(why, found);

    return EmitCanonicalJson(r, node, out, cap, written, truncated, why, limits);
}

}  // namespace entity_json
}  // namespace replay
