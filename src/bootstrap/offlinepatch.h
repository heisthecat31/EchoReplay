#pragma once

// The in-process equivalent of EchoRelay's `-offline -windowed`, for replay viewing only.
//
// WHY. Without rlay.dll the stock game does not know `-offline` or `-windowed`, and one unregistered
// switch makes the engine discard the whole command line. So a replay launch cannot ask for an offline
// local host at all unless rlay is installed. The owner asked for the offline patch to work without
// rlay, and not to conflict with it where it is installed.
//
// WHAT. Four code bytes sites (offlinepatch_facts.h) and the windowed mode bit, all applied from a
// wrapper around slot 0 of the CR15Game vtable (CLI.APPLY, the same moment rlay applies its own):
//   before the original runs   OR the windowed bit into CR15Game+0x7AE0, which CLI.APPLY reads;
//   after it returns           classify every site: all original -> write ours; all already patched
//                              (rlay did it inside that same call) -> write nothing; anything else ->
//                              refuse every site and log each foreign byte string.
// The executable on disk is never touched; only the mapped image of this process changes.
//
// WHEN. Only when this process is a replay session: ECHOREPLAY_OPEN is set, and so
// bootstrap::ClassifySession answers Replay. In every other session -- a live match above all --
// nothing is installed, not even the vtable slot, and boot says so in one line.
//
// The slot swap runs at DllMain (REPLAY_ATTACH_HOOK), because CLI.APPLY runs on the main thread
// before our worker exists. It is a pointer store into .rdata: no thread is frozen and nothing is
// loaded, so it is safe under the loader lock. rlay detours CLI.APPLY's CODE, not the table, so our
// wrapper calls through to whatever the function's entry now is, rlay's detour included.

#include <stddef.h>
#include <stdint.h>

namespace replay::offlinepatch {

enum class Status : uint8_t {
    NotRequested,    // not a replay session, or session.offline_patch is false: nothing installed
    Pending,         // the slot is ours; CLI.APPLY has not run yet
    Applied,         // we wrote every site
    AlreadyApplied,  // every site already held the patched bytes (rlay's -offline): nothing written
    RefusedBuild,    // this is not the echovr.exe the sites were read from
    RefusedSlot,     // the vtable slot did not hold CLI.APPLY: somebody else owns it
    RefusedBytes,    // a site held neither the original nor the patched bytes
    WriteFailed,     // VirtualProtect refused; the sites written before it were restored
};

const char* StatusName(Status s);

// True for the three statuses under which the boot-to-multiplayer arm hosts an offline match.
// Pending counts: the arm cannot run before CLI.APPLY, and CLI.APPLY is what settles it.
bool OfflineHostExpected(Status s);

enum class SiteState : uint8_t { Original, Patched, Foreign };

SiteState ClassifySite(const uint8_t* live, size_t siteIndex);

enum class Plan : uint8_t { Write, AlreadyApplied, Refuse };

// All Original -> Write; all Patched -> AlreadyApplied; any Foreign or any mix -> Refuse. A mix is
// refused rather than completed: a half-applied table is somebody else's work we do not understand.
Plan PlanSites(const SiteState* states, size_t count);

// Classifies, plans and (on Write) patches every site of an image mapped at `image`. The same path the
// wrapper takes, exposed so a test can drive it over a buffer.
Status PatchImage(uint8_t* image);

// The live process decision, logged. Called once from the attach hook.
bool InstallForThisProcess();

Status Current();

// The spans this process wrote, as RVAs, so SC7 can attribute them to us rather than report them as
// undeclared. Empty unless Current() == Applied.
struct Span {
    uint64_t rva;
    uint32_t len;
};
size_t WrittenSpans(Span* out, size_t cap);

// Test seams. InstallAt swaps `slot` (which must hold `expected`) for the wrapper, with `image` as the
// base the sites are patched in. ResetForTest puts every piece of module state back.
bool InstallAt(void** slot, void* expected, uint8_t* image);
void ResetForTest();

}  // namespace replay::offlinepatch
