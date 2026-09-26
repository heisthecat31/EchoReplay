// W1-F's offline half. Two independent things are checked here, and neither launches the game.
//
//  1. The SE-A string work - which tokens count as present, and that appending is idempotent and
//     space-separated. CMDLINE.TOKENIZE splits unquoted tokens on 0x20 alone, so "space" is a
//     correctness property and not a formatting preference.
//
//  2. A known-answer control that re-derives this packet's numbers from the shipping echovr.exe
//     with a second, deliberately independent PE reader. Every value session_facts.h ships is
//     recomputed from the file's bytes here, so a transcription slip fails the build instead of
//     surviving to a launch. Two derivations, one comparison.
//
// The last case is the one that matters most politically: it proves mechanically that -gametype
// does NOT set the boot-to-multiplayer bit, which is the misattribution keeping the project's
// offline profile out of the harness.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <string>
#include <vector>

#include <doctest.h>

// ★ NO COMMAS IN TEST OR SUBCASE NAMES. doctest treats a comma in --test-case / --subcase as a
// filter SEPARATOR, so a name containing one can never be selected: the filter matches nothing,
// doctest runs zero tests, exits 0 and prints SUCCESS. W1-C hit exactly that and caught it only
// because "62 skipped" looked wrong. Same family as a stale-binary green, and it DEFEATS the
// staleness gate - a fresh binary running no tests is still a green that means nothing.
// Corollary: never quote a filtered run without checking the executed count is non-zero.

#include "engine/session.h"
#include "engine/session_facts.h"

#include "boot_flag_expectation.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/livesafety.h"
#include "../local_inputs.h"

namespace {

namespace sf = replay::session_facts;
using replay::session::AugmentCommandLine;
using replay::session::HasToken;


// A minimal PE-on-disk reader. Independent of src/engine/session.cpp, which walks the MAPPED
// image: if the two agreed only because they shared code, this control would prove nothing.
class PeFile {
public:
    bool Load(const char* path) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
            return false;
        }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            fclose(f);
            return false;
        }
        bytes_.resize(static_cast<size_t>(size));
        const size_t got = fread(bytes_.data(), 1, bytes_.size(), f);
        fclose(f);
        if (got != bytes_.size()) {
            return false;
        }
        return Parse();
    }

    uint64_t ImageBase() const { return imageBase_; }

    const uint8_t* At(uint64_t va, size_t len) const {
        const uint64_t rva = va - imageBase_;
        for (const auto& s : sections_) {
            const uint64_t span = (s.virtualSize > s.rawSize) ? s.virtualSize : s.rawSize;
            if (rva >= s.virtualAddress && rva + len <= s.virtualAddress + span) {
                const size_t off = static_cast<size_t>(s.rawPointer + (rva - s.virtualAddress));
                if (off + len <= bytes_.size()) {
                    return bytes_.data() + off;
                }
            }
        }
        return nullptr;
    }

    // Every occurrence of `needle` anywhere in the file, returned as virtual addresses.
    std::vector<uint64_t> FindAll(const uint8_t* needle, size_t len) const {
        std::vector<uint64_t> hits;
        for (const auto& s : sections_) {
            if (s.rawSize < len) {
                continue;
            }
            const uint8_t* start = bytes_.data() + s.rawPointer;
            for (size_t i = 0; i + len <= s.rawSize; ++i) {
                if (memcmp(start + i, needle, len) == 0) {
                    hits.push_back(imageBase_ + s.virtualAddress + i);
                }
            }
        }
        return hits;
    }

private:
    struct Section {
        uint64_t virtualAddress;
        uint64_t virtualSize;
        uint64_t rawPointer;
        uint64_t rawSize;
    };

    bool Parse() {
        if (bytes_.size() < 0x40) {
            return false;
        }
        const uint32_t peOff = *reinterpret_cast<const uint32_t*>(bytes_.data() + 0x3C);
        if (peOff + sizeof(IMAGE_NT_HEADERS64) > bytes_.size() ||
            memcmp(bytes_.data() + peOff, "PE\0\0", 4) != 0) {
            return false;
        }
        const uint8_t* fh = bytes_.data() + peOff + 4;
        const uint16_t sectionCount = *reinterpret_cast<const uint16_t*>(fh + 2);
        const uint16_t optSize = *reinterpret_cast<const uint16_t*>(fh + 16);
        const uint8_t* opt = fh + 20;
        imageBase_ = *reinterpret_cast<const uint64_t*>(opt + 24);
        const uint8_t* sec = opt + optSize;
        for (uint16_t i = 0; i < sectionCount; ++i, sec += 40) {
            Section s{};
            s.virtualSize = *reinterpret_cast<const uint32_t*>(sec + 8);
            s.virtualAddress = *reinterpret_cast<const uint32_t*>(sec + 12);
            s.rawSize = *reinterpret_cast<const uint32_t*>(sec + 16);
            s.rawPointer = *reinterpret_cast<const uint32_t*>(sec + 20);
            if (s.rawPointer + s.rawSize <= bytes_.size()) {
                sections_.push_back(s);
            }
        }
        return !sections_.empty();
    }

    std::vector<uint8_t> bytes_;
    std::vector<Section> sections_;
    uint64_t imageBase_ = 0;
};

const PeFile* Exe() {
    static PeFile pe;
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        const std::string path = replay_test::FindEchoVrExeA();
        ok = !path.empty() && pe.Load(path.c_str());
    }
    return ok ? &pe : nullptr;
}

// The id the engine stores for a name, read the way ApplyCommandLine reads it: the qword
// immediately before the "OlPrEfIx<name>" literal.
uint64_t IdOfName(const PeFile& pe, const std::string& name) {
    const std::string pattern = "OlPrEfIx" + name;
    const auto hits =
        pe.FindAll(reinterpret_cast<const uint8_t*>(pattern.c_str()), pattern.size() + 1);
    for (uint64_t va : hits) {
        if ((va % 8) != 0) {
            continue;
        }
        const uint8_t* id = pe.At(va - 8, 8);
        if (id != nullptr) {
            return *reinterpret_cast<const uint64_t*>(id);
        }
    }
    return 0;
}


// ---------------------------------------------------------------------------------------------
// rlay.dll, the other mod that ships in the plugins directory. It is a Detours DLL (it carries
// .detourd) and it resolves echovr.exe targets with a 21-byte idiom, one per target:
//
//     48 8B 05 <rel32>   mov rax, [rip+moduleBase]
//     48 05 <RVA imm32>  add rax, <echovr RVA>
//     48 89 05 <rel32>   mov [rip+slot], rax
//     C3
//
// Parsing the shape rather than a hardcoded table means this FINDS the set instead of assuming its
// size, so a future rlay build that patches something new is caught rather than missed.
// ---------------------------------------------------------------------------------------------

const char* kDefaultRlay =
    R"(C:\Program Files\Oculus\Software\Software\ready-at-dawn-echo-arena\bin\win10\plugins\rlay.dll)";

const PeFile* Rlay() {
    static PeFile pe;
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        const char* path = kDefaultRlay;
        char env[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("REPLAY_RLAY_DLL", env, sizeof(env));
        if (n > 0 && n < sizeof(env)) {
            path = env;
        }
        ok = pe.Load(path);
    }
    return ok ? &pe : nullptr;
}

std::vector<uint32_t> RlayResolvedRvas(const PeFile& pe) {
    std::vector<uint32_t> found;
    const uint8_t head[3] = {0x48, 0x8B, 0x05};
    const uint8_t mid[2] = {0x48, 0x05};
    const uint8_t store[3] = {0x48, 0x89, 0x05};
    for (uint64_t va : pe.FindAll(head, sizeof(head))) {
        const uint8_t* at = pe.At(va, 21);
        if (at == nullptr) {
            continue;
        }
        if (memcmp(at + 7, mid, sizeof(mid)) != 0 ||
            memcmp(at + 13, store, sizeof(store)) != 0 || at[20] != 0xC3) {
            continue;
        }
        const uint32_t rva = *reinterpret_cast<const uint32_t*>(at + 9);
        if (rva >= 0x1000 && rva < 0x2200000) {
            found.push_back(rva);
        }
    }
    return found;
}

}  // namespace

TEST_CASE("HasToken sees whole tokens and ignores quoted values") {
    CHECK(HasToken("-noovr -spectatorstream", "-noovr"));
    CHECK(HasToken("-noovr -spectatorstream", "-spectatorstream"));
    CHECK(HasToken("-spectatorstream=echo_arena", "-spectatorstream"));
    CHECK(HasToken("+fixedtimestep", "-fixedtimestep"));   // three prefixes, one switch
    CHECK(HasToken("-NoOvr", "-noovr"));                   // the lookup is case-folded
    CHECK(HasToken("-a -noovr", "noovr"));                 // caller may omit the prefix

    CHECK_FALSE(HasToken("-noovrx", "-noovr"));            // prefix of a longer switch
    CHECK_FALSE(HasToken("-noov", "-noovr"));
    CHECK_FALSE(HasToken("", "-noovr"));
    CHECK_FALSE(HasToken("-level mpl_arena_a", "-gametype"));
    // ★ The three near-misses W1-C pinned when -offline became a mode signal, run against this
    // matcher too. A substring scan would arm on all three; whole-token matching refuses all three.
    // The switch is spelled -offline here because that is the one whose prefix-ness caused the
    // problem, but the property is the matcher's, not the switch's.
    CHECK_FALSE(HasToken("-offlinefoo", "-offline"));   // longer token with our name as a prefix
    CHECK_FALSE(HasToken("-x-offline", "-offline"));    // our name as a SUFFIX of a longer token
    CHECK_FALSE(HasToken("-nooffline", "-offline"));    // our name embedded in the middle
    CHECK(HasToken("-a -offline -b", "-offline"));      // and it still finds the real one
    // The engine's own parser strips every leading prefix character in a loop before hashing, so
    // "--offline" IS the -offline switch to it. Matching that is faithful, not sloppy.
    CHECK(HasToken("--offline", "-offline"));

    // A path that merely contains the switch text is a value, not a switch.
    const char* quotedPath = "-logpath \"C:\\x\\-noovr\\y\"";
    CHECK_FALSE(HasToken(quotedPath, "-noovr"));
}

