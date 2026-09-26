// Do the numbers src/audio/voip/voip_facts.h COMPILES AGAINST still hold in the shipped binary?
//
// ★ WHY THIS EXISTS BESIDE tools/w3c/gen_w3c_facts.py, WHICH ALREADY DECODES THEM. The generator
// produces the W3-C fact drop. This checks the C++ HEADER. They are two consumers of one
// derivation, and the header is the one the shipped code actually uses -- so a hand edit to it, or
// a merge that lands a different value, is invisible to the generator and visible here. The
// expectation comes from the header and the evidence comes from echovr.exe: two artifacts, neither
// generating the other.
//
//   passes on   echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05
//   fails on    --mutate, which perturbs one expected value and requires the comparison to reject it
//   NOT RUN     when the binary is absent, naming the path, because a missing binary is a gate
//               result and never a pass

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "audio/voip/voip_facts.h"
#include "../local_inputs.h"

namespace {

using namespace replay::audio;

constexpr uint64_t kImageBase = 0x140000000ull;

struct Image {
    std::vector<uint8_t> raw;
    struct Section {
        uint32_t vaddr, span, rsize, raddr;
    };
    std::vector<Section> sections;

    bool Load(const char* path) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
            return false;
        }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        raw.resize(static_cast<size_t>(n));
        const size_t got = fread(raw.data(), 1, raw.size(), f);
        fclose(f);
        if (got != raw.size() || raw.size() < 0x200) {
            return false;
        }
        uint32_t lfanew = 0;
        memcpy(&lfanew, raw.data() + 0x3C, sizeof lfanew);
        uint16_t nsec = 0, optsz = 0;
        memcpy(&nsec, raw.data() + lfanew + 6, sizeof nsec);
        memcpy(&optsz, raw.data() + lfanew + 20, sizeof optsz);
        const uint32_t base = lfanew + 24 + optsz;
        for (uint16_t i = 0; i < nsec; ++i) {
            const uint8_t* p = raw.data() + base + 40 * i;
            uint32_t vsize = 0, vaddr = 0, rsize = 0, raddr = 0;
            memcpy(&vsize, p + 8, 4);
            memcpy(&vaddr, p + 12, 4);
            memcpy(&rsize, p + 16, 4);
            memcpy(&raddr, p + 20, 4);
            sections.push_back({vaddr, vsize > rsize ? vsize : rsize, rsize, raddr});
        }
        return true;
    }

    const uint8_t* At(uint64_t va, size_t n) const {
        const uint64_t rva = va - kImageBase;
        for (const Section& s : sections) {
            if (rva >= s.vaddr && rva < uint64_t(s.vaddr) + s.span) {
                const uint64_t off = rva - s.vaddr;
                if (off + n > s.rsize) {
                    return nullptr;
                }
                return raw.data() + s.raddr + off;
            }
        }
        return nullptr;
    }
};

int g_fail = 0;
int g_checked = 0;

// The instruction must be byte-exact before an operand is decoded off it. Decoding an operand out of
// bytes that are not the recorded instruction produces a number, not a fact.
const uint8_t* Site(const Image& img, const char* id, uint64_t va, const char* hex) {
    const size_t n = strlen(hex) / 2;
    const uint8_t* p = img.At(va, n);
    if (p == nullptr) {
        printf("FAIL %-28s 0x%llX is not in the image\n", id, static_cast<unsigned long long>(va));
        ++g_fail;
        return nullptr;
    }
    char got[64] = {};
    for (size_t i = 0; i < n && i * 2 + 2 < sizeof got; ++i) {
        snprintf(got + i * 2, 3, "%02X", p[i]);
    }
    if (_stricmp(got, hex) != 0) {
        printf("FAIL %-28s 0x%llX holds %s, not the recorded %s\n", id,
               static_cast<unsigned long long>(va), got, hex);
        ++g_fail;
        return nullptr;
    }
    return p;
}

void Compare(const char* id, uint64_t expected, uint64_t decoded, const char* how) {
    ++g_checked;
    if (expected == decoded) {
        printf("ok   %-28s 0x%llX  (%s)\n", id, static_cast<unsigned long long>(decoded), how);
        return;
    }
    printf("FAIL %-28s the header says 0x%llX, the binary decodes to 0x%llX  (%s)\n", id,
           static_cast<unsigned long long>(expected), static_cast<unsigned long long>(decoded), how);
    ++g_fail;
}

