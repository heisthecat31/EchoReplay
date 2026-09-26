#include "bootstrap/sc7.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// MinHook's own instruction structs. The patch window is derived from them rather than typed, because a
// typed number is a second copy of MinHook's decision that drifts the day the vendored copy moves.
#include <trampoline.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/jsonesc.h"
#include "bootstrap/log.h"
#include "bootstrap/md5.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/offlinepatch.h"
#include "bootstrap/paths.h"

namespace replay {
namespace sc7 {
namespace {

// A hot-patched site is written at target-5 (a JMP_REL reaching the relay) with a JMP_REL_SHORT at the
// target itself; an ordinary site is a JMP_REL at the target. Cover both, or an armed hook reads as an
// undeclared patch and SC7 fails on its own success.
constexpr size_t kWindowBefore = sizeof(JMP_REL);
constexpr size_t kWindowAfter = sizeof(JMP_REL) + sizeof(JMP_REL_SHORT);

// The most bytes any MinHook disposition actually writes at one site: the hot-patch case, [target-5,
// target+2). The window is wider than that on both ends, and the difference is the blind spot.
constexpr size_t kMinHookMaxPatch = sizeof(JMP_REL) + sizeof(JMP_REL_SHORT);

struct Refusal {
    static void Set(Sc7Result& out, const char* reason) {
        out.refused = true;
        out.verdict = Verdict::CouldNotAsk;
        std::snprintf(out.reason, sizeof out.reason, "%s", reason);
    }
};

void AddRow(Row* rows, uint32_t& count, const char* id) {
    if (id == nullptr || id[0] == '\0') {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(rows[i].id, id) == 0) {
            return;
        }
    }
    if (count >= kMaxRows) {
        return;
    }
    std::snprintf(rows[count].id, kRowChars, "%s", id);
    ++count;
}

bool HasRow(const Row* rows, uint32_t count, const char* id) {
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(rows[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

// Level 0 touches no engine code and Level 1X patches a COM vtable or an import in our own process;
// neither can show up in echovr.exe's .text, so neither belongs in this denominator (D-18's own split).
// What is left is the rows whose own key declares kConfigTextDetour.
// The ledger row a key claims, read from the declaration rather than copied into a second place.
const char* RowForKey(const char* key) {
    const ConfigKey* keys = ConfigKeys();
    for (size_t i = 0; i < ConfigKeyCount(); ++i) {
        if (std::strcmp(keys[i].key, key) == 0) {
            return keys[i].ledgerRow;
        }
    }
    return nullptr;
}

// The key that claims a ledger row. A site carries the row string rather than the key, and
// Site::ledgerRow is copied straight off a ConfigKey (hookmgr.cpp:191), so a lookup by row lands back
// on the declaration that produced it.
const ConfigKey* KeyForRow(const char* row) {
    if (row == nullptr) {
        return nullptr;
    }
    const ConfigKey* keys = ConfigKeys();
    for (size_t i = 0; i < ConfigKeyCount(); ++i) {
        if (keys[i].ledgerRow != nullptr && std::strcmp(keys[i].ledgerRow, row) == 0) {
            return &keys[i];
        }
    }
    return nullptr;
}

// Does this row change echovr.exe's .text? The row's KEY answers; the row's SPELLING does not. While
// this asked the number, row 2.5b -- a Level-2 row that installs no detour, only engine setters and a
// restored .data byte -- entered the expected set where no site could ever match it, and SC7 read
// expected_rows 4 against declared_sites 3 on every launch that had the gate scope on.
bool RowPatchesText(const char* row) {
    const ConfigKey* key = KeyForRow(row);
    return key != nullptr && (key->flags & kConfigTextDetour) != 0;
}

// Binary search for the DIR64 field covering `offset`, if any.
bool RelocFieldAt(const ImageView& image, size_t offset, size_t* fieldStart) {
    if (image.relocCount == 0) {
        return false;
    }
    size_t lo = 0;
    size_t hi = image.relocCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (image.relocOffsets[mid] <= offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    const size_t start = image.relocOffsets[lo - 1];
    if (offset - start >= sizeof(uint64_t) || start + sizeof(uint64_t) > image.len) {
        return false;
    }
    *fieldStart = start;
    return true;
}

// A relocated field is excused only when it holds exactly what the loader would have written: the disk
// value plus the load delta. Anything else is a difference like any other.
bool DeltaConsistent(const ImageView& image, size_t fieldStart) {
    uint64_t onDisk = 0;
    uint64_t inMemory = 0;
    std::memcpy(&onDisk, image.disk + fieldStart, sizeof onDisk);
    std::memcpy(&inMemory, image.live + fieldStart, sizeof inMemory);
    return inMemory == onDisk + static_cast<uint64_t>(image.relocDelta);
}

// Which registered site's patch window covers this offset, or -1.
int OwnerOf(const ImageView& image, const SiteView* sites, size_t siteCount, size_t offset) {
    const uint64_t rva = image.textRva + offset;
    for (size_t i = 0; i < siteCount; ++i) {
        if (sites[i].rva == 0) {
            continue;
        }
        const uint64_t start = sites[i].rva >= kWindowBefore ? sites[i].rva - kWindowBefore : 0;
        if (rva >= start && rva < sites[i].rva + kWindowAfter) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Sample(uint8_t* dst, uint32_t* sampled, const uint8_t* src, size_t len) {
    const size_t n = len < kSampleBytes ? len : kSampleBytes;
    std::memcpy(dst, src, n);
    *sampled = static_cast<uint32_t>(n);
}

void RecordAttributed(Sc7Result& out, const ImageView& image, const SiteView& site, size_t start,
                      size_t len) {
    AddRow(out.observed, out.observedCount, site.row);
    for (uint32_t i = 0; i < out.detailCount; ++i) {
        if (std::strcmp(out.detail[i].factId, site.factId) == 0) {
            return;   // one entry per site; the row is what the harness compares
        }
    }
    if (out.detailCount >= kMaxRows) {
        return;
    }
    SiteDetail& d = out.detail[out.detailCount++];
    std::snprintf(d.row, kRowChars, "%s", site.row != nullptr ? site.row : "");
    std::snprintf(d.factId, sizeof d.factId, "%s", site.factId != nullptr ? site.factId : "");
    d.rva = image.textRva + start;
    d.len = static_cast<uint32_t>(len);
    Sample(d.after, &d.sampled, image.live + start, len);
}

void RecordUnattributed(Sc7Result& out, const ImageView& image, size_t start, size_t len) {
    ++out.unattributedTotal;
    if (out.unattributedCount >= kMaxRanges) {
        return;
    }
    Range& r = out.unattributed[out.unattributedCount++];
    r.owner[0] = '\0';
    r.rva = image.textRva + start;
    r.len = static_cast<uint32_t>(len);
    Sample(r.before, &r.sampled, image.disk + start, len);
    uint32_t ignored = 0;
    Sample(r.after, &ignored, image.live + start, len);
}

// ---------------------------------------------------------------------------------------------------
// Gathering the real inputs
// ---------------------------------------------------------------------------------------------------

struct TextSpan {
    uint32_t rva;
    uint32_t virtualSize;
    uint32_t rawPointer;
    uint32_t rawSize;
};

const IMAGE_NT_HEADERS64* NtHeaders(const uint8_t* base) {
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!Readable(dos, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!Readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }
    return nt;
}

bool FindText(const IMAGE_NT_HEADERS64* nt, TextSpan* out) {
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!Readable(section, sizeof *section)) {
            return false;
        }
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::strcmp(name, ".text") != 0) {
            continue;
        }
        out->rva = section->VirtualAddress;
        out->virtualSize = section->Misc.VirtualSize;
        out->rawPointer = section->PointerToRawData;
        out->rawSize = section->SizeOfRawData;
        return out->rawSize != 0 && out->virtualSize != 0;
    }
    return false;
}

// Every IMAGE_REL_BASED_DIR64 entry whose target lands inside the compared span, as offsets within it.
// Ascending, because the reloc table itself is ordered by block and by entry.
uint32_t* CollectRelocs(const uint8_t* base, const IMAGE_NT_HEADERS64* nt, uint32_t textRva,
                        size_t textLen, size_t* countOut, uint32_t* dirBytesOut,
                        uint32_t* blocksOut) {
    *countOut = 0;
    *dirBytesOut = 0;
    *blocksOut = 0;
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return nullptr;
    }
    *dirBytesOut = dir.Size;
    // Two passes: count, allocate, fill. .text usually holds very few relocations, but "usually" is not
    // a size to allocate against.
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t* out = nullptr;
        size_t n = 0;
        if (pass == 1) {
            if (*countOut == 0) {
                return nullptr;
            }
            out = static_cast<uint32_t*>(
                HeapAlloc(GetProcessHeap(), 0, *countOut * sizeof(uint32_t)));
            if (out == nullptr) {
                return nullptr;
            }
        }
        uint32_t walked = 0;
        while (walked + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size) {
            const IMAGE_BASE_RELOCATION* block =
                reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base + dir.VirtualAddress + walked);
            if (!Readable(block, sizeof *block) || block->SizeOfBlock < sizeof *block ||
                block->SizeOfBlock > dir.Size - walked) {
                break;
            }
            const size_t entries = (block->SizeOfBlock - sizeof *block) / sizeof(WORD);
            const WORD* entry = reinterpret_cast<const WORD*>(block + 1);
            if (!Readable(entry, entries * sizeof(WORD))) {
                break;
            }
            for (size_t i = 0; i < entries; ++i) {
                if ((entry[i] >> 12) != IMAGE_REL_BASED_DIR64) {
                    continue;
                }
                const uint64_t rva = block->VirtualAddress + (entry[i] & 0x0FFF);
                if (rva < textRva || rva + sizeof(uint64_t) > textRva + textLen) {
                    continue;
                }
                if (pass == 0) {
                    ++*countOut;
                } else if (n < *countOut) {
                    out[n++] = static_cast<uint32_t>(rva - textRva);
                }
            }
            walked += block->SizeOfBlock;
            if (pass == 0) {
                ++*blocksOut;
            }
        }
        if (pass == 1) {
            *countOut = n;
            return out;
        }
    }
    return nullptr;
}

// hookmgr's sites, translated into the comparison's coordinates. A site outside .text (1X rows patch our
// own process, not the game image) contributes nothing and is dropped.
size_t GatherSites(const uint8_t* base, uint32_t textRva, size_t textLen, SiteView* out, size_t cap,
                   uint32_t* declared, uint32_t* armed) {
    size_t n = 0;
    for (size_t i = 0; i < hookmgr::Count() && n < cap; ++i) {
        hookmgr::Site site;
        if (!hookmgr::SiteAt(i, site) || !RowPatchesText(site.ledgerRow)) {
            continue;
        }
        const void* mapped = buildguard::MappedAddress(site.pcVa);
        if (mapped == nullptr) {
            continue;
        }
        const uint64_t rva = static_cast<uint64_t>(reinterpret_cast<const uint8_t*>(mapped) - base);
        if (rva < textRva || rva >= textRva + textLen) {
            continue;
        }
        ++*declared;
        if (site.status == HookStatus::Armed) {
            ++*armed;
        }
        out[n].factId = site.factId;
        out[n].row = site.ledgerRow;
        out[n].rva = rva;
        out[n].armed = site.status == HookStatus::Armed;
        ++n;
    }

    // The offline lock installs its detour through MinHook directly rather than through hookmgr, so its
    // site is in no table this walk has seen. L5 measured what that costs: its 5-byte jump at 0x1042B0
    // came back as a range with nobody to attribute it to, one of thirteen, and the other twelve were
    // rlay's. It publishes the function it hooked, which is the same fact hookmgr would have carried.
    const char* lockRow = RowForKey("bootstrap.offline_bit_lock");
    const offlinelock::Report& lock = offlinelock::Current();
    if (n < cap && RowPatchesText(lockRow) && lock.functionVa > reinterpret_cast<uint64_t>(base)) {
        const uint64_t rva = lock.functionVa - reinterpret_cast<uint64_t>(base);
        if (rva >= textRva && rva < textRva + textLen) {
            const bool isArmed = lock.state == offlinelock::LockState::Locked;
            ++*declared;
            if (isArmed) {
                ++*armed;
            }
            out[n].factId = "OFFLINELOCK.TEARDOWN";
            out[n].row = lockRow;
            out[n].rva = rva;
            out[n].armed = isArmed;
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------
// The JSON document
// ---------------------------------------------------------------------------------------------------

// AUDIT_G G-14. snprintf returns the length it WOULD have written, so accumulating that raw walks the
// cursor past `cap` the first time a document outgrows kJsonCap; `cap - at` then underflows to SIZE_MAX
// and the next write runs off a 32 KB static. Every append in this file goes through here instead.
//
// AUDIT_L5FIX W1C-2. It used to clamp to `cap - 1` and carry on, so an overlong document was written
// truncated and WriteJson reported success -- a JSON document cut off mid-token is invalid JSON, which
// is the L5 failure class this packet exists to end.
//
// AUDIT_L5FIX_R2 W1C-R2-1. The first fix put the signal in the CURSOR (`at == cap`), which made every
// emission helper separately responsible for producing and preserving it. That is one obligation per
// helper and it was not met everywhere: the document's last emission is a plain format literal, so a
// helper that clamped instead of saturating handed BuildJson a cursor it could not tell from a
// complete document (29 truncated documents returned as complete over a 64..20000 cap sweep). The
// state is a property of the WRITER now: one sticky flag, set by whichever emission ran out of room,
// cleared by nothing, read once at the end of the document.
struct JsonWriter {
    char*  buf;
    size_t cap;
    size_t at = 0;
    bool   overflow = false;
};

void Append(JsonWriter& w, const char* fmt, ...) {
    if (w.overflow || w.at >= w.cap) {
        w.overflow = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(w.buf + w.at, w.cap - w.at, fmt, ap);
    va_end(ap);
    if (n < 0) {
        w.overflow = true;   // an encoding failure is a document we cannot vouch for, not a shorter one
        return;
    }
    const size_t room = w.cap - w.at - 1;   // vsnprintf NUL-terminates inside cap - at
    if (static_cast<size_t>(n) > room) {
        w.overflow = true;
        return;
    }
    w.at += static_cast<size_t>(n);
}

void AppendHex(JsonWriter& w, const uint8_t* bytes, uint32_t n) {
    for (uint32_t i = 0; i < n && !w.overflow; ++i) {
        Append(w, "%s%02x", i == 0 ? "" : " ", bytes[i]);
    }
}

// A fixed cap. The document is bounded by kMaxRows and kMaxRanges, both small, and a heap allocation on
// a path that may be reporting a corrupted image is a dependency this does not need.
constexpr size_t kJsonCap = 32 * 1024;

// P3 links a stub that writes a refusal under this same filename into this same directory until P12 is
// wired to it; whichever runs last wins. Every document this reader produces names itself, and WriteJson
// reads the file back to confirm the bytes on disk are the ones it wrote, so a foreign writer is a
// detected condition rather than a silently believed report.
constexpr const char* kWriter = "sc7-p12";

// One wording for the overflow, so the log line, the document and the test all name the same thing.
constexpr const char* kOverflowReason =
    "the sc7 report does not fit in the 32 KB document buffer, so no report was written - a truncated "
    "JSON document is not a report";

// Every string VALUE in this document is written by this one call, and every key, separator and
// bracket around it is this file's own ASCII literal. That split is the invariant: `Append`'s format
// strings carry no `%s` that lands inside a JSON string, so no caller can put a raw path back in.
void AppendQuoted(JsonWriter& w, const char* s) {
    Append(w, "\"");
    if (w.overflow) {
        return;
    }
    // AppendEscaped is shared with every other emitter in this build, so it keeps its own `cap`
    // saturation sentinel; this is the one place that sentinel is turned into the writer's flag.
    const size_t next = json::AppendEscaped(w.buf, w.cap, w.at, s);
    if (next >= w.cap) {
        w.overflow = true;
        return;
    }
    w.at = next;
    Append(w, "\"");
}

size_t BuildJson(const Sc7Result& r, char* buf, size_t cap) {
    JsonWriter w{buf, cap};
    Append(w, "{\n  \"writer\": ");
    AppendQuoted(w, kWriter);
    Append(w, ",\n  \"pass\": ");
    AppendQuoted(w, r.pass);
    Append(w, ",\n  \"verdict\": ");
    AppendQuoted(w, VerdictName(r.verdict));
    Append(w, ",\n");
    // H14 (i): a refused file renders NOT-RUN with a cause. An ABSENT file is the other row - nobody
    // asked - and the two must not be conflated, which is why a refusal is still a written file.
    Append(w, "%s", r.refused ? "  \"refused\": " : "  \"reason\": ");
    AppendQuoted(w, r.reason);
    Append(w, ",\n");
    // ★ A REFUSAL OMITS `expected_rows` ENTIRELY - it does not manufacture an empty one. P12: the reader
    // does not hand its consumer a vacuous denominator, and an empty set is a claim ("nothing was
    // declared") that a refusal is in no position to make. The key is simply absent, and `refused` is
    // what the harness reads first. `declared_rows` still reports the declaration, under a key that is
    // nobody's denominator, so the refusal can be audited.
    Append(w, "  \"declared_rows\": [");
    for (uint32_t i = 0; i < r.expectedCount; ++i) {
        Append(w, "%s", i ? ", " : "");
        AppendQuoted(w, r.expected[i].id);
    }
    Append(w, "],\n");
    // Printed before expected_rows: a reader meets what was excluded from the denominator, and why,
    // before it meets the denominator itself. Present on a refusal too - a refused pass still knows
    // which rows it could not have asked about.
    Append(w, "  \"deferred_rows\": [");
    for (uint32_t i = 0; i < r.deferredCount; ++i) {
        Append(w, "%s\n    { \"row\": ", i ? "," : "");
        AppendQuoted(w, r.deferred[i].id);
        Append(w, ", \"key\": ");
        AppendQuoted(w, r.deferred[i].key);
        Append(w, ", \"reason\": ");
        AppendQuoted(w, r.deferred[i].reason);
        Append(w, " }");
    }
    Append(w, "%s\n", r.deferredCount ? "\n  ]," : "],");
    if (!r.refused) {
        Append(w, "  \"expected_rows\": [");
        for (uint32_t i = 0; i < r.expectedCount; ++i) {
            Append(w, "%s", i ? ", " : "");
            AppendQuoted(w, r.expected[i].id);
        }
        Append(w, "],\n");
    }
    Append(w, "  \"observed_rows\": [");
    for (uint32_t i = 0; i < r.observedCount; ++i) {
        Append(w, "%s", i ? ", " : "");
        AppendQuoted(w, r.observed[i].id);
    }
    Append(w, "],\n  \"detail\": [");
    for (uint32_t i = 0; i < r.detailCount; ++i) {
        const SiteDetail& d = r.detail[i];
        Append(w, "%s\n    { \"rva\": \"0x%llX\", \"row\": ", i ? "," : "",
            static_cast<unsigned long long>(d.rva));
        AppendQuoted(w, d.row);
        Append(w, ", \"fact\": ");
        AppendQuoted(w, d.factId);
        Append(w, ", \"len\": %u, \"bytes\": \"", d.len);
        AppendHex(w, d.after, d.sampled);
        Append(w, "\" }");
    }
    Append(w, "%s\n  \"unattributed\": [",
        r.detailCount ? "\n  ]," : "],");
    for (uint32_t i = 0; i < r.unattributedCount; ++i) {
        const Range& u = r.unattributed[i];
        Append(w,
            "%s\n    { \"rva\": \"0x%llX\", \"len\": %u, \"expected\": \"",
            i ? "," : "", static_cast<unsigned long long>(u.rva), u.len);
        AppendHex(w, u.before, u.sampled);
        Append(w, "\", \"actual\": \"");
        AppendHex(w, u.after, u.sampled);
        // Empty means nobody claims it. An attributed range is still printed in full, because a
        // name for a patch is not permission to stop showing it.
        Append(w, "\", \"owner\": ");
        AppendQuoted(w, u.owner);
        Append(w, " }");
    }
    Append(w, "%s", r.unattributedCount ? "\n  ]," : "],");
    Append(w, "\n  \"third_party\": [");
    for (uint32_t i = 0; i < r.thirdPartyCount; ++i) {
        const ThirdPartyView& v = r.thirdParty[i];
        Append(w, "%s\n    { \"name\": ", i ? "," : "");
        AppendQuoted(w, v.name);
        Append(w, ", \"present\": %s, \"table_read\": %s, "
                  "\"declared\": %u, \"observed\": %u, \"provenance\": ",
            v.present ? "true" : "false", v.tableRead ? "true" : "false",
            v.declared, v.observed);
        // The L5 defect. `provenance` holds the loaded module's full path, so it carries `\P`, `\r`
        // and `\b` on the owner's machine - the field that broke the document, escaped like the rest.
        AppendQuoted(w, v.provenance);
        Append(w, ", \"unmatched\": [");
        for (uint32_t k = 0; k < v.unmatchedCount; ++k) {
            Append(w, "%s\"0x%llX\"", k ? ", " : "",
                static_cast<unsigned long long>(v.unmatched[k]));
        }
        Append(w, "] }");
    }
    Append(w, "%s", r.thirdPartyCount ? "\n  ]," : "],");
    Append(w, "\n  \"passes\": [");
    for (uint32_t i = 0; i < r.passCount; ++i) {
        const PassRecord& p = r.passes[i];
        Append(w, "%s\n    { \"pass\": ", i ? "," : "");
        AppendQuoted(w, p.pass);
        Append(w, ", \"verdict\": ");
        AppendQuoted(w, VerdictName(p.verdict));
        Append(w, ", \"reason\": ");
        AppendQuoted(w, p.reason);
        Append(w, " }");
    }
    Append(w, "%s", r.passCount ? "\n  ]," : "],");
    Append(w,
        "\n  \"unattributed_total\": %u,\n"
        "  \"unowned_total\": %u,\n"
        "  \"attributed_ranges\": %u,\n"
        "  \"declared_sites\": %u,\n"
        "  \"armed_ledger_sites\": %u,\n"
        "  \"window_blind_bytes\": %u,\n"
        "  \"probe_dir_fallback\": ",
        r.unattributedTotal, r.unownedTotal, r.attributedRanges,
        r.declaredSites, r.armedLedgerSites, r.windowBlindBytes);
    AppendQuoted(w, r.probeDirFallback);
    Append(w,
        ",\n  \"reloc_bytes_skipped\": %u,\n"
        "  \"reloc_fields_in_text\": %u,\n"
        "  \"reloc_blocks_walked\": %u,\n"
        "  \"reloc_dir_bytes\": %u,\n"
        "  \"reloc_delta\": %lld,\n"
        "  \"text_rva\": \"0x%llX\",\n"
        "  \"text_bytes\": %llu,\n"
        "  \"disk_md5\": ",
        r.relocBytesSkipped, r.relocFieldsInText, r.relocBlocksWalked, r.relocDirBytes,
        static_cast<long long>(r.relocDelta),
        static_cast<unsigned long long>(r.textRva),
        static_cast<unsigned long long>(r.textBytes));
    AppendQuoted(w, r.diskMd5);
    Append(w, ",\n  \"elapsed_us\": %llu\n}\n",
        static_cast<unsigned long long>(r.elapsedUs));
    // ONE check, for every emission above -- the literals included. AUDIT_L5FIX_R2 W1C-R2-1: this
    // document's last emission is a plain `Append` of a format literal, so a scheme where each helper
    // has to propagate saturation itself leaks exactly there, and a record ending near the cap is
    // written truncated with `WriteJson` reporting success. The writer carries the flag instead. 0
    // tells WriteJson the document did not fit; the buffer's tail is a prefix of a document, which is
    // not a document -- it must not be written.
    return w.overflow ? 0 : w.at;
}

// The document written when the real one does not fit. It is deliberately not a second hand-written
// copy of the schema: it emits the same four keys BuildJson leads a refusal with, through the same
// escape, and the harness reads `refused` before anything else. H14 (i) is
// why it is written at all -- could-not-ask and never-asked are two different rows, and an absent
// file is the second one.
size_t BuildOverflowRefusal(const Sc7Result& r, char* buf, size_t cap) {
    JsonWriter w{buf, cap};
    Append(w, "{\n  \"writer\": ");
    AppendQuoted(w, kWriter);
    Append(w, ",\n  \"pass\": ");
    AppendQuoted(w, r.pass);
    Append(w, ",\n  \"verdict\": ");
    AppendQuoted(w, VerdictName(Verdict::CouldNotAsk));
    Append(w, ",\n  \"refused\": ");
    AppendQuoted(w, kOverflowReason);
    Append(w, "\n}\n");
    return w.overflow ? 0 : w.at;
}

// The whole cap decision, in one place: the report when it fits, the named COULD-NOT-ASK refusal when
// it does not, 0 when not even that fits. `refused` says which. WriteJson does the file I/O around
// this and nothing else, so the cap sweep (SerializeForTest) drives the shipped decision rather than a
// second copy of it.
size_t Serialize(const Sc7Result& r, char* buf, size_t cap, bool* refused) {
    *refused = false;
    const size_t len = BuildJson(r, buf, cap);
    if (len != 0) {
        return len;
    }
    *refused = true;
    return BuildOverflowRefusal(r, buf, cap);
}

// The harness hands each probe its own artifact directory in REPLAY_PROBE_DIRS as `<dll base>=<dir>;...`.
// Matched at a field boundary, never as a substring: `echoreplay` must not be answered by `probe_echoreplay`.
// Returns nullptr when `out` now holds this module's artifact directory, and otherwise the reason it does
// not - which the caller logs and publishes, because a report written somewhere the harness never looks
// is indistinguishable from no report at all.
const char* ProbeDirForSelf(wchar_t* out, size_t cap) {
    const wchar_t* self = Resolved() ? SelfModulePath() : nullptr;
    if (self == nullptr) {
        return "this module's own path is not resolved yet";
    }
    const wchar_t* slash = wcsrchr(self, L'\\');
    const wchar_t* stem = slash != nullptr ? slash + 1 : self;
    wchar_t base[128];
    size_t n = 0;
    for (; stem[n] != L'\0' && stem[n] != L'.' && n + 1 < _countof(base); ++n) {
        base[n] = stem[n];
    }
    base[n] = L'\0';
    if (n == 0) {
        return "this module's file name has no base";
    }

    wchar_t dirs[4096];
    const DWORD len = GetEnvironmentVariableW(L"REPLAY_PROBE_DIRS", dirs, _countof(dirs));
    if (len == 0) {
        return "REPLAY_PROBE_DIRS is not set";
    }
    if (len >= _countof(dirs)) {
        return "REPLAY_PROBE_DIRS is longer than this reader's 4096-char buffer";
    }
    const wchar_t* cursor = dirs;
    while (*cursor != L'\0') {
        const wchar_t* end = wcschr(cursor, L';');
        const size_t fieldLen = end != nullptr ? static_cast<size_t>(end - cursor) : wcslen(cursor);
        size_t eqAt = 0;
        while (eqAt < fieldLen && cursor[eqAt] != L'=') {
            ++eqAt;
        }
        if (eqAt == n && n < fieldLen && _wcsnicmp(cursor, base, n) == 0) {
            const size_t valueLen = fieldLen - (n + 1);
            const wchar_t* eq = cursor + n;
            if (valueLen > 0 && valueLen + 1 <= cap) {
                std::memcpy(out, eq + 1, valueLen * sizeof(wchar_t));
                out[valueLen] = L'\0';
                return nullptr;
            }
            return "REPLAY_PROBE_DIRS names an empty or over-long directory for this module";
        }
        if (end == nullptr) {
            break;
        }
        cursor = end + 1;
    }
    return "REPLAY_PROBE_DIRS has no field for this module";
}

struct RunState {
    bool       sawFailure;
    Sc7Result  failing;              // the failing measurement, kept whole so the FAIL keeps its evidence
    PassRecord history[kMaxPasses];
    uint32_t   historyCount;
};

RunState g_run;

}  // namespace

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Pass:        return "PASS";
        case Verdict::Fail:        return "FAIL";
        case Verdict::CouldNotAsk: return "COULD-NOT-ASK";
    }
    return "COULD-NOT-ASK";
}

size_t PatchWindowBefore() { return kWindowBefore; }
size_t PatchWindowAfter() { return kWindowAfter; }

void Compare(const ImageView& image, const SiteView* sites, size_t siteCount, Sc7Result& out) {
    out.textRva = image.textRva;
    out.textBytes = image.len;
    out.relocDelta = image.relocDelta;
    out.relocFieldsInText = static_cast<uint32_t>(image.relocCount);
    out.relocBlocksWalked = image.relocBlocksWalked;
    out.relocDirBytes = image.relocDirBytes;

    // The window's blind spot, published rather than assumed away. An armed site accounts for at most
    // kMinHookMaxPatch of its window; an unarmed one accounts for none of it.
    for (size_t s = 0; s < siteCount; ++s) {
        if (sites[s].rva == 0) {
            continue;
        }
        const size_t written = sites[s].armed ? kMinHookMaxPatch : 0;
        out.windowBlindBytes += static_cast<uint32_t>(kWindowBefore + kWindowAfter - written);
    }

    size_t i = 0;
    while (i < image.len) {
        if (image.disk[i] == image.live[i]) {
            ++i;
            continue;
        }
        size_t field = 0;
        if (RelocFieldAt(image, i, &field)) {
            if (DeltaConsistent(image, field)) {
                out.relocBytesSkipped += sizeof(uint64_t);
                i = field + sizeof(uint64_t);
                continue;
            }
            // A relocated field holding something other than disk + delta is reported WHOLE. Byte runs
            // would split it wherever the two values happen to agree - a pointer overwritten in place
            // usually shares a byte or two with the one it replaced - and three fragments of one qword
            // is a worse description of what happened than the qword is.
            const int fieldOwner = OwnerOf(image, sites, siteCount, field);
            if (fieldOwner >= 0) {
                RecordAttributed(out, image, sites[fieldOwner], field, sizeof(uint64_t));
            } else {
                RecordUnattributed(out, image, field, sizeof(uint64_t));
            }
            i = field + sizeof(uint64_t);
            continue;
        }
        // A differing run, cut wherever the owning site changes, so a patch that spills past its own
        // window is not laundered by the part of it that sits inside one.
        const int owner = OwnerOf(image, sites, siteCount, i);
        const size_t start = i;
        while (i < image.len && image.disk[i] != image.live[i] &&
               OwnerOf(image, sites, siteCount, i) == owner) {
            size_t inner = 0;
            if (RelocFieldAt(image, i, &inner) && DeltaConsistent(image, inner)) {
                break;
            }
            ++i;
        }
        if (owner >= 0) {
            RecordAttributed(out, image, sites[owner], start, i - start);
        } else {
            RecordUnattributed(out, image, start, i - start);
        }
    }
}

namespace {

// rlay installs a byte patch as `mov edx,<len>` ... `add rbx,<rva>` ; `mov rcx,rbx` ; `call [writer]`,
// with rbx holding echovr's base. The length is the immediate closest to the `add` with no intervening
// call, because a call ends one site's argument setup and begins the next's. A site whose length is
// loaded from a register instead of an immediate gets length 0 - it is published as a declared site that
// attributes nothing, which is the only reading that cannot launder a byte.
constexpr size_t kLenSearchBack = 64;
constexpr uint32_t kMaxPatchLen = 64;

// A detour target is named as an absolute address (`add rax,<rva>` off the module base) and handed to the
// hooking library, which overwrites the entry point with a 5-byte jump padded out to the end of the last
// instruction it clobbered. Eight is the widest such write observed at any of rlay's sites (L5: 0x116720,
// 8 bytes); a wider one is REPORTED rather than excused, which is the direction that keeps a foreign
// patch visible and costs at most a FAIL that names the site so the bound can be raised on evidence.
constexpr uint32_t kDetourSpan = 8;

// How far past the `add rbx` the `mov rcx,rbx` that hands the address to the writer may sit.
constexpr size_t kCallSetupSpan = 31;

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

uint32_t PatchLenBefore(const uint8_t* text, size_t at) {
    const size_t from = at > kLenSearchBack ? at - kLenSearchBack : 0;
    for (size_t j = at >= 5 ? at - 5 : 0; j > from; --j) {
        if (text[j] != 0xBA) {
            continue;
        }
        const uint32_t len = ReadU32(text + j + 1);
        if (len == 0 || len > kMaxPatchLen) {
            continue;
        }
        for (size_t k = j; k + 1 < at; ++k) {
            if (text[k] == 0xFF && text[k + 1] == 0x15) {
                return 0;   // that immediate belonged to the previous site
            }
        }
        return len;
    }
    return 0;
}

void AddPatch(ThirdPartyPatch* out, size_t cap, size_t& count, uint32_t& dropped, uint64_t rva,
              uint32_t len) {
    for (size_t i = 0; i < count; ++i) {
        if (out[i].rva == rva) {
            if (len > out[i].len) {
                out[i].len = len;
            }
            return;
        }
    }
    if (count >= cap) {
        ++dropped;
        return;
    }
    out[count].rva = rva;
    out[count].len = len;
    ++count;
}

}  // namespace

size_t ScanRlayPatchTable(const uint8_t* text, size_t len, uint64_t maxRva, ThirdPartyPatch* out,
                          size_t cap, uint32_t* dropped) {
    size_t count = 0;
    uint32_t over = 0;
    if (text == nullptr || out == nullptr || cap == 0) {
        if (dropped != nullptr) {
            *dropped = 0;
        }
        return 0;
    }
    // Every read is bounded against `len` where it happens rather than by one bound on the loop: the
    // two shapes reach different distances, and a single conservative bound would silently stop finding
    // the shorter one near the end of the section.
    for (size_t i = 0; i < len; ++i) {
        // add rbx, imm32 ; ... ; mov rcx,rbx - the byte-patch shape.
        if (i + 7 <= len && text[i] == 0x48 && text[i + 1] == 0x81 && text[i + 2] == 0xC3) {
            const uint32_t rva = ReadU32(text + i + 3);
            bool movesRcx = false;
            for (size_t k = i + 7; k + 3 <= len && k < i + kCallSetupSpan; ++k) {
                if (text[k] == 0x48 && text[k + 1] == 0x8B && text[k + 2] == 0xCB) {
                    movesRcx = true;
                    break;
                }
            }
            if (rva != 0 && rva < maxRva && movesRcx) {
                AddPatch(out, cap, count, over, rva, PatchLenBefore(text, i));
            }
            continue;
        }
        // mov rax,[base] ; add rax, imm32 ; mov [target], rax - the detour shape.
        if (i >= 7 && i + 9 <= len && text[i] == 0x48 && text[i + 1] == 0x05 && text[i - 7] == 0x48 &&
            text[i - 6] == 0x8B && text[i - 5] == 0x05 && text[i + 6] == 0x48 &&
            text[i + 7] == 0x89 && text[i + 8] == 0x05) {
            const uint32_t rva = ReadU32(text + i + 2);
            if (rva != 0 && rva < maxRva) {
                AddPatch(out, cap, count, over, rva, kDetourSpan);
            }
        }
    }
    if (dropped != nullptr) {
        *dropped = over;
    }
    return count;
}

namespace {

// Our own in-memory -offline (offlinepatch.h). Its spans are the ones this process actually wrote, so it
// declares nothing unless it wrote, and a byte at one of its sites that it did not write stays unowned.
void GatherOfflinePatch(ThirdPartySet& party) {
    std::memset(&party, 0, sizeof party);
    std::snprintf(party.name, sizeof party.name, "offline-patch");
    offlinepatch::Span spans[kMaxPartyPatches];
    const size_t n = offlinepatch::WrittenSpans(spans, kMaxPartyPatches);
    for (size_t i = 0; i < n; ++i) {
        party.patches[i] = {spans[i].rva, spans[i].len};
    }
    party.patchCount = static_cast<uint32_t>(n);
    party.present = n > 0;
    party.tableRead = true;
    std::snprintf(party.provenance, sizeof party.provenance,
                  "%u span(s) this plugin wrote for a replay session (offline patch: %s)",
                  party.patchCount, offlinepatch::StatusName(offlinepatch::Current()));
}

void GatherRlay(ThirdPartySet& rlay, uint64_t maxRva);

}  // namespace

// Ours first: rlay's declaration is read from its install code, so it covers the -offline sites even on
// a launch where rlay never wrote them and we did. First match wins in Attribute.
size_t GatherThirdParties(ThirdPartySet* out, size_t cap, uint64_t maxRva) {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    size_t count = 0;
    if (cap >= 2) {
        GatherOfflinePatch(out[count++]);
    }
    GatherRlay(out[count++], maxRva);
    return count;
}

namespace {

void GatherRlay(ThirdPartySet& rlay, uint64_t maxRva) {
    std::memset(&rlay, 0, sizeof rlay);
    std::snprintf(rlay.name, sizeof rlay.name, "rlay");

    const HMODULE mod = GetModuleHandleW(L"rlay.dll");
    rlay.present = mod != nullptr;
    if (!rlay.present) {
        std::snprintf(rlay.provenance, sizeof rlay.provenance,
                      "rlay.dll is not loaded in this process, so it declares nothing here");
        return;
    }

    wchar_t path[kPathMax];
    char narrow[kPathMax];
    narrow[0] = '\0';
    if (GetModuleFileNameW(mod, path, _countof(path)) != 0) {
        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, static_cast<int>(sizeof narrow), nullptr,
                            nullptr);
    }

    const uint8_t* base = reinterpret_cast<const uint8_t*>(mod);
    const IMAGE_NT_HEADERS64* nt = NtHeaders(base);
    TextSpan text = {};
    if (nt == nullptr || !FindText(nt, &text)) {
        std::snprintf(rlay.provenance, sizeof rlay.provenance,
                      "%s is loaded but its .text header did not parse", narrow);
        return;
    }
    uint32_t over = 0;
    const size_t found = ScanRlayPatchTable(base + text.rva, text.virtualSize, maxRva, rlay.patches,
                                            kMaxPartyPatches, &over);
    rlay.patchCount = static_cast<uint32_t>(found);
    rlay.droppedPatches = over;
    // A loaded rlay whose install code no longer parses is a could-not-ask, not an empty declaration:
    // `tableRead` is what Decide keys on, and it is false exactly when nothing was found.
    rlay.tableRead = found > 0;
    std::snprintf(rlay.provenance, sizeof rlay.provenance,
                  "%u span(s) read from %s's own patch-install code%s", rlay.patchCount, narrow,
                  over != 0 ? " (TRUNCATED at the cap)" : "");
}

}  // namespace

// Ranges no party owns. Derived from the record - an empty `owner` and nothing else - so a result that
// was never attributed reads as fully unowned rather than as fully attributed. The cap's leftovers count
// here too: a range that was never recorded cannot have been given an owner.
uint32_t UnownedRanges(const Sc7Result& r) {
    uint32_t unowned = r.unattributedTotal - r.unattributedCount;
    for (uint32_t i = 0; i < r.unattributedCount; ++i) {
        if (r.unattributed[i].owner[0] == '\0') {
            ++unowned;
        }
    }
    return unowned;
}

void Attribute(Sc7Result& out, const ThirdPartySet* parties, size_t count) {
    const size_t used = count < kMaxParties ? count : kMaxParties;
    out.attributedRanges = 0;
    out.thirdPartyCount = static_cast<uint32_t>(used);
    for (size_t p = 0; p < used; ++p) {
        ThirdPartyView& v = out.thirdParty[p];
        std::memset(&v, 0, sizeof v);
        std::snprintf(v.name, sizeof v.name, "%s", parties[p].name);
        std::snprintf(v.provenance, sizeof v.provenance, "%s", parties[p].provenance);
        v.present = parties[p].present;
        v.tableRead = parties[p].tableRead;
        v.declared = parties[p].patchCount;
    }

    for (uint32_t i = 0; i < out.unattributedCount; ++i) {
        Range& r = out.unattributed[i];
        r.owner[0] = '\0';
        for (size_t p = 0; p < used && r.owner[0] == '\0'; ++p) {
            const ThirdPartySet& set = parties[p];
            for (uint32_t k = 0; k < set.patchCount; ++k) {
                const ThirdPartyPatch& span = set.patches[k];
                if (span.len == 0 || r.rva < span.rva || r.rva + r.len > span.rva + span.len) {
                    continue;
                }
                if (set.present) {
                    std::snprintf(r.owner, sizeof r.owner, "%s", set.name);
                    ++out.thirdParty[p].observed;
                    ++out.attributedRanges;
                } else if (out.thirdParty[p].unmatchedCount < kMaxRanges) {
                    // The bytes are where rlay patches, and rlay is not here. That is not rlay's patch.
                    out.thirdParty[p].unmatched[out.thirdParty[p].unmatchedCount++] = r.rva;
                }
                break;
            }
        }
    }
    out.unownedTotal = UnownedRanges(out);
}

// A row nobody can answer yet, recorded rather than scored. The reason is the key as one hyphenated
// token so a harness can match it: `tick.mode` unpublished becomes `tick-mode-not-yet-published`.
void AddDeferred(Sc7Result& out, const char* row, const char* key) {
    if (out.deferredCount >= kMaxRows) {
        return;
    }
    DeferredRow& d = out.deferred[out.deferredCount++];
    std::snprintf(d.id, sizeof d.id, "%s", row);
    std::snprintf(d.key, sizeof d.key, "%s", key);
    std::snprintf(d.reason, sizeof d.reason, "%s-not-yet-published", key);
    for (char* c = d.reason; *c != '\0'; ++c) {
        if (*c == '.') {
            *c = '-';
        }
    }
}

bool IsDeferred(const Sc7Result& r, const char* row) {
    for (uint32_t i = 0; i < r.deferredCount; ++i) {
        if (std::strcmp(r.deferred[i].id, row) == 0) {
            return true;
        }
    }
    return false;
}

void DeclaredRows(Sc7Result& out) {
    const ConfigKey* keys = ConfigKeys();
    for (size_t i = 0; i < ConfigKeyCount(); ++i) {
        if ((keys[i].flags & kConfigTextDetour) == 0) {
            continue;
        }
        // config::RowDeclarationState, not a second copy of the rule here: SC7's denominator and the
        // gate hookmgr applies before installing a detour have to be the same question, or SC7 audits a
        // configuration nobody ran. The third word is why this asks for the state rather than the
        // predicate - at the post-init pass tick.mode's provider is still deciding on its own thread,
        // and a denominator that guesses either way is missing data given a confident answer.
        switch (config::RowDeclarationState(keys[i].key)) {
            case config::RowState::Declared:
                AddRow(out.expected, out.expectedCount, keys[i].ledgerRow);
                break;
            case config::RowState::Unresolved:
                AddDeferred(out, keys[i].ledgerRow, keys[i].key);
                break;
            case config::RowState::NotDeclared:
                break;
        }
    }
}

void Decide(Sc7Result& out) {
    if (out.refused) {
        out.verdict = Verdict::CouldNotAsk;
        return;
    }
    out.unownedTotal = UnownedRanges(out);
    // §9.12. An empty declaration is never a pass. With a Level-1/2 detour standing in .text it is a
    // FAIL, because the ledger that is supposed to account for it is silent; with no detour at all there
    // is nothing to measure and no denominator to measure it against, which is a refusal, not a green.
    if (out.expectedCount == 0) {
        // AUDIT_K1 K1-2. The deferral was tested FIRST here, so one unrelated row waiting on its
        // provider suppressed the FAIL the paragraph above promises: a detour nobody declared and
        // nobody deferred rendered as could-not-ask. Deferral excuses the ROW it names and no other,
        // so the observation set is split the way the main path below already splits it, and a
        // detour at a row outside the deferred set decides this branch however many other rows are
        // waiting. Only an entirely-deferred observation set is the refusal.
        uint32_t undeferred = 0;
        for (uint32_t i = 0; i < out.observedCount; ++i) {
            if (!IsDeferred(out, out.observed[i].id)) {
                ++undeferred;
            }
        }
        if (undeferred > 0) {
            out.verdict = Verdict::Fail;
            std::snprintf(out.reason, sizeof out.reason,
                          "ledger-empty: %u armed Level-1/2 site(s) and %u observed detour(s), %u of "
                          "them at row(s) no enabled ledger row declares and no deferral excuses",
                          out.armedLedgerSites, out.observedCount, undeferred);
            return;
        }
        if (out.deferredCount > 0) {
            Refusal::Set(out, "ledger-deferred: every enabled Level-1/2 row is waiting on a provider's "
                              "published resolution, so this pass has no denominator it may score");
            return;
        }
        if (out.armedLedgerSites > 0 || out.observedCount > 0) {
            out.verdict = Verdict::Fail;
            std::snprintf(out.reason, sizeof out.reason,
                          "ledger-empty: %u armed Level-1/2 site(s) and %u observed detour(s), but no "
                          "enabled ledger row declares any of them",
                          out.armedLedgerSites, out.observedCount);
        } else {
            Refusal::Set(out, "ledger-empty: no Level-1 or Level-2 ledger row is enabled, so SC7 has no "
                              "denominator and this launch cannot answer it");
        }
        return;
    }

    uint32_t missing = 0;
    uint32_t extra = 0;
    uint32_t deferredObserved = 0;
    for (uint32_t i = 0; i < out.expectedCount; ++i) {
        if (!HasRow(out.observed, out.observedCount, out.expected[i].id)) {
            ++missing;
        }
    }
    for (uint32_t i = 0; i < out.observedCount; ++i) {
        // A deferred row is out of the numerator too. Calling its detour undeclared would score the row
        // this pass just said it could not ask about, and in the harsher direction.
        if (!HasRow(out.expected, out.expectedCount, out.observed[i].id)) {
            if (IsDeferred(out, out.observed[i].id)) {
                ++deferredObserved;
            } else {
                ++extra;
            }
        }
    }
    // A range that lies where a third party patches, with that third party ABSENT, is not that third
    // party's patch - somebody else wrote those bytes. It is named before anything else can pass.
    for (uint32_t p = 0; p < out.thirdPartyCount; ++p) {
        const ThirdPartyView& v = out.thirdParty[p];
        if (v.unmatchedCount == 0) {
            continue;
        }
        out.verdict = Verdict::Fail;
        size_t at = static_cast<size_t>(
            std::snprintf(out.reason, sizeof out.reason,
                          "third-party-absent: %u range(s) lie inside %s's declared patch set while %s "
                          "is NOT loaded, so somebody else wrote them:", v.unmatchedCount, v.name,
                          v.name));
        for (uint32_t i = 0; i < v.unmatchedCount && at < sizeof out.reason; ++i) {
            at += static_cast<size_t>(std::snprintf(out.reason + at, sizeof out.reason - at, " 0x%llX",
                                                    static_cast<unsigned long long>(v.unmatched[i])));
        }
        return;
    }
    // A loaded third party whose declaration could not be read cannot answer for the ranges left over.
    // That is S9.1's could-not-ask: not a FAIL (the patch may well be its own) and never a pass.
    if (out.unownedTotal > 0) {
        for (uint32_t p = 0; p < out.thirdPartyCount; ++p) {
            if (out.thirdParty[p].present && !out.thirdParty[p].tableRead) {
                char why[240];
                std::snprintf(why, sizeof why,
                              "third-party-unreadable: %s is loaded but its patch declaration did not "
                              "parse, so this pass cannot say whether the %u range(s) left over are its",
                              out.thirdParty[p].name, out.unownedTotal);
                Refusal::Set(out, why);
                return;
            }
        }
    }
    if (missing == 0 && extra == 0 && out.unownedTotal == 0) {
        // AUDIT_H H-2. Out of the numerator is not the same as free. A detour standing in .text at a row
        // whose declaration could not be read is exactly S9.1's could-not-ask, and rendering it PASS
        // tells a consumer keying on the verdict word that the question was put and answered.
        if (deferredObserved > 0) {
            char why[240];
            std::snprintf(why, sizeof why,
                          "ledger-deferred-observed: %u detour(s) stand in .text at row(s) whose "
                          "declaration this pass could not read, so nothing here may render as a pass",
                          deferredObserved);
            Refusal::Set(out, why);
            return;
        }
        out.verdict = Verdict::Pass;
        char attributed[160];
        attributed[0] = '\0';
        size_t at = 0;
        for (uint32_t p = 0; p < out.thirdPartyCount && at < sizeof attributed; ++p) {
            if (out.thirdParty[p].observed == 0) {
                continue;
            }
            // Named and counted, never folded into "ok": a consumer reading only the verdict word still
            // learns that foreign code is standing in .text and whose it is.
            at += static_cast<size_t>(std::snprintf(attributed + at, sizeof attributed - at,
                                                    "; %u range(s) attributed to %s (loaded)",
                                                    out.thirdParty[p].observed, out.thirdParty[p].name));
        }
        std::snprintf(out.reason, sizeof out.reason,
                      "live .text matches the on-disk image except for the %u declared ledger row(s)%s%s",
                      out.expectedCount, attributed,
                      out.deferredCount ? "; some row(s) could not be asked and were not scored" : "");
        return;
    }
    out.verdict = Verdict::Fail;
    std::snprintf(out.reason, sizeof out.reason,
                  "%u declared row(s) with no detour, %u undeclared detour(s), %u differing range(s) in "
                  ".text no ledger and no third party accounts for (%u more attributed to a loaded third "
                  "party), %u row(s) not askable on this pass",
                  missing, extra, out.unownedTotal, out.attributedRanges, out.deferredCount);
}

bool ComputeTextDiff(Sc7Result& out) {
    std::memset(&out, 0, sizeof out);

    LARGE_INTEGER freq = {};
    LARGE_INTEGER started = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&started);

