// Drives the wiretap at the recorded rate, in this process, with no game and no socket.
//
// The chain is: map echovr.exe (engineimage.cpp), build a CR15NetGame whose CBroadcaster the
// engine's own container code initialised (synthengine.cpp), register through the shipped
// IWiretap -- which calls the engine's real NRadEngine::CBroadcaster::Listen -- and then invoke
// each listener node the way both engine dispatchers do, through node+40, feeding it R4's
// captured Echo Arena session at the intervals that session actually had.
//
// This is what docs/spikes/W1-L.md section 9 check 3 became. The check it replaces asked for
// ~44 Hz of live remote traffic, which docs/HARNESS.md section 4a refuses at layer 2 and would
// be right to; the same 44.02 Hz is here, out of freshcap/arena.bin, and unlike a live match it
// is the same bytes in the same order on every run.
//
// Exit codes are the verdict. tests/wiretap/test_feed.cpp asserts them, so a mutant that
// produces nothing cannot pass by never having got as far as loading the engine image.

#include <windows.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ratio>
#include <string>
#include <vector>

#include "engine/wiretap.h"
#include "engine/wiretap_facts.h"
#include "engineimage.h"
#include "synthengine.h"
#include "wirefeed.h"
#include "../local_inputs.h"

namespace {

using namespace replay::wiretap_facts;
using replay::IWiretap;

enum Exit {
    kPass = 0,
    kUnavailable = 2,   // the wiretap refused; Diagnostic() says why
    kNodeAudit = 3,     // the engine wrote a node W1-L cannot dispatch through
    kRegistration = 4,  // Listen did not return a handle
    kDelivery = 5,      // the feed did not arrive as the capture says it should
    kTeardown = 6,      // Unlisten / UnlistenAll did not stop delivery, or a guard did not hold
    kPacing = 7,        // the feed did not take the time the capture takes
    kEngineImage = 10,  // could not map echovr.exe
    kSynthEngine = 11,  // could not build the broadcaster out of engine code
    kNoCorpus = 12,     // the capture is not on this machine
};


std::string FromEnv(const char* name, const char* fallback) {
    char   value[MAX_PATH]{};
    size_t len = 0;
    getenv_s(&len, value, sizeof value, name);
    return len > 1 ? std::string(value) : std::string(fallback);
}

struct Stream {
    const char* name = nullptr;
    uint64_t    symbol = 0;
    uint16_t    handle = IWiretap::kInvalidHandle;
    uint64_t    fed = 0;
    uint64_t    delivered = 0;
    uint64_t    wrong = 0;  // arrived, but not as the bytes that were fed
    uint64_t    firstNs = 0;
    uint64_t    lastNs = 0;
    uint32_t    bodyMin = 0;
    uint32_t    bodyMax = 0;
};

Stream gStreams[3];

const uint8_t* gExpectBody = nullptr;
uint32_t       gExpectBytes = 0;
uint64_t       gExpectStamp = 0;
uint64_t       gExpectPeer = 0;
DWORD          gSinkThread = 0;
IWiretap*      gTap = nullptr;
bool           gReentrantReached = false;
uint16_t       gReentrantResult = 0;

void Sink(const replay::WireMessage& msg, void* ctx) {
    auto* stream = static_cast<Stream*>(ctx);
    ++stream->delivered;
    gSinkThread = GetCurrentThreadId();
    if (msg.bytes != gExpectBytes || msg.stamp != gExpectStamp || msg.peer != gExpectPeer ||
        (gExpectBytes != 0 && std::memcmp(msg.data, gExpectBody, gExpectBytes) != 0)) {
        ++stream->wrong;
    }
}

// Registration from inside a dispatch is the one thing the engine has no deferral for, so the
// wiretap refuses it rather than relinking a chain a dispatcher is walking. Proving the refusal
// needs a live callback, and this is the only place there is one.
void ProbeReentrantListen(const replay::WireMessage&, void* ctx) {
    if (gReentrantReached) {
        return;
    }
    gReentrantReached = true;
    gReentrantResult = gTap->Listen(kNamePlayerStateMsg, Sink, ctx);
}

int64_t QpcNow() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

int64_t QpcHz() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

// Sleep down to the last two milliseconds, then spin. At 44 Hz the interval is 22.7 ms and the
// scheduler's granularity is a good fraction of that, so sleeping the whole way would make the
// measured elapsed time a measurement of Sleep rather than of the feed.
void WaitUntil(int64_t startTicks, int64_t hz, uint64_t targetNs) {
    // std::nano::den rather than a spelled divisor: one multiply then one divide keeps the whole
    // 125 s in exact ticks, where two divisions would drop the sub-microsecond remainder on every
    // one of 23,932 messages.
    const int64_t deadline =
        startTicks + static_cast<int64_t>(targetNs * static_cast<uint64_t>(hz) / std::nano::den);
    for (;;) {
        const int64_t remaining = deadline - QpcNow();
        if (remaining <= 0) {
            return;
        }
        if (remaining > hz / 500) {
            Sleep(1);
        } else {
            YieldProcessor();
        }
    }
}

using ProxyFn = void(__fastcall*)(void*, void**, const void*, uint64_t, uint64_t, uint64_t);

struct Invocable {
    ProxyFn  proxy = nullptr;
    void*    context = nullptr;
    void**   memberSlot = nullptr;
    uint64_t symbol = 0;
    Stream*  stream = nullptr;
};

bool IsExecutable(const void* at) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(at, &info, sizeof info) != sizeof info) {
        return false;
    }
    const DWORD exec =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & exec) != 0;
}