uint32_t Disp32(const uint8_t* p, size_t at) {
    uint32_t v = 0;
    memcpy(&v, p + at, sizeof v);
    return v;
}

}  // namespace


// ---------------------------------------------------------------------------------------------
// THE COMPOSITION ARM
//
// Every check above asks whether one offset is what the header says. None of them asks the
// question that actually matters for the walk: do these offsets COMPOSE the way HandleOf composes
// them? Six individually correct numbers wired together wrongly produce a plausible handle for the
// wrong instance, and no per-offset check can see it.
//
// The engine's own dispatcher answers it, in four encoded bytes. sub_140D8A630 loads the
// dense-to-pool map, indexes it with the DENSE index the scan found, loads the pool table, and
// indexes THAT with the u16 it just read:
//
//     140D8A734  mov   rax, [rsi+0E0h]              rax = dense-to-pool map
//     140D8A73F  movzx ecx, word ptr [rax+rbx*2]    rbx = dense index (from the imul by 78h)
//     140D8A743  mov   rax, [rsi+0D0h]              rax = pool table
//     140D8A74F  movzx ecx, word ptr [rax+rcx*4+2]  rcx = the pool index just read
//
// The SIB byte names the base, the index register and the scale, so the chain
// dense -> denseMap -> poolIndex -> poolTable -> generation is decidable from the bytes rather
// than from my reading of them. Swapping the two tables, or indexing either with the wrong
// register, changes a SIB byte and this goes red.
// ---------------------------------------------------------------------------------------------
namespace {

// x64 register numbers, low three bits. No REX.X/B is set in any of these instructions, which is
// itself asserted -- an extended register would mean this is not the encoding that was read.
enum Reg { RAX = 0, RCX = 1, RBX = 3, RSI = 6 };

struct Sib {
    int scale, index, base;
};

Sib DecodeSib(uint8_t sib) {
    return Sib{1 << (sib >> 6), (sib >> 3) & 7, sib & 7};
}

void CheckLink(const char* what, bool ok, const char* detail) {
    ++g_checked;
    if (ok) {
        printf("ok   %-28s %s\n", what, detail);
    } else {
        printf("FAIL %-28s %s\n", what, detail);
        ++g_fail;
    }
}

// `bump` perturbs the expected INDEX REGISTER, which is the field that encodes the wiring: with it
// set the arm must reject the shipped bytes, or it is proving nothing.
void CheckComposition(const Image& img, uint32_t bump) {
    const uint8_t* imul = Site(img, "COMPOSE.dense_index", 0x140D8A684, "486BC378");
    const uint8_t* ldMap = Site(img, "COMPOSE.load_densemap", 0x140D8A734, "488B86E0000000");
    const uint8_t* useMap = Site(img, "COMPOSE.index_densemap", 0x140D8A73F, "0FB70C58");
    const uint8_t* ldPool = Site(img, "COMPOSE.load_pooltable", 0x140D8A743, "488B86D0000000");
    const uint8_t* usePool = Site(img, "COMPOSE.index_pooltable", 0x140D8A74F, "0FB74C8802");
    if (!imul || !ldMap || !useMap || !ldPool || !usePool) {
        return;   // Site() already reported; the bytes are not the recorded ones
    }

    // imul rax, rbx, 78h -- the dense index is rbx, and the stride is the CI stride.
    const int denseReg = imul[2] & 7;
    CheckLink("COMPOSE.dense_index",
              denseReg == RBX + static_cast<int>(bump) && imul[3] == voip_facts::kCiStride,
              "the scan's dense index is the register multiplied by VOIP.CI_STRIDE");

    // Both table loads must come off the same object register, and land in the same scratch.
    CheckLink("COMPOSE.tables_same_object",
              (ldMap[2] & 7) == RSI + static_cast<int>(bump) && (ldPool[2] & 7) == RSI &&
                  ((ldMap[2] >> 3) & 7) == RAX && ((ldPool[2] >> 3) & 7) == RAX,
              "both tables are loaded from the component system into the same scratch register");

    // The dense-to-pool map is indexed by the DENSE index, scaled for a u16 array.
    const Sib m = DecodeSib(useMap[3]);
    CheckLink("COMPOSE.densemap_by_dense",
              m.base == RAX && m.index == (RBX | static_cast<int>(bump << 2)) && m.scale == 2,
              "the dense-to-pool map is indexed by the dense index, u16 stride");

    // The pool table is indexed by the value the previous instruction produced -- NOT by the dense
    // index. That is the hop a wrong wiring would collapse, and it is one SIB byte away.
    const int poolIdxReg = (useMap[2] >> 3) & 7;
    const Sib q = DecodeSib(usePool[3]);
    CheckLink("COMPOSE.pooltable_by_poolindex",
              q.base == RAX && q.index == poolIdxReg && q.index != RBX &&
                  q.scale == static_cast<int>(voip_facts::kPoolEntryStride + bump),
              "the pool table is indexed by the POOL index the map produced, at the pool stride");

    CheckLink("COMPOSE.generation_offset",
              usePool[4] == voip_facts::kPoolGenOff + bump,
              "the generation is read at VOIP.POOL_GEN_OFF inside that entry");
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : nullptr;
    bool mutate = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mutate") == 0) {
            mutate = true;
            if (path == argv[i]) {
                path = nullptr;
            }
        }
    }
    const std::string found = replay_test::FindEchoVrExeA();
    if (path == nullptr || strcmp(path, "--mutate") == 0) {
        if (found.empty()) {
            printf("NOT RUN: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE), so no W3-C fact was checked\n");
            return 2;
        }
        path = found.c_str();
    }

    Image img;
    if (!img.Load(path)) {
        printf("NOT RUN: %s could not be read, so no W3-C fact was checked\n", path);
        return 2;
    }

    // The mutation perturbs ONE expected value. Without an arm that fails, the comparisons above
    // have never been observed to do anything.
    const uint32_t bump = mutate ? 1u : 0u;

    if (const uint8_t* p = Site(img, "VOIP.CS_COUNT_OFF", 0x140D8A639, "440FB799FC000000")) {
        Compare("VOIP.CS_COUNT_OFF", voip_facts::kCsCountOff + bump, Disp32(p, 4),
                "movzx r11d, word ptr [rcx+d32]");
    }
    if (const uint8_t* p = Site(img, "VOIP.CS_CI_BASE_OFF", 0x140D8A666, "488BA900010000")) {
        Compare("VOIP.CS_CI_BASE_OFF", voip_facts::kCsCiBaseOff + bump, Disp32(p, 3),
                "mov rbp, [rcx+d32]");
    }
    if (const uint8_t* p = Site(img, "VOIP.CI_STRIDE", 0x140D8A684, "486BC378")) {
        Compare("VOIP.CI_STRIDE", voip_facts::kCiStride + bump, p[3], "imul rax, rbx, imm8");
    }
    if (const uint8_t* p = Site(img, "VOIP.CI_USERID_OFF", 0x140D8A688, "8B4C2820")) {
        Compare("VOIP.CI_USERID_OFF", voip_facts::kCiUserIdOff + bump, p[3],
                "mov ecx, [rax+rbp+d8]");
    }
    if (const uint8_t* p = Site(img, "VOIP.CS_DENSE_MAP_OFF", 0x140D8A734, "488B86E0000000")) {
        Compare("VOIP.CS_DENSE_MAP_OFF", voip_facts::kCsDenseMapOff + bump, Disp32(p, 3),
                "mov rax, [rsi+d32]");
    }
    if (const uint8_t* p = Site(img, "VOIP.CS_POOL_TABLE_OFF", 0x140D8A743, "488B86D0000000")) {
        Compare("VOIP.CS_POOL_TABLE_OFF", voip_facts::kCsPoolTableOff + bump, Disp32(p, 3),
                "mov rax, [rsi+d32]");
    }
    if (const uint8_t* p = Site(img, "VOIP.POOL_GEN_OFF", 0x140D8A74F, "0FB74C8802")) {
        Compare("VOIP.POOL_GEN_OFF", voip_facts::kPoolGenOff + bump, p[4],
                "movzx ecx, word ptr [rax+rcx*4+d8]");
        Compare("VOIP.POOL_ENTRY_STRIDE", voip_facts::kPoolEntryStride + bump,
                uint64_t(1) << (p[3] >> 6), "the SIB scale of the same instruction");
    }
    if (const uint8_t* p = Site(img, "VOIP.CS_NETGAME_OFF", 0x140D8A6BF, "488B8EB8000000")) {
        Compare("VOIP.CS_NETGAME_OFF", voip_facts::kCsNetGameOff + bump, Disp32(p, 3),
                "mov rcx, [rsi+d32]");
    }
    if (const uint8_t* p = Site(img, "VOIP.PROVIDER_GATE_VA", 0x140D376E3, "488B0DF6B83601")) {
        int32_t rel = 0;
        memcpy(&rel, p + 3, sizeof rel);
        Compare("VOIP.PROVIDER_GATE_VA", voip_facts::kProviderGateVa + bump,
                0x140D376E3ull + 7 + rel, "mov rcx, cs:qword -- ReceiveVoip's first gate");
    }
    if (const uint8_t* p = Site(img, "VOIP.CI_DECODER_STATE_OFF", 0x140D3772E, "837D2C00")) {
        Compare("VOIP.CI_DECODER_STATE_OFF", voip_facts::kCiDecoderStateOff + bump, p[2],
                "cmp dword ptr [rbp+d8], 0");
    }
    if (const uint8_t* p = Site(img, "VOIP.CI_TALKER_A_OFF", 0x140D37734, "394524")) {
        Compare("VOIP.CI_TALKER_A_OFF", voip_facts::kCiTalkerAOff + bump, p[2], "cmp [rbp+d8], eax");
    }
    if (const uint8_t* p = Site(img, "VOIP.CI_TALKER_B_OFF", 0x140D3773D, "394528")) {
        Compare("VOIP.CI_TALKER_B_OFF", voip_facts::kCiTalkerBOff + bump, p[2], "cmp [rbp+d8], eax");
    }
    if (const uint8_t* p = Site(img, "VOIP.MUTE_QUERY", 0x140D37749, "E882524DFF")) {
        int32_t rel = 0;
        memcpy(&rel, p + 1, sizeof rel);
        Compare("VOIP.MUTE_QUERY", voip_facts::kMuteQueryVa + bump, 0x140D37749ull + 5 + rel,
                "the rel32 of ReceiveVoip's gate-4 call");
    }

    if (const uint8_t* p = Site(img, "VOIP.NO_USER_SENTINEL_VA", 0x140CCC6AC, "8B05EE073301")) {
        int32_t rel = 0;
        memcpy(&rel, p + 2, sizeof rel);
        Compare("VOIP.NO_USER_SENTINEL_VA", voip_facts::kNoUserSentinelVa + bump,
                0x140CCC6ACull + 6 + rel, "the id VOIP.DISCONNECT stores back into CI+0x20");
    }

    CheckComposition(img, bump);

    // The two engine calls the replay drives. A function address is only as good as the bytes at
    // it, so each is compared as its whole recorded 20-byte prologue rather than as an address
    // that happens to be in range -- the same rule the runtime guard applies before calling them.
    struct Fn {
        const char* id;
        uint64_t va;
        const uint8_t* prologue;
    };
    const Fn fns[] = {
        {"VOIP.CONNECT", voip_facts::kConnectVa, voip_facts::kConnectPrologue},
        {"VOIP.DISCONNECT", voip_facts::kDisconnectVa, voip_facts::kDisconnectPrologue},
    };
    for (const Fn& f : fns) {
        ++g_checked;
        const uint8_t* got = img.At(f.va + bump, 20);
        if (got != nullptr && memcmp(got, f.prologue, 20) == 0) {
            printf("ok   %-28s 0x%llX  (20-byte prologue matches)\n", f.id,
                   static_cast<unsigned long long>(f.va));
        } else {
            printf("FAIL %-28s the 20 bytes at 0x%llX are not the prologue the header carries\n",
                   f.id, static_cast<unsigned long long>(f.va + bump));
            ++g_fail;
        }
    }

    // ★ The symbol is WALKED, not read from a table: lea <name>-1 / and ~7 / mov / call the lookup.
    // Then the same walk is run on the CR15PlayerBroadcasterCS site and must reproduce W1-E's
    // LOCAL.SYM_R14PLAYERBROADCASTER -- a known answer this file cannot fabricate.
    struct Walk {
        const char* id;
        uint64_t leaVa;
        const char* name;
        uint64_t expected;
        bool mustReject;   // a negative control: the name comparison MUST turn this away
    };
    const Walk walks[] = {
        {"CONTROL(R14PlayerBroadcaster)", 0x140152466, "OlPrEfIxR14PlayerBroadcaster",
         0xA0F1C26FE59A20D4ull, false},
        // A DEAD NAME THAT IS A PREFIX OF A LIVE ONE IS REPORTED PRESENT BY EVERY SUBSTRING TEST,
        // and a symbol hasher is total -- it hashes a name nothing is registered under just as
        // happily, after which the lookup returns null forever and says nothing. This entry asks
        // for a name that exists nowhere and is a proper prefix of the one that does; the run FAILS
        // if it is accepted.
        {"NEGATIVE(prefix of real name)", 0x140D32504, "OlPrEfIxR14NetVoip", 0ull, true},
        {"VOIP.RECEIVER_CS_SYMBOL", 0x140D32504, "OlPrEfIxR14NetVoipReceiver",
         voip_facts::kReceiverCsSymbol, false},
    };
    for (const Walk& w : walks) {
        const uint8_t* lea = img.At(w.leaVa, 7);
        if (lea == nullptr || lea[0] != 0x48 || lea[1] != 0x8D) {
            printf("FAIL %-28s 0x%llX is not a lea\n", w.id,
                   static_cast<unsigned long long>(w.leaVa));
            ++g_fail;
            continue;
        }
        int32_t disp = 0;
        memcpy(&disp, lea + 3, sizeof disp);
        const uint64_t nameVa = w.leaVa + 7 + disp;
        // EXACT, NOT A PREFIX. strncmp over strlen(name) accepts every name the real one starts
        // with, so "R14NetVoip" passes against "R14NetVoipReceiver" and the check reports a name
        // registered nowhere as present. The terminator is what makes the comparison a comparison,
        // and the negative control in the table above is what proves it does.
        const uint8_t* name = img.At(nameVa + 1, 64);
        const size_t want = strlen(w.name);
        const bool matched = name != nullptr &&
                             strncmp(reinterpret_cast<const char*>(name), w.name, want) == 0 &&
                             name[want] == 0;
        if (w.mustReject) {
            ++g_checked;
            if (matched) {
                printf("FAIL %-28s a name registered nowhere was accepted; the name check is a "
                       "prefix test and proves nothing\n", w.id);
                ++g_fail;
            } else {
                printf("ok   %-28s rejected, as a name nothing is registered under must be\n",
                       w.id);
            }
            continue;
        }
        if (!matched) {
            printf("FAIL %-28s 0x%llX does not point at %s\n", w.id,
                   static_cast<unsigned long long>(w.leaVa), w.name);
            ++g_fail;
            continue;
        }
        const uint8_t* q = img.At(nameVa & ~7ull, 8);
        if (q == nullptr) {
            printf("FAIL %-28s the symbol qword is not in the image\n", w.id);
            ++g_fail;
            continue;
        }
        uint64_t sym = 0;
        memcpy(&sym, q, sizeof sym);
        // The control is checked WITHOUT the mutation: a reader that fails its own known answer
        // must stop the run whatever else is being asked.
        const bool isControl = strncmp(w.id, "CONTROL", 7) == 0;
        Compare(w.id, w.expected + (isControl ? 0u : bump), sym,
                "the qword the engine's own lea/and/mov loads");
    }

    if (mutate) {
        if (g_fail == 0) {
            printf("MUTATION NOT CAUGHT: every comparison passed with a perturbed expectation, so "
                   "this check proves nothing\n");
            return 1;
        }
        printf("mutation caught: %d of %d comparisons rejected the perturbed expectation\n", g_fail,
               g_checked);
        return 0;
    }
    printf("%d of %d W3-C values in voip_facts.h agree with echovr.exe\n", g_checked - g_fail,
           g_checked);
    return g_fail == 0 ? 0 : 1;
}