    // The ledger, first and from the declaration alone, so the denominator exists whether or not the
    // image can be read. A refusal that has already named its expected rows is still a refusal, but it
    // is one the reader can be audited against.
    DeclaredRows(out);

    if (!hookmgr::Initialized()) {
        Refusal::Set(out, "hookmgr is not initialised, so no site table exists to subtract");
        return false;
    }

    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const IMAGE_NT_HEADERS64* nt = base != nullptr ? NtHeaders(base) : nullptr;
    if (nt == nullptr) {
        Refusal::Set(out, "the main module's PE header did not parse");
        return false;
    }
    TextSpan text = {};
    if (!FindText(nt, &text)) {
        Refusal::Set(out, "the main module has no readable .text section header");
        return false;
    }
    const size_t compareLen = text.virtualSize < text.rawSize ? text.virtualSize : text.rawSize;
    if (!Readable(base + text.rva, compareLen)) {
        Refusal::Set(out, "the mapped .text span is not readable");
        return false;
    }

    wchar_t exePath[kPathMax];
    if (GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(_countof(exePath))) == 0) {
        Refusal::Set(out, "GetModuleFileNameW failed for the main module");
        return false;
    }

    // The build guard already hashed this file at startup; re-reading 35 MB to learn the same thing
    // would be the instrument perturbing the run it is measuring.
    const BuildIdentity& identity = buildguard::LastIdentity();
    if (identity.md5Read) {
        std::snprintf(out.diskMd5, sizeof out.diskMd5, "%s", identity.md5);
    } else if (!Md5File(exePath, out.diskMd5, nullptr)) {
        Refusal::Set(out, "the on-disk image could not be hashed");
        return false;
    }
    if (_stricmp(out.diskMd5, ExpectedMd5()) != 0) {
        std::snprintf(out.reason, sizeof out.reason,
                      "disk image mismatch: %s on disk, %s expected - the file behind the mapping is "
                      "not the build every offset was measured against",
                      out.diskMd5, ExpectedMd5());
        out.refused = true;
        out.verdict = Verdict::CouldNotAsk;
        return false;
    }

