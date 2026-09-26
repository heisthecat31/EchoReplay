#pragma once

// Every measured or hypothesised constant the converter depends on (DESIGN-FORMATS §5.3, §6.4, §14).
// Values here are the design's defaults and initial thresholds; status says whether a measurement
// stands behind them. P1 writes measured facts, P7 writes thresholds and `enabled`; both by data
// (tests/import/native_facts.json, pinned by the facts guard), never by hand.

#include <stdint.h>

namespace replay::import {

enum class FactStatus : uint8_t { kUnmeasured, kMeasured };

template <class T>
struct Fact {
    const char* id;
    T value;
    FactStatus status;
};

struct Vec3f {
    float x, y, z;
};

// Which API basis drives an engine orientation (H2, H3).
enum class OriSource : uint8_t { HeadBasis, BodyBasis, SameAsHmd };

// What the disc body's hdr0 owner means (H5).
enum class DiscOwnerMeaning : uint8_t { InHand, Possession };

enum InferRule : uint8_t {
    kRuleGeoAttach,       // I-1
    kRuleMovingAttached,  // I-2
    kRuleHeldPlayer,      // I-3
    kRuleGrip,            // I-4
    kRuleBaseline,        // I-5
    kRuleStun,            // I-6
    kRuleBoost,           // I-7
    kRuleBrake,           // I-8
    kRuleThrusters,       // I-9
    kRulePunch,           // I-10
    kRuleCount,
};

// Ungated rules are direct readings of state and always run; gated ones run only when P7's held-out
// precision >= 0.85 and recall >= 0.50 at both 30 and 15 Hz set `enabled`.
struct RuleGate {
    const char* id;
    bool gated;
    bool enabled;
};

struct InferFacts {
    RuleGate rules[kRuleCount];

    Fact<uint32_t> geoBitLeft;        // I-1: which of 0x20/0x40 is the left arm
    Fact<uint32_t> geoBitRight;
    float movingAttachedMinSpeed;     // I-2, m/s; gated: no speed threshold separates the native 0x80
    Fact<uint32_t> idleWord;          // I-5: AND of bits28 over all native idle frames
    Fact<float> stunDurationMedianS;  // I-6: used when a stun span runs off the end; unmeasured =>
                                      // Infer uses the observed truncated span
    float stunStatWindowS;            // I-6 puncher: stats.stuns increment within +-window of the edge
    float stunNearRadiusM;

    float boostWindowS;               // I-7
    float boostMinGainMps;
    float boostMinPeakAccel;
    float boostMaxTurnDeg;
    Fact<float> boostSpanS;           // how long the bit is held once fired

    float brakeMaxAccel;              // I-8, m/s^2 along v (negative)
    uint8_t brakeMinFrames;
    float brakeMaxSpeedRatio;
    float brakeMaxTurnDeg;

    float thrustMinAccel;             // I-9, m/s^2
    float thrustMaxAccel;
    uint8_t thrustMinFrames;
    float thrustMinCos;
    Fact<Vec3f> thrusterAxis;         // hand-local, sign included

