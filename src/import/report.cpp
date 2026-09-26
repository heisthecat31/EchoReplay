#include "import/report.h"

#include <math.h>
#include <stdio.h>
#include <string>

#include <windows.h>

#include "import/convert.h"

namespace replay::import {
namespace {

std::string Utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(bytes > 0 ? size_t(bytes - 1) : 0, '\0');
    if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

const char* StatusName(FactStatus s) { return s == FactStatus::kMeasured ? "measured" : "unmeasured"; }

const char* OriName(OriSource s) {
    switch (s) {
    case OriSource::HeadBasis: return "HeadBasis";
    case OriSource::BodyBasis: return "BodyBasis";
    case OriSource::SameAsHmd: return "SameAsHmd";
    }
    return "?";
}

// Just enough of a JSON writer for one small document; the vendored yyjson is built reader-only.
class Json {
public:
    void Open(const char* key) { Key(key), text_ += '{', first_ = true; }
    void OpenArray(const char* key) { Key(key), text_ += '[', first_ = true; }
    void Close() { text_ += '}', first_ = false; }
    void CloseArray() { text_ += ']', first_ = false; }

    void Add(const char* key, uint64_t v) { Key(key), text_ += std::to_string(v); }
    void Add(const char* key, uint32_t v) { Add(key, uint64_t(v)); }
    void Add(const char* key, bool v) { Key(key), text_ += v ? "true" : "false"; }
    void Add(const char* key, float v) { Add(key, double(v)); }
    void Add(const char* key, double v) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.9g", isfinite(v) ? v : 0.0);
        Key(key), text_ += buf;
    }
    void Add(const char* key, const char* v) { Key(key), String(v); }
    void Add(const char* key, const std::string& v) { Key(key), String(v.c_str()); }
    void Add(const char* key, const Vec3f& v) {
        OpenArray(key);
        Add(nullptr, v.x), Add(nullptr, v.y), Add(nullptr, v.z);
        CloseArray();
    }

    template <class T, class V>
    void Fact(const replay::import::Fact<T>& f, const V& value) {
        Open(f.id);
        Add("status", StatusName(f.status));
        Add("value", value);
        Close();
    }

    const std::string& Text() const { return text_; }

private:
    // A null key is an array element.
    void Key(const char* key) {
        if (!first_) text_ += ',';
        first_ = false;
        if (key) String(key), text_ += ':';
    }
    void String(const char* s) {
        text_ += '"';
        for (; *s; ++s) {
            const unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') {
                text_ += '\\';
                text_ += char(c);
            } else if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                text_ += buf;
            } else {
                text_ += char(c);
            }
        }
        text_ += '"';
    }

    std::string text_;
    bool first_ = true;
};

void WriteFacts(Json& j, const InferFacts& infer, const EmitFacts& emit) {
    j.Open("facts");
    j.Open("rules");
    for (const RuleGate& g : infer.rules) {
        j.Open(g.id);
        j.Add("gated", g.gated);
        j.Add("enabled", g.enabled);
        j.Close();
    }
    j.Close();

    j.Open("measured");
    j.Fact(infer.geoBitLeft, infer.geoBitLeft.value);
    j.Fact(infer.geoBitRight, infer.geoBitRight.value);
    j.Fact(infer.idleWord, infer.idleWord.value);
    j.Fact(infer.stunDurationMedianS, infer.stunDurationMedianS.value);
    j.Fact(infer.boostSpanS, infer.boostSpanS.value);
    j.Fact(infer.thrusterAxis, infer.thrusterAxis.value);
    j.Fact(emit.worldAxesMatchEngine, emit.worldAxesMatchEngine.value);
    j.Fact(emit.comFromHead, emit.comFromHead.value);
    j.Fact(emit.hmdOri, OriName(emit.hmdOri.value));
    j.Fact(emit.bodyOri, OriName(emit.bodyOri.value));
    j.Fact(emit.handAxesDirect, emit.handAxesDirect.value);
    j.Fact(emit.discOwner, emit.discOwner.value == DiscOwnerMeaning::Possession ? "Possession" : "InHand");
    j.Fact(emit.bendBack, emit.bendBack.value);
    j.Fact(emit.bendFwd, emit.bendFwd.value);
    j.Fact(emit.restPoseHeight, emit.restPoseHeight.value);
    j.Fact(emit.discIdValue, emit.discIdValue.value);
    j.Fact(emit.gametypeId, emit.gametypeId.value);
    j.Fact(emit.defaultLoadout, emit.defaultLoadout.value != nullptr);
    j.Fact(emit.goalTypes, emit.goalTypes.value != nullptr);
    j.Close();

    j.Open("thresholds");
    j.Add("movingAttachedMinSpeed", infer.movingAttachedMinSpeed);
    j.Add("stunStatWindowS", infer.stunStatWindowS);
    j.Add("stunNearRadiusM", infer.stunNearRadiusM);
    j.Add("boostWindowS", infer.boostWindowS);
    j.Add("boostMinGainMps", infer.boostMinGainMps);
    j.Add("boostMinPeakAccel", infer.boostMinPeakAccel);
    j.Add("boostMaxTurnDeg", infer.boostMaxTurnDeg);
    j.Add("brakeMaxAccel", infer.brakeMaxAccel);
    j.Add("brakeMinFrames", uint32_t(infer.brakeMinFrames));
    j.Add("brakeMaxSpeedRatio", infer.brakeMaxSpeedRatio);
    j.Add("brakeMaxTurnDeg", infer.brakeMaxTurnDeg);
    j.Add("thrustMinAccel", infer.thrustMinAccel);
    j.Add("thrustMaxAccel", infer.thrustMaxAccel);
    j.Add("thrustMinFrames", uint32_t(infer.thrustMinFrames));
    j.Add("thrustMinCos", infer.thrustMinCos);
    j.Add("punchMinHandSpeed", infer.punchMinHandSpeed);
    j.Add("punchMinFrames", uint32_t(infer.punchMinFrames));
    j.Close();
    j.Close();
}

