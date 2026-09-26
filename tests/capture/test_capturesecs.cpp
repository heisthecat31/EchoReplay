// W2-A. The capture deadline's carrier: ECHOREPLAY_CAPTURE_SECS, its bound and its refusals.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS ARM IS FOR. `-replaycapturesecs` was the second plugin-private switch on the engine's
// line, and an unregistered switch makes ApplyCommandLine skip EVERY handler on
// the line -- -offline and all four host pins included. The carrier moved to the
// environment. These cases drive the real GetEnvironmentVariableW against the real process
// environment rather than a seam, because the thing being asserted is that this reads the
// ENVIRONMENT and not the command line, and a seam would let a no-op read pass.
//
// ★ THE ARM ON THE ARM: make ReadCaptureSecondsRequest stop calling GetEnvironmentVariableW -- an
// early `return false`, or reading a different name -- and "a value that is set is read" below goes
// red. Verified by mutation, not by assumption.
// ---------------------------------------------------------------------------------------------

#include "doctest.h"

#include <windows.h>

#include <string.h>

#include <string>

#include "capture/capturesecs.h"

using namespace replay::capture;

namespace {

// Sets the carrier for the length of a case and puts the environment back the way it was found --
// including the case where it was already set machine-wide (RK-11), which is exactly the state a
// test that clobbered it would leave behind for every case after it.
class ScopedCarrier {
public:
    explicit ScopedCarrier(const wchar_t* value) {
        const DWORD n = ::GetEnvironmentVariableW(kCaptureSecsEnvVar, prior_, kPriorChars);
        had_ = n != 0 && n < kPriorChars;
        ::SetEnvironmentVariableW(kCaptureSecsEnvVar, value);
    }
    ~ScopedCarrier() {
        ::SetEnvironmentVariableW(kCaptureSecsEnvVar, had_ ? prior_ : nullptr);
    }
    ScopedCarrier(const ScopedCarrier&) = delete;
    ScopedCarrier& operator=(const ScopedCarrier&) = delete;

private:
    static constexpr DWORD kPriorChars = 256;
    wchar_t prior_[kPriorChars] = {0};
    bool had_ = false;
};

CaptureSecsRefusal Parse(const wchar_t* text, uint32_t* seconds) {
    return ParseCaptureSeconds(text, wcslen(text), seconds);
}

}  // namespace

TEST_CASE("the capture-seconds bound refuses rather than clamps, at both ends") {
    uint32_t secs = 12345;

    CHECK(Parse(L"30", &secs) == CaptureSecsRefusal::None);
    CHECK(secs == 30);
    CHECK(Parse(L"86400", &secs) == CaptureSecsRefusal::None);
    CHECK(secs == 86400);
    CHECK(Parse(L"300", &secs) == CaptureSecsRefusal::None);
    CHECK(secs == 300);

    // One below the floor and one above the ceiling. Both leave the out-parameter at zero: a
    // refusal that also wrote a number is a clamp wearing a refusal's name, and a clamped deadline
    // runs a capture of a length nobody asked for.
    secs = 999;
    CHECK(Parse(L"29", &secs) == CaptureSecsRefusal::OutOfRange);
    CHECK(secs == 0);
    secs = 999;
    CHECK(Parse(L"86401", &secs) == CaptureSecsRefusal::OutOfRange);
    CHECK(secs == 0);
    CHECK(Parse(L"0", &secs) == CaptureSecsRefusal::OutOfRange);

    // Far past the u32 domain: it must refuse, not wrap into range. 4294967326 is 30 modulo 2^32,
    // which is the floor -- a reader that overflowed would accept it and arm a 30 s deadline.
    CHECK(Parse(L"4294967326", &secs) == CaptureSecsRefusal::OutOfRange);
    CHECK(secs == 0);
    CHECK(Parse(L"99999999999999999999", &secs) == CaptureSecsRefusal::OutOfRange);
}

