#include "engine/oracletap.h"

#include "bootstrap/buildguard.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"

#include <stdio.h>
#include <string.h>

namespace replay {

namespace {

struct Site {
    const char*    factId;
    uint64_t       va;
    const uint8_t* prologue;
};

// The three sites the tap will ever call into, and the two handlers it deliberately will NOT.
// Both handlers have zero code cross-references in the whole .text section, so they are reachable
// only through libevent's callback node with a request object we do not have; they are listed here
// so a probe can report that they are intact, never so that anything calls them.
const Site kSites[] = {
    {"ORACLE.SESSION_BUILDER", facts::ORACLE_SESSION_BUILDER, facts::ORACLE_SESSION_BUILDER_PROLOGUE},
    // ★ The SECOND /session builder. The handler picks between the two on a type predicate, and
    // S5.4.7 names only the first. A tap bound to one arm returns an empty document in the other
    // mode, which reads as "nothing to report" rather than as "this mode is not handled".
    {"ORACLE.SESSION_BUILDER_ALT", facts::ORACLE_SESSION_BUILDER_ALT,
     facts::ORACLE_SESSION_BUILDER_ALT_PROLOGUE},
    {"ORACLE.BONES_BUILDER", facts::ORACLE_BONES_BUILDER, facts::ORACLE_BONES_BUILDER_PROLOGUE},
    {"ORACLE.SESSION_ROUTE_HANDLER", facts::ORACLE_SESSION_ROUTE_HANDLER,
     facts::ORACLE_SESSION_ROUTE_HANDLER_PROLOGUE},
    {"ORACLE.BONES_ROUTE_HANDLER", facts::ORACLE_BONES_ROUTE_HANDLER,
     facts::ORACLE_BONES_ROUTE_HANDLER_PROLOGUE},
};
constexpr size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

bool DefaultClock(uint64_t* simTimeUs, uint32_t* frameIndex) {
    uint64_t scaled = 0, sim = 0, frame = 0;
    if (!tick::ReadEngineClock(&scaled, &sim, &frame)) {
        return false;
    }
    *simTimeUs = sim;
    *frameIndex = uint32_t(frame);
    return true;
}

class Tap final : public IOracleTap {
public:
    Tap(uint8_t* sessionBuf, uint32_t sessionCap, uint8_t* bonesBuf, uint32_t bonesCap)
        : m_sessionBuf(sessionBuf), m_sessionCap(sessionCap), m_bonesBuf(bonesBuf),
          m_bonesCap(bonesCap) {
        snprintf(m_why, sizeof(m_why),
                 "no builders bound; the two prototypes are integration item V-I-W2I1 and this tap "
                 "will not call an address whose argument shape it guessed");
    }

    bool Sample(OracleSample& out) override {
        memset(&out, 0, sizeof(out));

        uint64_t simBefore = 0, simAfter = 0;
        uint32_t frameBefore = 0, frameAfter = 0;
        if (!m_clock(&simBefore, &frameBefore)) {
            m_status = TapStatus::NoClock;
            snprintf(m_why, sizeof(m_why), "the engine clock is not readable; nothing was invoked");
            out.status = m_status;
            return false;
        }

        if (m_session == nullptr || m_bones == nullptr) {
            m_status = TapStatus::NotBound;
            out.status = m_status;
            // The bracket is still published: a caller that only wants to know whether the two
            // readings hold across a pair of calls can see that here, and the probe reports it.
            out.bracket.simTimeBeforeUs = simBefore;
            out.bracket.frameIndexBefore = frameBefore;
            out.bracket.simTimeAfterUs = simBefore;
            out.bracket.frameIndexAfter = frameBefore;
            return false;
        }

        const uint32_t sessionLen = m_session(m_sessionCtx, m_sessionBuf, m_sessionCap);
        const uint32_t bonesLen = m_bones(m_bonesCtx, m_bonesBuf, m_bonesCap);

        if (!m_clock(&simAfter, &frameAfter)) {
            m_status = TapStatus::NoClock;
            snprintf(m_why, sizeof(m_why),
                     "the engine clock stopped being readable between the two builders");
            out.status = m_status;
            return false;
        }

        out.bracket.simTimeBeforeUs = simBefore;
        out.bracket.simTimeAfterUs = simAfter;
        out.bracket.frameIndexBefore = frameBefore;
        out.bracket.frameIndexAfter = frameAfter;

        if (sessionLen == 0) {
            m_status = TapStatus::SessionBuilderFailed;
            snprintf(m_why, sizeof(m_why), "the /session builder wrote 0 bytes");
            out.status = m_status;
            return false;
        }
        if (bonesLen == 0) {
            m_status = TapStatus::BonesBuilderFailed;
            snprintf(m_why, sizeof(m_why), "the /player_bones builder wrote 0 bytes");
            out.status = m_status;
            return false;
        }

        out.sessionBytes = m_sessionBuf;
        out.sessionLen = sessionLen;
        out.bonesBytes = m_bonesBuf;
        out.bonesLen = bonesLen;

        // ★ The payloads are published either way.  A skewed pair is REPORTED with its evidence, not
        // discarded: hiding it would leave the caller unable to tell "the tap did not run" from "the
        // tap ran and the two builders straddled a frame", which are different faults.
        if (simBefore != simAfter || frameBefore != frameAfter) {
            m_status = TapStatus::Skewed;
            snprintf(m_why, sizeof(m_why),
                     "a frame boundary fell between the two builders: sim %llu->%llu, frame %u->%u",
                     (unsigned long long)simBefore, (unsigned long long)simAfter, frameBefore,
                     frameAfter);
            out.status = m_status;
            return false;
        }

        m_status = TapStatus::Ok;
        m_why[0] = '\0';
        out.status = m_status;
        return true;
    }