    float punchMinHandSpeed;          // I-10, m/s relative to the head
    uint8_t punchMinFrames;
};

struct GoalTypeName {
    const char* apiName;  // e.g. "INSIDE SHOT"
    uint16_t code;        // PostScoreUpdate goalType, <= 10
};

struct EmitFacts {
    Fact<bool> worldAxesMatchEngine;       // H0
    Fact<Vec3f> comFromHead;               // H1: com = head.position + d
    Fact<OriSource> hmdOri;                // H2
    Fact<OriSource> bodyOri;               // H3
    Fact<bool> handAxesDirect;             // H4
    Fact<DiscOwnerMeaning> discOwner;      // H5
    Fact<float> bendBack;                  // native medians
    Fact<float> bendFwd;
    Fact<float> restPoseHeight;
    Fact<uint32_t> discIdValue;            // O-2
    Fact<uint64_t> gametypeId;             // O-4
    Fact<const uint64_t*> defaultLoadout;  // O-3: 20 Symbol64 item ids, or null when not found
    Fact<const GoalTypeName*> goalTypes;   // O-1: null-apiName terminated
    uint16_t genericGoalType;              // unknown goal_type strings map here, counted
};

inline constexpr InferFacts kInferFacts{
    .rules = {
        {"I-1", false, true},
        {"I-2", true, false},
        {"I-3", false, true},
        {"I-4", false, true},
        {"I-5", false, true},
        {"I-6", false, true},
        {"I-7", true, false},
        {"I-8", true, false},
        {"I-9", true, false},
        {"I-10", true, false},
    },
    .geoBitLeft = {"I-1.geoBitLeft", 0x20u, FactStatus::kMeasured},
    .geoBitRight = {"I-1.geoBitRight", 0x40u, FactStatus::kMeasured},
    .movingAttachedMinSpeed = 1e-3f,
    .idleWord = {"I-5.idleWord", 0x10u, FactStatus::kMeasured},
    .stunDurationMedianS = {"I-6.stunDurationMedianS", 2.0f, FactStatus::kMeasured},
    .stunStatWindowS = 0.5f,
    .stunNearRadiusM = 2.5f,
    .boostWindowS = 0.150f,
    .boostMinGainMps = 1.5f,
    .boostMinPeakAccel = 10.0f,
    .boostMaxTurnDeg = 45.0f,
    .boostSpanS = {"I-7.boostSpanS", 0.2222f, FactStatus::kMeasured},
    .brakeMaxAccel = -6.0f,
    .brakeMinFrames = 2,
    .brakeMaxSpeedRatio = 0.6f,
    .brakeMaxTurnDeg = 30.0f,
    .thrustMinAccel = 0.8f,
    .thrustMaxAccel = 4.0f,
    .thrustMinFrames = 5,
    .thrustMinCos = 0.7f,
    .thrusterAxis = {"I-9.thrusterAxis", {0.0f, 0.0f, 1.0f}, FactStatus::kMeasured},
    .punchMinHandSpeed = 5.0f,
    .punchMinFrames = 2,
};

// O-1, read from the engine's goal-type namer sub_14016EE60 (a switch on the code; 0 is "[NO GOAL]").
inline constexpr GoalTypeName kGoalTypeNames[] = {
    {"SLAM DUNK", 1},   {"INSIDE SHOT", 2},      {"LONG SHOT", 3},     {"BOUNCE SHOT", 4},
    {"LONG BOUNCE SHOT", 5}, {"HEADBUTT", 6},    {"LONG HEADBUTT", 7}, {"BUMPER SHOT", 8},
    {"LONG BUMPER SHOT", 9}, {"SELF GOAL", 10},  {nullptr, 0},
};

inline constexpr EmitFacts kEmitFacts{
    .worldAxesMatchEngine = {"H0", true, FactStatus::kMeasured},
    .comFromHead = {"H1", {0.0f, 0.0f, 0.0f}, FactStatus::kMeasured},
    .hmdOri = {"H2", OriSource::BodyBasis, FactStatus::kMeasured},
    .bodyOri = {"H3", OriSource::HeadBasis, FactStatus::kMeasured},
    .handAxesDirect = {"H4", true, FactStatus::kMeasured},
    .discOwner = {"H5", DiscOwnerMeaning::Possession, FactStatus::kMeasured},
    .bendBack = {"P1.bendBack", 0.17453f, FactStatus::kMeasured},
    .bendFwd = {"P1.bendFwd", -1.02974f, FactStatus::kMeasured},
    .restPoseHeight = {"P1.restPoseHeight", 1.775f, FactStatus::kMeasured},
    .discIdValue = {"O-2.discIdValue", 0u, FactStatus::kMeasured},
    .gametypeId = {"O-4.gametypeId", 0xCB60A4DE7E1CAF73ull, FactStatus::kUnmeasured},  // echo_arena
    .defaultLoadout = {"O-3.defaultLoadout", nullptr, FactStatus::kUnmeasured},
    .goalTypes = {"O-1.goalTypes", kGoalTypeNames, FactStatus::kMeasured},
    .genericGoalType = 0,  // O-1 settles the engine's generic code
};

}  // namespace replay::import
