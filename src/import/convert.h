#pragma once

// The whole pipeline and its CLI (DESIGN-FORMATS §7.8; packet P5). Convert is what the viewer's
// worker thread (P6) and `echodemo convert` both call.

#include <stdint.h>

#include "import/report.h"
#include "import/result.h"

namespace replay::import {

// Part of the viewer's cache key and the golden digests: bump it with any change to output bytes.
constexpr uint32_t kConverterVersion = 2;

// ReadSource -> Normalise -> Infer -> Emit -> WriteReportJson. Honours progress->cancel between
// stages and inside each; on any refusal no output file is left behind.
Result Convert(const wchar_t* inPath, const wchar_t* outPath, const ConvertOptions& options,
               ConvertReport& report, Progress* progress);

// `echodemo convert <in> <out.echodemo> [--session N] [--rate 45] [--report <json>] [--max-model-mb N]`.
// --max-model-mb applies the viewer's in-game ceiling (ConvertOptions::maxModelBytes); unset is unlimited.
// argv holds only what follows the verb. Prints the report summary or the refusal sentence.
int RunConvertCommand(int argc, wchar_t** argv);

// CLI exit code: 0 ok, 3 I/O, 2 every other refusal.
int ExitCodeFor(const Result& result);

}  // namespace replay::import
