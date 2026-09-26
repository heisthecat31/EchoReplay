// W1-F. What the engine's boot does to the two mode bits, derived from the launch's own switches
// instead of fixed once for every profile.
//
// ApplyCommandLine (sub_140116720) in the shipping echovr.exe (md5 caa05ee3bca2f5df0809994e93e2cc05)
// writes the two bits at exactly these sites.
//
// ★ THE CENSUS METHOD, stated as what it actually is. It has TWO halves and the
// first alone is what let two rounds of this defect through:
//   1. A DISPLACEMENT-TEXT census - every instruction in the image whose disassembly names the field
//      as a displacement (instructions.disasm LIKE '%7AE0%' / '%8A70%', image-wide), each classified.
//      This is a scan by FORM. It cannot see a write made through a register that already holds the
//      address, which is exactly what AUDIT_W1-F_R3 caught the byte scan missing.
//   2. An ADDRESS-ESCAPE check on every lea/add in that first list that FORMS the field's address,
//      then a census of every write through the register it loads. Without this half, step 1 is a
//      form scan wearing an effect scan's name.
// Run over sub_140116720 (1648 instructions, func_va query on the same DB), half 2 finds exactly one
// address escape - `0x140117F49 lea rbx,[rdi+7AE0h]` - and seven register-form writes through it:
//   0x140117F60 or [rbx],200000h    0x140117FC2 or [rbx],400000h    0x140117FE3-FE8 bts rcx,17h;
//   mov [rbx],rcx (0x800000)        0x140118088 or [rbx],r15        0x1401180DF or [rbx],r15
//   0x14011813F or [rbx],20000h     0x140118193 or [rbx],4000000h
// None of them carries bit 25: the four immediates are 0x200000, 0x400000, 0x20000 and 0x4000000,
// the bts sets bit 23, and r15's every definition in this function is 0x80 (0x140116BEA), 0
// (0x140116ED2, 0x140116EEB), 4 (0x14011725F) or the token length returned at 0x140116E0B - a value
// that would have to exceed 33 MB to reach bit 25. So the seven-setter count below survives half 2.
// There is NO lea/add forming +0x8A70 anywhere in the function, so the five clears are the whole set.
//
// The counts below are the totals AFTER both halves: seven boot-MP setters (all displacement-form,
// none of the seven register-form writes joins them) and one set + five clears of the online bit.
//
//   CR15Game+0x7AE0 |= 0x2000000 (boot into multiplayer), seven sites and no eighth:
//     0x140116CDF the no -level default   0x140116D3F -mp        0x140116D9F -mpappid
//     0x140116F4D -spectatorstream (0x2100000)                   0x140117690 -lobbyid
//     0x140117727 -region                 0x14011726A the moderator block, guarded by a flag the
//                                         -moderator/-moderateuser/-moderategroup handlers set
//   CR15Game+0x8A70 bit 0 (online): ONE set site, the no -level default at 0x140116CEA, and FIVE
//     clears: 0x140116D4A -mp, 0x140117275 moderator block, 0x14011769F -lobbyid, 0x140117736
//     -region (all `and [rdi+8A70h],~1`), and -spectatorstream at 0x140116F43-F60 as
//     `mov rax,[rdi+8A70h]; and rax,~1; or rax,20h; mov [rdi+8A70h],rax` (clears bit 0, sets 0x20).
//     -mpappid is the only setter that leaves bit 0 alone. The other +0x8A70 writers in the image
//     (0x1400F6493 ctor store, 0x1401042F8/0x140104314 `and ~0x42`, 0x1400FE0C3/0x1400FE164 `or 0x40`)
//     never touch bit 0.
//
// The handlers are one straight if-chain in address order - the only backward jumps in the function
// are the string-strip loops - so the clears always run after the one set, and the order of the
// switches on the command line does not matter. The sweep measured the two ends: `bare` reports
// 0x42800000 / 0x1, `bare-level` 0x40800000 / 0x0.
//
// The command line is the input on purpose: the same probe DLL is loaded into every profile, so the
// only thing that can tell the cases apart is what the launch itself asked for. The old form keyed on
// -level alone and scored `-level -region` (offline-local, and every replay-local built from it) as a
// FAIL for correct engine behaviour.
//
// Switch recognition follows the ENGINE's tokenizer (cmainargsyntax.cpp, sub_1400DA830), because it is
// the engine's reading of the line that decides the bits: a token is a switch when its first character
// is `-`, `+` or the en dash (0x96 in the ANSI line, U+2013 in the W one); the whole leading run of
// those is stripped; the rest is compared case-folded as ONE token. `--region`, `+region` and `-REGION`
// are -region; `-region=usc` and `-region:usc` are a switch named `region=usc` that the engine does not
// know, so they set nothing. The bootstrap's HasSwitch treats `=` and `:` as boundaries and does not
// strip `+` or a second `-`; it is not used here for that reason.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "engine/session_facts.h"