TEST_CASE("a capture-seconds value that is not a whole number of seconds is refused by name") {
    uint32_t secs = 0;
    CHECK(Parse(L"", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(Parse(L"   ", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(Parse(L"60s", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(Parse(L"6 0", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(Parse(L"60.0", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(Parse(L"0x3C", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(Parse(L"sixty", &secs) == CaptureSecsRefusal::NotANumber);
    // Signed, both ways. A '-' is plainly wrong; a '+' is refused too, because a value carrying a
    // sign was written by something that thinks this field can be negative.
    CHECK(Parse(L"-60", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(Parse(L"+60", &secs) == CaptureSecsRefusal::NotANumber);
    CHECK(secs == 0);

    // What a launcher leaves behind is not malformed: surrounding whitespace and one pair of
    // quotes, neither of which can be part of a number.
    CHECK(Parse(L"  120  ", &secs) == CaptureSecsRefusal::None);
    CHECK(secs == 120);
    CHECK(Parse(L"\"120\"", &secs) == CaptureSecsRefusal::None);
    CHECK(secs == 120);

    // Every refusal has text, and no two share it -- a log line that cannot say which refusal it
    // was is not diagnosable.
    const char* none = CaptureSecsRefusalText(CaptureSecsRefusal::None);
    const char* tooLong = CaptureSecsRefusalText(CaptureSecsRefusal::TooLong);
    const char* nan = CaptureSecsRefusalText(CaptureSecsRefusal::NotANumber);
    const char* range = CaptureSecsRefusalText(CaptureSecsRefusal::OutOfRange);
    CHECK(strcmp(tooLong, none) != 0);
    CHECK(strcmp(nan, none) != 0);
    CHECK(strcmp(range, none) != 0);
    CHECK(strcmp(tooLong, nan) != 0);
    CHECK(strcmp(range, nan) != 0);
}

TEST_CASE("a capture-seconds value that is set is read off the environment") {
    ScopedCarrier carrier(L"120");
    CaptureSecsRequest req;
    REQUIRE(ReadCaptureSecondsRequest(&req));
    CHECK(req.present);
    CHECK(req.hasSeconds);
    CHECK(req.seconds == 120);
    CHECK(req.refusal == CaptureSecsRefusal::None);
    CHECK(std::wstring(req.value) == L"120");
}

TEST_CASE("an absent capture-seconds carrier is not an error and arms nothing") {
    ScopedCarrier carrier(nullptr);
    CaptureSecsRequest req;
    CHECK_FALSE(ReadCaptureSecondsRequest(&req));
    CHECK_FALSE(req.present);
    CHECK_FALSE(req.hasSeconds);
    CHECK(req.seconds == 0);
    CHECK(req.refusal == CaptureSecsRefusal::None);
}

TEST_CASE("an empty capture-seconds carrier reads as absent, because Win32 cannot say otherwise") {
    // GetEnvironmentVariableW answers 0 for both "not set" and "set to nothing", so an empty value
    // has no encoding here. Measured rather than believed, and stated so the day somebody tries to
    // give the empty string a third meaning.
    ScopedCarrier carrier(L"");
    CaptureSecsRequest req;
    CHECK_FALSE(ReadCaptureSecondsRequest(&req));
    CHECK_FALSE(req.present);
}

TEST_CASE("a malformed or out-of-range carrier is a counted refusal that quotes what it read") {
    {
        ScopedCarrier carrier(L"soon");
        CaptureSecsRequest req;
        REQUIRE(ReadCaptureSecondsRequest(&req));
        CHECK(req.present);
        CHECK_FALSE(req.hasSeconds);
        CHECK(req.refusal == CaptureSecsRefusal::NotANumber);
        CHECK(std::wstring(req.value) == L"soon");
    }
    {
        ScopedCarrier carrier(L"5");
        CaptureSecsRequest req;
        REQUIRE(ReadCaptureSecondsRequest(&req));
        CHECK_FALSE(req.hasSeconds);
        CHECK(req.refusal == CaptureSecsRefusal::OutOfRange);
        CHECK(req.seconds == 0);
        CHECK(std::wstring(req.value) == L"5");
    }
    {
        // Longer than the reader's buffer. It must refuse rather than truncate: the leading digits
        // of a too-long value are a different, entirely plausible number.
        const std::wstring huge(kMaxCaptureSecsChars + 40, L'3');
        ScopedCarrier carrier(huge.c_str());
        CaptureSecsRequest req;
        REQUIRE(ReadCaptureSecondsRequest(&req));
        CHECK(req.present);
        CHECK_FALSE(req.hasSeconds);
        CHECK(req.refusal == CaptureSecsRefusal::TooLong);
        CHECK(req.seconds == 0);
    }
}

TEST_CASE("the legacy switch is still recognised on a command line, so the log can say so") {
    // The whole point of the diagnostic: by the time it runs the engine has thrown the line away.
    CHECK(CommandLineMentionsCaptureSecs(L"echovr.exe -offline -replaycapturesecs 300"));
    CHECK(CommandLineMentionsCaptureSecs(L"-replaycapturesecs 300"));
    CHECK(CommandLineMentionsCaptureSecs(L"-replaycapturesecs"));
    CHECK(CommandLineMentionsCaptureSecs(L"+replaycapturesecs=300"));
    CHECK(CommandLineMentionsCaptureSecs(L"-replaycapturesecs:300"));
    CHECK(CommandLineMentionsCaptureSecs(L"-REPLAYCAPTURESECS 300"));
    CHECK(CommandLineMentionsCaptureSecs(L"\"-replaycapturesecs\" 300"));
    // The en dash the engine's tokeniser also honours: a line that has been through a word
    // processor still voids the launch, so it still has to be reported.
    CHECK(CommandLineMentionsCaptureSecs(L"\x96" L"replaycapturesecs 300"));

    CHECK_FALSE(CommandLineMentionsCaptureSecs(nullptr));
    CHECK_FALSE(CommandLineMentionsCaptureSecs(L""));
    CHECK_FALSE(CommandLineMentionsCaptureSecs(L"echovr.exe -offline -level mpl_arena_a"));
    CHECK_FALSE(CommandLineMentionsCaptureSecs(L"-replayviewer C:\\x.echodemo"));
    // A longer switch that merely starts the same way is a different switch, and reporting it would
    // send somebody looking for a token that is not there.
    CHECK_FALSE(CommandLineMentionsCaptureSecs(L"-replaycapturesecsx 300"));
    // Not a switch at all: no prefix, so the engine's tokeniser never treats it as one.
    CHECK_FALSE(CommandLineMentionsCaptureSecs(L"replaycapturesecs 300"));
    CHECK_FALSE(CommandLineMentionsCaptureSecs(L"C:\\replaycapturesecs\\x.txt"));
}

TEST_CASE("the legacy-switch warning names the token and the audit that struck it") {
    // The warning is the only evidence such a launch leaves, so it has to carry both: what was on
    // the line, and where to read what it cost. Pinned here because a scanner and a message that
    // drift apart stop describing the same thing.
    const std::string warning(kLegacyCaptureSecsWarning);
    CHECK(warning.find("-replaycapturesecs") != std::string::npos);
    CHECK(warning.find("AUDIT_W1-F_R4") != std::string::npos);
    CHECK(warning.find("ECHOREPLAY_CAPTURE_SECS") != std::string::npos);
    CHECK(std::wstring(kCaptureSecsEnvVar) == L"ECHOREPLAY_CAPTURE_SECS");
}
