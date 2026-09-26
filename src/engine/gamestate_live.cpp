// The live half of the game-state sampler: the engine's own typed getters, the session's netGame
// pointer, and the mapped-image reads the phase vocabulary is cross-checked against.  It is a
// separate translation unit from gamestate.cpp so that the sampler's logic links -- and is tested --
// with no engine, no Windows and no MinHook behind it.

#include <stdint.h>

#include <type_traits>

#include "bootstrap/buildguard.h"
#include "engine/gamestate.h"
#include "engine/session.h"
#include "generated/facts_generated.h"

namespace replay {
namespace gamestate {
namespace {

// Binding a raw address to a function type is the one cast a compiler makes in total silence, and
// it is how GS.GET_SYMBOL -- a THREE-argument entry point whose store is argument 2 -- came to be
// called as (store, symbol) and killed the client at 0x140D5CF01 (L2-RUN-6 F1). This used to be
// four reinterpret_casts, and reinterpret_cast will convert ANY address to ANY function type
// without a word. Two things stop that here. Naming the signature makes the only thing that
// compiles a type matching the field being assigned -- swap GetSymbolFn for GetIntFn, or drop the
// result buffer from the type, and it does not build. And the address is not called at all until
// the ARGUMENT MOVES at it match the registry: all four getters share a byte-identical stack frame,
// so the moves at GS.GETTER_ARGMOVE_OFF are the only bytes that tell the families apart, and
// a re-derivation that collapses them now fails at bind rather than at mov r15,[rax+0x2E00].
// How far that goes, exactly: GET_INT, GET_FLOAT and GET_BOOL carry the SAME argmove bytes
// (48 8B FA 48 8B C1 45 33) because they take the same (store, symbol) shape; only GET_SYMBOL,
// which writes through a result buffer, differs. So the argmove proves the shape, not the kind,
// and the static_assert below is what catches a regeneration that gave two kinds one VA --
// the failure the argmove alone would wave through.
// Read out of echovr.exe (md5 caa05ee3bca2f5df0809994e93e2cc05) at each getter's VA + 0x1F, and
// read from the registry: GS.GET_*.ARGMOVE / GS.GETTER_ARGMOVE_OFF, each anchored at a
// call site that reaches the getter, so a re-derivation cannot quietly disagree with this file.

static_assert(facts::GS_GET_INT != facts::GS_GET_FLOAT && facts::GS_GET_INT != facts::GS_GET_BOOL &&
                  facts::GS_GET_INT != facts::GS_GET_SYMBOL &&
                  facts::GS_GET_FLOAT != facts::GS_GET_BOOL &&
                  facts::GS_GET_FLOAT != facts::GS_GET_SYMBOL &&
                  facts::GS_GET_BOOL != facts::GS_GET_SYMBOL,
              "two getter kinds resolved to one address");

template <typename Sig, size_t N>
Sig Guarded(const char* factId, uint64_t va, const uint8_t (&argMove)[N], uint64_t argMoveOff) {
    static_assert(std::is_pointer<Sig>::value &&
                      std::is_function<typename std::remove_pointer<Sig>::type>::value,
                  "Guarded<Sig> takes the pointer-to-function type the entry point is called as");
    (void)factId;
    const void* at = buildguard::MappedAddress(va);
    if (at == nullptr) {
        return nullptr;
    }
    const uint8_t* moves = static_cast<const uint8_t*>(at) + argMoveOff;
    if (!replay::Readable(moves, N)) {
        return nullptr;
    }
    for (size_t i = 0; i < N; ++i) {
        if (moves[i] != argMove[i]) {
            return nullptr;
        }
    }
    return reinterpret_cast<Sig>(const_cast<void*>(at));
}

class LiveAccess final : public IEngineAccess {
public:
    bool Bind();

    void* NetGame() override {
        replay::ISessionEntry* entry = session::Interface();
        return entry != nullptr ? entry->NetGame() : nullptr;
    }

    bool Readable(const void* at, size_t bytes) override { return replay::Readable(at, bytes); }
    bool ReadImageQword(uint64_t va, uint64_t* out) override;

    int32_t GetInt(void* store, uint64_t symbol) override { return getInt_(store, symbol); }
    float GetFloat(void* store, uint64_t symbol) override { return getFloat_(store, symbol); }
    bool GetBoolChecked(void* store, uint64_t symbol, bool& out) override {
        if (!getBool_) return false;
        out = getBool_(store, symbol);
        return true;
    }

    // The engine writes the CSymbol64 through the result buffer and returns that same pointer; the
    // buffer is ours, so the returned pointer is not trusted for the read.
    uint64_t GetSymbol(void* store, uint64_t symbol) override {
        uint64_t result = 0;
        getSymbol_(&result, store, symbol);
        return result;
    }

private:
    GetIntFn    getInt_ = nullptr;
    GetSymbolFn getSymbol_ = nullptr;
    GetFloatFn  getFloat_ = nullptr;
    GetBoolFn   getBool_ = nullptr;
};

bool LiveAccess::Bind() {
    getInt_ = Guarded<GetIntFn>("GS.GET_INT", ::facts::GS_GET_INT, ::facts::GS_GET_INT_ARGMOVE,
                                ::facts::GS_GETTER_ARGMOVE_OFF);
    getSymbol_ = Guarded<GetSymbolFn>("GS.GET_SYMBOL", ::facts::GS_GET_SYMBOL,
                                      ::facts::GS_GET_SYMBOL_ARGMOVE, ::facts::GS_GETTER_ARGMOVE_OFF);
    getFloat_ = Guarded<GetFloatFn>("GS.GET_FLOAT", ::facts::GS_GET_FLOAT,
                                    ::facts::GS_GET_FLOAT_ARGMOVE, ::facts::GS_GETTER_ARGMOVE_OFF);
    // Kept and called since the two phase flags: sudden_death_active and match_is_over read back
    // through this one, and a drop that rebased to nothing must not look like three good getters
    // and one uninteresting field.
    getBool_ = Guarded<GetBoolFn>("GS.GET_BOOL", ::facts::GS_GET_BOOL,
                                  ::facts::GS_GET_BOOL_ARGMOVE, ::facts::GS_GETTER_ARGMOVE_OFF);
    return getInt_ != nullptr && getSymbol_ != nullptr && getFloat_ != nullptr &&
           getBool_ != nullptr;
}

bool LiveAccess::ReadImageQword(uint64_t va, uint64_t* out) {
    const void* at = buildguard::MappedAddress(va);
    if (at == nullptr || !replay::Readable(at, sizeof(uint64_t))) {
        return false;
    }
    *out = *reinterpret_cast<const uint64_t*>(at);
    return true;
}

LiveAccess g_live;
bool g_liveBound = false;
bool g_liveTried = false;

}  // namespace

IEngineAccess* LiveEngineAccess() {
    // Bound once: MappedAddress walks the PE headers, which is two Readable probes -- i.e. two
    // VirtualQuery calls -- and none of that belongs anywhere near the capture tick.
    if (!g_liveTried) {
        g_liveTried = true;
        g_liveBound = g_live.Bind();
    }
    return g_liveBound ? &g_live : nullptr;
}

}  // namespace gamestate
}  // namespace replay