TEST_CASE("AugmentCommandLine appends only what is missing and separates with a space") {
    const char* want[] = {"-noovr", "-spectatorstream", "-fixedtimestep"};
    char out[512];

    SUBCASE("appends all three to a bare line") {
        const size_t n = AugmentCommandLine("echovr.exe", want, 3, out, sizeof(out));
        CHECK(n == strlen(out));
        CHECK(std::string(out) == "echovr.exe -noovr -spectatorstream -fixedtimestep");
    }

    SUBCASE("is idempotent") {
        AugmentCommandLine("echovr.exe", want, 3, out, sizeof(out));
        char again[512];
        AugmentCommandLine(out, want, 3, again, sizeof(again));
        CHECK(std::string(again) == std::string(out));
    }

    SUBCASE("never repeats a token the user already supplied in any prefix form") {
        AugmentCommandLine("game -spectatorstream=echo_arena +noovr", want, 3, out, sizeof(out));
        CHECK(std::string(out) == "game -spectatorstream=echo_arena +noovr -fixedtimestep");
    }

    SUBCASE("separates with 0x20 and nothing else") {
        AugmentCommandLine("game", want, 1, out, sizeof(out));
        const char* sep = strchr(out, '\0') - strlen("-noovr") - 1;
        CHECK(*sep == ' ');
    }

    SUBCASE("refuses rather than truncating when it will not fit") {
        char tiny[16];
        const size_t n = AugmentCommandLine("echovr.exe", want, 3, tiny, sizeof(tiny));
        CHECK(n == 0);
        CHECK(std::string(tiny) == "echovr.exe");  // the original survives intact
    }

    SUBCASE("an original longer than the buffer yields an empty string rather than a partial one") {
        char tiny[4];
        CHECK(AugmentCommandLine("echovr.exe", want, 3, tiny, sizeof(tiny)) == 0);
        CHECK(tiny[0] == '\0');
    }
}

TEST_CASE("known-answer control: session_facts.h against the shipping echovr.exe") {
    const PeFile* pe = Exe();
    if (pe == nullptr) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    CHECK(pe->ImageBase() == facts::PE_IMAGE_BASE);

    SUBCASE("the four call sites hold their recorded prologues") {
        struct Site {
            const char*    id;
            uint64_t       va;
            const uint8_t* prologue;
        } sites[] = {
            {"SESSION.CREATE_OFFLINE_MATCH", sf::kEnterOfflineMatch, sf::kEnterOfflinePrologue},
            {"SESSION.BEGIN_MULTIPLAYER", sf::kBeginMultiplay, sf::kBeginMultiplayPrologue},
            {"SESSION.SPECTATE_CREATE", sf::kSpectateCreate, sf::kSpectateCreatePrologue},
            {"GAME.BOOT_BRANCH", sf::kBootBranch, sf::kBootBranchPrologue},
        };
        for (const Site& s : sites) {
            const uint8_t* at = pe->At(s.va, sf::kPrologueLen);
            REQUIRE_MESSAGE(at != nullptr, s.id);
            CHECK_MESSAGE(memcmp(at, s.prologue, sf::kPrologueLen) == 0, s.id);
        }
    }

    SUBCASE("GAME.PTR_VA is what CR15Game::Initialize stores its this pointer into") {
        // 48 89 0D <rel32> = mov [rip+rel32], rcx, at GAME.INITIALIZE+0x33.
        const uint8_t* at = pe->At(facts::GAME_SINGLETON_WRITE_VA, 7);
        REQUIRE(at != nullptr);
        CHECK(at[0] == 0x48);
        CHECK(at[1] == 0x89);
        CHECK(at[2] == 0x0D);
        const int32_t disp = *reinterpret_cast<const int32_t*>(at + 3);
        CHECK(facts::GAME_SINGLETON_WRITE_VA + 7 + static_cast<int64_t>(disp) == sf::kGamePtrVa);
    }

    SUBCASE("the SessionFlags offset is the pointer NetGame is constructed with") {
        // lea r12, [rsi+0x8A70] in CR15Game::Initialize, passed as the NetGame ctor's arg6...
        const uint8_t* lea = pe->At(0x140109898ull, 7);
        REQUIRE(lea != nullptr);
        CHECK(lea[0] == 0x4C);
        CHECK(lea[1] == 0x8D);
        CHECK(lea[2] == 0xA6);
        CHECK(*reinterpret_cast<const uint32_t*>(lea + 3) == sf::kSessionFlagsOff);

        // ... and stored at NetGame+0x2DA0: 48 89 86 A0 2D 00 00 = mov [rsi+0x2DA0], rax.
        const uint8_t* store = pe->At(0x140146229ull, 7);
        REQUIRE(store != nullptr);
        CHECK(store[0] == 0x48);
        CHECK(store[1] == 0x89);
        CHECK(store[2] == 0x86);
        CHECK(*reinterpret_cast<const uint32_t*>(store + 3) == facts::NETGAME_ROLEFLAGS_OFF);
    }

    SUBCASE("6719 is a cmov over 6721 and both immediates are unique on that path") {
        const uint8_t expected[] = {0xB8, 0x41, 0x1A, 0x00, 0x00,   // mov eax, 6721
                                    0xB9, 0x3F, 0x1A, 0x00, 0x00,   // mov ecx, 6719
                                    0x45, 0x85, 0xF6,               // test r14d, r14d
                                    0x66, 0x0F, 0x45, 0xC1};        // cmovnz ax, cx
        constexpr uint64_t kPortSelectVa = 0x140159C1Cull;  // facts-ok: SESSION.HTTP_PORT_SELECT_VA PC_PRESENT, so gen_facts emits no value
        const uint8_t* at = pe->At(kPortSelectVa, sizeof(expected));
        REQUIRE(at != nullptr);
        CHECK(memcmp(at, expected, sizeof(expected)) == 0);
        CHECK(*reinterpret_cast<const uint16_t*>(at + 1) == sf::kHttpPortDefault);
        CHECK(*reinterpret_cast<const uint16_t*>(at + 6) == sf::kHttpPortHost);

        // The whole acceptance argument is "6719 can only come from here", so the immediate must
        // not appear anywhere else as a mov-imm32.
        const uint8_t movEcx6719[] = {0xB9, 0x3F, 0x1A, 0x00, 0x00};
        CHECK(pe->FindAll(movEcx6719, sizeof(movEcx6719)).size() == 1);
    }

    SUBCASE("the name ids are the ones the engine compares against") {
        CHECK(IdOfName(*pe, "echo_arena") == 0xCB60A4DE7E1CAF73ull);
        CHECK(IdOfName(*pe, "echo_combat") == 0x3D5C3976578A321Aull);
        CHECK(IdOfName(*pe, "mpl_arena_a") == 0x576ED3F8428EBC4Bull);
        // A name that does not exist must resolve to nothing rather than to a neighbour's id.
        CHECK(IdOfName(*pe, "mpl_arena_a_that_does_not_exist") == 0);
    }

    SUBCASE("-gametype does not set the boot-to-multiplayer bit") {
        // W1-J's launch guard refuses -gametype on the grounds that it ORs 0x2000000 into
        // CR15Game+0x7AE0. Its handler is the -gametype string load at 0x140116E5C through the
        // store into GAME.GAMETYPE_HASH_OFF; the next OR of that bit is 0x3C0 bytes further on,
        // inside the moderator block. Search the handler's whole span for the instruction.
        const uint8_t orBootMp[] = {0x48, 0x81, 0x8F, 0xE0, 0x7A, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x02};  // or qword [rdi+0x7AE0], 0x2000000
        const uint64_t handlerStart = 0x140116E5Cull;
        const uint64_t handlerEnd = 0x140116EF5ull;  // the -spectatorstream string load
        const uint8_t* span = pe->At(handlerStart, static_cast<size_t>(handlerEnd - handlerStart));
        REQUIRE(span != nullptr);
        const size_t spanLen = static_cast<size_t>(handlerEnd - handlerStart);
        bool found = false;
        for (size_t i = 0; i + sizeof(orBootMp) <= spanLen; ++i) {
            if (memcmp(span + i, orBootMp, sizeof(orBootMp)) == 0) {
                found = true;
            }
        }
        CHECK_FALSE(found);

        // Positive control: the span really is -gametype's handler, because it contains the store
        // into GAME.GAMETYPE_HASH_OFF. Without this the search above could pass by looking at the
        // wrong 153 bytes.
        const uint8_t gametypeStore[] = {0x48, 0x89, 0x87, 0x68, 0x85, 0x00, 0x00};
        bool storeFound = false;
        for (size_t i = 0; i + sizeof(gametypeStore) <= spanLen; ++i) {
            if (memcmp(span + i, gametypeStore, sizeof(gametypeStore)) == 0) {
                storeFound = true;
            }
        }
        CHECK(storeFound);
        CHECK(*reinterpret_cast<const uint32_t*>(gametypeStore + 3) == sf::kGameGametypeOff);

        // The control: the same instruction IS present in -spectatorstream's handler, as the
        // 0x2100000 form. If this half stopped matching, the search above would pass vacuously.
        const uint8_t orSpectator[] = {0x48, 0x81, 0x8F, 0xE0, 0x7A, 0x00, 0x00,
                                       0x00, 0x00, 0x10, 0x02};  // ..., 0x2100000
        const uint8_t* atSpectator = pe->At(0x140116F4Dull, sizeof(orSpectator));
        REQUIRE(atSpectator != nullptr);
        CHECK(memcmp(atSpectator, orSpectator, sizeof(orSpectator)) == 0);
    }

    SUBCASE("the gamespaces bit is not a witness that GAME.BOOT_BRANCH ran") {
        // F-F2. The only write of 0x100 sits in the ELSE arm of the boot branch AND behind a
        // second level-is-set guard, so it stays clear after a multiplayer boot. A suppressor that
        // read it as "the branch has not run yet" would clear bit 25 too late while logging that
        // it was in time.
        //
        //   0x140108BEF  mov  eax, [r14+0x7AE0]      ;         //   0x140108BF6  shr  rax, 0x19              ;  > bit 25
        //   0x140108BFA  test al, 1                  ; /
        //   0x140108BFC  jz   +0x30                  ; clear -> the gamespace arm
        //   0x140108C0B  call sub_140100B90          ; set   -> MULTIPLAYER boot
        //   0x140108C1E  mov  dword [r14+0x7B14], 1  ; the multiplayer arm's own footprint
        //   0x140108C2E  cmp  qword [r14+0x950], -1  ; the SECOND guard
        //   0x140108C4D  or   qword [r14+0x7AE0], 0x100
        const uint8_t test25[] = {0x41, 0x8B, 0x86, 0xE0, 0x7A, 0x00, 0x00,   // mov eax,[r14+7AE0]
                                  0x48, 0xC1, 0xE8, 0x19,                     // shr rax,0x19
                                  0xA8, 0x01,                                 // test al,1
                                  0x74};                                      // jz -> gamespace arm
        const uint8_t* at = pe->At(0x140108BEFull, sizeof(test25));
        REQUIRE(at != nullptr);
        CHECK(memcmp(at, test25, sizeof(test25)) == 0);

        // the multiplayer arm writes the boot-state field, and nothing else on this path does
        const uint8_t bootState[] = {0x41, 0xC7, 0x86, 0x14, 0x7B, 0x00, 0x00,
                                     0x01, 0x00, 0x00, 0x00};
        const uint8_t* mp = pe->At(0x140108C1Eull, sizeof(bootState));
        REQUIRE(mp != nullptr);
        CHECK(memcmp(mp, bootState, sizeof(bootState)) == 0);
        CHECK(*reinterpret_cast<const uint32_t*>(mp + 3) == sf::kGameBootStateOff);

        // the second guard, and the 0x100 write behind it
        const uint8_t guard[] = {0x49, 0x83, 0xBE, 0x50, 0x09, 0x00, 0x00, 0xFF};
        const uint8_t* g2 = pe->At(0x140108C2Eull, sizeof(guard));
        REQUIRE(g2 != nullptr);
        CHECK(memcmp(g2, guard, sizeof(guard)) == 0);
        CHECK(*reinterpret_cast<const uint32_t*>(g2 + 3) == sf::kGameLevelHashOff);

        const uint8_t orGamespaces[] = {0x49, 0x81, 0x8E, 0xE0, 0x7A, 0x00, 0x00,
                                        0x00, 0x01, 0x00, 0x00};
        const uint8_t* og = pe->At(0x140108C4Dull, sizeof(orGamespaces));
        REQUIRE(og != nullptr);
        CHECK(memcmp(og, orGamespaces, sizeof(orGamespaces)) == 0);
        CHECK(*reinterpret_cast<const uint32_t*>(og + 7) == sf::kModeBitGamespaces);

        // And it is the ONLY one: if a second write existed the "not a witness" reasoning would
        // have to be redone rather than assumed.
        CHECK(pe->FindAll(orGamespaces, sizeof(orGamespaces)).size() == 1);
    }

    SUBCASE("the two citations the audit found mid-instruction are instruction starts") {
        // F-F3. Both were cited one and two bytes into their instruction. These are the real ones.
        const uint8_t teamWrite[] = {0x66, 0x89, 0x87, 0x60, 0x85, 0x00, 0x00};  // mov [rdi+8560],ax
        const uint8_t* tw = pe->At(0x140116F73ull, sizeof(teamWrite));
        REQUIRE(tw != nullptr);
        CHECK(memcmp(tw, teamWrite, sizeof(teamWrite)) == 0);
        CHECK(*reinterpret_cast<const uint32_t*>(tw + 3) == sf::kGameTeamOff);

        const uint8_t teamRead[] = {0x44, 0x0F, 0xB7, 0x83, 0x60, 0x85, 0x00, 0x00};
        const uint8_t* tr = pe->At(0x1400FDC04ull, sizeof(teamRead));
        REQUIRE(tr != nullptr);
        CHECK(memcmp(tr, teamRead, sizeof(teamRead)) == 0);
        CHECK(*reinterpret_cast<const uint32_t*>(tr + 4) == sf::kGameTeamOff);
    }

    SUBCASE("an absent -level is what sets the boot-to-multiplayer bit") {
        // ★ The sweep measured modeFlags = 0x42800000 on a launch with NO arguments, so bit 25 was
        // set with no switch present. This is why. ApplyCommandLine tests the level hash and, if no
        // -level was supplied, puts the client into multiplayer mode AND marks the session online:
        //
        //   48 83 BF 50 09 00 00 FF                cmp qword [rdi+0x950], -1
        //   75 13                                  jnz over
        //   48 81 8F E0 7A 00 00 00 00 00 02       or  qword [rdi+0x7AE0], 0x2000000
        //   48 83 8F 70 8A 00 00 01                or  qword [rdi+0x8A70], 1
        //
        // So supplying -level does not merely avoid setting the bit: it is what PREVENTS the bit.
        const uint8_t expected[] = {0x48, 0x83, 0xBF, 0x50, 0x09, 0x00, 0x00, 0xFF,
                                    0x75, 0x13,
                                    0x48, 0x81, 0x8F, 0xE0, 0x7A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
                                    0x48, 0x83, 0x8F, 0x70, 0x8A, 0x00, 0x00, 0x01};
        const uint8_t* at = pe->At(0x140116CD5ull, sizeof(expected));
        REQUIRE(at != nullptr);
        CHECK(memcmp(at, expected, sizeof(expected)) == 0);
        // The three offsets this depends on, read back out of the operands rather than restated.
        CHECK(*reinterpret_cast<const uint32_t*>(at + 3) == sf::kGameLevelHashOff);
        CHECK(*reinterpret_cast<const uint32_t*>(at + 13) == sf::kGameModeFlagsOff);
        CHECK(*reinterpret_cast<const uint32_t*>(at + 17) == sf::kModeBitBootMp);
        CHECK(*reinterpret_cast<const uint32_t*>(at + 24) == sf::kSessionFlagsOff);
        CHECK(at[28] == sf::kFlagBitOnline);
    }

    SUBCASE("-httpport writes a port and does not set the boot-to-multiplayer bit") {
        // 66 89 87 5E 85 00 00 = mov [rdi+0x855E], ax, right after the ntohs call.
        const uint8_t* at = pe->At(0x140117258ull, 7);
        REQUIRE(at != nullptr);
        CHECK(at[0] == 0x66);
        CHECK(at[1] == 0x89);
        CHECK(at[2] == 0x87);
        CHECK(*reinterpret_cast<const uint32_t*>(at + 3) == sf::kGameHttpPortOff);
    }

    SUBCASE("WinMain reads the live command line 46 bytes after loading us") {
        // E8 rel32 -> the plugin loader.
        const uint8_t* loader = pe->At(sf::kCmdlineLoaderCallVa, 5);
        REQUIRE(loader != nullptr);
        CHECK(loader[0] == 0xE8);
        const int32_t rel = *reinterpret_cast<const int32_t*>(loader + 1);
        CHECK(sf::kCmdlineLoaderCallVa + 5 + static_cast<int64_t>(rel) == facts::ENTRY_PLUGIN_LOAD);

        // FF 15 rel32 -> the IAT slot for GetCommandLineA.
        const uint8_t* getcl = pe->At(sf::kCmdlineGetCommandLineVa, 6);
        REQUIRE(getcl != nullptr);
        CHECK(getcl[0] == 0xFF);
        CHECK(getcl[1] == 0x15);
        const int32_t iatRel = *reinterpret_cast<const int32_t*>(getcl + 2);
        CHECK(sf::kCmdlineGetCommandLineVa + 6 + static_cast<int64_t>(iatRel) ==
              sf::kCmdlineIatGetCommandLineA);

        CHECK(sf::kCmdlineGetCommandLineVa - sf::kCmdlineLoaderCallVa == 46);
    }
}