    const HANDLE file = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Refusal::Set(out, "the on-disk image could not be opened for reading");
        return false;
    }
    uint8_t* disk = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, compareLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    bool read = disk != nullptr;
    if (read) {
        LARGE_INTEGER at = {};
        at.QuadPart = text.rawPointer;
        read = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) != FALSE;
    }
    for (size_t done = 0; read && done < compareLen;) {
        const DWORD want =
            static_cast<DWORD>((compareLen - done) > (1u << 20) ? (1u << 20) : (compareLen - done));
        DWORD got = 0;
        if (!ReadFile(file, disk + done, want, &got, nullptr) || got == 0) {
            read = false;
            break;
        }
        done += got;
    }
    CloseHandle(file);
    if (!read) {
        if (disk != nullptr) {
            VirtualFree(disk, 0, MEM_RELEASE);
        }
        Refusal::Set(out, "the on-disk .text could not be read in full");
        return false;
    }

    size_t relocCount = 0;
    uint32_t relocDirBytes = 0;
    uint32_t relocBlocks = 0;
    uint32_t* relocs =
        CollectRelocs(base, nt, text.rva, compareLen, &relocCount, &relocDirBytes, &relocBlocks);

    SiteView sites[kMaxRows];
    const size_t siteCount = GatherSites(base, text.rva, compareLen, sites, _countof(sites),
                                         &out.declaredSites, &out.armedLedgerSites);

    ImageView image = {};
    image.disk = disk;
    image.live = base + text.rva;
    image.len = compareLen;
    image.textRva = text.rva;
    image.relocDelta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(base) -
                                            nt->OptionalHeader.ImageBase);
    image.relocOffsets = relocs;
    image.relocCount = relocCount;
    image.relocBlocksWalked = relocBlocks;
    image.relocDirBytes = relocDirBytes;

    Compare(image, sites, siteCount, out);
    ThirdPartySet parties[kMaxParties];
    const size_t partyCount = GatherThirdParties(parties, _countof(parties), text.rva + compareLen);
    Attribute(out, parties, partyCount);
    Decide(out);

    if (relocs != nullptr) {
        HeapFree(GetProcessHeap(), 0, relocs);
    }
    VirtualFree(disk, 0, MEM_RELEASE);

    LARGE_INTEGER finished = {};
    QueryPerformanceCounter(&finished);
    if (freq.QuadPart > 0) {
        out.elapsedUs = static_cast<uint64_t>((finished.QuadPart - started.QuadPart) * 1000000 /
                                              freq.QuadPart);
    }
    return !out.refused;
}

