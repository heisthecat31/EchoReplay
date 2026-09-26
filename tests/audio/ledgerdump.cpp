// audio_ledgerdump: checkledger.h's table and record shape, printed by the code that owns them.
//
// tests/audio/check.ps1 asks this binary for the scored-check ceiling, the check names and the
// reserved words rather than parsing checkledger.h with a regex. A regex over a source file is a
// second implementation of that file's meaning, and the earlier revision that ran two of them and
// called the agreement "derived twice" was one source counted twice. The --sample records are written
// by the same FormatRecord the probe uses, so what the check holds them against is the writer itself.
#include <cstdio>
#include <cstring>

#include "checkledger.h"

using namespace replay::audio::ledger;

namespace {

void PrintTable() {
    // The three final words, produced by Verdict itself rather than listed: one state per word.
    Ledger failed;
    failed.Check(kCheckExportsBound, false);
    Ledger empty;
    Ledger held;
    for (int i = 0; i < kCheckCeiling; ++i) {
        held.Check(static_cast<CheckId>(i), true);
    }

    printf("{ \"checkCeiling\": %d,\n  \"checks\": [", static_cast<int>(kCheckCeiling));
    for (int i = 0; i < kCheckCeiling; ++i) {
        printf("%s\"%s\"", i == 0 ? "" : ", ", kCheckNames[i]);
    }
    printf("],\n  \"finalWords\": [\"%s\", \"%s\", \"%s\"],\n"
           "  \"inProgressVerdict\": \"%s\",\n  \"pendingReason\": \"%s\",\n"
           "  \"forgottenReason\": \"%s\",\n  \"completesAt\": \"%s\" }\n",
           Verdict(held, false), Verdict(failed, false), Verdict(empty, true), kInProgressVerdict,
           kPendingReason, kForgottenReason, kCompletesAt);
}

void PrintRecord(const Ledger& l, const Record& r) {
    static char out[12288];
    FormatRecord(l, r, out, sizeof out);
    fputs(out, stdout);
}

Record Timed(const char* detail) {
    Record r;
    r.probeId = "W2-H-audio";
    r.wwiseUpMs = 4100.0;
    r.sinkWaitMs = 9250.0;
    r.sinkPolls = 21;
    r.detail = detail;
    return r;
}

// The ledger state inside the sink wait, which is where a launch that dies at ~13 s leaves its
// surviving record: the exports bound, the sound engine up, nothing else reached yet. f870d62 wrote
// this state as PASS with seven checks marked forgotten.
int SampleProvisional() {
    Ledger l;
    l.Check(kCheckExportsBound, true);
    Record r = Timed("sample: the ledger state inside the sink wait");
    r.provisional = true;
    PrintRecord(l, r);
    return 0;
}

// All eight asked and held, at a live match: the only PASS there is.
int SampleFinalPass() {
    Ledger l;
    for (int i = 0; i < kCheckCeiling; ++i) {
        l.Check(static_cast<CheckId>(i), true);
    }
    Record r = Timed("sample: every check asked and held at a live match");
    r.completionReached = true;
    r.matchSeenMs = 61000.0;
    r.selectorFirstMs = 30500.0;
    r.sinkListFirstMs = 30500.0;
    PrintRecord(l, r);
    return 0;
}

// The same eight, but the launch never reached the match the descriptor declares. Not a PASS.
int SampleFinalTruncated() {
    Ledger l;
    for (int i = 0; i < kCheckCeiling; ++i) {
        l.Check(static_cast<CheckId>(i), true);
    }
    Record r = Timed("sample: every check asked and held, but no live CR15NetGame inside the deadline");
    r.selectorFirstMs = 30500.0;
    r.sinkListFirstMs = 30500.0;
    PrintRecord(l, r);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        PrintTable();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--sample") == 0) {
        if (strcmp(argv[2], "provisional") == 0) {
            return SampleProvisional();
        }
        if (strcmp(argv[2], "final-pass") == 0) {
            return SampleFinalPass();
        }
        if (strcmp(argv[2], "final-truncated") == 0) {
            return SampleFinalTruncated();
        }
    }
    fprintf(stderr, "usage: audio_ledgerdump [--sample provisional|final-pass|final-truncated]\n");
    return 2;
}
