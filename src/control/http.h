#pragma once

// W2-G. Ledger row 0.16: three routes on the game's own localhost HTTP listener.
//
// ============================================================================================
// WHY THIS SURFACE IS WORTH HAVING, since the owner did not ask for it.
//
// It is a third front-end on a command table the console needs anyway -- roughly a hundred lines
// of adapter over control/command.h and control/dispatch.h, with no command logic of its own. If
// it had needed its own logic it would have been dropped. W1-K left nothing to derive: every value
// below is already in facts/facts.tsv and src/generated/facts_generated.h, so this adds no fact,
// no anchor and no parked read.
//
// It opens no socket. The listener is the engine's, already bound, already serving fifteen stock
// routes; we add three to it. The egress blocker trampolines connect/sendto and permits loopback,
// so nothing here is in its way and nothing here is what it exists to stop.
// ============================================================================================
//
// * TWO RULES FROM W1-K, HELD RATHER THAN DOCUMENTED.
//
//   1. A handler never blocks. It runs inside CR15Game::Update, so a file read or a wait in one is
//      a frame hitch. Nothing here does I/O.
//   2. A handler never mutates transport state. The HTTP pump runs AFTER all three plugin Update
//      slots, so a seek applied in a handler lands mid-frame, after our tick already computed this
//      frame's state. Mutations are queued and drained at the top of the next tick, which is why
//      /replay/command answers "accepted" and not "done".
//
// * THE RETURN VALUE OF THE REGISTRATION CALL CANNOT BE TRUSTED ON ITS OWN. When the listener is
// not bound, sub_1401F9060 takes a short path ending `pop rbx; retn` with no `xor eax,eax`, so RAX
// holds whatever the callee left there. Zero does not mean success. The bound pointer is checked
// first, and only then is the return value read.
//
// * THE COMMAND ROUTE IS POST-ONLY, and that is a decision rather than REST manners. A GET that
// mutates can be fired by anything that makes the machine's browser touch a URL. The command
// travels in the query string either way, so no request body is ever parsed -- which also keeps
// this file away from engine entry points nothing has verified for us.

#include <stddef.h>

#include "control/httpreg.h"

namespace replay {
namespace control {
namespace http {

// Registers the routes on `netGame`'s embedded listener. Tick thread only, and safe to call every
// tick: it does nothing once it has succeeded on THIS netGame, and nothing while netGame is null.
//
// False means not registered, with Diagnostic() saying why. Never fatal -- the console, the panel
// and the launch switch are all unaffected by an HTTP surface that did not come up.
// control.http_routes_live. Outside contained offline playback the routes
// go on the GAME'S listener in a live match, where `record stop` and the directory listing are
// reachable by anything that can reach it. Default false; Start refuses while it is.
void SetLiveAllowed(bool on);

bool Start(void* netGame);

// Unregisters what was registered. Safe when Start never succeeded. `liveNetGame` is the netGame
// the caller can still see -- the routes are only handed back when it is the one they were
// registered on, because after a rebuild the old listener is somebody else's memory.
void Stop(void* liveNetGame);

// True when the routes are live on THIS netGame's current listener. The question the tick has to
// ask: a cross-level load rebuilds the session, and the routes go with the old object.
bool RegisteredOn(void* netGame);

// Forgets the registration without touching the old listener -- for a caller that knows the
// session was rebuilt. The next Start() re-registers on the new one.
void Invalidate(const char* why);

bool Running();

// Never null. Says what happened, whether or not it worked.
const char* Diagnostic();

}  // namespace http
}  // namespace control
}  // namespace replay