namespace w1f {

struct FlagSwitch {
    const wchar_t* name;
    bool           setsBootMp;
    bool           clearsOnline;
};

inline constexpr FlagSwitch kFlagSwitches[] = {
    {L"-mp", true, true},
    {L"-mpappid", true, false},
    {L"-spectatorstream", true, true},
    {L"-moderator", true, true},
    {L"-moderateuser", true, true},
    {L"-moderategroup", true, true},
    {L"-lobbyid", true, true},
    {L"-region", true, true},
};

inline bool IsSwitchLead(wchar_t c) {
    return c == L'-' || c == L'+' || c == 0x2013 || c == 0x96;
}

inline wchar_t FoldAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
}

// The token buffer every reader here uses. It must stay longer than the longest name any of them
// compares against, because a token that overflows it is CUT rather than flagged: at 64 the cut token
// is 63 characters and no registered name is close, so a cut can never produce a false match. That
// invariant is asserted below rather than commented, which is what AUDIT_W1-F_R4 F4's dead `truncated`
// flag was standing in for - the flag could not change an outcome and removing it left the suite green.
inline constexpr size_t kTokenCap = 64;

// One argv token from `*cursor`, quotes removed, copied into `out`. Returns false at the end of the
// line. A token longer than `cap` is cut; see kTokenCap for why that is safe.
inline bool NextToken(const wchar_t** cursor, wchar_t* out, size_t cap) {
    const wchar_t* p = *cursor;
    while (*p == L' ' || *p == L'\t') {
        ++p;
    }
    if (*p == 0) {
        *cursor = p;
        return false;
    }
    size_t n = 0;
    bool quoted = false;
    for (; *p != 0; ++p) {
        if (*p == L'"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && (*p == L' ' || *p == L'\t')) {
            break;
        }
        if (n + 1 < cap) {
            out[n++] = *p;
        }
    }
    out[n] = 0;
    *cursor = p;
    return true;
}

// The engine's rule for one token against one registered name, both with their lead run stripped.
inline bool TokenIsSwitch(const wchar_t* token, const wchar_t* name) {
    if (!IsSwitchLead(*token)) {
        return false;
    }
    while (IsSwitchLead(*token)) {
        ++token;
    }
    while (IsSwitchLead(*name)) {
        ++name;
    }
    for (; *token != 0 && *name != 0; ++token, ++name) {
        if (FoldAscii(*token) != FoldAscii(*name)) {
            return false;
        }
    }
    return *token == 0 && *name == 0;
}

// Whether the engine's parser would recognise `name` on this line. The first token is the program
// path and is never a switch, whatever it starts with.
inline bool EngineSwitchGiven(const wchar_t* commandLine, const wchar_t* name) {
    if (commandLine == nullptr) {
        return false;
    }
    const wchar_t* cursor = commandLine;
    wchar_t token[kTokenCap];
    if (!NextToken(&cursor, token, kTokenCap)) {
        return false;
    }
    while (NextToken(&cursor, token, kTokenCap)) {
        if (TokenIsSwitch(token, name)) {
            return true;
        }
    }
    return false;
}

inline bool LevelSwitchGiven(const wchar_t* commandLine) {
    return EngineSwitchGiven(commandLine, L"-level");
}

inline const wchar_t* BootMpSetterGiven(const wchar_t* commandLine) {
    for (const FlagSwitch& s : kFlagSwitches) {
        if (s.setsBootMp && EngineSwitchGiven(commandLine, s.name)) {
            return s.name;
        }
    }
    return nullptr;
}