bool WriteJson(const wchar_t* dir, const Sc7Result& result) {
    if (dir == nullptr || dir[0] == L'\0') {
        return false;
    }
    wchar_t path[kPathMax];
    const size_t dirLen = wcslen(dir);
    const wchar_t* separator = (dirLen > 0 && dir[dirLen - 1] == L'\\') ? L"" : L"\\";
    if (_snwprintf_s(path, _countof(path), _TRUNCATE, L"%s%ssc7_text_diff.json", dir, separator) < 0) {
        return false;
    }

    static char json[kJsonCap];
    // AUDIT_L5FIX W1C-2: a report that did not fit is said by name and the refusal goes on disk in its
    // place, never the prefix -- and WriteJson still returns false, because the question this pass was
    // asked did not get answered.
    bool overflowed = false;
    const size_t len = Serialize(result, json, sizeof json, &overflowed);
    if (len == 0) {
        return false;
    }
    if (overflowed) {
        log::Direct(LogLevel::Warn, "sc7", "%s: %s", result.pass, kOverflowReason);
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, json, static_cast<DWORD>(len), &written, nullptr) != FALSE &&
                    written == len;
    CloseHandle(file);
    if (!ok) {
        return false;
    }

    // ★ LAST WRITER WINS, so verify the bytes on disk are ours. P3 links a stub that writes a refusal
    // under this same name into this same directory until it is wired to this reader; if it ran after
    // us, the file the harness reads is not the file we produced, and reporting success would be a
    // claim about a document we did not write.
    static char readback[kJsonCap];
    const HANDLE check = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (check == INVALID_HANDLE_VALUE) {
        log::Direct(LogLevel::Warn, "sc7", "%s: sc7_text_diff.json could not be read back after writing",
                    result.pass);
        return false;
    }
    DWORD got = 0;
    const bool read = ReadFile(check, readback, static_cast<DWORD>(sizeof readback), &got, nullptr) != FALSE;
    CloseHandle(check);
    if (!read || got != len || std::memcmp(readback, json, len) != 0) {
        log::Direct(LogLevel::Warn, "sc7",
                    "%s: sc7_text_diff.json on disk is not the document this reader wrote - another "
                    "writer (P3's pending stub writes the same name) got there after us",
                    result.pass);
        return false;
    }
    return !overflowed;
}

