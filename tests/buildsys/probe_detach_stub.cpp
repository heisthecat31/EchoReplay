// The FreeLibrary detach hook the probe DLLs need, and only the probe DLLs.
//
// Every probe compiles src/plugin/dllmain.cpp -- attaching exactly the way the shipped plugin does is
// the whole point of a probe -- and that entry point takes the vectored crash logger out of the chain
// before our pages are unmapped on a FreeLibrary detach. The removal lives in
// pawn_provision.cpp, which no probe links, so at 70032e4 eight probe targets stopped linking with one
// LNK2019 each.
//
// A no-op is the CORRECT body here rather than a convenience: a DLL that cannot reach
// InstallCrashLogger has no handler in the chain to remove. It is not a weakening of the wiring for
// EchoReplay.dll, which carries pawn_provision.cpp and therefore its real RemoveCrashLogger -- and if
// this file were ever linked into that DLL the result would be LNK2005, a loud duplicate symbol, not a
// quiet shadow of the real one.
//
// Who gets it is DERIVED from the link graph in the root CMakeLists.txt (carries dllmain.cpp, does not
// carry pawn_provision.cpp), so nothing here or there names a target and the next probe that appears
// needs no edit.

#include "engine/pawn_provision.h"

namespace replay {
namespace PawnProvision {

void RemoveCrashLogger() {}

}  // namespace PawnProvision
}  // namespace replay
