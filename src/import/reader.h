#pragma once

// Source readers (DESIGN-FORMATS §7.2, §7.3; packet P2). Each fills a SourceMatch from scratch,
// streaming: the inflated text is never held whole and bones columns are never parsed (§8).
// `progress` may be null. On refusal `out` is unspecified and must not be used.

#include "import/result.h"
#include "import/sourcemodel.h"

namespace replay::import {

// Zip (entry 0) or plain UTF-8 text; the container is sniffed from the bytes, not the extension.
// maxModelBytes: refuse with TooLarge once the model's allocations pass it; 0 is unlimited.
Result ReadEchoReplay(const wchar_t* path, SourceMatch& out, Progress* progress, uint64_t maxModelBytes = 0);

// Zstd-framed (or zip, or raw) varint-delimited Envelope stream; format_version must be 2.
Result ReadTape(const wchar_t* path, SourceMatch& out, Progress* progress, uint64_t maxModelBytes = 0);

// Dispatches on the extension (.echoreplay / .tape, case-insensitive); BadArguments otherwise.
Result ReadSource(const wchar_t* path, SourceMatch& out, Progress* progress, uint64_t maxModelBytes = 0);

}  // namespace replay::import