TEST_CASE("IsOfflineFlags is the authoritative mode signal and only bit 6 sets it") {
    // The 2026-08-29 ruling makes this the signal W1-C's detector reads, so it is worth an
    // exhaustive check over the three bits that participate rather than a few examples.
    for (uint64_t f = 0; f < 8; ++f) {
        const uint64_t flags = ((f & 1) ? sf::kFlagBitOnline : 0) |
                               ((f & 2) ? sf::kFlagBitDedicated : 0) |
                               ((f & 4) ? sf::kFlagBitOffline : 0);
        CHECK(sf::IsOfflineFlags(flags) == ((flags & 0x44) == 0x40));
    }
    CHECK_FALSE(sf::IsOfflineFlags(0));                                          // no session
    CHECK(sf::IsOfflineFlags(sf::kFlagBitOffline));                              // the offline entry
    CHECK_FALSE(sf::IsOfflineFlags(sf::kFlagBitOffline | sf::kFlagBitDedicated)); // a server host
    // ★ The online bit does not veto it. -mp sets bit 0 and the no--level default sets it too, so a
    // session can carry both; bit 6 is written only by the two offline entry points and wins.
    CHECK(sf::IsOfflineFlags(sf::kFlagBitOffline | sf::kFlagBitOnline));
    // And the stale session-active bit is irrelevant to it, which is the point of not using that.
    CHECK(sf::IsOfflineFlags(sf::kFlagBitOffline | sf::kFlagBitActive));
    CHECK_FALSE(sf::IsOfflineFlags(sf::kFlagBitActive));
}

TEST_CASE("SessionNetworkName covers every enumerator") {
    using replay::session::SessionNetwork;
    CHECK(std::string(replay::session::SessionNetworkName(SessionNetwork::NotYetKnown)) ==
          "not-yet-known");
    CHECK(std::string(replay::session::SessionNetworkName(SessionNetwork::Offline)) == "offline");
    CHECK(std::string(replay::session::SessionNetworkName(SessionNetwork::LiveOnlineMatch)) ==
          "live-online-match");
}

TEST_CASE("IsHostFlags matches the predicate the engine evaluates") {
    // host = bit1 || ((f & 0x44) == 0x40), read from NETGAME_HOST_PREDICATE_VA and again from the
    // port selection. Exhaustive over the three bits that participate.
    for (uint64_t f = 0; f < 8; ++f) {
        const uint64_t flags = ((f & 1) ? sf::kFlagBitHostAlt : 0) |
                               ((f & 2) ? sf::kFlagBitDedicated : 0) |
                               ((f & 4) ? sf::kFlagBitOffline : 0);
        const bool expected = (flags & 2) != 0 || (flags & 0x44) == 0x40;
        CHECK(sf::IsHostFlags(flags) == expected);
    }
    CHECK_FALSE(sf::IsHostFlags(0));
    CHECK(sf::IsHostFlags(sf::kFlagBitOffline));
    CHECK_FALSE(sf::IsHostFlags(sf::kFlagBitOffline | sf::kFlagBitDedicated));
}