inline const wchar_t* OnlineClearerGiven(const wchar_t* commandLine) {
    for (const FlagSwitch& s : kFlagSwitches) {
        if (s.clearsOnline && EngineSwitchGiven(commandLine, s.name)) {
            return s.name;
        }
    }
    return nullptr;
}

// ★★★ AN UNREGISTERED SWITCH VOIDS THE WHOLE LINE.
// sub_1400DA830 LABEL_15 finds the syntax object's +0x38 zero (the ctor sub_1400D1C00 writes it at
// 0x1400D1C4B and nothing on the registration path writes it again), so an unknown token takes
// `return sub_14151E280(..., "'%s' is not a valid command.", token)`. That call logs and returns
// NON-ZERO; sub_1400DA830 returns it mid-loop, so no later token is parsed; and ApplyCommandLine's own
// call site does `0x140116732 call sub_140503AB0; test eax,eax; jnz 0x1401181D1` - the epilogue. EVERY
// switch handler is then skipped, silently: -offline, -level, -region and all four 127.0.0.1 pins are
// ignored and the process runs on, online, looking configured. The bits keep their constructor values
// (0x1400F6166 mov qword [rdi+7AE0h],40000000h and 0x1400F6493 mov [rdi+8A70h],r15), so BOTH are clear
// - but a probe must not report that as a boot-flag verdict, because it is not one: it is the answer to
// a different question, and printing CLEAR/CLEAR would certify a launch that ignored its own pins.
//
// ★ THE SET BELOW IS WHAT WE HAVE A CITATION FOR, NOT A CENSUS OF THE ENGINE'S REGISTRY, and it fails
// CLOSED on purpose: a real engine switch we have no citation for is reported as voiding the line
// (over-refusal), never the reverse. That is the same direction, and the same union of sources, as the
// harness's plan-time twin Get-KnownSwitches - which is the check that
// keeps an unknown token off a launch line in the first place. This one exists because the harness
// cannot see the line the process actually received; test_session.cpp re-derives the set from the
// profile lines in its own row table, so the two sources have to agree for the suite to pass.
struct RegisteredSwitch {
    const wchar_t* name;
    const char*    citation;
};

inline constexpr RegisteredSwitch kRegisteredSwitches[] = {
    // A - carried by a sanctioned profile in $CanonicalProfiles
    {L"-level", "harness.ps1 bare-level; handler at 0x140116CD5 reads the level hash"},
    {L"-gametype", "harness.ps1 offline-local; W1-F spike Q2"},
    {L"-loginhost", "harness.ps1 offline-local pin"},
    {L"-confighost", "harness.ps1 offline-local pin"},
    {L"-radserverdbhost", "harness.ps1 offline-local pin"},
    {L"-servermanagerhost", "harness.ps1 offline-local pin"},
    // B - registered at runtime by rlay.dll (docs/HARNESS.md:383, :389). -noovr is stock; rlay only
    //     patches its "requires -spectatorstream" check at +0x11690D.
    {L"-offline", "rlay AddArgSyntax, HARNESS.md:389"},
    {L"-windowed", "rlay AddArgSyntax, HARNESS.md:383"},
    {L"-noovr", "stock; sole effect 0x1401168BA or [rdi+7AE0h],8000000h"},
    // C - a handler inside ApplyCommandLine, at the byte-level site this header lists above
    {L"-mp", "0x140116D3F / 0x140116D4A"},
    {L"-mpappid", "0x140116D9F"},
    {L"-spectatorstream", "0x140116F43-F60 / 0x140116F4D"},
    {L"-moderator", "moderator block, 0x14011726A / 0x140117275"},
    {L"-moderateuser", "moderator block"},
    {L"-moderategroup", "moderator block"},
    {L"-lobbyid", "0x140117690 / 0x14011769F"},
    {L"-region", "0x140117727 / 0x140117736"},
    // D - a handler the fact registry identifies (facts/facts.tsv:594, GAME.HTTPPORT_OFF)
    {L"-httpport", "0x140117258 mov [rdi+8560h],ax after ntohs"},
};

