// sizecheck -- SC3's instrument. Bytes per second PER STREAM, computed from a real capture.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS TOOL COVERS, WHAT IT REFUSES, AND WHAT IT DOES NOT COVER.
//
// COVERS   the on-disk cost of a .echodemo, attributed to the stream that caused it, plus the
//          completeness provenance of every stream -- which ones have an engine-sourced `observed`
//          and which are declared unverified.
// REFUSES  a file it cannot open, a file with no chapters, and a file whose own timestamps give a
//          duration of zero. Each is RED with a reason, never a pass with a zero denominator.
// DOES NOT a per-FIELD cost breakdown: the container hands out blocks, not fields, and inventing an
// COVER    attribution below the block would be arithmetic dressed as a measurement. Nor does it say
//          whether the contents are CORRECT -- that is O1, O2 and echodemo verify.
//
// ★ NO FIGURE HERE IS A HARDCODED DENOMINATOR. The duration comes from the file's own chapter span,
// the player count from its manifest, and the VOIP allowance from the file's own voice-record count.
// S5.10 is explicit that the budget is a gate with a stated tolerance and not a claim, and a tool that
// carried the design's table inside it would be checking the design against itself.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "capture/capture_types.h"
#include "capture/completeness.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"

using namespace replay;

namespace {

// SC3, restated as a rate so a file of any length can be judged: 6.0 MB over a 10-minute 4v4 with no
// voice. Both halves of that sentence are used -- the rate AND the reference player count -- and the
// report prints the file's own figures beside them so a reader can see the scaling rather than trust it.
constexpr double kSc3BytesPerSecondNoVoip = 6.0e6 / 600.0;
constexpr double kSc3ReferencePlayers = 8.0;
// S5.4.6: 50 packets per second per active talker. The allowance is derived from the file's own voice
// record count through that rate, not from a flat ceiling -- m6: a flat 11 MB fails a normal
// three-talker match through no fault of the encoder.
constexpr double kVoipPacketsPerTalkerSecond = 50.0;
constexpr double kVoipBytesPerTalkerSecond = 4400.0;

struct Row {
    uint16_t stream = 0;
    uint64_t bytes = 0;
    uint64_t records = 0;
    uint64_t observed = 0;
    uint64_t blocks = 0;
    uint64_t gapped = 0;
};

Row& RowFor(std::vector<Row>& rows, uint16_t s) {
    for (Row& r : rows) {
        if (r.stream == s) return r;
    }
    rows.push_back(Row{});
    rows.back().stream = s;
    return rows.back();
}

int Fail(const char* why) {
    printf("SIZECHECK-RESULT status=FAIL reason=%s\n", why);
    return 1;
}

int NotRun(const char* why) {
    printf("SIZECHECK-RESULT status=NOT-RUN reason=%s\n", why);
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("sizecheck <file.echodemo> [--quiet]\n");
        return 2;
    }
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--quiet") == 0) quiet = true;
    }

    DemoReader r;
    if (!r.Open(argv[1])) {
        printf("REFUSED: %s\n", r.RefusalReason());
        return NotRun("file-unreadable");
    }
    const Manifest& m = r.GetManifest();
    const size_t chapters = r.ChapterCount();
    if (chapters == 0) {
        return NotRun("no-chapters");
    }

    std::vector<Row> rows;
    uint64_t tMin = ~0ull, tMax = 0;
    uint64_t frameBytes = 0;
    uint64_t undecodable = 0;

    for (size_t i = 0; i < chapters; ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.tBeginUs < tMin) tMin = k.tBeginUs;
        if (k.tEndUs > tMax) tMax = k.tEndUs;
        frameBytes += kChapterHeadBytes + r.ChapterPayloadLen(i);

        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            ++undecodable;
            continue;
        }
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            Row& row = RowFor(rows, v.id);
            row.bytes += v.bytes;
            row.records += v.storedCount;
            row.observed += v.observedCount;
            ++row.blocks;
            if (v.observedCount > v.storedCount) {
                row.gapped += v.observedCount - v.storedCount;
            }
        }
    }

    if (tMax <= tMin) {
        return NotRun("duration-zero");
    }
    const double seconds = double(tMax - tMin) / 1e6;

    uint64_t fileBytes = 0;
    {
        HANDLE h = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                              nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            GetFileSizeEx(h, &sz);
            fileBytes = static_cast<uint64_t>(sz.QuadPart);
            CloseHandle(h);
        }
    }

    uint64_t voipRecords = 0, voipBytes = 0;
    for (const Row& row : rows) {
        if (row.stream == kStreamVoip) {
            voipRecords = row.records;
            voipBytes = row.bytes;
        }
    }
    const double talkerSeconds = voipRecords / kVoipPacketsPerTalkerSecond;

    if (!quiet) {
        printf("file             : %ls\n", argv[1]);
        printf("bytes on disk    : %" PRIu64 "\n", fileBytes);
        printf("chapter frames   : %" PRIu64 " bytes over %zu chapters\n", frameBytes, chapters);
        printf("duration         : %.3f s (from the file's own chapter span)\n", seconds);
        printf("players          : %u (from the manifest)\n", unsigned(m.playerCount));
        printf("recovered        : %s\n", r.WasRecovered() ? "yes" : "no");
        if (undecodable) {
            printf("undecodable      : %" PRIu64 " chapters -- their bytes are counted, their\n"
                   "                   contents are not attributed to any stream\n", undecodable);
        }
        printf("\n%-18s %12s %10s %12s %10s %10s\n", "stream", "bytes", "records", "KB/s", "B/record",
               "unstored");
        printf("%-18s %12s %10s %12s %10s %10s\n", "------", "-----", "-------", "----", "--------",
               "--------");
        for (const Row& row : rows) {
            const double kbs = double(row.bytes) / seconds / 1000.0;
            const double perRec = row.records ? double(row.bytes) / double(row.records) : 0.0;
            printf("%-18s %12" PRIu64 " %10" PRIu64 " %12.3f %10.2f %10" PRIu64 "\n",
                   StreamName(row.stream), row.bytes, row.records, kbs, perRec, row.gapped);
        }
    }

    // The gate. Voice is measured separately because SC3 measures it separately, and folding it in
    // would fail a normal three-talker match for a cost the ruling explicitly allows.
    const uint64_t nonVoipBytes = fileBytes > voipBytes ? fileBytes - voipBytes : fileBytes;
    const double playerScale =
        m.playerCount > 0 ? double(m.playerCount) / kSc3ReferencePlayers : 1.0;
    const double nonVoipBudget = kSc3BytesPerSecondNoVoip * seconds * playerScale;
    const double voipBudget = kVoipBytesPerTalkerSecond * talkerSeconds;

    if (!quiet) {
        printf("\nSC3, scaled to this file rather than to a 10-minute reference:\n");
        printf("  non-voice      : %" PRIu64 " B against a budget of %.0f B "
               "(%.1f KB/s x %.1f s x %.2f players/8)\n",
               nonVoipBytes, nonVoipBudget, kSc3BytesPerSecondNoVoip / 1000.0, seconds, playerScale);
        printf("  voice          : %" PRIu64 " B against %.0f B "
               "(%.1f talker-seconds derived from %" PRIu64 " voice records at 50/s)\n",
               voipBytes, voipBudget, talkerSeconds, voipRecords);
    }

    const bool nonVoipOk = double(nonVoipBytes) <= nonVoipBudget;
    // THREE STATES, NOT TWO. This used to read voipRecords == 0 || bytes <= budget, so a file
    // with NO voice at all reported the voice budget as MET -- a capability claim backed by the
    // absence of evidence, the same shape W2-B just found in netvar_apply_available. Nothing was
    // attempted, so nothing was over budget, so it looked fine. No attempt is UNKNOWN, and
    // defaulting unknown to true is the whole defect.
    const char* voipVerdict = voipRecords == 0
                                  ? "NOT-APPLICABLE (no voice records in this file)"
                                  : (double(voipBytes) <= voipBudget * 1.05 ? "PASS" : "FAIL");
    const bool voipOk = voipRecords == 0 || double(voipBytes) <= voipBudget * 1.05;

    // The completeness provenance table. A stream whose observed count equals its stored count is not
    // evidence of anything on its own -- that is what a stream with no detector looks like too -- so the
    // report prints whether the stream EVER diverged, and says plainly that agreement is not proof.
    if (!quiet) {
        printf("\ncompleteness, as the file states it:\n");
        for (const Row& row : rows) {
            if (!StreamCarriesSamples(row.stream)) continue;
            printf("  %-18s observed=%-10" PRIu64 " stored=%-10" PRIu64 " %s\n",
                   StreamName(row.stream), row.observed, row.records,
                   row.observed == row.records ? "(never diverged -- not by itself proof of anything)"
                                               : "(diverged; the file names the runs)");
        }
    }

    printf("SIZECHECK-VOIP %s\n", voipVerdict);
    printf("SIZECHECK-RESULT status=%s bytes=%" PRIu64 " seconds=%.3f players=%u "
           "nonvoip_bytes=%" PRIu64 " nonvoip_budget=%.0f voip_bytes=%" PRIu64 " voip_budget=%.0f\n",
           (nonVoipOk && voipOk) ? "PASS" : "FAIL", fileBytes, seconds, unsigned(m.playerCount),
           nonVoipBytes, nonVoipBudget, voipBytes, voipBudget);
    if (!nonVoipOk) {
        return Fail("sc3-nonvoip-over-budget");
    }
    if (!voipOk) {
        return Fail("sc3-voip-over-budget");
    }
    return 0;
}
