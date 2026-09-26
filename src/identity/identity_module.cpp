// Publishes IIdentity into ReplayServices.  The dressing itself -- DressRoster in dresser.cpp --
// is driven by W2-B's load path through that pointer, and logs every outcome, pass or fail, with
// the stage that decided it: DESIGN 9.3's three breakers are all SILENT in the engine.

#include "bootstrap/services.h"
#include "engine/entity.h"
#include "engine/identity_access.h"
#include "engine/session.h"

namespace replay {
namespace {

const char* const kReplayModuleName = "identity";

ReplayServices* g_services = nullptr;
IIdentity*      g_identity = nullptr;

void* NetGameForIdentity() {
    if (!g_services || !g_services->session) return nullptr;
    return g_services->session->NetGame();
}

// Hop 4's provider.  Asked on every hop-4 read rather than cached at init: the entity service
// resolves its component systems out of the game space and re-resolves them across a level load,
// so module order does not decide whether this answers -- the call does.
void* RemotePlayerCsForIdentity() {
    if (!g_services || !g_services->entity) return nullptr;
    return entity::RemotePlayerSystem(g_services->entity);
}

bool IdentityInit(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    REPLAY_REQUIRE(services, session);
    g_services = services;

    g_identity = identity_access::Create(&NetGameForIdentity, &RemotePlayerCsForIdentity);
    if (!g_identity) {
        services->log->Write(LogLevel::Error, "identity", "identity_access::Create refused");
        return false;
    }
    services->identity = g_identity;

    // Reported at startup, not on first use: a detoured entry point is a coexistence fact the
    // operator wants before a match, and by the time the first player is dressed it is too late
    // to choose a different plugin load order.
    if (!identity_access::BuildMatches()) {
        services->log->Write(LogLevel::Warn, "identity",
                             "cosmetics disabled: these entry points do not match the recorded "
                             "build: %s", identity_access::BuildMismatchDetail());
    }
    return true;
}

void IdentityShutdown() {
    if (g_services) g_services->identity = nullptr;
    identity_access::Destroy(g_identity);
    g_identity = nullptr;
    g_services = nullptr;
}

REPLAY_MODULE("identity", 40, IdentityInit, nullptr, IdentityShutdown);

}  // namespace
}  // namespace replay
