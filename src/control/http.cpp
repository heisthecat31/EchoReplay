#include "control/http.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "control/command.h"
#include "control/dispatch.h"
#include "control/diagwriter.h"
#include "control/notes.h"
#include "control/urlquery.h"
#include "generated/facts_generated.h"

namespace replay {
namespace control {
namespace http {
namespace {

const char* const kReplayModuleName = "control";

// ---- The engine entry points -----------------------------------------------------------------
//
// Every signature here is W1-K's, read from the call sites rather than from a decompiler's guess --
// its own attempt log records that Hex-Rays typed the registration call with four arguments and a
// void return, and both are wrong.

// FIVE arguments, the fifth on the stack. Getting that wrong is the stack corruption the packet
// brief warns about; a plain five-parameter declaration puts it exactly where the stock call sites
// write it (Win64's 5th integer slot at rsp+0x20).
// RouteRegisterFn and RouteUnregisterFn are declared once, in control/httpreg.h, so the fake a
// test registers through cannot drift from the one this file calls.
using GetOutputHeadersFn = void* (*)(void* req);
using GetOutputBufferFn = void* (*)(void* req);
using AddHeaderFn = int (*)(void* headers, const char* key, const char* value);
using EvbufferAddFn = int (*)(void* buf, const void* data, size_t len);
using SendReplyFn = void (*)(void* req, int code, const char* reason, void* databuf);

struct Engine {
    RouteRegisterFn    routeRegister = nullptr;
    RouteUnregisterFn  routeUnregister = nullptr;
    GetOutputHeadersFn outputHeaders = nullptr;
    GetOutputBufferFn  outputBuffer = nullptr;
    AddHeaderFn        addHeader = nullptr;
    EvbufferAddFn      evbufferAdd = nullptr;
    SendReplyFn        sendReply = nullptr;
};

Engine g_fn;
Registration g_reg;
// Control.http_routes_live, pushed in by the control module, which is the
// one place with a config. False means "contained sessions only", which is the shipped default.
bool g_liveAllowed = false;
char   g_diagnostic[160] = "not started";
uint64_t g_reregistrations = 0;

void SetDiagnostic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_diagnostic, sizeof g_diagnostic, fmt, args);
    va_end(args);
}

// facts.tsv records addresses against the image base the PE declares; the loader may map it
// elsewhere, and this binary is DYNAMICBASE + HIGH_ENTROPY_VA. buildguard is the one place that
// rebases, and this is the only way an address in this file becomes a pointer.
template <typename Fn>
Fn Resolve(uint64_t pcVa) {
    const void* mapped = buildguard::MappedAddress(pcVa);
    return reinterpret_cast<Fn>(const_cast<void*>(mapped));
}

// ---- Routes ----------------------------------------------------------------------------------

const char* const kStatusUri = "/replay/status";
const char* const kHelpUri = "/replay/help";
const char* const kResultUri = "/replay/result";
const char* const kCommandUri = "/replay/command";

void Reply(void* req, int code, const char* json) {
    void* headers = g_fn.outputHeaders(req);
    if (headers != nullptr) {
        g_fn.addHeader(headers, "Content-Type", "application/json; charset=utf-8");
    }
    void* out = g_fn.outputBuffer(req);
    if (out != nullptr && json != nullptr) {
        g_fn.evbufferAdd(out, json, strlen(json));
    }
    // NULL reason, so evhttp_response_code_ looks the phrase up from its own table. The engine's
    // own ReplyWithJson passes "", which produces an empty reason phrase on the wire.
    g_fn.sendReply(req, code, nullptr, out);
}

void AppendJsonString(TextOut* t, const char* s) {
    t->Line("\"");
    for (const char* p = s; *p != 0; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') {
            t->Line("\\%c", c);
        } else if (c < 0x20) {
            t->Line("\\u%04X", static_cast<unsigned>(c));
        } else {
            t->Line("%c", c);
        }
    }
    t->Line("\"");
}

void ReplyStatus(void* req) {
    dispatch::Snapshot s;
    dispatch::ReadSnapshot(&s);

    char info[512];
    dispatch::FormatInfo(s, info, sizeof info);

    char body[2048];
    TextOut t(body, sizeof body);
    t.Line("{\"transport\":%s,", s.transportPresent ? "true" : "false");
    t.Line("\"state\":%d,", s.transportState);
    t.Line("\"rate\":%.6g,", static_cast<double>(s.rate));
    // 0 means UNKNOWN and is emitted as null rather than as a number a client would plot.
    if (s.rateCeiling > 0.0f) {
        t.Line("\"rateCeiling\":%.6g,", static_cast<double>(s.rateCeiling));
    } else {
        t.Line("\"rateCeiling\":null,");
    }
    t.Line("\"playheadUs\":%lld,", static_cast<long long>(s.playheadUs));
    t.Line("\"durationUs\":%llu,", static_cast<unsigned long long>(s.durationUs));
    t.Line("\"asSeen\":%s,", s.asSeen ? "true" : "false");
    t.Line("\"roundsKnown\":%s,", s.roundsKnown ? "true" : "false");
    t.Line("\"roundCount\":%zu,", s.roundCount);
    t.Line("\"panelVisible\":%s,", s.panelVisible ? "true" : "false");
    t.Line("\"browserOpen\":%s,", s.browserOpen ? "true" : "false");
    t.Line("\"degradations\":%u,", s.uiDegradations);
    t.Line("\"queued\":%llu,", static_cast<unsigned long long>(s.queued));
    t.Line("\"executed\":%llu,", static_cast<unsigned long long>(s.executed));
    t.Line("\"refused\":%llu,", static_cast<unsigned long long>(s.refused));
    // A command that RAN and was turned down. "refused" only ever counted pushes a full queue
    // rejected, so a client watching this field saw 0 through a run of refusals.
    t.Line("\"declined\":%llu,", static_cast<unsigned long long>(s.declined));
    t.Line("\"lastSeq\":%llu,", static_cast<unsigned long long>(s.lastSeq));
    // Only an Ok sidecar has a bookmark count. For every other state the count is not "none", it
    // is not known -- the same shape "resumeUs" already publishes two lines down, and a confident
    // 0 here reads to a client exactly like a file with no bookmarks in it.
    if (s.notesState == static_cast<int32_t>(notes::LoadResult::Ok)) {
        t.Line("\"bookmarks\":%zu,", s.bookmarkCount);
    } else {
        t.Line("\"bookmarks\":null,");
    }
    t.Line("\"notesState\":%d,", s.notesState);
    if (s.resumeUs != 0) {
        t.Line("\"resumeUs\":%lld,", static_cast<long long>(s.resumeUs));
    } else {
        t.Line("\"resumeUs\":null,");
    }
    t.Line("\"egressArmed\":%s,", s.egressArmed ? "true" : "false");
    t.Line("\"status\":");
    AppendJsonString(&t, s.uiStatus);
    t.Line(",\"info\":");
    AppendJsonString(&t, info);
    t.Line(",\"lastResult\":");
    AppendJsonString(&t, s.lastResult);
    t.Line("}");
    Reply(req, t.Truncated() ? 500 : 200, body);
}

void ReplyHelp(void* req) {
    // Rendered from the same table the console prints, so the two surfaces cannot advertise
    // different commands.
    char body[4096];
    TextOut t(body, sizeof body);
    t.Line("{\"commands\":[");
    for (size_t i = 0; i < SpecCount(); ++i) {
        const CommandSpec& spec = SpecAt(i);
        t.Line(i == 0 ? "{\"name\":" : ",{\"name\":");
        AppendJsonString(&t, spec.name);
        t.Line(",\"usage\":");
        AppendJsonString(&t, spec.usage);
        t.Line(",\"help\":");
        AppendJsonString(&t, spec.help);
        t.Line("}");
    }
    t.Line("]}");
    Reply(req, t.Truncated() ? 500 : 200, body);
}

void ReplyCommand(void* req, const char* uri) {
    char line[kMaxLineChars];
    if (!QueryValue(uri, "line", line, sizeof line)) {
        Reply(req, 400,
              "{\"ok\":false,\"error\":\"expected ?line=<command>, percent-encoded\"}");
        return;
    }

    // The query arrives as bytes. Widening here, at the edge, is the only conversion in the path;
    // the parser itself is wide-char native so a path with a non-ASCII character survives it.
    wchar_t wide[kMaxLineChars];
    const int got = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, wide,
                                        static_cast<int>(kMaxLineChars));
    if (got <= 0) {
        Reply(req, 400, "{\"ok\":false,\"error\":\"the command is not valid UTF-8\"}");
        return;
    }