void RecordPass(Sc7Result& result) {
    if (g_run.historyCount < kMaxPasses) {
        PassRecord& rec = g_run.history[g_run.historyCount++];
        std::snprintf(rec.pass, sizeof rec.pass, "%s", result.pass);
        rec.verdict = result.verdict;
        std::snprintf(rec.reason, sizeof rec.reason, "%s", result.reason);
    }

    if (result.verdict == Verdict::Fail) {
        g_run.sawFailure = true;
        g_run.failing = result;
    } else if (g_run.sawFailure) {
        // The launch saw an undeclared patch. A later PASS means it has since been removed and a later
        // refusal means this pass could not look - neither is a retraction, so the failing measurement
        // is republished in full and this pass's own word survives in `passes`.
        const Verdict thisWord = result.verdict;
        char thisPass[sizeof result.pass];
        std::snprintf(thisPass, sizeof thisPass, "%s", result.pass);
        result = g_run.failing;
        std::snprintf(result.pass, sizeof result.pass, "%s", thisPass);
        std::snprintf(result.reason, sizeof result.reason, "%s (%s later reported %s; see passes)",
                      g_run.failing.reason, thisPass, VerdictName(thisWord));
    }

    for (uint32_t i = 0; i < g_run.historyCount; ++i) {
        result.passes[i] = g_run.history[i];
    }
    result.passCount = g_run.historyCount;
}