TEST_CASE("rlay.dll patches no site W1-F calls or hooks") {
    const PeFile* rlay = Rlay();
    REQUIRE_MESSAGE(rlay != nullptr, "rlay.dll not readable; set REPLAY_RLAY_DLL to its path");

    const std::vector<uint32_t> patched = RlayResolvedRvas(*rlay);
    // Positive control. If the parser stopped matching, the "none of ours is in there" check below
    // would pass by finding nothing at all, which is the classic vacuous green.
    CHECK(patched.size() >= 12);
    auto has = [&](uint32_t rva) {
        return std::find(patched.begin(), patched.end(), rva) != patched.end();
    };
    CHECK(has(0x116720));   // CLI.APPLY - the one the coordinator flagged
    CHECK(has(0x1B8650));   // the NetGame state machine
    CHECK(has(0x0FEA00));   // the switch registry, which is how it adds -offline / -server

    // Every echovr address W1-F calls into, hooks, or reads a prologue from. A collision here is a
    // trampoline fighting a Detours patch, which is the failure mode S4.5.3 rule 4 exists for.
    struct Site {
        const char* id;
        uint32_t    rva;
    } ours[] = {
        {"SESSION.CREATE_OFFLINE_MATCH (SE-B's stock call)", 0x0FE0A0},
        {"SESSION.BEGIN_MULTIPLAYER", 0x0FDA70},
        {"SESSION.SPECTATE_CREATE", 0x15A050},
        {"NETGAME_INIT", 0x157FB0},
        {"GAME.BOOT_BRANCH", 0x1088D0},
        {"GAME.INITIALIZE", 0x109810},
        {"NETGAME.UPDATE (the probe's engine-thread pump)", facts::NETGAME_UPDATE_RVA},
        {"CMDLINE.TOKENIZE", 0x0D9000},
        {"SESSION.STATE_NAME", 0x16D370},
    };
    for (const Site& site : ours) {
        CHECK_MESSAGE(!has(site.rva), site.id);
    }
}

TEST_CASE("the augmented token set carries nothing that boots into multiplayer") {
    size_t count = 0;
    const char* const* tokens = replay::session::AugmentTokens(&count);

    // OWNER-WATCH-FIX: nothing is augmented any more. -fixedtimestep armed CLOCK.LIMITER_BIT, a
    // sleep-to-33.3ms frame limiter, on every launch including live capture.
    CHECK(count == 0);
    CHECK(tokens == nullptr);

    // The eighteen the harness refuses, plus -noovr, which is fatal without -spectatorstream and
    // so can never appear alone either. Vacuous while the set is empty, and it must stay here: it
    // is the rule any future token has to clear.
    const char* banned[] = {"-spectatorstream", "-noovr",   "-mp",           "-mpappid",
                            "-region",          "-gametype", "-httpport",    "-lobbyid",
                            "-lobbyteam",       "-moderator", "-moderateuser", "-moderategroup"};
    for (size_t i = 0; i < count; ++i) {
        for (const char* bad : banned) {
            CHECK_MESSAGE(!HasToken(tokens[i], bad), tokens[i]);
        }
    }
}

TEST_CASE("an empty augment set leaves the command line byte-identical") {
    // The augmenter still copies (it memcpys the original before appending anything), so what has
    // to hold is CONTENT, not a zero return: with no tokens the game must parse exactly the line
    // the launcher wrote, with no trailing space and nothing appended.
    const char* original = "echovr.exe -region usc -offline";
    char buffer[128];
    memset(buffer, 0x7F, sizeof buffer);
    const size_t n =
        replay::session::AugmentCommandLine(original, nullptr, 0, buffer, sizeof(buffer));
    CHECK(n == strlen(original));
    CHECK(std::string(buffer) == std::string(original));
    CHECK(buffer[n] == '\0');
}

// ---------------------------------------------------------------------------------------------
// ★ The reporting proof. This is the check that would have caught sweep 4's silence in minutes.
//
// It reproduces the real condition offline: several probe DLLs in one process, each embedding the
// same bootstrap, the first to load winning coexist::ClaimInstance and every later one refused.
// Two copies of the same DLL under different names reproduce that exactly, because the claim is
// keyed on the resolved log path plus the pid, not on the file name.
//
// ★ Precisely what the host reproduces, because it is TWO causes and not one. The log confirms both:
//   - the instance claim refuses every copy after the first ("sharing this folder with ...");
//   - and the BUILD GUARD refuses in this host, because it is not echovr.exe, which stops
//     modreg::InitAll for every copy including the first.
// Either alone is enough to silence a module-init verdict, and both are real sweep conditions - the
// second is what any probe meets on a non-game host. So this proves the wider claim: the static
// initialiser files where the module path cannot, for either reason.
//
// Three assertions, and the third is what makes the first two mean anything:
//   the first-loaded copy files            (the bootstrap path still works)
//   the REFUSED copy files                 (the static initialiser is doing its job)
//   the legacy-shaped mutation goes SILENT  (a control that has only ever agreed proves nothing)
// ---------------------------------------------------------------------------------------------

namespace {

std::string BinDir() {
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof self);
    std::string s(self);
    return s.substr(0, s.find_last_of(0x5C));
}