    const ParseResult parsed = Parse(wide);
    char body[2560];
    TextOut t(body, sizeof body);
    if (!parsed.ok) {
        t.Line("{\"ok\":false,\"error\":");
        AppendJsonString(&t, parsed.error);
        t.Line("}");
        Reply(req, 400, body);
        return;
    }

    // Submit answers a read from the published snapshot and QUEUES a write. A 202 on the write is
    // the honest code: the command is accepted, and it runs at the top of the next tick.
    const dispatch::Result r = dispatch::Submit(parsed.cmd);
    const bool immediate = dispatch::IsImmediate(parsed.cmd.id);
    // ★ THE SEQUENCE NUMBER IS THE WHOLE REASON A 202 IS HONEST. Without it the client is told
    // "accepted" and has no way to ever learn what happened; with it, GET /replay/result?seq=N
    // answers, and so does lastSeq on /replay/status. The handler still does not wait -- W1-K's
    // rule that a handler never blocks is why the answer is collected rather than returned.
    t.Line("{\"ok\":%s,\"queued\":%s,\"seq\":%llu,\"result\":", r.ok ? "true" : "false",
           immediate ? "false" : "true", static_cast<unsigned long long>(r.seq));
    AppendJsonString(&t, r.text);
    t.Line("}");
    Reply(req, r.ok ? (immediate ? 200 : 202) : 409, body);
}

