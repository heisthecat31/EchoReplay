#pragma once

// Cutting a time window out of a recording: the "Edith, clip the last 3 minutes" file.
//
// A CORE chapter opens with a keyframe and every encoder resets at a chapter boundary, so any chapter
// is a legal place for a file to start. What a chapter does NOT carry is the identity blocks (names,
// numbers, loadouts): those are written into S_EVENT only when they change. A clip that started at
// minute 7 of a match would otherwise show nobody's cosmetics. So every ReplayIdentitySnapshot event
// from before the window is carried into the clip's first CORE chapter, ahead of that chapter's own
// events, stamped at the chapter's start. Later blocks for the same entrant still win, as they do in
// the original file.
//
// The source may still be open for writing (the live recording): the reader's recovery scan reads
// it, and a torn last chapter is simply not in the clip.

#include <stdint.h>

namespace replay {
namespace capture {

struct ClipResult {
    uint32_t chaptersIn = 0;
    uint32_t chaptersOut = 0;
    uint32_t identityCarried = 0;  // identity events copied in from before the window
    uint32_t undecodable = 0;      // chapters inside the window that would not decode (dropped)
    uint64_t firstUs = 0, lastUs = 0;  // the window actually written, capture axis
    bool ok = false;
    const char* error = nullptr;
};

// Window [fromUs, toUs] on the file's own capture axis (ChapterKey times). toUs == 0 means "to the
// end". The first chapter kept is the CORE chapter that contains fromUs (so the clip may begin up to
// one chapter, ~2 s, early). The result is re-opened and checked before ok is set.
ClipResult ClipRange(const wchar_t* in, const wchar_t* out, uint64_t fromUs, uint64_t toUs);

// The end of the recording's capture axis (the last chapter's tEndUs), 0 if it cannot be read.
uint64_t RecordingEndUs(const wchar_t* path);
// The start (first chapter's tBeginUs).
uint64_t RecordingBeginUs(const wchar_t* path);

}  // namespace capture
}  // namespace replay
