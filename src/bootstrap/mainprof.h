// A sampling profiler for the thread the engine renders on.
//
// PERF-3 needed one because the `cost:` line can only measure what the plugin itself runs, and the
// question that outlived PERF-2 is about the rest of the frame: an empty arena costs ~14 ms with
// one saturated thread, of which the plugin charges itself 3.5. Nothing on this machine could say
// where the other 10 went. This says it, by suspending that thread a few hundred times a second,
// reading its instruction pointer and bucketing it by module and by 4 KB of echovr.exe -- addresses
// the offline IDA database resolves to functions afterwards.
//
// OFF unless `bootstrap.profiler` is on. It is a diagnostic, not a feature: it suspends the frame
// thread, which is exactly what a shipping build must never do.
#pragma once

namespace replay {
namespace mainprof {

// Called from the playback tick, on the engine's frame thread, to say which thread that is.
void NoteFrameThread();

// Starts the sampler if `bootstrap.profiler` is set. Returns whether it started.
bool Start();
void Stop();

}  // namespace mainprof
}  // namespace replay