void RunPass(const char* passName) {
    Sc7Result result;
    ComputeTextDiff(result);
    std::snprintf(result.pass, sizeof result.pass, "%s", passName != nullptr ? passName : "?");
    RecordPass(result);

    wchar_t dir[kPathMax];
    const char* fallback = ProbeDirForSelf(dir, _countof(dir));
    if (fallback != nullptr) {
        std::snprintf(result.probeDirFallback, sizeof result.probeDirFallback, "%s", fallback);
        const wchar_t* state = Resolved() ? StateDir() : nullptr;
        if (state == nullptr) {
            log::Direct(LogLevel::Warn, "sc7",
                        "%s: nowhere to write sc7_text_diff.json - %s, and there is no state directory "
                        "either", result.pass, fallback);
            return;
        }
        CreateDirectoryW(state, nullptr);
        _snwprintf_s(dir, _countof(dir), _TRUNCATE, L"%s", state);
    }
    const bool wrote = WriteJson(dir, result);
    if (fallback != nullptr) {
        // The harness reads only the artifact directory it named, so a report that landed anywhere else
        // renders as never-asked. Say where it went and why, every time.
        log::Direct(LogLevel::Warn, "sc7",
                    "%s: sc7_text_diff.json went to %ls instead of this launch's artifact directory - %s",
                    result.pass, dir, fallback);
    }
    log::Direct(result.verdict == Verdict::Pass ? LogLevel::Info : LogLevel::Warn, "sc7",
                "%s: %s - %s (%u expected row(s), %u deferred, %u observed, %u undeclared range(s) "
                "of which %u attributed to a loaded third party and %u to nobody, %u reloc byte(s) "
                "excused, %llu bytes of .text in %llu us)%s",
                result.pass, VerdictName(result.verdict), result.reason, result.expectedCount,
                result.deferredCount, result.observedCount, result.unattributedTotal,
                result.attributedRanges, result.unownedTotal, result.relocBytesSkipped,
                static_cast<unsigned long long>(result.textBytes),
                static_cast<unsigned long long>(result.elapsedUs),
                wrote ? "" : " [the report could not be written]");
}

void ResetForTest() { std::memset(&g_run, 0, sizeof g_run); }

size_t SerializeForTest(const Sc7Result& result, char* buf, size_t cap, bool* refused) {
    bool ignored = false;
    return Serialize(result, buf, cap, refused != nullptr ? refused : &ignored);
}

}  // namespace sc7
}  // namespace replay