void WriteSource(Json& j, const ConvertReport& report, const wchar_t* sourcePath) {
    const SourceStats& s = report.source;
    j.Open("source");
    j.Add("path", Utf8(sourcePath));
    j.Add("kind", report.kind == SourceKind::Tape ? "tape" : "echoreplay");
    j.Add("bytes", s.sourceBytes);
    char hash[17];
    snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(report.sourceHash));
    j.Add("xxh64", hash);
    j.Add("zipEntries", s.zipEntries);
    j.Add("lines", s.lines);
    j.Add("badLines", s.badLines);
    j.Add("errCodeFrames", s.errCodeFrames);
    j.Add("duplicateFrames", s.duplicateFrames);
    j.Add("bonesSkipped", s.bonesSkipped);
    j.Add("unresolvedPlayerRefs", s.unresolvedPlayerRefs);
    j.Add("truncatedSource", s.truncatedSource);
    static constexpr const char* kPartNames[kPartCount] = {"head", "body", "lhand", "rhand", "disc"};
    j.Open("mirrorGuardTrips");
    for (int p = 0; p < kPartCount; ++p) j.Add(kPartNames[p], s.basisTrips[p]);
    j.Close();
    j.Close();
}

void WriteRepairs(Json& j, const ConvertReport& report) {
    j.Open("repairs");
    j.Add("sessionsFound", report.sessionsFound);
    j.Add("sessionChosen", report.sessionChosen);
    j.Add("framesKept", report.framesKept);
    j.Add("repairedStamps", report.repairedStamps);
    j.Add("retimedWhole", report.retimedWhole);
    j.Add("leadInDropped", report.leadInDropped);
    j.Add("tailDropped", report.tailDropped);
    j.Add("handOffsetClamped", report.handOffsetClamped);
    j.Add("comRejected", report.comRejected);
    // Legacy holes are converted, not refused (R2); the list is what `echodemo verify` names.
    j.Add("requires_allow_gaps", report.requiresAllowGaps);
    j.OpenArray("gaps");
    for (const SourceGap& g : report.gapList) {
        j.Open(nullptr);
        j.Add("atS", double(g.atUs) / 1e6);
        j.Add("lengthS", double(g.lengthUs) / 1e6);
        j.Close();
    }
    j.CloseArray();
    j.Close();
}

}  // namespace

Result WriteReportJson(const ConvertReport& report, const InferFacts& infer, const EmitFacts& emit,
                       const wchar_t* sourcePath, const wchar_t* path) {
    Json j;
    j.Open(nullptr);
    j.Add("converterVersion", kConverterVersion);
    WriteSource(j, report, sourcePath);
    WriteFacts(j, infer, emit);
    WriteRepairs(j, report);

    j.Open("inferred");
    for (int r = 0; r < kRuleCount; ++r) j.Add(infer.rules[r].id, report.inferredRows[r]);
    j.Add("stunPuncherGuessed", report.stunPuncherGuessed);
    j.Add("unknownGoalTypes", report.unknownGoalTypes);
    j.Close();

    j.Open("output");
    j.Add("durationS", double(report.durationUs) / 1e6);
    j.Add("ticks", report.ticks);
    j.Add("playerRows", report.playerRows);
    j.Add("bodyRows", report.bodyRows);
    j.Add("eventRows", report.eventRows);
    j.Add("netvarRows", report.netvarRows);
    j.Close();
    j.Close();

    const std::string& text = j.Text();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0) f = nullptr;
    const bool written = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = f && fclose(f) == 0;
    if (!written || !closed) {
        if (f) DeleteFileW(path);
        return Refuse(ResultCode::Io, "the conversion report could not be written");
    }
    return Success();
}

}  // namespace replay::import
