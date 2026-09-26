#include "bootstrap/attachhook.h"

#include <cstring>

#include "bootstrap/config.h"
#include "bootstrap/log.h"

namespace replay {
namespace attachhook {
namespace {

constexpr size_t kMaxHooks = 8;

// Zero-initialised, for the same reason modreg's table is: a registrar is a dynamic initialiser and
// zero-initialisation happens before all of them, so appending here has no ordering dependency.
const AttachHook* g_hooks[kMaxHooks];
size_t            g_count;
bool              g_installed[kMaxHooks];
size_t            g_overflow;
bool              g_ran;

}  // namespace

size_t Count() { return g_count; }

const AttachHook* At(size_t i) { return i < g_count ? g_hooks[i] : nullptr; }

size_t RunAll() {
    if (g_ran) {
        return 0;
    }
    g_ran = true;
    if (g_overflow != 0) {
        log::Direct(LogLevel::Error, "attach", "%llu attach hook(s) could not register; the table holds %llu",
                    static_cast<unsigned long long>(g_overflow),
                    static_cast<unsigned long long>(kMaxHooks));
    }

    // Name order, so the sequence does not depend on link order.
    for (size_t i = 1; i < g_count; ++i) {
        const AttachHook* key = g_hooks[i];
        const bool wasInstalled = g_installed[i];
        size_t j = i;
        while (j > 0 && std::strcmp(g_hooks[j - 1]->name, key->name) > 0) {
            g_hooks[j] = g_hooks[j - 1];
            g_installed[j] = g_installed[j - 1];
            --j;
        }
        g_hooks[j] = key;
        g_installed[j] = wasInstalled;
    }

    size_t installed = 0;
    for (size_t i = 0; i < g_count; ++i) {
        const AttachHook* hook = g_hooks[i];
        if (hook->install == nullptr) {
            continue;
        }
        // Its ledger row's key. config::LoadForAttach has already read config.json if there is one, so
        // this is the user's answer and not just the declared default.
        if (hook->configKey != nullptr && !config::Interface()->GetBool(hook->configKey, true)) {
            log::Direct(LogLevel::Info, "attach", "%s: skipped, %s is false", hook->name, hook->configKey);
            continue;
        }
        g_installed[i] = hook->install();
        if (g_installed[i]) {
            ++installed;
            log::Direct(LogLevel::Info, "attach", "%s: installed inline", hook->name);
        } else {
            // Not an error on its own: a hook that reports it could not install is behaving correctly,
            // and the design says the row is refusable with a named consequence.
            log::Direct(LogLevel::Warn, "attach", "%s: declined to install", hook->name);
        }
    }
    return installed;
}

bool Installed(const char* name) {
    for (size_t i = 0; i < g_count; ++i) {
        if (std::strcmp(g_hooks[i]->name, name) == 0) {
            return g_installed[i];
        }
    }
    return false;
}

namespace {

void Append(const AttachHook& hook) {
    if (g_count >= kMaxHooks) {
        ++g_overflow;
        return;
    }
    g_hooks[g_count] = &hook;
    g_installed[g_count] = false;
    ++g_count;
}

}  // namespace

void RegisterForTest(const AttachHook& hook) { Append(hook); }

void ResetForTest() {
    g_count = 0;
    g_overflow = 0;
    g_ran = false;
    std::memset(g_hooks, 0, sizeof g_hooks);
    std::memset(g_installed, 0, sizeof g_installed);
}

}  // namespace attachhook

namespace replay_attach_detail {

AttachHookRegistrar::AttachHookRegistrar(const AttachHook& hook) {
    ::replay::attachhook::RegisterForTest(hook);
}

}  // namespace replay_attach_detail
}  // namespace replay
