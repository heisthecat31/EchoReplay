#pragma once

// W2-E-31c. The rig-state census line's one test seam: the address ReadRigEnabled() dereferences.
//
// CAM.SPECTATOR_ENABLED_GLOBAL is at 0x1420D3AE4, 34 MB past the base of any test executable, so
// buildguard::MappedAddress refuses it offline and the readable-0 / readable-1 renderings of the
// line would have no failing input -- the acceptance would rest on a line only a launch can
// produce. A test points this at a dword it owns; nullptr (the default, and what production never
// changes) resolves the fact through buildguard as before.

#include <stdint.h>

namespace replay {
namespace camera_module {

void SetRigFlagForTest(const void* flag);

}  // namespace camera_module
}  // namespace replay