// The outcome of one queued command, by the number POST /replay/command handed back. A read from
// the dispatcher's result ring: no file, no lock, no wait -- everything a handler is allowed to do.
void ReplyResult(void* req, const char* uri) {
    char seqText[32];
    if (!QueryValue(uri, "seq", seqText, sizeof seqText)) {
        Reply(req, 400, "{\"ok\":false,\"error\":\"expected ?seq=<number from the command reply>\"}");
        return;
    }
    unsigned long long seq = 0;
    for (const char* p2 = seqText; *p2 != 0; ++p2) {
        if (*p2 < '0' || *p2 > '9' || seq > 0xFFFFFFFFFFFFull) {
            Reply(req, 400, "{\"ok\":false,\"error\":\"seq is a decimal number\"}");
            return;
        }
        seq = seq * 10 + static_cast<unsigned long long>(*p2 - '0');
    }

    dispatch::Result done;
    char body[2560];
    TextOut t(body, sizeof body);
    if (!dispatch::TakeResult(seq, &done)) {
        // Pending and displaced are one answer on purpose: both mean "this ring does not hold that
        // number", and claiming to know which would be a guess. 204 rather than 404 -- the request
        // was fine, there is simply nothing yet.
        t.Line("{\"ok\":false,\"pending\":true,\"seq\":%llu}", seq);
        Reply(req, 204, body);
        return;
    }
    t.Line("{\"ok\":%s,\"pending\":false,\"seq\":%llu,\"result\":", done.ok ? "true" : "false",
           seq);
    AppendJsonString(&t, done.text);
    t.Line("}");
    Reply(req, 200, body);
}

void StatusHandler(void* req, void*) { ReplyStatus(req); }

void HelpHandler(void* req, void*) { ReplyHelp(req); }

const char* RequestUri(void* req) {
    return *reinterpret_cast<const char* const*>(static_cast<uint8_t*>(req) +
                                                 facts::HTTP_REQ_URI_OFF);
}

void CommandHandler(void* req, void*) {
    // Never null inside a route handler: the dispatcher diverts a null uri to evhttp_send_error
    // before the route list is walked (W1-K 7.4). Checked anyway, because this is engine memory.
    const char* uri = RequestUri(req);
    if (uri == nullptr) {
        Reply(req, 400, "{\"ok\":false,\"error\":\"no uri\"}");
        return;
    }
    ReplyCommand(req, uri);
}

void ResultHandler(void* req, void*) {
    const char* uri = RequestUri(req);
    if (uri == nullptr) {
        Reply(req, 400, "{\"ok\":false,\"error\":\"no uri\"}");
        return;
    }
    ReplyResult(req, uri);
}

