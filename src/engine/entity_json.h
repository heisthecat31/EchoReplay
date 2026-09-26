#pragma once

// B.4 (E-4) -- canonical JSON for a live CJson node tree, walked with the ENGINE'S OWN read
// accessors.  Owned by W1-E.
//
// ---------------------------------------------------------------------------------------
// WHY THIS EXISTS.  ENG-W1E-READERS S1.3 established that the entrant profile at
// CR15NetGame + IDENT.PROFILE_JSON_OFF + 592*index is a CJson NODE TREE and that this image
// ships `cjsoninspectorread` with no write-side counterpart, so ProfileJsonBytes refused
// NoJsonWriter for every entrant.  A missing engine serialiser is not a missing capability:
// the document is fully readable one node at a time, so the bytes are produced HERE, from the
// engine's own accessors, and never by guessing at node layout.  Nothing below dereferences a
// node.  Every field of every node arrives through a function pointer the caller binds to the
// engine entry point that reads it.
//
// THE ACCESSOR SET IS THE ENGINE'S, read out of the shipped image:
//
//   kind        IDENT.JSON_NODE_KIND    0x1405FE8C0  switch over the node's tag -> 1..6
//   name        IDENT.JSON_NODE_NAME    0x1405F72A0  the child's key; what CR15NetGame's own
//                                                    unlocks walk (0x1401552D0) symbolises
//   stringValue IDENT.JSON_NODE_STRING  0x1405FE250  tag 2 only, else the default
//   intValue    IDENT.JSON_NODE_INT     0x1405F3040  tag 3 only, else the default
//   boolValue   IDENT.JSON_NODE_BOOL    0x1405EE820  tag 5 -> 1, tag 6 -> 0, else the default
//   nextElem    IDENT.JSON_NEXT_ELEM    0x1405F7320  children of an OBJECT; see below
//   elemCount   IDENT.JSON_ELEM_COUNT   0x1405EFA20  child count of an object OR an array
//   pathCount   IDENT.JSON_FIND_NODE    0x1405EFA60  child COUNT at a path -- NOT a node.  See
//                                                    below: this row's name is a misreading that
//                                                    cost every live entrant its profile bytes.
//   pathElem    IDENT.JSON_PATH_ELEM    0x1405F73C0  the child iterator AT A PATH; the only way
//                                                    to reach the DOCUMENT ROOT's children
//
// ---------------------------------------------------------------------------------------
// THE DOCUMENT ROOT IS NOT A NODE
//
//   sub_1405EFA60 (IDENT.JSON_FIND_NODE) ends:  if (*d == 1) return sub_14135BCA0(d);
//                                               if (!*d)     return sub_14135C270(d);
//   sub_1405EFA20 (IDENT.JSON_ELEM_COUNT) ends on the SAME two callees and the same two tags.
//   So JSON_FIND_NODE returns a CHILD COUNT, and IDENT.JSON_IS_EMPTY (0x1405F1360) is literally
//   `ElemCountAtPath(json, "") == 0` -- "the root has no children", never "there is no root".
//   Handing that count to the walker as a node pointer is what made every live profile refuse
//   NodeUnreadable at a bogus address like 0x11.  The root is a DESCRIPTOR (*(json+0)), it has
//   no node pointer at all, and IDENT.JSON_PATH_ELEM is the engine's own way in -- CR15NetGame's
//   /session writer (0x1401552D0) reaches the unlocks children through it.
//
// ---------------------------------------------------------------------------------------
// FOUR HONEST BOUNDARIES, each a distinct refusal rather than a plausible byte
//
//   ARRAYS.  IDENT.JSON_NEXT_ELEM opens with `d = descriptor(node); if (!d || *d) return 0` --
//   it iterates the tag-0 container ONLY, and an array is tag 1 (IDENT.JSON_ELEM_COUNT reads
//   the two containers through two different functions, tag 0 and tag 1).  So this build's read
//   API cannot hand back an array's elements at all.  An array with elemCount == 0 is emitted
//   as `[]`, because that IS the whole document; a populated one is ArrayNotWalkable, naming
//   the path.  Emitting `[]` for a populated array would be the confident wrong answer.
//
//   NON-INTEGER NUMBERS.  The only per-NODE accessor for tag 4 is 0x1405FC9E0, which computes
//   the double and returns `result.m128_f32[0]` -- a float32.  Re-quantising a double the
//   client held is exactly what locked constraint 8 forbids, so a Float node is NumberNotExact.
//   (0x1405F6990 returns the full double, but only by PATH from the document root, which is a
//   different route and a different packet.)
//
//   TAGS THE ACCESSOR DOES NOT NAME.  IDENT.JSON_NODE_KIND returns 0 both for a null node and
//   for any tag outside its switch; tag 7 exists (0x1405ECF40 and 0x1405ED0B0 both test
//   `*node != 7`) and is not in that switch.  Writing `null` for a value we could not read is
//   the defect this subsystem exists to remove, so kind 0 is UnknownNodeKind, naming the path.
//
//   A CHILD WALK THAT ENDS EARLY.  Every container's walked child count is compared against
//   IDENT.JSON_ELEM_COUNT, which reaches the count through a different function than the
//   iterator does.  A disagreement is ChildCountDisagrees, not a shorter document.
// ---------------------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

