// B.12's acceptance item, shipped rather than described: "A pointer in ReplayServices may be null when
// its owning packet has not registered yet ... Every consumer checks. W1-C's acceptance includes a module
// that asserts on a null it did not declare."
//
// It is also the end-to-end proof that the registry works in the real DLL. If the "self-check" line is
// missing from echoreplay.log, static self-registration is broken and no Wave-2 module would run either -
// which is a much better thing to learn from a log line than from a feature that silently never starts.
#include "bootstrap/services.h"

#include "bootstrap/config.h"
#include "bootstrap/log.h"
#include "bootstrap/modreg.h"

namespace {

const char* const kReplayModuleName = "self-check";

bool Init(replay::ReplayServices* services) {
    // Declared: this module needs the two things W1-C itself provides. Anything else in ReplayServices is
    // undeclared here, so this module must never touch it - and REPLAY_REQUIRE is what turns "must never"
    // into a diagnostic naming the field instead of a null dereference in someone else's Wave-2 feature.
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);

    services->log->Write(replay::LogLevel::Info, kReplayModuleName,
                         "module registry live: %llu module(s) registered, %llu config key(s) declared",
                         static_cast<unsigned long long>(replay::modreg::Count()),
                         static_cast<unsigned long long>(replay::ConfigKeyCount()));
    return true;
}

void Shutdown() {}

}  // namespace

// Order 0: nothing depends on it, and running first means its line is near the top of the log where
// somebody debugging a silent load will actually look.
REPLAY_MODULE("self-check", 0, Init, nullptr, Shutdown);
