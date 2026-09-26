#include "import/convert.h"

#include <stdio.h>
#include <chrono>
#include <ratio>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "import/emit.h"
#include "import/inferred.h"
#include "import/reader.h"

#define XXH_NAMESPACE ZSTD_
#define XXH_STATIC_LINKING_ONLY
#include "common/xxhash.h"

namespace replay::import {
namespace {

// Share of the progress bar per stage, from the owner-file timings: reading (inflate + JSON) dominates.
constexpr uint32_t kReadEnd = 650, kInferEnd = 700, kEmitEnd = 990, kWhole = std::milli::den;

void EnterStage(Progress* progress, uint32_t base, uint32_t end) {
    if (!progress) return;
    progress->stageBase = base;
    progress->stageSpan = end - base;
    progress->permille.store(base, std::memory_order_relaxed);
}

bool Cancelled(const Progress* progress) { return progress && progress->cancel.load(std::memory_order_relaxed); }

Result HashFile(const wchar_t* path, uint64_t& hash) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return Refuse(ResultCode::Io, "the source could not be opened to hash it");
    XXH64_state_t state;
    XXH64_reset(&state, 0);
    std::vector<char> buffer(1 << 20);
    DWORD got = 0;
    bool ok = true;
    for (;;) {
        ok = ReadFile(file, buffer.data(), DWORD(buffer.size()), &got, nullptr) != 0;
        if (!ok || got == 0) break;
        XXH64_update(&state, buffer.data(), got);
    }
    CloseHandle(file);
    if (!ok) return Refuse(ResultCode::Io, "the source could not be read to hash it");
    hash = XXH64_digest(&state);
    return Success();
}

}  // namespace

Result Convert(const wchar_t* inPath, const wchar_t* outPath, const ConvertOptions& options, ConvertReport& report,
               Progress* progress) {
    report = ConvertReport{};
    if (!inPath || !*inPath || !outPath || !*outPath) {
        return Refuse(ResultCode::BadArguments, "both a source and an output path are needed");
    }

    SourceMatch match;
    EnterStage(progress, 0, kReadEnd);
    Result r = ReadSource(inPath, match, progress, options.maxModelBytes);
    if (!r.ok) return r;
    uint64_t sourceHash = 0;
    r = HashFile(inPath, sourceHash);
    if (!r.ok) return r;
    if (Cancelled(progress)) return Refuse(ResultCode::Cancelled, "conversion cancelled");

    EnterStage(progress, kReadEnd, kInferEnd);
    r = Normalise(match, options, report);
    if (!r.ok) return r;
    report.sourceHash = sourceHash;
    InferredTracks tracks;
    r = Infer(match, kInferFacts, tracks, report);
    if (!r.ok) return r;
    if (Cancelled(progress)) return Refuse(ResultCode::Cancelled, "conversion cancelled");

    EnterStage(progress, kInferEnd, kEmitEnd);
    r = Emit(match, tracks, kEmitFacts, options, outPath, report, progress);
    if (!r.ok) return r;

    const std::wstring defaultReport = std::wstring(outPath) + L".import.json";
    const wchar_t* reportPath = options.reportPath ? options.reportPath : defaultReport.c_str();
    r = WriteReportJson(report, kInferFacts, kEmitFacts, inPath, reportPath);
    if (!r.ok) {
        DeleteFileW(outPath);
        return r;
    }
    if (progress) progress->permille.store(kWhole, std::memory_order_relaxed);
    return Success();
}

int RunConvertCommand(int argc, wchar_t** argv) {
    const wchar_t* in = nullptr;
    const wchar_t* out = nullptr;
    ConvertOptions options;
    for (int i = 0; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (wcscmp(a, L"--session") == 0 && i + 1 < argc) {
            options.session = _wtoi(argv[++i]);
        } else if (wcscmp(a, L"--rate") == 0 && i + 1 < argc) {
            options.rateHz = static_cast<uint32_t>(_wtoi(argv[++i]));
        } else if (wcscmp(a, L"--report") == 0 && i + 1 < argc) {
            options.reportPath = argv[++i];
        } else if (wcscmp(a, L"--max-model-mb") == 0 && i + 1 < argc) {
            options.maxModelBytes = uint64_t(_wtoi64(argv[++i])) << 20;
        } else if (a[0] == L'-' && a[1] == L'-') {
            in = out = nullptr;
            break;
        } else if (!in) {
            in = a;
        } else if (!out) {
            out = a;
        } else {
            in = out = nullptr;
            break;
        }
    }
    if (!in || !out) {
        fprintf(stderr, "usage: echodemo convert <in.echoreplay|in.tape> <out.echodemo> [--session N] [--rate 45] "
                        "[--report <json>] [--max-model-mb N]\n");
        return ExitCodeFor(Refuse(ResultCode::BadArguments, "usage"));
    }

    ConvertReport report;
    const auto t0 = std::chrono::steady_clock::now();
    const Result r = Convert(in, out, options, report, nullptr);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!r.ok) {
        fprintf(stderr, "%s: %s\n", ResultCodeName(r.code), r.message);
        return ExitCodeFor(r);
    }

    printf("converted in %.2f s: %.1f s of match, %u ticks at %u Hz\n", seconds, double(report.durationUs) / 1e6,
           report.ticks, options.rateHz);
    printf("source: %s, %u lines (%u bad, %u duplicate, %u err_code), %u of %u session(s) kept as #%u\n",
           report.kind == SourceKind::Tape ? "tape" : "echoreplay", report.source.lines, report.source.badLines,
           report.source.duplicateFrames, report.source.errCodeFrames, report.framesKept, report.sessionsFound,
           report.sessionChosen);
    printf("repairs: %u restamped%s, lead-in %u, tail %u, hand offsets clamped %u, com rejected %u\n",
           report.repairedStamps, report.retimedWhole ? " (re-timed whole)" : "", report.leadInDropped,
           report.tailDropped, report.handOffsetClamped, report.comRejected);
    printf("gaps: %u (kept as known import gaps)\n", report.gaps);
    for (const SourceGap& g : report.gapList) {
        printf("  at %+.3f s: %.3f s\n", double(g.atUs) / 1e6, double(g.lengthUs) / 1e6);
    }
    printf("inferred:");
    for (int i = 0; i < kRuleCount; ++i) {
        const RuleGate& gate = kInferFacts.rules[i];
        if (gate.enabled) printf(" %s=%u", gate.id, report.inferredRows[i]);
    }
    printf(" (stun puncher guessed %u, unknown goal types %u)\n", report.stunPuncherGuessed, report.unknownGoalTypes);
    printf("rows: player %u, body %u, event %u, netvar %u\n", report.playerRows, report.bodyRows, report.eventRows,
           report.netvarRows);
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory)) {
        printf("peak memory: private %.1f MB, working set %.1f MB\n", double(memory.PeakPagefileUsage) / 1048576.0,
               double(memory.PeakWorkingSetSize) / 1048576.0);
    }
    return 0;
}

}  // namespace replay::import