namespace replay {
namespace entity_json {

// The values IDENT.JSON_NODE_KIND's switch produces.  Registered as facts and read from
// facts_generated.h at the binding site; named here so the emitter branches on words.
enum class Kind : uint32_t {
    None   = 0,   // a null node, or a tag the accessor's switch does not name
    String = 1,
    Int    = 2,
    Float  = 3,
    Bool   = 4,
    Array  = 5,
    Object = 6,
};

// The engine's CJson read API as this walker needs it.  `json` is the document handle every
// accessor takes first.  `readable` is the caller's committed-memory probe: it is a member
// rather than a direct buildguard call so the walker stays pure over its inputs and the
// unreadable-node arm is reachable from a test with no game present.
struct CJsonReader {
    void*       json;
    uint32_t    (*kind)(void* json, void* node);
    const char* (*name)(void* json, void* node);
    const char* (*stringValue)(void* json, void* node, const char* dflt);
    int64_t     (*intValue)(void* json, void* node, int64_t dflt);
    int         (*boolValue)(void* json, void* node, int dflt);
    void*       (*nextElem)(void* json, void* parent, void* prev);
    int64_t     (*elemCount)(void* json, void* node);
    bool        (*readable)(const void* p, size_t bytes);
    // The DOCUMENT-level pair, appended rather than inserted so existing aggregate
    // initialisers keep their meaning.  pathElem iterates the children of the node at path
    // ("" is the document root); pathCount is how many children that node has.  They are the
    // only way to the root, which is a descriptor and has no node pointer on this build.
    void*       (*pathElem)(void* json, const char* path, void* prev);
    int64_t     (*pathCount)(void* json, const char* path);
};

enum class JsonRefusal : uint32_t {
    None = 0,
    NoReader,             // the accessor table is incomplete -- a site did not verify
    NoRoot,               // the document has no root node
    NodeUnreadable,       // a node pointer is not in committed memory
    KeyUnreadable,        // a child of an object has no readable key
    StringUnreadable,     // a String node's value pointer is null or not readable
    StringTooLong,        // a key or value runs past kMaxStringBytes without a terminator
    StringNotUtf8,        // the bytes are not valid UTF-8, so no canonical JSON encodes them
    DepthLimit,
    NodeLimit,
    Cycle,                // a child is one of its own ancestors
    ArrayNotWalkable,     // a populated array; this build's read API cannot reach its elements
    NumberNotExact,       // a Float node; the only per-node accessor is float32-lossy
    UnknownNodeKind,      // IDENT.JSON_NODE_KIND answered 0 for a node that exists
    ChildCountDisagrees,  // the walk and IDENT.JSON_ELEM_COUNT do not agree
    TreeChanged,          // the sizing pass and the emitting pass produced different lengths
    ShortBuffer,          // `cap` is shorter than the document; nothing was written
    NoSuchKey,            // a segment of the caller's key path names no child of its parent
};

const char* JsonRefusalText(JsonRefusal);

// The path of the node a refusal names, or "" when the refusal has no path (NoReader, NoRoot).
// Valid until the next EmitCanonicalJson call on this thread.
const char* LastRefusalPath();

// The node the last refusal was raised ON, and the byte extent that was being proven of it.
// A refusal that cannot name its node is a refusal a reader cannot act on: the path says WHERE in
// the document, these two say WHICH bytes were asked for and were not there.
const void* LastRefusalAt();
size_t      LastRefusalExtent();
// True when the last emit wrote `null` in place of a non-integer number (RECORDER-3 F4).  The
// document is complete apart from those values, and the recorder flags the row rather than
// throwing the document away.
bool        LastEmitDroppedNumber();

// The byte extent of a CJson NODE that this build's accessors read at FIXED offsets, and the
// exact span a node proof must cover -- no more.  Byte-exact from echovr.exe md5
// caa05ee3bca2f5df0809994e93e2cc05:
//   0x1401067B0  48 8B 41 18 C3  mov rax,[rcx+18h]/ret   the descriptor, node+0x18 (the deepest
//                                                        fixed read: NODE_KIND, NEXT_ELEM and
//                                                        ELEM_COUNT all reach it)
//   0x14135EFC0  48 8B 42 08     mov rax,[rdx+8]         the sibling link, node+0x08
//   0x14135EFB0  48 8D 41 28 C3  lea rax,[rcx+28h]/ret   the inline key, node+0x28
// The key is a variable-length inline array and is proven by the string scanner's own per-page
// probe, not by this constant.  Proving a PAGE here would refuse a node whose neighbouring page
// is not committed even though the engine never touches it -- the failure this constant exists
// to prevent.
constexpr size_t kNodeAccessedExtent = 0x20;

struct JsonEmitLimits {
    uint32_t maxDepth;
    uint32_t maxNodes;
};

JsonEmitLimits DefaultLimits();

// Longest key or string value this walker will scan for a terminator.
constexpr size_t kMaxStringBytes = 4096;

// Serialise the tree under `root` as canonical RFC 8259 JSON: no insignificant whitespace,
// keys in TREE ORDER (the order IDENT.JSON_NEXT_ELEM yields them, which is the order the
// engine itself walks), `"` `\` and every byte below 0x20 escaped, `\b \f \n \r \t` used where
// RFC 8259 defines them and `\u00XX` otherwise.
//
// `written` is the length of the DOCUMENT on every path, so a caller refused with ShortBuffer
// sizes its second call from it.  `truncated` says `cap` was short.  The short arm writes
// NOTHING -- `out` is byte-for-byte untouched -- because a JSON document cut at a byte boundary
// parses as nothing and would reach the demo file looking like a capture.  That guarantee is
// why the tree is walked TWICE: once to size, once to write, with the two lengths compared.
bool EmitCanonicalJson(const CJsonReader&, void* root, void* out, size_t cap, size_t& written,
                       bool& truncated, JsonRefusal* why = nullptr,
                       const JsonEmitLimits* limits = nullptr);

// Resolve a '.'-separated KEY PATH from `root` -- "unlocks", or "a.b" -- one level at a time,
// through the same accessors the emitter walks with.  Returns the node, or null with *why set.
//
// It does NOT hand the dotted string to IDENT.JSON_FIND_NODE.  That accessor takes a path from
// the DOCUMENT ROOT, and this image never settles what its separator and escaping are -- the
// by-path double getter at 0x1405F6990 wants `|` and `[` given a meaning that nothing in the
// binary states -- so passing it "a.b" would be a guessed syntax answering with a confident
// wrong node.  Descending with IDENT.JSON_NEXT_ELEM and IDENT.JSON_NODE_NAME compares the key's
// exact bytes instead, which is also what makes the subtree provably a subtree of the whole
// document: it is the same node the whole-document walk reaches, in the same iteration order.
//
// The cost of that choice: a key that itself contains '.' is not addressable here, and comes
// back NoSuchKey rather than as some other node.  A null or empty path is `root` itself, which
// is the same convention IDENT.JSON_FIND_NODE uses for "".
void* FindByKeyPath(const CJsonReader&, void* root, const char* path, JsonRefusal* why = nullptr);

// FindByKeyPath then EmitCanonicalJson, with EVERY rule of the whole-document call unchanged:
// the same canonical encoding, the same depth/node/cycle bounds, the same refusals, and the same
// cap contract (`written` is the length of the SUBTREE on every path, the short arm writes
// nothing).  A path that names no node is NoSuchKey and writes nothing.  On an emit refusal
// LastRefusalPath() is relative to the SUBTREE root, because that is the walk that refused.
// Serialise the DOCUMENT.  There is no root node to hand in -- see the block at the top of this
// file -- so the root object's children come from `pathElem`/`pathCount` and every node below it
// from the per-node accessors.  `path` may be null or empty for the whole document.
bool EmitDocumentJson(const CJsonReader& r, const char* path, void* out, size_t cap,
                      size_t& written, bool& truncated, JsonRefusal* why = nullptr,
                      const JsonEmitLimits* limits = nullptr);

bool EmitCanonicalJsonAtPath(const CJsonReader&, void* root, const char* path, void* out,
                             size_t cap, size_t& written, bool& truncated,
                             JsonRefusal* why = nullptr, const JsonEmitLimits* limits = nullptr);

}  // namespace entity_json
}  // namespace replay
