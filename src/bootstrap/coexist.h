// S4.5.3: plugins\ is a shared community folder with at least two independent loader ecosystems, and load
// order is FindFirstFileW order, which is not a guarantee. Six rules; these are the three that are code
// rather than discipline.
//
//   Rule 1  never assume we are alone, never assume order. Presence is DETECTED with GetModuleHandleA
//           and re-checked lazily on the worker thread - never LoadLibrary, and never in DllMain.
//   Rule 2  never take the dbgcore.dll slot. Renaming ourselves would evict whichever loader is
//           installed, so we check our own file name and refuse if it is not ours.
//   Rule 5  clean uninstall = delete one file. Enforced in paths.h; recorded here for the log.
//
// Nothing in this file makes a decision that depends on another plugin. Detection exists so the log can
// say what else was in the room when something went wrong - V-I11 is a T4 sweep with two plugins loaded,
// and a log that does not record the other one cannot support it.
#pragma once

#include <stddef.h>

namespace replay {
namespace coexist {

struct Report {
    bool     ourNameIsTheLoaderSlot;   // we were renamed to dbgcore.dll: refuse
    bool     duplicateInstance;        // a second copy of this plugin is already attached
    bool     rlayLoaded;
    unsigned pluginDllCount;           // files in our own directory whose name contains ".dll"
    unsigned otherPluginDlls;          // ... not counting ourselves
};

// Safe from DllMain: a name comparison and one named-object creation. Returns false when this instance
// must not proceed - either we were renamed into the loader's slot, or another copy of us is already in.
bool ClaimInstance(Report* report);

// GetModuleHandleA only. Never loads anything, so it is safe to call at any time and answers "is it
// loaded right now", which is the only question that has an answer under an unordered loader.
bool IsModuleLoaded(const char* moduleName);

// Worker thread. Enumerates our own directory and writes what shares it into the log.
void Survey(Report* report);

void ReleaseForTest();

}  // namespace coexist
}  // namespace replay