    TapStatus   Status() const override { return m_status; }
    const char* Diagnostic() const override { return m_why; }

    void Bind(BuilderFn session, void* sessionCtx, BuilderFn bones, void* bonesCtx) {
        m_session = session;
        m_sessionCtx = sessionCtx;
        m_bones = bones;
        m_bonesCtx = bonesCtx;
    }
    void BindClock(ClockFn fn) { m_clock = (fn != nullptr) ? fn : &DefaultClock; }

private:
    uint8_t*  m_sessionBuf;
    uint32_t  m_sessionCap;
    uint8_t*  m_bonesBuf;
    uint32_t  m_bonesCap;
    BuilderFn m_session = nullptr;
    void*     m_sessionCtx = nullptr;
    BuilderFn m_bones = nullptr;
    void*     m_bonesCtx = nullptr;
    ClockFn   m_clock = &DefaultClock;
    TapStatus m_status = TapStatus::NotBound;
    char      m_why[192];
};

}  // namespace

const char* TapStatusName(TapStatus s) {
    switch (s) {
        case TapStatus::Ok:                   return "ok";
        case TapStatus::NoClock:              return "no-clock";
        case TapStatus::NotBound:             return "not-bound";
        case TapStatus::SiteRefused:          return "site-refused";
        case TapStatus::SessionBuilderFailed: return "session-builder-failed";
        case TapStatus::BonesBuilderFailed:   return "bones-builder-failed";
        case TapStatus::Skewed:               return "skewed";
    }
    return "?";
}

namespace oracletap {

bool VerifySites(char* why, size_t whyCap) {
    size_t bad = 0;
    const char* first = nullptr;
    for (size_t i = 0; i < kSiteCount; ++i) {
        if (!buildguard::VerifySite(kSites[i].factId, kSites[i].va, kSites[i].prologue, 20)) {
            if (first == nullptr) {
                first = kSites[i].factId;
            }
            ++bad;
        }
    }
    if (bad == 0) {
        snprintf(why, whyCap, "all %u oracle sites match their recorded prologue",
                 unsigned(kSiteCount));
        return true;
    }
    snprintf(why, whyCap, "%u of %u oracle sites disagree with their prologue, first %s", unsigned(bad),
             unsigned(kSiteCount), first);
    return false;
}

size_t      SiteCount() { return kSiteCount; }
const char* SiteFactId(size_t i) { return i < kSiteCount ? kSites[i].factId : nullptr; }
uint64_t    SiteAddress(size_t i) { return i < kSiteCount ? kSites[i].va : 0; }

IOracleTap* Create(uint8_t* sessionBuf, uint32_t sessionCap, uint8_t* bonesBuf, uint32_t bonesCap) {
    if (sessionBuf == nullptr || sessionCap == 0 || bonesBuf == nullptr || bonesCap == 0) {
        return nullptr;
    }
    return new Tap(sessionBuf, sessionCap, bonesBuf, bonesCap);
}

void Destroy(IOracleTap* t) { delete static_cast<Tap*>(t); }

void BindBuilders(IOracleTap* t, BuilderFn session, void* sessionCtx, BuilderFn bones,
                  void* bonesCtx) {
    if (t != nullptr) {
        static_cast<Tap*>(t)->Bind(session, sessionCtx, bones, bonesCtx);
    }
}

void BindClock(IOracleTap* t, ClockFn fn) {
    if (t != nullptr) {
        static_cast<Tap*>(t)->BindClock(fn);
    }
}

}  // namespace oracletap
}  // namespace replay