// A path in a detail string would otherwise put a raw backslash in the document and the whole
// verdict would be lost to a parse error, which is how sweep 4 lost one.
std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c >= 0x20) {
            out.push_back(c);
        } else {
            out.push_back(' ');
        }
    }
    return out;
}

void WriteVerdict(const char* path, int code, const std::string& detail, uint64_t checked,
                  double elapsedS, uint64_t spanNs) {
    if (path == nullptr) {
        return;
    }
    std::FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
        return;
    }
    const double span = static_cast<double>(spanNs) / 1e9;
    std::fprintf(f, "{\n  \"probe\": \"W1-L-feed\",\n  \"verdict\": \"%s\",\n  \"exit\": %d,\n",
                 code == kPass ? "PASS" : (code >= kEngineImage ? "NOT-RUN" : "FAIL"), code);
    std::fprintf(f, "  \"checked\": %" PRIu64 ",\n  \"detail\": \"%s\",\n", checked,
                 JsonEscape(detail).c_str());
    std::fprintf(f, "  \"elapsedSeconds\": %.3f,\n  \"captureSpanSeconds\": %.6f,\n", elapsedS,
                 span);
    std::fprintf(f, "  \"sinkThread\": %lu,\n  \"streams\": [\n", gSinkThread);
    for (size_t i = 0; i < 3; ++i) {
        const Stream& s = gStreams[i];
        const double own = static_cast<double>(s.lastNs - s.firstNs) / 1e9;
        std::fprintf(f,
                     "    { \"name\": \"%s\", \"symbol\": \"0x%016" PRIX64 "\", \"handle\": %u, "
                     "\"fed\": %" PRIu64 ", \"delivered\": %" PRIu64 ", \"wrong\": %" PRIu64
                     ", \"bodyBytesMin\": %u, \"bodyBytesMax\": %u, \"hzOverCapture\": %.4f, "
                     "\"hzOverStream\": %.4f }%s\n",
                     s.name != nullptr ? s.name : "", s.symbol, s.handle, s.fed, s.delivered,
                     s.wrong, s.bodyMin, s.bodyMax,
                     span > 0 ? static_cast<double>(s.delivered) / span : 0.0,
                     own > 0 ? static_cast<double>(s.delivered) / own : 0.0, i + 1 < 3 ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    const char* verdictPath = argc > 1 ? argv[1] : nullptr;
    // Unpaced replay feeds the same bytes in the same order and asserts the same counts; it
    // just cannot say anything about the rate, so it never reports the pacing check as passed.
    const bool paced = FromEnv("REPLAY_WIRETAP_FAST", "0") != "1";

    gStreams[0].name = kNamePlayerStateMsg;
    gStreams[1].name = kNameDeltaUnreliableVars;
    gStreams[2].name = kNameComponentEvent;

    w1l::EngineImage image;
    std::string error;
    const std::string exe = replay_test::FindEchoVrExeA();
    const std::wstring wide(exe.begin(), exe.end());
    if (!image.Map(wide.c_str(), error)) {
        std::printf("FEEDHOST engine image: %s\n", error.c_str());
        WriteVerdict(verdictPath, kEngineImage, error, 0, 0, 0);
        return kEngineImage;
    }
    std::printf("FEEDHOST image at %p, %u imports bound, %u poisoned\n",
                reinterpret_cast<void*>(image.Base()), image.BoundImports(),
                image.PoisonedImports());

    w1l::SynthNetGame game;
    if (!game.Build(image, error)) {
        std::printf("FEEDHOST synth engine: %s\n", error.c_str());
        WriteVerdict(verdictPath, kSynthEngine, error, 0, 0, 0);
        return kSynthEngine;
    }

    w1l::WireFeed feed;
    const std::string corpus = FromEnv("REPLAY_FRESHCAP_DIR", "");
    if (!feed.Load(corpus, error)) {
        std::printf("FEEDHOST corpus: %s\n", error.c_str());
        WriteVerdict(verdictPath, kNoCorpus, error, 0, 0, 0);
        return kNoCorpus;
    }
    std::printf("FEEDHOST corpus %s: %" PRIu64 " datagrams, %zu messages, %.6f s\n",
                corpus.c_str(), feed.Datagrams(), feed.Messages().size(),
                static_cast<double>(feed.SpanNs()) / 1e9);

#if defined(W1L_MUT_WRONG_BUS)
    // The control for "registers and never fires". The wiretap is handed a CR15NetGame whose
    // broadcaster slot holds a second bus, so registration succeeds against real engine code
    // and returns a real handle -- and the feed, which dispatches through the first bus, never
    // reaches it. That is the shape R10's and R12's rival Listen addresses would have produced,
    // and the known-answer control on the symbols cannot see it.
    void* const netGame = game.RivalNetGame();
#else
    void* const netGame = game.NetGame();
#endif
    gTap = replay::WiretapCreateAt(netGame, reinterpret_cast<const void*>(image.Base()));
    if (gTap == nullptr || !gTap->Available()) {
        const std::string why =
            gTap != nullptr ? gTap->Diagnostic() : "WiretapCreateAt returned null";
        std::printf("FEEDHOST wiretap unavailable: %s\n", why.c_str());
        WriteVerdict(verdictPath, kUnavailable, why, 0, 0, feed.SpanNs());
        return kUnavailable;
    }
    std::printf("FEEDHOST wiretap: %s\n", gTap->Diagnostic());

    uint64_t checked = 0;
    for (Stream& s : gStreams) {
        s.symbol = gTap->Symbol(s.name);
        s.handle = gTap->Listen(s.name, Sink, &s);
        ++checked;
        std::printf("FEEDHOST listen %-40s 0x%016" PRIX64 " -> %u\n", s.name, s.symbol, s.handle);
        if (s.handle == IWiretap::kInvalidHandle || s.symbol == 0) {
            WriteVerdict(verdictPath, kRegistration, "Listen returned no handle", checked, 0,
                         feed.SpanNs());
            return kRegistration;
        }
    }

    // Read back what the engine wrote, and refuse to dispatch through anything that is not
    // code. A node whose invoker is a data pointer is the shape this packet believed before it
    // read the two dispatchers, and calling it would be an access violation, not a verdict.
    // The handle Listen returns is the node's index in the pool: the engine reads the index out
    // of its free chain, writes the node, and returns that index. Reading node[handle] back is
    // therefore both the layout audit and a check on that identity, since a node at the wrong
    // index would not carry the symbol the tap asked for.
    void* const fedBus = game.Broadcaster();
    std::printf("FEEDHOST pool holds %" PRIu64 " nodes\n", w1l::ListenerNodeCount(fedBus));
    std::vector<Invocable> invocables;
    for (Stream& s : gStreams) {
        w1l::ListenerNode node{};
        ++checked;
        if (!w1l::ReadListenerNode(fedBus, s.handle, node)) {
            // No node here at all: the tap went somewhere else. Deliberately not a node-audit
            // failure -- the feed is what has to notice that, and it does.
            std::printf("FEEDHOST no node %u on the bus being fed\n", s.handle);
            continue;
        }
        std::printf("FEEDHOST node %u sym=0x%016" PRIX64 " skip=%u ctx=%p member=%p invoker=%p\n",
                    s.handle, node.symbol, node.skip, node.context, node.memberSlot,
                    node.invoker);
        if (node.symbol != s.symbol || node.skip != 0) {
            WriteVerdict(verdictPath, kNodeAudit, "the engine wrote a node W1-L did not ask for",
                         checked, 0, feed.SpanNs());
            return kNodeAudit;
        }
        if (node.invoker == nullptr || !IsExecutable(node.invoker)) {
            std::printf("FEEDHOST node %u invoker is not code: both engine dispatchers call "
                        "node+40, so a tap in this shape can never fire\n",
                        s.handle);
            WriteVerdict(verdictPath, kNodeAudit, "node+40 does not hold executable code",
                         checked, 0, feed.SpanNs());
            return kNodeAudit;
        }
        Invocable one;
        one.proxy = reinterpret_cast<ProxyFn>(node.invoker);
        one.context = node.context;
        one.memberSlot = w1l::ListenerMemberSlot(fedBus, s.handle);
        one.symbol = node.symbol;
        one.stream = &s;
        invocables.push_back(one);
    }

    const int64_t hz = QpcHz();
    const int64_t started = QpcNow();
    for (const w1l::FeedMessage& m : feed.Messages()) {
        Invocable* target = nullptr;
        for (Invocable& inv : invocables) {
            if (inv.symbol == m.symbol) {
                target = &inv;
                break;
            }
        }
        if (target == nullptr) {
            continue;
        }
        if (paced) {
            WaitUntil(started, hz, m.offsetNs);
        }
        gExpectBody = feed.Body(m);
        gExpectBytes = m.bodyBytes;
        gExpectStamp = m.offsetNs;
        gExpectPeer = m.fromServer ? 1u : 0u;
        if (Stream* s = target->stream) {
            if (s->fed == 0) {
                s->firstNs = m.offsetNs;
                s->bodyMin = m.bodyBytes;
                s->bodyMax = m.bodyBytes;
            }
            s->bodyMin = m.bodyBytes < s->bodyMin ? m.bodyBytes : s->bodyMin;
            s->bodyMax = m.bodyBytes > s->bodyMax ? m.bodyBytes : s->bodyMax;
            s->lastNs = m.offsetNs;
            ++s->fed;
        }
        target->proxy(target->context, target->memberSlot, gExpectBody, gExpectBytes,
                      gExpectStamp, gExpectPeer);
    }
    const double elapsed = static_cast<double>(QpcNow() - started) / static_cast<double>(hz);
    const double span = static_cast<double>(feed.SpanNs()) / 1e9;

    uint64_t delivered = 0;
    for (const Stream& s : gStreams) {
        delivered += s.delivered;
        checked += s.fed;
        const double own = static_cast<double>(s.lastNs - s.firstNs) / 1e9;
        std::printf("FEEDHOST %-40s fed %6" PRIu64 " delivered %6" PRIu64 " wrong %" PRIu64
                    " %u-%uB %.4f Hz over the capture, %.4f Hz over its own span\n",
                    s.name, s.fed, s.delivered, s.wrong, s.bodyMin, s.bodyMax,
                    span > 0 ? static_cast<double>(s.delivered) / span : 0.0,
                    own > 0 ? static_cast<double>(s.delivered) / own : 0.0);
    }
    std::printf("FEEDHOST fed in %.3f s of wall clock against a %.6f s capture (paced=%d)\n",
                elapsed, span, paced ? 1 : 0);

    if (delivered == 0) {
        WriteVerdict(verdictPath, kDelivery,
                     "the tap registered and never fired: nothing dispatched on this bus "
                     "reached it",
                     checked, elapsed, feed.SpanNs());
        return kDelivery;
    }
    for (const Stream& s : gStreams) {
        if (s.delivered != s.fed || s.wrong != 0) {
            WriteVerdict(verdictPath, kDelivery,
                         "a stream lost messages or delivered bytes that were not fed", checked,
                         elapsed, feed.SpanNs());
            return kDelivery;
        }
    }
    ++checked;
    if (paced && (elapsed < span * 0.98 || elapsed > span * 1.10)) {
        WriteVerdict(verdictPath, kPacing, "the feed did not take the time the capture takes",
                     checked, elapsed, feed.SpanNs());
        return kPacing;
    }

    // Teardown on the same live structure. A dispatch that arrives after UnlistenAll must be
    // refused: in the game the node can outlive the removal, so the tap has to be the thing
    // that stops, not the pool.
    const uint64_t beforeTeardown = delivered;
    gTap->UnlistenAll();
    ++checked;
    for (const Invocable& inv : invocables) {
        const uint8_t byte = 0;
        inv.proxy(inv.context, inv.memberSlot, &byte, 1, 0, 0);
    }
    uint64_t afterTeardown = 0;
    for (const Stream& s : gStreams) {
        afterTeardown += s.delivered;
    }
    std::printf("FEEDHOST deliveries %" PRIu64 " -> %" PRIu64 " across UnlistenAll\n",
                beforeTeardown, afterTeardown);
    ++checked;
    if (afterTeardown != beforeTeardown) {
        WriteVerdict(verdictPath, kTeardown, "a dispatch arriving after UnlistenAll was accepted",
                     checked, elapsed, feed.SpanNs());
        return kTeardown;
    }

    // Install and remove 200 times against the engine's own pool. The pool is an array that
    // only grows, so what this shows is that handles stay valid and nothing refuses partway --
    // and it records the growth, which is what a later packet needs to size kMaxTaps against.
    const uint64_t poolBefore = w1l::ListenerNodeCount(fedBus);
    for (int i = 0; i < 200; ++i) {
        const uint16_t handle = gTap->Listen(kNamePlayerStateMsg, Sink, &gStreams[0]);
        if (handle == IWiretap::kInvalidHandle) {
            WriteVerdict(verdictPath, kRegistration, "Listen failed during the churn cycle",
                         checked, elapsed, feed.SpanNs());
            return kRegistration;
        }
        gTap->Unlisten(handle);
    }
    ++checked;
    std::printf("FEEDHOST pool %" PRIu64 " -> %" PRIu64 " across 200 install/remove cycles\n",
                poolBefore, w1l::ListenerNodeCount(fedBus));

    // Registration from inside a dispatch. Only this tap's own node is driven: the pool is an
    // array the engine never compacts, and the nodes the churn left behind are unlinked from
    // the bucket chain, so dispatching them would be a call the engine itself would never make.
    const uint16_t probe =
        gTap->ListenSymbol(gStreams[0].symbol, ProbeReentrantListen, &gStreams[0]);
    ++checked;
    if (probe == IWiretap::kInvalidHandle) {
        WriteVerdict(verdictPath, kRegistration, "could not install the reentrancy probe",
                     checked, elapsed, feed.SpanNs());
        return kRegistration;
    }
    w1l::ListenerNode probeEntry{};
    if (w1l::ReadListenerNode(fedBus, probe, probeEntry) && probeEntry.invoker != nullptr &&
        IsExecutable(probeEntry.invoker)) {
        const uint8_t byte = 0;
        reinterpret_cast<ProxyFn>(probeEntry.invoker)(
            probeEntry.context, w1l::ListenerMemberSlot(fedBus, probe), &byte, 1, 0, 0);
    }
    std::printf("FEEDHOST reentrant Listen reached=%d result=%u (%u expected)\n",
                gReentrantReached ? 1 : 0, gReentrantResult, IWiretap::kInvalidHandle);
    ++checked;
    if (!gReentrantReached || gReentrantResult != IWiretap::kInvalidHandle) {
        WriteVerdict(verdictPath, kTeardown, "Listen was not refused from inside a dispatch",
                     checked, elapsed, feed.SpanNs());
        return kTeardown;
    }

    replay::WiretapDestroy(gTap);
    gTap = nullptr;
    WriteVerdict(verdictPath, kPass,
                 "fed the recorded session through the engine's own listener nodes", checked,
                 elapsed, feed.SpanNs());
    return kPass;
}
