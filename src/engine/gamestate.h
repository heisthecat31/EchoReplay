#pragma once

// The local game-state sampler -- ARCH-21 S4.2, in the shape S4.2's own fallback names: the engine's
// typed getters are CALLED every tick, and no slot address is ever cached.  The cached-slot shape S4.2
// describes first cannot be built: the four getters are (store, CSymbol64) -> value and hand back no
// slot address, and no address-returning resolver exists in the image.  Calling the getters closes the
// 533 ms stale-read window by construction -- there is no cache to go stale -- and leaves exactly three
// raw dereferences on the path, which is what Readable() gates.

#include <stddef.h>
#include <stdint.h>

#include "engine/gamestate_types.h"

namespace replay {
namespace gamestate {

// The engine surface the sampler needs.  Nothing offline can call into echovr.exe, so the live
// implementation and the tests' fake store meet here.
class IEngineAccess {
public:
    virtual ~IEngineAccess() {}

    // The CR15NetGame the read chains start from, or null when no session is up.
    virtual void* NetGame() = 0;

    // The house rule for every raw pointer this tree dereferences (buildguard.h:85).
    virtual bool Readable(const void* at, size_t bytes) = 0;

    // Reads the qword at a recorded VA out of the image the loader actually mapped.  False means
    // "not mapped / not readable" -- the vocabulary check refuses on false rather than assuming.
    virtual bool ReadImageQword(uint64_t va, uint64_t* out) = 0;

    // The engine's own typed getters.  `store` is the pointer at the depth the fact row names.
    virtual int32_t  GetInt(void* store, uint64_t symbol) = 0;
    virtual uint64_t GetSymbol(void* store, uint64_t symbol) = 0;
    virtual float    GetFloat(void* store, uint64_t symbol) = 0;
    // The bool getter, added with the two phase flags.  Non-pure because this interface is frozen
    // and every fake predates it, and it answers through an out-param rather than returning the
    // value: the default is "this build cannot read a bool", which the caller must be able to tell
    // apart from a flag that is genuinely false.
    virtual bool     GetBoolChecked(void* store, uint64_t symbol, bool& out) {
        (void)store; (void)symbol; (void)out;
        return false;
    }
};

// The engine's OWN entry points as the Win64 ABI actually calls them -- NOT as IEngineAccess spells
// them. IEngineAccess is this project's 2-argument interface and a fake satisfies it with whatever
// convention C++ picks; these typedefs are the only place the real convention is written down, and
// gamestate_live.cpp binds through them so a wrong arity cannot compile.
//
// GS.GET_SYMBOL returns a CSymbol64 BY VALUE, and MSVC returns that class in memory: argument 1 is
// the hidden result buffer, argument 2 is the STORE and argument 3 is the symbol. Read off the
// pinned echovr.exe at 0x140D5CED0 -- `mov rdi,r8` / `mov rax,rdx` / `mov r12,rcx`, then
// `mov r15,[rax+0x2E00]` (the store is dereferenced out of RDX) and `mov [r12],rsi` / `mov rax,r12`
// at 0x140D5D962. Both call sites agree: 0x140155D01 and 0x1401B0376 each do `lea rcx,<local>`,
// store into rdx, symbol into r8. Binding it as (store, symbol) put our CSymbol64 in RDX and the
// engine executed [symbol+0x2E00] -- the L2-RUN-6 0xC0000005 at 0x140D5CF01.
//
// GET_INT/GET_FLOAT/GET_BOOL are the other family and genuinely take (store, symbol): `mov rdi,rdx`
// / `mov rax,rcx` at 0x140D05730, 0x140D36750 and 0x140CE4460, byte-identical to each other and NOT
// to GET_SYMBOL, and their call sites (e.g. 0x1401AF264, 0x1401B04CD) set RCX and RDX only, never
// R8. GET_FLOAT shares GET_SYMBOL's stack frame -- 7 pushes, `lea rbp,[rsp-0F0h]`, `sub rsp,1F0h` --
// so frame shape is NOT the discriminator; the three argument moves at +0x1F are.
//
// The parameter NAMES are load-bearing: tools/gen_facts.py --check parses these typedefs for the
// declared arity and for which argument is named `store`, and asks the shipped exe whether the code
// at each fact's address agrees.
typedef int32_t   (*GetIntFn)(void* store, uint64_t symbol);
typedef float     (*GetFloatFn)(void* store, uint64_t symbol);
typedef bool      (*GetBoolFn)(void* store, uint64_t symbol);
typedef uint64_t* (*GetSymbolFn)(uint64_t* result, void* store, uint64_t symbol);

// A field GameStateSample declares that this build cannot fill, and why, by name.  A clear present
// bit with no reason attached is the "missing data defaults to a confident answer" failure wearing a
// bit mask, so every permanently absent field is listed here and the list is reported.
struct UnreadableField {
    const char* field;
    const char* reason;
};
const UnreadableField* UnreadableFields(size_t* count);

// The live engine.  Returns null when the four getter VAs do not land inside the mapped image --
// a wrong build, or a rebase that did not happen; the caller reports that rather than calling
// through a rebased-to-nothing address.
IEngineAccess* LiveEngineAccess();

// Both the caller-owned constructor (tests pass their fake) and the process-wide instance.
IGameState* CreateGameState(IEngineAccess* access);
void        DestroyGameState(IGameState* state);

}  // namespace gamestate
}  // namespace replay