// The kTokenCap invariant, enforced rather than described: a cut token is kTokenCap-1 characters, so
// it can equal no name here. -servermanagerhost, at 18, is the longest.
inline constexpr size_t LongestRegisteredNameLen() {
    size_t longest = 0;
    for (const RegisteredSwitch& r : kRegisteredSwitches) {
        size_t n = 0;
        while (r.name[n] != 0) {
            ++n;
        }
        if (n > longest) {
            longest = n;
        }
    }
    return longest;
}
static_assert(LongestRegisteredNameLen() + 1 < kTokenCap,
              "a token cut at kTokenCap must be longer than every registered name, or a cut token "
              "could equal one and NextToken would need to report the cut");

// The first token on the line that the engine would not recognise, copied into `out` as the engine
// would have looked it up (lead run stripped, case folded). Returns false when every switch on the
// line is registered. The program path is never a switch and a value never begins with a lead
// character, so both are skipped exactly as sub_1400DA830 skips them.
inline bool FirstUnregisteredSwitch(const wchar_t* commandLine, wchar_t* out, size_t cap) {
    if (cap > 0) {
        out[0] = 0;
    }
    if (commandLine == nullptr) {
        return false;
    }
    const wchar_t* cursor = commandLine;
    wchar_t token[kTokenCap];
    if (!NextToken(&cursor, token, kTokenCap)) {
        return false;
    }
    while (NextToken(&cursor, token, kTokenCap)) {
        if (!IsSwitchLead(token[0])) {
            continue;
        }
        bool known = false;
        for (const RegisteredSwitch& r : kRegisteredSwitches) {
            if (TokenIsSwitch(token, r.name)) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }
        const wchar_t* name = token;
        while (IsSwitchLead(*name)) {
            ++name;
        }
        size_t n = 0;
        while (name[n] != 0 && n + 1 < cap) {
            out[n] = FoldAscii(name[n]);
            ++n;
        }
        if (cap > 0) {
            out[n] = 0;
        }
        return true;
    }
    return false;
}

inline bool LineVoided(const wchar_t* commandLine) {
    wchar_t unused[kTokenCap];
    return FirstUnregisteredSwitch(commandLine, unused, kTokenCap);
}

inline bool BootMpBitExpectedSet(const wchar_t* commandLine) {
    return !LevelSwitchGiven(commandLine) || BootMpSetterGiven(commandLine) != nullptr;
}

inline bool OnlineBitExpectedSet(const wchar_t* commandLine) {
    return !LevelSwitchGiven(commandLine) && OnlineClearerGiven(commandLine) == nullptr;
}

// Three outcomes, never two. On a voided line the expectation above is not wrong, it is unasked: no
// handler ran, so the bits answer the constructor rather than the switches, and a caller that reduced
// this to a bool would print PASS or FAIL for a launch whose -offline and pins were ignored.
enum class BitVerdict { AsExpected, NotAsExpected, LineVoided };

inline BitVerdict BootMpBitVerdict(uint64_t modeFlags, const wchar_t* commandLine) {
    if (LineVoided(commandLine)) {
        return BitVerdict::LineVoided;
    }
    const bool set = (modeFlags & replay::session_facts::kModeBitBootMp) != 0;
    return set == BootMpBitExpectedSet(commandLine) ? BitVerdict::AsExpected : BitVerdict::NotAsExpected;
}

inline BitVerdict OnlineBitVerdict(uint64_t sessionFlags, const wchar_t* commandLine) {
    if (LineVoided(commandLine)) {
        return BitVerdict::LineVoided;
    }
    const bool set = (sessionFlags & replay::session_facts::kFlagBitOnline) != 0;
    return set == OnlineBitExpectedSet(commandLine) ? BitVerdict::AsExpected : BitVerdict::NotAsExpected;
}

// Whether the launch carries any token the engine would take for a switch after the program path.
// `bare` carries none, so "no switch reached the offline bit" has nothing to test there.
inline bool AnySwitchGiven(const wchar_t* commandLine) {
    if (commandLine == nullptr) {
        return false;
    }
    const wchar_t* cursor = commandLine;
    wchar_t token[kTokenCap];
    if (!NextToken(&cursor, token, kTokenCap)) {
        return false;
    }
    while (NextToken(&cursor, token, kTokenCap)) {
        if (IsSwitchLead(token[0])) {
            return true;
        }
    }
    return false;
}

}  // namespace w1f
