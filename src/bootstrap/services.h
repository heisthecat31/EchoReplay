// The frozen contracts of the design Appendix B.12: ReplayServices, ILog, IConfig, ReplayModule and
// REPLAY_MODULE. Owned by W1-C; consumed by every product module.
//
// ★ EVERYTHING HERE IS IN `namespace replay`. Architect ruling, 2026-08-28, over the whole
// appendix rather than case by case, and the reason is recorded with it: a global ILog / IConfig /
// IEntity in a DLL that shares a process with the engine AND with rlay.dll collides at link time in
// somebody else's translation unit. One rule for the whole appendix means no consumer has to remember
// which types are qualified and which are not.
//
// The macros at the foot of this file use fully qualified names, so REPLAY_MODULE and REPLAY_REQUIRE
// still work from global scope in a packet's own translation unit.
//
// The interfaces owned by other packets are forward-declared. A pointer to an incomplete type is all
// ReplayServices needs, so this header compiles before any of those packets has landed, and a consumer
// that actually dereferences one includes the owner's header alongside this one.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {

struct TickInfo;              // B.3, W1-D
class ITick;                  // B.3, W1-D
class IReplayClock;           // B.5, W2-D
class IEntity;                // B.4, W1-E
class IPosePublisher;         // B.4, W1-E
class ISessionEntry;          // B.6, W1-F
class IReplayCamera;          // B.7, W1-H
class IAsSeen;                // B.11.1, W1-E
class INetVar;                // B.11.2, W2-A
class IIdentity;              // B.11.3, W2-C
class IAudioState;            // B.11.4, W2-H
class IAudioRate;             // B.9, W2-H
class IReplayTransport;       // B.8, W2-B
class IWiretap;               // B.14, W1-L
class IDemoIndex;             // B.0, W1-B

enum class LogLevel : uint8_t { Trace, Info, Warn, Error };

class ILog {
public:
    // Formats into a fixed per-thread buffer and appends to a lock-free ring the worker
    // drains. Never blocks, never allocates. Truncates at 512 bytes rather than growing.
    virtual void Write(LogLevel, const char* module, const char* fmt, ...) = 0;
    virtual bool Enabled(LogLevel) const = 0;          // cheap early-out on the tick path
    // Prints the enabled intervention-ledger rows at startup (4.4). Called once.
    virtual void DumpLedger() = 0;

protected:
    ~ILog() = default;
};

class IConfig {
public:
    // All reads are from an immutable snapshot taken at load; there is no reload path,
    // so a value cannot change under a packet mid-session.
    virtual bool        GetBool  (const char* key, bool  dflt) const = 0;
    virtual int64_t     GetInt   (const char* key, int64_t dflt) const = 0;
    virtual double      GetDouble(const char* key, double dflt) const = 0;
    virtual const char* GetString(const char* key, const char* dflt) const = 0;
    // Every ledger row and every stream has a key (4.4, 6.6.1). An unknown key returns
    // the default AND is logged once, so a typo in a key name is visible rather than silent.
    virtual bool        Has(const char* key) const = 0;

protected:
    ~IConfig() = default;
};

struct ReplayServices {                 // borrowed; valid for the process lifetime
    ITick*            tick;
    IReplayClock*     clock;
    IEntity*          entity;
    IPosePublisher*   pose;
    ISessionEntry*    session;
    IReplayCamera*    camera;
    IAsSeen*          asSeen;
    INetVar*          netVar;
    IIdentity*        identity;
    IAudioState*      audioState;
    IWiretap*         wiretap;            // B.14; null until W1-L has registered
    IAudioRate*       audioRate;
    IReplayTransport* transport;         // null until W2-B has registered
    IDemoIndex*       demoIndex;
    const IConfig*    config;
    ILog*             log;
};

struct ReplayModule {
    const char* name;
    int         order;                              // ascending
    bool      (*init)(ReplayServices*);
    void      (*tick)(const TickInfo&);             // may be null
    void      (*shutdown)();
};

namespace replay_detail {

// Appending to a zero-initialised array of pointers is what makes this safe from a static
// constructor: the storage has static storage duration and is zero-initialised before any
// dynamic initialiser in any translation unit runs, so there is no initialisation-order
// dependency between a module and the registry it registers into.
struct ModuleRegistrar {
    explicit ModuleRegistrar(const ReplayModule& mod);
};

}  // namespace replay_detail
}  // namespace replay

#define REPLAY_MODULE_PASTE_(a, b) a##b
#define REPLAY_MODULE_NAME_(a, b) REPLAY_MODULE_PASTE_(a, b)

// A module's translation unit is the only file it adds (B.10). src/plugin/dllmain.cpp is never
// edited to make a module run.
//
// The descriptor is `static const` with a constant initialiser - function pointers and a string
// literal are constant expressions - so it is fully formed before the registrar's constructor takes
// its address, whatever order the linker gives the two objects.
#define REPLAY_MODULE(NAME, ORDER, INIT, TICK, SHUTDOWN)                                   \
    static const ::replay::ReplayModule REPLAY_MODULE_NAME_(replay_module_, __LINE__) = {  \
        NAME, ORDER, INIT, TICK, SHUTDOWN};                                                \
    static const ::replay::replay_detail::ModuleRegistrar REPLAY_MODULE_NAME_(             \
        replay_module_registrar_, __LINE__)(REPLAY_MODULE_NAME_(replay_module_, __LINE__))

// B.12: "A pointer in ReplayServices may be null ... Every consumer checks." A module states what it
// needs; a null it did not declare is a hard init failure with a named diagnostic, never a crash on
// first use two hours into a session.
//
//     bool MyInit(ReplayServices* s) {
//         REPLAY_REQUIRE(s, log);
//         REPLAY_REQUIRE(s, entity);
//         ...
//     }
namespace replay {
void ReplayReportMissingService(const ReplayServices* services, const char* field,
                                const char* module);
}  // namespace replay

#define REPLAY_REQUIRE(SERVICES, FIELD)                                                    \
    do {                                                                                   \
        if ((SERVICES) == nullptr || (SERVICES)->FIELD == nullptr) {                        \
            ::replay::ReplayReportMissingService((SERVICES), #FIELD, kReplayModuleName);   \
            return false;                                                                  \
        }                                                                                  \
    } while (false)