bool ResolveEntryPoints() {
    // The one site we CALL with arguments of our own construction is prologue-guarded. VerifySite
    // rebases, range-checks and compares in one call, which is also how another mod's hook at the
    // same address is detected rather than being called through.
    if (!buildguard::VerifySite("HTTP.ROUTE_REGISTER", facts::HTTP_ROUTE_REGISTER,
                                facts::HTTP_ROUTE_REGISTER_PROLOGUE,
                                sizeof facts::HTTP_ROUTE_REGISTER_PROLOGUE)) {
        SetDiagnostic("HTTP.ROUTE_REGISTER does not hold its recorded prologue; not registering");
        return false;
    }

    g_fn.routeRegister = Resolve<RouteRegisterFn>(facts::HTTP_ROUTE_REGISTER);
    g_fn.routeUnregister = Resolve<RouteUnregisterFn>(facts::HTTP_UNREGISTER);
    g_fn.outputHeaders = Resolve<GetOutputHeadersFn>(facts::HTTP_GET_OUTPUT_HEADERS);
    g_fn.outputBuffer = Resolve<GetOutputBufferFn>(facts::HTTP_GET_OUTPUT_BUFFER);
    g_fn.addHeader = Resolve<AddHeaderFn>(facts::HTTP_ADD_HEADER);
    g_fn.evbufferAdd = Resolve<EvbufferAddFn>(facts::HTTP_EVBUFFER_ADD);
    g_fn.sendReply = Resolve<SendReplyFn>(facts::HTTP_SEND_REPLY);

    const void* const all[] = {
        reinterpret_cast<const void*>(g_fn.routeRegister),
        reinterpret_cast<const void*>(g_fn.routeUnregister),
        reinterpret_cast<const void*>(g_fn.outputHeaders),
        reinterpret_cast<const void*>(g_fn.outputBuffer),
        reinterpret_cast<const void*>(g_fn.addHeader),
        reinterpret_cast<const void*>(g_fn.evbufferAdd),
        reinterpret_cast<const void*>(g_fn.sendReply),
    };
    for (const void* p : all) {
        if (p == nullptr) {
            // MappedAddress returns null when the address is outside this image, which means the
            // rebase did not land -- exactly the silent failure this project has been bitten by.
            SetDiagnostic("an HTTP entry point did not rebase into the mapped image");
            g_fn = Engine();
            return false;
        }
    }
    return true;
}

// The listener is at a fixed offset inside CR15NetGame and its bound evhttp one indirection
// further. Null for a null netGame, so the identity of "no session" is a clean pair of nulls.
const void* LiveEvhttp(void* netGame) {
    if (netGame == nullptr) return nullptr;
    uint8_t* const listener = static_cast<uint8_t*>(netGame) + facts::HTTP_LISTENER_OFF;
    return *reinterpret_cast<void* const*>(listener + facts::HTTP_LISTENER_EVHTTP_OFF);
}

}  // namespace

bool RegisteredOn(void* netGame) {
    return !NeedsReregister(g_reg.running, g_reg.netGame, g_reg.evhttp, netGame,
                            LiveEvhttp(netGame));
}

void Invalidate(const char* why) {
    if (!g_reg.running) return;
    ++g_reregistrations;
    log::Direct(LogLevel::Warn, kReplayModuleName,
                "http routes dropped: %s (they were registered on netGame %p / evhttp %p). The "
                "listener is that object's, so nothing is unregistered through it.",
                why, g_reg.netGame, g_reg.evhttp);
    DropLatch(&g_reg);
    SetDiagnostic("the session was rebuilt (%s); the routes are not registered", why);
}

void SetLiveAllowed(bool on) { g_liveAllowed = on; }

