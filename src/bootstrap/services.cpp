// The one function REPLAY_REQUIRE calls. It lives in the static library rather than beside the self-check
// module because every packet's REPLAY_REQUIRE has to resolve to it, including from a test binary that
// links no product module at all.
#include "bootstrap/services.h"

#include "bootstrap/log.h"

namespace replay {

void ReplayReportMissingService(const ReplayServices* services, const char* field,
                                const char* module) {
    // Deliberately not through services->log. The whole premise is that a service may be null, and the
    // one that is null may be the log; the direct path writes synchronously and needs nothing from
    // anybody.
    log::Direct(LogLevel::Error, module != nullptr ? module : "?",
                        "required service '%s' is null (services=%p). This module declared it, so it is "
                        "a wiring error and not an optional dependency; refusing to initialise.",
                        field, static_cast<const void*>(services));
}

}  // namespace replay
