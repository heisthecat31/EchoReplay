#include "bootstrap/modreg.h"

#include <windows.h>

#include <cstring>

#include "bootstrap/log.h"

namespace replay {
namespace modreg {
namespace {

constexpr size_t kMaxModules = 64;

struct Slot {
    const ReplayModule* mod;
    bool                live;   // init returned true
};

// Zero-initialised storage with static storage duration. A REPLAY_MODULE's registrar is a dynamic
// initialiser and may run before or after any other translation unit's, but zero-initialisation happens
// before all of them - so appending here from a constructor has no ordering dependency at all. A
// std::vector here would be the classic static-initialisation-order bug: the first module to register
// would push into a vector that had not been constructed.
Slot   g_slots[kMaxModules];
size_t g_count;
size_t g_ticking;      // modules with a tick function, compacted to the front of g_tick
const ReplayModule* g_tick[kMaxModules];
size_t g_failures;
size_t g_overflow;
bool   g_initialised;

constexpr size_t kMaxPreExit = 8;
PreExitFn g_preExit[kMaxPreExit];
size_t    g_preExitCount;

}  // namespace

size_t Count() { return g_count; }

const ReplayModule* At(size_t i) { return i < g_count ? g_slots[i].mod : nullptr; }

size_t InitAll(ReplayServices* services) {
    if (g_initialised) {
        return g_count - g_failures;
    }
    g_initialised = true;
    g_failures = 0;
    g_ticking = 0;

    if (g_overflow != 0) {
        log::Direct(LogLevel::Error, "modreg",
                    "%llu module(s) could not register: the table holds %llu",
                    static_cast<unsigned long long>(g_overflow),
                    static_cast<unsigned long long>(kMaxModules));
    }

    // Insertion sort by (order, name). Deterministic, in place, and n is a handful.
    for (size_t i = 1; i < g_count; ++i) {
        const Slot key = g_slots[i];
        size_t j = i;
        while (j > 0) {
            const ReplayModule* prev = g_slots[j - 1].mod;
            const bool after = prev->order < key.mod->order ||
                               (prev->order == key.mod->order &&
                                std::strcmp(prev->name, key.mod->name) <= 0);
            if (after) {
                break;
            }
            g_slots[j] = g_slots[j - 1];
            --j;
        }
        g_slots[j] = key;
    }

    for (size_t i = 1; i < g_count; ++i) {
        if (std::strcmp(g_slots[i - 1].mod->name, g_slots[i].mod->name) == 0) {
            log::Direct(LogLevel::Warn, "modreg",
                        "two modules are both called '%s'; a diagnostic naming one of them is ambiguous",
                        g_slots[i].mod->name);
        }
    }

    for (size_t i = 0; i < g_count; ++i) {
        const ReplayModule* mod = g_slots[i].mod;
        if (mod->init == nullptr) {
            g_slots[i].live = true;
        } else {
            g_slots[i].live = mod->init(services);
        }
        if (!g_slots[i].live) {
            ++g_failures;
            log::Direct(LogLevel::Error, "modreg", "module '%s' (order %d) refused to initialise",
                        mod->name, mod->order);
            continue;
        }
        if (mod->tick != nullptr) {
            g_tick[g_ticking++] = mod;
        }
        log::Direct(LogLevel::Info, "modreg", "module '%s' (order %d) ready%s", mod->name, mod->order,
                    mod->tick != nullptr ? "" : ", no tick");
    }
    return g_count - g_failures;
}

size_t InitFailures() { return g_failures; }

void TickAll(const TickInfo& info) {
    for (size_t i = 0; i < g_ticking; ++i) {
        g_tick[i]->tick(info);
    }
}

void ShutdownAll() {
    for (size_t i = g_count; i > 0; --i) {
        Slot& slot = g_slots[i - 1];
        if (slot.live && slot.mod->shutdown != nullptr) {
            slot.mod->shutdown();
        }
        slot.live = false;
    }
    g_ticking = 0;
    g_initialised = false;
}

bool RegisterPreExit(PreExitFn fn) {
    if (fn == nullptr) {
        return false;
    }
    for (size_t i = 0; i < g_preExitCount; ++i) {
        if (g_preExit[i] == fn) {
            return true;
        }
    }
    if (g_preExitCount == kMaxPreExit) {
        return false;
    }
    g_preExit[g_preExitCount++] = fn;
    return true;
}

void RunPreExit(uint32_t budgetMs) {
    const uint64_t start = GetTickCount64();
    for (size_t i = 0; i < g_preExitCount; ++i) {
        const uint64_t spent = GetTickCount64() - start;
        g_preExit[i](spent >= budgetMs ? 0 : budgetMs - static_cast<uint32_t>(spent));
    }
}

void ResetForTest() {
    g_preExitCount = 0;
    g_count = 0;
    g_ticking = 0;
    g_failures = 0;
    g_overflow = 0;
    g_initialised = false;
    std::memset(g_slots, 0, sizeof g_slots);
}

namespace {

void Append(const ReplayModule& mod) {
    if (g_count >= kMaxModules) {
        // Cannot log yet: this runs during CRT static initialisation, before the log has a path. The
        // count is reported by InitAll, which runs when there is somewhere to report it to.
        ++g_overflow;
        return;
    }
    g_slots[g_count].mod = &mod;
    g_slots[g_count].live = false;
    ++g_count;
}

}  // namespace

void RegisterForTest(const ReplayModule& mod) { Append(mod); }

}  // namespace modreg
}  // namespace replay

namespace replay {
namespace replay_detail {

ModuleRegistrar::ModuleRegistrar(const ReplayModule& mod) {
    ::replay::modreg::Append(mod);
}

}  // namespace replay_detail
}  // namespace replay