bool Start(void* netGame) {
    // ★ THE ROUTES GO ON THE GAME'S OWN LISTENER, AND IN A LIVE SESSION
    // THAT LISTENER IS NOT OURS TO EXTEND. No verb here can write engine state live -- every one of
    // those is gated separately -- but `record stop` ends the owner's recording of a live match and
    // `list` enumerates his replay directory, and the endpoint is unauthenticated and reachable by
    // anything that can reach echovr's listener. So: contained offline playback, or an explicit
    // `control.http_routes_live=true`. The in-game console and the recorder surface are how
    // recording is controlled live. Gated HERE rather than at the caller because the per-tick
    // re-registration (DIAG-LEVEL-MISMATCH-404) comes back through this same door.
    if (!livesafety::HttpRoutesAllowed(g_liveAllowed)) {
        SetDiagnostic("refused: this session is not contained offline playback, and "
                      "control.http_routes_live is off");
        return false;
    }
    if (RegisteredOn(netGame) && g_reg.running) {
        return true;
    }
    // A caller that reached here on a live session it is not registered against has not invalidated
    // yet. Drop the stale record first: registering a second time while still holding the old
    // listener would leave Stop() unregistering through a freed object.
    if (g_reg.running) {
        Invalidate("a later Start found a different CR15NetGame");
    }
    if (netGame == nullptr) {
        SetDiagnostic("no CR15NetGame yet");
        return false;
    }
    if (g_fn.routeRegister == nullptr && !ResolveEntryPoints()) {
        return false;   // reason already set
    }

    void* listener = static_cast<uint8_t*>(netGame) + facts::HTTP_LISTENER_OFF;

    // ============================================================================================
    // THE BOUND CHECK COMES FIRST, AND IT IS THE ONLY SOUND TEST.
    //
    // When *(listener + 8) is null, sub_1401F9060 takes a short path ending `pop rbx; retn` at
    // 0x1401F90AE with no `xor eax,eax` anywhere on it -- RAX holds whatever the callee left in it.
    // So a zero return does NOT mean the route was registered. Reading the return value is only
    // meaningful once this pointer is known non-null.
    // ============================================================================================
    const void* evhttp =
        *reinterpret_cast<void* const*>(static_cast<uint8_t*>(listener) +
                                        facts::HTTP_LISTENER_EVHTTP_OFF);
    if (evhttp == nullptr) {
        SetDiagnostic("the engine's HTTP listener is not bound; nothing to register on");
        return false;
    }

    // A confirmation, not a gate. W1-K named bit 0x4000 as an "HTTP subsystem live" latch that is
    // co-extensive with the bound listener, so a bound listener with the bit clear would mean that
    // reading is wrong -- worth a line in the log, not worth refusing over.
    const uint64_t* const* roleFlags = reinterpret_cast<const uint64_t* const*>(
        static_cast<uint8_t*>(netGame) + facts::NETGAME_ROLEFLAGS_OFF);
    if (roleFlags != nullptr && *roleFlags != nullptr && (**roleFlags & 0x4000ull) == 0) {
        log::Direct(LogLevel::Warn, kReplayModuleName,
                    "the listener is bound but roleflags bit 0x4000 is clear; the pump may not run "
                    "and W1-K's reading of that latch would need revisiting");
    }

    const unsigned kGet = facts::HTTP_WRAPPER_METHOD_GET;
    const unsigned kPost = facts::HTTP_WRAPPER_METHOD_POST;

    // The command route is POST-only. A mutating GET can be fired by anything that gets this
    // machine's browser to touch a URL, and the surface exists for tooling that can issue a POST.
    const Route routes[] = {
        {kStatusUri, &StatusHandler, kGet},
        {kHelpUri, &HelpHandler, kGet},
        {kResultUri, &ResultHandler, kGet},
        {kCommandUri, &CommandHandler, kPost},
    };

    const size_t total = sizeof routes / sizeof routes[0];
    size_t already = 0;
    const size_t registered =
        Apply(&g_reg, g_fn.routeRegister, listener, netGame, evhttp, routes, total, &already);
    if (already != 0) {
        // -1 is "a callback for that exact URI already exists", and the callee does NOT replace it.
        // Reported so a second plugin claiming our URI is diagnosable.
        log::Direct(LogLevel::Warn, kReplayModuleName,
                    "%zu of %zu routes were already present on this listener and were left alone",
                    already, total);
    }
    if (registered == 0) {
        SetDiagnostic("no route registered on the bound listener");
        return false;
    }
    SetDiagnostic("%zu/%zu routes on the engine's listener (netGame %p, %llu re-registration(s)): "
                  "%s, %s, %s, %s (POST)",
                  registered, total, netGame,
                  static_cast<unsigned long long>(g_reregistrations), kStatusUri, kHelpUri,
                  kResultUri, kCommandUri);
    return true;
}

void Stop(void* liveNetGame) {
    // Unregistered rather than left dangling: the listener outlives this module on a FreeLibrary,
    // and a route pointing into an unmapped DLL is a crash on the next request. But only through
    // the identity we registered on -- after a session rebuild that pointer is somebody else's,
    // which is why the evhttp read happens only once the netGame has already matched.
    const void* liveEvhttp = (liveNetGame != nullptr && liveNetGame == g_reg.netGame)
                                 ? LiveEvhttp(liveNetGame)
                                 : nullptr;
    const char* const uris[] = {kStatusUri, kHelpUri, kResultUri, kCommandUri};
    const bool freed = Release(&g_reg, g_fn.routeUnregister, liveNetGame, liveEvhttp, uris,
                               sizeof uris / sizeof uris[0]);
    SetDiagnostic(freed ? "stopped" : "stopped without unregistering: the listener was not ours");
}

bool Running() { return g_reg.running; }

const char* Diagnostic() { return g_diagnostic; }

}  // namespace http
}  // namespace control
}  // namespace replay