bool RunHost(const std::string& root, const std::vector<std::string>& dlls) {
    std::string cmd = std::string(1, 0x22) + BinDir() + "\\session_probehost.exe" + std::string(1, 0x22) +
                      " " + std::string(1, 0x22) + root + std::string(1, 0x22);
    for (const std::string& d : dlls) {
        cmd += " " + std::string(1, 0x22) + root + "\\" + d + std::string(1, 0x22);
    }
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    // ★ The host exits non-zero if any LoadLibrary failed. Without checking it, "the mutation filed
    // nothing" would pass just as well when the mutation DLL never loaded at all - the control would
    // be vacuous in exactly the direction that matters.
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

bool Filed(const std::string& root, const std::string& stem) {
    const std::string path = root + "\\" + stem + "\\done.json";
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string ReadVerdict(const std::string& root, const std::string& stem) {
    const std::string path = root + "\\" + stem + "\\done.json";
    FILE* fh = nullptr;
    if (fopen_s(&fh, path.c_str(), "rb") != 0 || fh == nullptr) {
        return std::string();
    }
    char buffer[4096] = {0};
    const size_t n = fread(buffer, 1, sizeof buffer - 1, fh);
    fclose(fh);
    return std::string(buffer, n);
}

// How many entry points the verdict NAMES, and how many it says are live. They are one fact reported
// twice, and the names used to be a literal - four of them, printed beside a measured count of
// fifteen, so a launch that lost `connect` filed a verdict naming `connect` as live. -1 means the
// field is missing, which is a failure and never a pass.
int NamedHooks(const std::string& body) {
    const size_t open = body.find("\"hooks\":[");
    if (open == std::string::npos) {
        return -1;
    }
    const size_t close = body.find(']', open);
    if (close == std::string::npos) {
        return -1;
    }
    int quotes = 0;
    for (size_t i = open + 9; i < close; ++i) {
        if (body[i] == '"') {
            ++quotes;
        }
    }
    return quotes / 2;
}

int LiveHooks(const std::string& body) {
    const size_t at = body.find("\"hooksLive\":");
    if (at == std::string::npos) {
        return -1;
    }
    int live = -1;
    if (sscanf_s(body.c_str() + at + 12, "%d", &live) != 1) {
        return -1;
    }
    return live;
}

}  // namespace

TEST_CASE("the probe files a verdict even when its instance claim is refused") {
    const std::string bin = BinDir();
    char tempRoot[MAX_PATH] = {0};
    GetTempPathA(sizeof tempRoot, tempRoot);
    char unique[MAX_PATH];
    _snprintf_s(unique, sizeof unique, _TRUNCATE, "%sw1f_reporting_%lu", tempRoot,
                GetCurrentProcessId());
    CreateDirectoryA(unique, nullptr);
    const std::string root(unique);

    // aaa sorts first, so it wins the claim; probe_session is then the REFUSED one, which is the
    // position it always occupies in a real sweep. zzz is the mutation, refused as well.
    REQUIRE(CopyFileA((bin + "\\probe_session.dll").c_str(),
                      (root + "\\probe_aaa_first.dll").c_str(), FALSE) != 0);
    REQUIRE(CopyFileA((bin + "\\probe_session.dll").c_str(),
                      (root + "\\probe_session.dll").c_str(), FALSE) != 0);
    REQUIRE(CopyFileA((bin + "\\probe_session_nofiler.dll").c_str(),
                      (root + "\\probe_zzz_nofiler.dll").c_str(), FALSE) != 0);

    // REQUIRE, not CHECK: a host that did not load all three proves nothing either way.
    REQUIRE_MESSAGE(RunHost(root, {"probe_aaa_first.dll", "probe_session.dll",
                                   "probe_zzz_nofiler.dll"}),
                    "the host failed to load one of the three DLLs, so the results below are void");

    CHECK(Filed(root, "probe_aaa_first"));
    CHECK_MESSAGE(Filed(root, "probe_session"),
                  "the refused copy filed nothing: this is exactly sweep 4");
    CHECK_MESSAGE(!Filed(root, "probe_zzz_nofiler"),
                  "the legacy-shaped mutation filed, so this control cannot detect the defect");

    // ★ The verdict's hook NAMES against its own hook COUNT, in both positions a sweep produces: the
    // copy that won the instance claim and armed, and the copy that was refused and armed nothing.
    // A constant name list passes neither - it names four entry points over a count of ten in the
    // first and over a count of zero in the second.
    for (const char* stem : {"probe_aaa_first", "probe_session"}) {
        const std::string probe(stem);
        CAPTURE(probe);
        const std::string body = ReadVerdict(root, stem);
        REQUIRE_FALSE(body.empty());
        const int named = NamedHooks(body);
        const int live = LiveHooks(body);
        REQUIRE(named >= 0);   // an absent field is a failure, not a quiet pass
        REQUIRE(live >= 0);
        CHECK_MESSAGE(named == live,
                      "the verdict names a different set of entry points than it counts");
    }
}

// ★ The probe's S5/S6/C3 expectation, and the proof that it can still fail on BOTH branches.
//
// A derived expectation that happens to agree on every profile you can run is the same trap as a
// constant one, so every row is exercised twice: once with the flags the rule expects, which must
// score RIGHT, and once with the other value, which must score WRONG. If any negative case ever
// passes, the check has stopped asserting anything.
//
// The rows are the sites boot_flag_expectation.h names: -level beside EVERY switch that ORs the
// boot-to-multiplayer bit, the switches that clear the online bit and the two that do not, and a
// near-miss for each setter - a prefix or substring match would put the near-miss on the setter's
// branch and would read the removal of a table row as harmless.
TEST_CASE("the boot-flag expectation is derived from all of the launch's switches") {
    // Measured in the sweep: `bare` 0x42800000 / 0x1, `bare-level` 0x40800000 / 0x0. Bit 25 is the
    // only difference in modeFlags and bit 0 the only one in sessionFlags, so the same two values
    // serve every row: what the engine writes for "set" and for "clear" does not depend on which
    // handler wrote it.
    const uint64_t kModeSet = 0x42800000ull;
    const uint64_t kModeClear = 0x40800000ull;
    const uint64_t kOnlineSet = 0x1ull;
    const uint64_t kOnlineClear = 0x0ull;

    struct Row {
        const wchar_t* commandLine;
        bool           bootMp;   // expected value of CR15Game+0x7AE0 bit 25
        bool           online;   // expected value of CR15Game+0x8A70 bit 0
        const char*    why;
        // ★ The token that voids this line, folded as the engine would look it up, or nullptr when
        // the whole line parses. On a voided line the two columns above are still checked - they are
        // the TOKENIZER model, and pinning that `-region=usc` is not `-region` is the point of the
        // near-miss rows - but no BIT verdict may be reached: ApplyCommandLine returned at
        // 0x140116739 and neither handler ran. AUDIT_W1-F_R4 F2.
        const wchar_t* voidedBy = nullptr;
    };
    const Row rows[] = {
        // the two measured ends
        {L"echovr.exe", true, true, "bare: the no -level default sets both"},
        {L"echovr.exe -level mpl_arena_a -gametype echo_arena", false, false, "bare-level"},
        // -level beside each setter of the boot-to-multiplayer bit
        {L"echovr.exe -level mpl_arena_a -mp", true, false, "-mp ORs 0x2000000 and clears online"},
        {L"echovr.exe -level mpl_arena_a -mpappid 1234", true, false, "-mpappid ORs, leaves online"},
        {L"echovr.exe -level mpl_arena_a -spectatorstream echo_arena", true, false, "0x2100000"},
        {L"echovr.exe -level mpl_arena_a -moderator", true, false, "moderator block"},
        {L"echovr.exe -level mpl_arena_a -moderateuser 5", true, false, "moderator block"},
        {L"echovr.exe -level mpl_arena_a -moderategroup 5", true, false, "moderator block"},
        {L"echovr.exe -level mpl_arena_a -lobbyid 0000-1111", true, false, "-lobbyid"},
        {L"echovr.exe -level mpl_arena_a -region usc", true, false, "-region"},
        // the profiles wave 2 runs, as the test harness spelled them
        {L"echovr.exe -region usc -offline -windowed -level mpl_arena_a -gametype echo_arena "
         L"-loginhost 127.0.0.1 -confighost 127.0.0.1 -radserverdbhost 127.0.0.1 "
         L"-servermanagerhost 127.0.0.1",
         true, false, "offline-local: -region sets boot-MP and clears online, -level clears neither"},
        // ★ replay-local, as the design notes and HC-1c (§4.0) now define it: its argument vector is
        // offline-local's, ELEMENT-WISE IDENTICAL, with no token of ours on the engine's line ever.
        // The capture to open travels in the child's environment as ECHOREPLAY_OPEN (a2ad110), which
        // no engine parser reads - so it cannot appear here, and its absence is the point rather than
        // an omission. The line below is therefore the offline-local line, and it must score the
        // same: if these two rows ever disagree, the profile stopped being a copy.
        {L"echovr.exe -region usc -offline -windowed -level mpl_arena_a -gametype echo_arena "
         L"-loginhost 127.0.0.1 -confighost 127.0.0.1 -radserverdbhost 127.0.0.1 "
         L"-servermanagerhost 127.0.0.1",
         true, false, "replay-local: offline-local element-wise; ECHOREPLAY_OPEN is in the child Env"},
        {L"echovr.exe -region usc -offline -windowed -level mpl_arena_a -gametype echo_arena "
         L"-loginhost 127.0.0.1 -confighost 127.0.0.1 -radserverdbhost 127.0.0.1 "
         L"-servermanagerhost 127.0.0.1",
         true, false, "replay-local-bare: the same vector again, its Env carries a directory"},
        {L"echovr.exe -noovr -spectatorstream echo_arena", true, false,
         "flat: no -level, and -spectatorstream clears online by a register RMW at 0x140116F58"},
        // each clearer WITHOUT -level: the default sets both, and only the clearer can take online
        // back. With -level beside it the clearer is unobservable (the default never set the bit).
        {L"echovr.exe -mp", true, false, "-mp alone clears online"},
        {L"echovr.exe -spectatorstream echo_arena", true, false, "-spectatorstream alone clears online"},
        {L"echovr.exe -moderator", true, false, "-moderator alone clears online"},
        {L"echovr.exe -moderateuser 5", true, false, "-moderateuser alone clears online"},
        {L"echovr.exe -moderategroup 5", true, false, "-moderategroup alone clears online"},
        {L"echovr.exe -lobbyid 0000-1111", true, false, "-lobbyid alone clears online"},
        {L"echovr.exe -region usc", true, false, "-region alone clears online"},
        // the engine's tokenizer strips any leading run of - + or the en dash, and has no = or :
        // form: the name is the whole token
        {L"echovr.exe --region usc", true, false, "--region is -region to the engine"},
        {L"echovr.exe +region usc", true, false, "+region is -region to the engine"},
        {L"echovr.exe \x2013region usc", true, false, "en-dash region is -region to the engine"},
        {L"echovr.exe -+-region usc", true, false, "any leading run of lead characters"},
        {L"echovr.exe -region=usc", true, true, "-region=usc is an unknown switch: nothing clears", L"region=usc"},
        {L"echovr.exe -region:usc", true, true, "-region:usc is an unknown switch: nothing clears", L"region:usc"},
        {L"echovr.exe -level=mpl_arena_a", true, true, "-level=x is not -level: the default runs", L"level=mpl_arena_a"},
        {L"echovr.exe --level mpl_arena_a", false, false, "--level is -level to the engine"},
        // switch order does not matter: the handlers run in code order
        {L"echovr.exe -region usc -level mpl_arena_a", true, false, "setter before -level"},
        {L"echovr.exe -mpappid 1 -level mpl_arena_a", true, false,
         "-mpappid does not clear online, but -level means the default never set it"},
        {L"echovr.exe -mpappid 1", true, true, "no -level: the default sets online and -mpappid keeps it"},
        // near-misses: each setter's spelling, extended, and the two -level look-alikes
        {L"echovr.exe -level mpl_arena_a -mpx", false, false, "-mpx is not -mp", L"mpx"},
        {L"echovr.exe -level mpl_arena_a -mpappids 1", false, false, "not -mpappid", L"mpappids"},
        {L"echovr.exe -level mpl_arena_a -spectatorstreams", false, false, "not -spectatorstream", L"spectatorstreams"},
        {L"echovr.exe -level mpl_arena_a -moderators", false, false, "not -moderator", L"moderators"},
        {L"echovr.exe -level mpl_arena_a -moderateusers 5", false, false, "not -moderateuser", L"moderateusers"},
        {L"echovr.exe -level mpl_arena_a -moderategroups 5", false, false, "not -moderategroup", L"moderategroups"},
        {L"echovr.exe -level mpl_arena_a -lobbyids 1", false, false, "not -lobbyid", L"lobbyids"},
        {L"echovr.exe -level mpl_arena_a -regions usc", false, false, "not -region", L"regions"},
        {L"echovr.exe -level mpl_arena_a -xregion usc", false, false, "not -region either", L"xregion"},
        {L"echovr.exe -levelup -region usc", true, false, "-levelup is not -level; -region sets", L"levelup"},
        {L"echovr.exe -nolevel", true, true, "-nolevel is not -level", L"nolevel"},
        // the value of a switch is not a switch
        {L"echovr.exe -level mpl_arena_a -gametype C:\\-region", false, false,
         "a switch VALUE that begins with a lead character is still a value, not -region"},
    };

    using V = w1f::BitVerdict;
    for (const Row& r : rows) {
        INFO(std::string(r.why));
        CHECK_MESSAGE(w1f::BootMpBitExpectedSet(r.commandLine) == r.bootMp, "boot-MP expectation");
        CHECK_MESSAGE(w1f::OnlineBitExpectedSet(r.commandLine) == r.online, "online expectation");
        // the rule scores the expected value RIGHT and the other value WRONG
        // which token voids the line, if any - asserted by NAME, so a row cannot be voided by a
        // different token than the one it is about
        wchar_t offender[w1f::kTokenCap];
        const bool voided = w1f::FirstUnregisteredSwitch(r.commandLine, offender, w1f::kTokenCap);
        CHECK_MESSAGE(voided == (r.voidedBy != nullptr), "voided-line expectation");
        if (voided) {
            CHECK(std::wstring(offender) == std::wstring(r.voidedBy != nullptr ? r.voidedBy : L""));
            // no bit verdict on a voided line, whichever way the bits happen to read
            CHECK(w1f::BootMpBitVerdict(kModeSet, r.commandLine) == V::LineVoided);
            CHECK(w1f::BootMpBitVerdict(kModeClear, r.commandLine) == V::LineVoided);
            CHECK(w1f::OnlineBitVerdict(kOnlineSet, r.commandLine) == V::LineVoided);
            CHECK(w1f::OnlineBitVerdict(kOnlineClear, r.commandLine) == V::LineVoided);
            continue;
        }
        // the rule scores the expected value RIGHT and the other value WRONG
        CHECK(w1f::BootMpBitVerdict(r.bootMp ? kModeSet : kModeClear, r.commandLine) == V::AsExpected);
        CHECK(w1f::BootMpBitVerdict(r.bootMp ? kModeClear : kModeSet, r.commandLine) == V::NotAsExpected);
        CHECK(w1f::OnlineBitVerdict(r.online ? kOnlineSet : kOnlineClear, r.commandLine) == V::AsExpected);
        CHECK(w1f::OnlineBitVerdict(r.online ? kOnlineClear : kOnlineSet, r.commandLine) == V::NotAsExpected);
    }

    SUBCASE("the switch that decided is the one the verdict names") {
        // a null name is a red CHECK, not a crash in std::wstring's constructor
        auto name = [](const wchar_t* s) { return s != nullptr ? std::wstring(s) : std::wstring(L"<null>"); };
        CHECK(w1f::BootMpSetterGiven(L"echovr.exe -level x") == nullptr);
        CHECK(name(w1f::BootMpSetterGiven(L"echovr.exe -level x -REGION usc")) == L"-region");
        CHECK(w1f::OnlineClearerGiven(L"echovr.exe -mpappid 1") == nullptr);
        CHECK(name(w1f::OnlineClearerGiven(L"echovr.exe -lobbyid 1")) == L"-lobbyid");
        CHECK(name(w1f::OnlineClearerGiven(L"echovr.exe -spectatorstream echo_arena")) == L"-spectatorstream");
    }

    SUBCASE("every table row is a setter, and every setter is in the table") {
        // The table is the model. A row that sets neither bit would be dead, and the counts pin the
        // sites boot_flag_expectation.h names: seven boot-MP sites and five online clears, with the
        // three moderator names sharing one site each. -mpappid is the only setter that leaves
        // online alone, so it is the only row allowed to say so.
        size_t setters = 0;
        size_t clearers = 0;
        for (const w1f::FlagSwitch& s : w1f::kFlagSwitches) {
            CHECK(s.setsBootMp);
            if (s.setsBootMp) ++setters;
            if (s.clearsOnline) ++clearers;
            if (!s.clearsOnline) CHECK(std::wstring(s.name) == L"-mpappid");
        }
        CHECK(setters == 8);
        CHECK(clearers == 7);
    }

    SUBCASE("switch recognition is the engine's, not the bootstrap's") {
        CHECK(w1f::EngineSwitchGiven(L"echovr.exe --region usc", L"-region"));
        CHECK(w1f::EngineSwitchGiven(L"echovr.exe +region usc", L"-region"));
        CHECK(w1f::EngineSwitchGiven(L"echovr.exe \x2013region usc", L"-region"));
        CHECK(w1f::EngineSwitchGiven(L"echovr.exe -REGION usc", L"-region"));
        CHECK(w1f::EngineSwitchGiven(L"echovr.exe \"-region\" usc", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"echovr.exe -region=usc", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"echovr.exe -region:usc", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"echovr.exe -regions usc", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"echovr.exe -xregion usc", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"echovr.exe region usc", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"echovr.exe -logpath C:\\-region", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"-region", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(L"\"C:\\a b\\-region.exe\" x", L"-region"));
        CHECK_FALSE(w1f::EngineSwitchGiven(nullptr, L"-region"));
        // a token longer than kTokenCap is cut, and a cut token is 63 characters - longer than every
        // name here, so a cut can never produce a match. The `truncated` flag that
        // used to guard this was dead code (removing it left the suite green); the invariant is now a
        // static_assert in the header, and this row measures the behaviour it protects.
        std::wstring longToken(L"echovr.exe -region");
        longToken.append(200, L'x');
        CHECK_FALSE(w1f::EngineSwitchGiven(longToken.c_str(), L"-region"));
        // and the bootstrap's own reading diverges on exactly the forms the engine accepts or
        // refuses, which is why the expectation does not use it
        CHECK_FALSE(replay::bootstrap::HasSwitch(L"echovr.exe --region usc", L"-region"));
        CHECK(replay::bootstrap::HasSwitch(L"echovr.exe -region=usc", L"-region"));
    }
}

// ★★★ AUDIT_W1-F_R4 F2. One token the engine has not registered makes ApplyCommandLine return at
// 0x140116739 with EVERY handler skipped - -offline, -level, -region and all four 127.0.0.1 pins
// silently ignored - so the two bits answer the constructor, not the launch. A probe that scored them
// anyway would print a boot-flag verdict for a launch that was never configured. So the detection runs
// FIRST, it names the token, and it is its own outcome rather than a value of the other two.
TEST_CASE("an unregistered switch voids the line, and that is its own outcome") {
    auto voidedBy = [](const wchar_t* line) {
        wchar_t name[w1f::kTokenCap];
        const bool voided = w1f::FirstUnregisteredSwitch(line, name, w1f::kTokenCap);
        return voided ? std::wstring(name) : std::wstring(L"<none>");
    };
    using V = w1f::BitVerdict;
    const uint64_t kModeSet = 0x42800000ull;
    const uint64_t kModeClear = 0x40800000ull;
    const uint64_t kOnlineSet = 0x1ull;
    const uint64_t kOnlineClear = 0x0ull;

    // The line wave 2 nearly launched: replay-local's vector with -replayviewer appended. Nothing
    // registers it (grep -rn AddArgSyntax src/ is empty), so this line configures nothing at all.
    const wchar_t* withViewer =
        L"echovr.exe -region usc -offline -windowed -level mpl_arena_a -gametype echo_arena "
        L"-loginhost 127.0.0.1 -confighost 127.0.0.1 -radserverdbhost 127.0.0.1 "
        L"-servermanagerhost 127.0.0.1 -replayviewer D:\\fixture.echoreplay";
    CHECK(w1f::LineVoided(withViewer));
    CHECK(voidedBy(withViewer) == L"replayviewer");
    // ★ and never a boot-flag verdict, whichever way the bits actually read
    CHECK(w1f::BootMpBitVerdict(kModeSet, withViewer) == V::LineVoided);
    CHECK(w1f::BootMpBitVerdict(kModeClear, withViewer) == V::LineVoided);
    CHECK(w1f::OnlineBitVerdict(kOnlineSet, withViewer) == V::LineVoided);
    CHECK(w1f::OnlineBitVerdict(kOnlineClear, withViewer) == V::LineVoided);

    // the engine's own spellings of it are the same token, and so is the capture-seconds switch that
    // The design notes moved into the environment for the same reason
    CHECK(voidedBy(L"echovr.exe -offline --RePlayViewer x") == L"replayviewer");
    CHECK(voidedBy(L"echovr.exe -offline +replayviewer x") == L"replayviewer");
    CHECK(voidedBy(L"echovr.exe -offline -replaycapturesecs 60") == L"replaycapturesecs");
    // the FIRST unknown token is the one that voids: nothing after it is parsed
    CHECK(voidedBy(L"echovr.exe -zzz -replayviewer x") == L"zzz");

    // ★ ANTI-VACUITY, both directions. A set that refused everything would pass every row above while
    // saying nothing, so every sanctioned profile line has to come back clean. These are the argument
    // vectors as the test harness spells them ($CanonicalProfiles), which is the second source for
    // the citation list in boot_flag_expectation.h - if the two ever disagree, this fails.
    const wchar_t* sanctioned[] = {
        L"echovr.exe",
        L"echovr.exe -level mpl_arena_a",
        L"echovr.exe -level mpl_arena_a -gametype echo_arena -loginhost 127.0.0.1 "
        L"-confighost 127.0.0.1 -radserverdbhost 127.0.0.1 -servermanagerhost 127.0.0.1",
        L"echovr.exe -region usc -offline -windowed -level mpl_arena_a -gametype echo_arena "
        L"-loginhost 127.0.0.1 -confighost 127.0.0.1 -radserverdbhost 127.0.0.1 "
        L"-servermanagerhost 127.0.0.1",
        L"echovr.exe -noovr -spectatorstream echo_arena",
        L"echovr.exe -windowed",
    };
    for (const wchar_t* line : sanctioned) {
        INFO(std::string("a sanctioned profile line must parse to the end"));
        CHECK(voidedBy(line) == L"<none>");
    }
    // a value is not a switch, and neither is the program path, however either is spelled
    CHECK(voidedBy(L"\"C:\\Program Files\\ready-at-dawn\\echovr.exe\" -level mpl_arena_a") == L"<none>");
    CHECK(voidedBy(L"echovr.exe -level mpl_arena_a -gametype C:\\out") == L"<none>");
    // \u2605 and the set is a CITATION list, not a census of the engine's registry, so it fails closed:
    // -logpath is very likely a real engine switch and we have no citation for it, so it is reported
    // as voiding the line. Over-refusal is the safe direction - the harness refuses the launch and
    // somebody produces the citation - and the reverse would let an unregistered token onto a line
    // whose four pins would then be ignored.
    CHECK(voidedBy(L"echovr.exe -logpath C:\\out") == L"logpath");
    CHECK(voidedBy(nullptr) == L"<none>");

    // every switch the expectation models must itself be registered, or the model would be scoring a
    // line the engine had already abandoned
    for (const w1f::FlagSwitch& fs : w1f::kFlagSwitches) {
        std::wstring line = L"echovr.exe ";
        line += fs.name;
        INFO(std::string("a kFlagSwitches member must be on the registered list"));
        CHECK_FALSE(w1f::LineVoided(line.c_str()));
    }
}

// S4 and S7 are NOT-ASKED on a launch that carries nothing for them to test. `bare` has no
// argument at all; a quoted program path is not a switch, and neither is a switch value.
TEST_CASE("a launch with no switch is told apart from one with any") {
    CHECK_FALSE(w1f::AnySwitchGiven(L"echovr.exe"));
    CHECK_FALSE(w1f::AnySwitchGiven(L"\"C:\\Program Files\\ready-at-dawn\\echovr.exe\""));
    CHECK_FALSE(w1f::AnySwitchGiven(L"\"C:\\ready-at-dawn\\echovr.exe\" "));
    CHECK_FALSE(w1f::AnySwitchGiven(nullptr));
    CHECK(w1f::AnySwitchGiven(L"echovr.exe -level mpl_arena_a"));
    CHECK(w1f::AnySwitchGiven(L"\"C:\\Program Files\\ready-at-dawn\\echovr.exe\" -noovr"));
    CHECK(w1f::AnySwitchGiven(L"echovr.exe\t-windowed"));
    CHECK(w1f::AnySwitchGiven(L"echovr.exe \"-level\" x"));
}

// ★★★ THE L5 REGRESSION (launch run 20260902-071934Z, diagnosis DIAG-L5).
// SuppressBootToMultiplayer ran unconditionally every tick and cleared CR15Game+0x7AE0 bit
// 0x2000000 on `-region usc -offline -windowed -level mpl_arena_a ...`, where -region sets the bit
// by correct engine behaviour and rlay's -offline patch makes the arm that bit selects the launch's
// ONLY boot path. The game idled in the main menu for 234 s with no login and no level load.
TEST_CASE("the boot-to-multiplayer bit is cleared only for a replay session with no offline host") {
    using replay::bootstrap::SessionMode;
    using replay::session::BootBitPolicy;
    using replay::session::ClassifyBootBitPolicy;

    SUBCASE("-offline outranks every other reading of the launch") {
        // Replay + -offline is not a corner case, it is the L5 profile: ECHOREPLAY_OPEN named the
        // replay directory AND the line carried -offline, so a gate keyed on the mode alone would
        // have cleared the bit again.
        CHECK(ClassifyBootBitPolicy(SessionMode::Replay, true) ==
              BootBitPolicy::LeaveIntactOfflineHost);
        CHECK(ClassifyBootBitPolicy(SessionMode::Offline, true) ==
              BootBitPolicy::LeaveIntactOfflineHost);
        CHECK(ClassifyBootBitPolicy(SessionMode::LiveOnlineMatch, true) ==
              BootBitPolicy::LeaveIntactOfflineHost);
        CHECK(ClassifyBootBitPolicy(SessionMode::RefusedStaleSwitch, true) ==
              BootBitPolicy::LeaveIntactOfflineHost);
        CHECK(ClassifyBootBitPolicy(SessionMode::Unknown, true) ==
              BootBitPolicy::LeaveIntactOfflineHost);
    }

    SUBCASE("a replay request on a line with no -offline is the one case that suppresses") {
        CHECK(ClassifyBootBitPolicy(SessionMode::Replay, false) ==
              BootBitPolicy::SuppressForReplaySession);
        CHECK(ClassifyBootBitPolicy(SessionMode::Offline, false) ==
              BootBitPolicy::LeaveIntactNoReplay);
        CHECK(ClassifyBootBitPolicy(SessionMode::LiveOnlineMatch, false) ==
              BootBitPolicy::LeaveIntactNoReplay);
        CHECK(ClassifyBootBitPolicy(SessionMode::RefusedStaleSwitch, false) ==
              BootBitPolicy::LeaveIntactNoReplay);
        CHECK(ClassifyBootBitPolicy(SessionMode::Unknown, false) ==
              BootBitPolicy::LeaveIntactNoReplay);
    }

    // The whole composition, driven by the readers the shipped code uses, over the exact inputs
    // launch-01 had. This is the retroactive proof: with the gate removed the answer here is
    // SuppressForReplaySession, which is what L5 did.
    SUBCASE("the L5 launch, through the real readers, leaves the bit intact") {
        const wchar_t* kL5 =
            L"\"C:\\Games\\EchoVR\\bin\\win10\\echovr.exe\" -region usc -offline -windowed "
            L"-level mpl_arena_a -gametype echo_arena -loginhost 127.0.0.1 -confighost 127.0.0.1 "
            L"-radserverdbhost 127.0.0.1 -servermanagerhost 127.0.0.1";

        // The bit really is set by this line: that is why clearing it mattered.
        CHECK(w1f::BootMpBitExpectedSet(kL5));
        CHECK(std::wstring(w1f::BootMpSetterGiven(kL5)) == L"-region");
        CHECK(w1f::LevelSwitchGiven(kL5));
        CHECK_FALSE(w1f::LineVoided(kL5));

        const wchar_t* kOpenVar = replay::bootstrap::OpenEnvVarName();
        std::wstring saved;
        const DWORD had = GetEnvironmentVariableW(kOpenVar, nullptr, 0);
        if (had != 0) {
            saved.resize(had);
            saved.resize(GetEnvironmentVariableW(kOpenVar, &saved[0], had));
        }

        REQUIRE(SetEnvironmentVariableW(kOpenVar, L"C:\\Replays") != 0);
        CHECK(replay::bootstrap::OpenRequestPresent());
        const SessionMode mode =
            replay::bootstrap::ClassifySession(kL5, replay::bootstrap::OpenRequestPresent(), true);
        CHECK(mode == SessionMode::Replay);
        const bool offlineSwitchSeen = replay::bootstrap::HasSwitch(kL5, L"-offline");
        CHECK(offlineSwitchSeen);
        CHECK(ClassifyBootBitPolicy(mode, offlineSwitchSeen) ==
              BootBitPolicy::LeaveIntactOfflineHost);

        // Same line without the replay request: still intact, for the other reason.
        REQUIRE(SetEnvironmentVariableW(kOpenVar, nullptr) != 0);
        CHECK_FALSE(replay::bootstrap::OpenRequestPresent());
        const SessionMode plain =
            replay::bootstrap::ClassifySession(kL5, replay::bootstrap::OpenRequestPresent(), true);
        CHECK(plain == SessionMode::Offline);
        CHECK(ClassifyBootBitPolicy(plain, replay::bootstrap::HasSwitch(kL5, L"-offline")) ==
              BootBitPolicy::LeaveIntactOfflineHost);

        // And the case the suppression exists for: a replay request with no offline host.
        const wchar_t* kReplayOnly = L"echovr.exe -level mpl_arena_a -gametype echo_arena";
        REQUIRE(SetEnvironmentVariableW(kOpenVar, L"C:\\Replays\\a.echodemo") != 0);
        CHECK(ClassifyBootBitPolicy(
                  replay::bootstrap::ClassifySession(kReplayOnly,
                                                     replay::bootstrap::OpenRequestPresent(), true),
                  replay::bootstrap::HasSwitch(kReplayOnly, L"-offline")) ==
              BootBitPolicy::SuppressForReplaySession);

        SetEnvironmentVariableW(kOpenVar, saved.empty() ? nullptr : saved.c_str());
    }

    SUBCASE("every policy and every suppression outcome has its own name") {
        CHECK(std::string(replay::session::BootBitPolicyName(
                  BootBitPolicy::LeaveIntactOfflineHost)) == "left-intact-offline-local-host");
        CHECK(std::string(replay::session::BootBitPolicyName(
                  BootBitPolicy::SuppressForReplaySession)) == "cleared-replay-session");
        CHECK(std::string(replay::session::BootBitPolicyName(
                  BootBitPolicy::LeaveIntactNoReplay)) == "left-intact-no-replay-session");
        CHECK(std::string(replay::session::BootSuppressionName(
                  replay::session::BootSuppression::RefusedByPolicy)) == "refused-by-policy");
    }
}

// The gate lives INSIDE the suppressor, so the protection cannot be lost by a caller forgetting it.
// Remove the policy check and the two refusals below fall through to the engine read - which is the
// mutation this case exists to redden.
TEST_CASE("the suppressor refuses on policy before it reads any engine state") {
    using replay::session::BootBitPolicy;
    using replay::session::BootSuppression;
    using replay::session::SuppressBootToMultiplayer;

    const uint32_t before = replay::session::BootSuppressionCount();
    CHECK(SuppressBootToMultiplayer(BootBitPolicy::LeaveIntactOfflineHost) ==
          BootSuppression::RefusedByPolicy);
    CHECK(SuppressBootToMultiplayer(BootBitPolicy::LeaveIntactNoReplay) ==
          BootSuppression::RefusedByPolicy);
    // BACKLOG-1 O5. The allowed policy now meets a SECOND refusal, and it is the process predicate
    // rather than the command line answering the same question in a different scheme. A test binary
    // never installs the offline-bit lock, so without the override this is where it stops.
    CHECK(SuppressBootToMultiplayer(BootBitPolicy::SuppressForReplaySession) ==
          BootSuppression::RefusedUncontained);
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
    CHECK(SuppressBootToMultiplayer(BootBitPolicy::SuppressForReplaySession) ==
          BootSuppression::RefusedUncontained);

    // Not a CR15Game assertion: this host has no engine, so the only claim made is that the allowed
    // policy in a CONTAINED process gets PAST both gates the cases above are stopped by. Without
    // this arm every check here would pass against a suppressor that refused unconditionally.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    const BootSuppression contained =
        SuppressBootToMultiplayer(BootBitPolicy::SuppressForReplaySession);
    CHECK(contained != BootSuppression::RefusedByPolicy);
    CHECK(contained != BootSuppression::RefusedUncontained);
    replay::livesafety::ResetForTest();
    CHECK(replay::session::BootSuppressionCount() == before);
}

// ---------------------------------------------------------------------------------------------
// L3 F1: entering a replay before the session is in a level destroys the session.
//
// Launch test L3, S5 F1, from the plugin log and the engine's own [r14(client)] log:
//
//     18:06:33.621 [session] NetGame state 0 (logged out) -> 1 (loading root)
//     18:06:33.636 [control] open request submitted (attempt 1): load accepted
//     18:06:33.652 [session] entering offline match: level 0x576ED3F8428EBC4B ...
//     18:06:35.340 [session] NetGame state 1 (loading root) -> 2 (logging in)
//     18:06:35.371 [session] NetGame state 2 (logging in) -> -94 (login failed)
//     [18:06:35]: [LOGIN] Log in request failed: One or more prerequisites are missing
//     [18:06:35]: [NETGAME] Remaining in state (login failed) after leaving game
//
// The stock call at ledger row 0.3 tears the live session down and builds a new one. Run at
// state 1 it took away the half-built session the engine's login was about to use, the login
// reported its prerequisites missing, and -94 was terminal for the remaining 4.5 minutes.
// ---------------------------------------------------------------------------------------------

// Two Opens in one frame. The second cycle request arrives after the first
// entry was performed and before NetGame has left state 9; it must fold into the first.
TEST_CASE("a level cycle request folds into a performed entry that has not left the level yet") {
    using replay::session::CycleRequest;
    using replay::session::CycleRequestVerdict;
    using replay::session::kCycleLeaveGraceMs;
    const uint64_t t0 = 100000;

    CHECK(CycleRequestVerdict(false, 0, t0, true) == CycleRequest::Queue);        // the first Open
    CHECK(CycleRequestVerdict(true, 0, t0, true) == CycleRequest::InFlight);      // same tick
    CHECK(CycleRequestVerdict(false, t0, t0 + 16, true) == CycleRequest::InFlight);  // next frame
    CHECK(CycleRequestVerdict(false, t0, t0 + kCycleLeaveGraceMs - 1, true) ==
          CycleRequest::InFlight);
    // Out of the level the latch is cleared by the pump; back in, a new Open cycles again.
    CHECK(CycleRequestVerdict(false, 0, t0 + 4000, false) == CycleRequest::NotInLevel);
    CHECK(CycleRequestVerdict(false, 0, t0 + 9000, true) == CycleRequest::Queue);
    // An entry that never left must not swallow every later request.
    CHECK(CycleRequestVerdict(false, t0, t0 + kCycleLeaveGraceMs, true) == CycleRequest::Queue);
}

TEST_CASE("an entry request is refused by name until the session is in a level") {
    using replay::EntryRefusal;
    using replay::EntryRefusalName;
    using replay::session::EntryVerdict;

    SUBCASE("every refusal has its own name") {
        CHECK(std::string(EntryRefusalName(EntryRefusal::NotAsked)) == "not-asked");
        CHECK(std::string(EntryRefusalName(EntryRefusal::Accepted)) == "accepted");
        CHECK(std::string(EntryRefusalName(EntryRefusal::NotInLevelYet)) == "not-in-level-yet");
        CHECK(std::string(EntryRefusalName(EntryRefusal::CannotEnter)) == "cannot-enter");
    }

    // The whole state table session.cpp prints, not a sample of it: 7/8/9 enter, everything else
    // waits. Delete the state term from EntryVerdict and every row below 7 turns Accepted.
    SUBCASE("the whole state table is sorted into enter and wait") {
        const int32_t states[] = {-100, -99, -98, -97, -96, -95, -94, -93, -92, -91,
                                  0,    1,   2,   3,   4,   5,   6,   7,   8,   9};
        for (int32_t state : states) {
            const EntryRefusal verdict = EntryVerdict(true, true, true, false, state);
            const bool inLevel = state == 7 || state == 8 || state == 9;
            CAPTURE(state);
            const std::string named = replay::session::StateName(state);
            CAPTURE(named);
            CHECK(verdict == (inLevel ? EntryRefusal::Accepted : EntryRefusal::NotInLevelYet));
        }
    }

    // The three states L3 actually visited, and the two it never reached.
    SUBCASE("the states L3 visited are all refused and the ones it never reached are not") {
        CHECK(EntryVerdict(true, true, true, false, 1) == EntryRefusal::NotInLevelYet);
        CHECK(EntryVerdict(true, true, true, false, 2) == EntryRefusal::NotInLevelYet);
        CHECK(EntryVerdict(true, true, true, false, -94) == EntryRefusal::NotInLevelYet);
        CHECK(EntryVerdict(true, true, true, false, 7) == EntryRefusal::Accepted);
        CHECK(EntryVerdict(true, true, true, false, 9) == EntryRefusal::Accepted);
    }

    // cannot-enter is a different word from not-in-level-yet, and it wins even at state 9: a
    // caller polling a switched-off strategy must be told to stop, not to keep asking.
    SUBCASE("a strategy that cannot enter at all says so instead of saying not yet") {
        CHECK(EntryVerdict(false, true, true, false, 9) == EntryRefusal::CannotEnter);
        CHECK(EntryVerdict(true, false, true, false, 9) == EntryRefusal::CannotEnter);
        CHECK(EntryVerdict(true, true, false, false, 9) == EntryRefusal::CannotEnter);
        CHECK(EntryVerdict(false, true, true, false, 1) == EntryRefusal::CannotEnter);
    }

    // The caller's Matches() short-circuit is answered on a loaded level and nowhere else. If it
    // could bypass the precondition a stale Matches() would re-open the L3 hole.
    SUBCASE("already-there does not make a state below the level enterable") {
        CHECK(EntryVerdict(true, true, true, true, 9) == EntryRefusal::Accepted);
        CHECK(EntryVerdict(true, true, true, true, 1) == EntryRefusal::NotInLevelYet);
    }

    // The recorded sequence, replayed in order through the same function the entry point asks,
    // with the healthy tail L2 and L5 reached and L3 never did. The assertion is the ORDER: every
    // ask before the level waits, and the first acceptance is the first in-level state.
    SUBCASE("the L3 sequence refuses throughout and accepts once a level exists") {
        const int32_t recorded[] = {0, 1, 2, -94, 3, 4, 5, 6, 7, 8, 9};
        const int count = static_cast<int>(sizeof(recorded) / sizeof(recorded[0]));
        int firstAccept = -1;
        for (int i = 0; i < count; ++i) {
            const EntryRefusal verdict = EntryVerdict(true, true, true, false, recorded[i]);
            if (verdict == EntryRefusal::Accepted) {
                firstAccept = i;
                break;
            }
            CAPTURE(recorded[i]);
            CHECK(verdict == EntryRefusal::NotInLevelYet);
        }
        REQUIRE(firstAccept != -1);
        CHECK(recorded[firstAccept] == 7);
        // The moment L3 asked was index 1 (state 1 "loading root") and it is a refusal.
        CHECK(EntryVerdict(true, true, true, false, recorded[1]) == EntryRefusal::NotInLevelYet);
    }
}

// ENG-GAMETYPE-ENABLE (F2): the substitution predicate that decides whether a recorded gametype must
// be replaced before it seeds CR15Game. The manifest widens the engine's 32-bit gametype, and a
// spectator capture records the all-ones sentinel; neither can be a recognized full-64-bit CMatSym
// hash, and writing it verbatim leaves the inner replayed session's game-type node unrecognized (the
// crash's upstream cause). Drop the sentinel term or the high-half term and one of these rows flips.
TEST_CASE("a recorded gametype that cannot be a recognized hash is marked for override") {
    using replay::session::NeedsGametypeOverride;

    // The exact value the crashing capture carried, plus the raw sentinels and a truncated real hash.
    CHECK(NeedsGametypeOverride(0x00000000FFFFFFFFull));   // widened 0xFFFFFFFF "unassigned"  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    CHECK(NeedsGametypeOverride(~0ull));                    // all-ones sentinel
    CHECK(NeedsGametypeOverride(0x000000007E1CAF73ull));    // a real gametype truncated to 32 bits
    CHECK(NeedsGametypeOverride(0x0000000000000001ull));    // any non-zero value with a clear high half

    // A recognized full-64-bit CMatSym hash passes through untouched, and 0 stays the wildcard.
    CHECK_FALSE(NeedsGametypeOverride(0xCB60A4DE7E1CAF73ull));   // echo_arena, recognized
    CHECK_FALSE(NeedsGametypeOverride(0x888AC0CFD102345Full));   // echo_arena_practice_ai, recognized
    CHECK_FALSE(NeedsGametypeOverride(0ull));                    // wildcard: leave the command-line value
}

// The gate lives INSIDE RequestEntry so no caller can lose it, and a refusal must perform
// NOTHING: no stock call, and no latch for the tick to perform later. This host has no CR15Game,
// so the refusal it reaches is cannot-enter; the claim made here is not about which word but
// about what a refused request leaves behind, which is the half that killed L3.
TEST_CASE("a refused entry request calls nothing and latches nothing") {
    replay::ISessionEntry* entry = replay::session::Interface();
    REQUIRE(entry != nullptr);

    // Nothing in this binary has asked yet, and "nobody asked" is not "the answer was accepted".
    CHECK(replay::session::LastEntryRefusal() == replay::EntryRefusal::NotAsked);

    const uint32_t before = replay::session::EntryAttemptCount();
    const uint64_t level = replay::session::HashName("mpl_arena_a");
    const uint64_t gametype = replay::session::HashName("echo_arena");

    CHECK_FALSE(entry->RequestEntry(level, gametype));
    CHECK(replay::session::LastEntryRefusal() != replay::EntryRefusal::Accepted);
    CHECK(entry->LastRefusal() == replay::session::LastEntryRefusal());
    CHECK_FALSE(replay::session::EntryRequestPending());
    CHECK(replay::session::EntryAttemptCount() == before);
    CHECK(std::string(entry->FailureReason()).find("accepted") == std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// The guarded load must refuse a GUARD PAGE, which is what replay::Readable did by name
// (buildguard.cpp:83, `(info.Protect & PAGE_GUARD) != 0`) and what the __try that replaced it did not.
//
// This case is unlike every other one in this file: a filter that does not take the code does not
// make an assertion fail, it leaves the exception UNHANDLED on this thread and the process dies. So
// the assertion is really "the run got here at all", and the checks below exist to prove the fixture
// armed a real guard page rather than a page that was never going to fault. On the game thread that
// death is the owner's match ending, and every thread stack in echovr.exe ends in a guard page.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a guard page is refused by the safe read, not left to the thread") {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uint8_t* page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(page != nullptr);
    *page = 0x5A;

    // The control: an ordinary committed page reads, so a false below is the guard and not the probe
    // refusing everything.
    CHECK(replay::session::ProbeReadableByte(page));

    DWORD old = 0;
    REQUIRE(VirtualProtect(page, si.dwPageSize, PAGE_READWRITE | PAGE_GUARD, &old) != 0);
    MEMORY_BASIC_INFORMATION mbi{};
    REQUIRE(VirtualQuery(page, &mbi, sizeof mbi) == sizeof mbi);
    REQUIRE((mbi.Protect & PAGE_GUARD) != 0);   // the fixture really is armed

    // 0x80000001, which is neither EXCEPTION_ACCESS_VIOLATION nor EXCEPTION_IN_PAGE_ERROR.
    CHECK_FALSE(replay::session::ProbeReadableByte(page));

    // The load CONSUMED the guard on its way past -- which is both the proof that the fault fired and
    // the reason refusing is the right answer: the page is ordinary now and a second read of it says
    // nothing about the page the caller asked about.
    MEMORY_BASIC_INFORMATION after{};
    REQUIRE(VirtualQuery(page, &after, sizeof after) == sizeof after);
    CHECK((after.Protect & PAGE_GUARD) == 0);
    CHECK(replay::session::ProbeReadableByte(page));

    // ...and the two codes that were already covered still are: a released reservation is an access
    // violation, and it must refuse rather than crash for the same reason.
    REQUIRE(VirtualFree(page, 0, MEM_RELEASE) != 0);
    CHECK_FALSE(replay::session::ProbeReadableByte(page));
    CHECK_FALSE(replay::session::ProbeReadableByte(nullptr));
}
