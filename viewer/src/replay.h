#pragma once
// The replay model: one or more .echodemo parts on one timeline, each player's head / hands / body as
// tracks, the disc, the score and the events. Sampled at any time t (seconds on the joined timeline).

#include <stdint.h>

#include <filesystem>
#include <string>
#include <vector>

#include "erv_api.h"
#include "vmath.h"

namespace erv {

struct PoseKey {
    double t;
    Q4 head_q, l_q, r_q;
    V3 l_off, r_off;
    float trig_l, trig_r;
    uint32_t hold_l, hold_r;
};

struct BodyKey {
    double t;
    V3 com;
    Q4 ori;
    V3 vel;
};

struct PlayerTrack {
    uint32_t slot = 0;
    uint32_t team = 0xFFFF;
    std::string name;
    std::vector<PoseKey> pose;     // sorted by t
    std::vector<BodyKey> body;     // head position over time (the player's net body)
    double first = 0, last = 0;    // when the player is in the recording
    std::vector<double> voice;     // times a voice frame arrived
    std::vector<std::pair<double, double>> stunned;   // [from, to]
    bool guessed = false;
    bool recorder = false;         // the person who recorded (the local engine's own samples)          // body matched to this name by order only (the recording did not say whose it was)
};

// One speaker's recorded voice: Opus frames, kept apart from the drawn players (a speaker may have no pose).
struct VoiceTrack {
    uint32_t slot = 0;
    std::string name;
    std::vector<double> t;
    std::vector<uint32_t> off;     // frame k is bytes[off[k], off[k+1])
    std::vector<uint8_t> bytes;
};

struct Event {
    double t;
    uint32_t kind;   // ERV_EVENT_*
    int a = -1, b = -1;   // player track index (stun: victim, puncher; goal: scorer, assist), -1 none
    uint32_t team = 0, points = 0;
    float f0 = 0, f1 = 0;
    uint32_t goal_type = 0;   // goal: 1 SLAM DUNK .. 10 SELF GOAL, 0 generic
};

struct State {
    double t;
    uint32_t phase, round, blue, orange;
    float clock;
    float countdown = 0;
};

// One roster row of one keyframe: who held an entrant number then, on which team, and whether they were active.
struct RosterRow {
    double t;
    uint32_t entrant, generation, team, active;
};

struct Part {
    std::filesystem::path path;
    uint64_t level_id = 0, gametype_id = 0;
    std::string level_name;   // from EchoConstruct's roster ("" unknown)
    double offset = 0;        // where this part starts on the joined timeline
    double duration = 0;
    std::vector<PlayerTrack> players;
    std::vector<BodyKey> disc;
    std::vector<State> states;
    std::vector<Event> events;
    std::vector<VoiceTrack> voices;
    std::vector<RosterRow> roster;   // by t (keyframes)
    bool imported = false;
    uint32_t voice_frames = 0;
};

struct Replay {
    std::vector<Part> parts;
    double duration = 0;
    std::string title;
    std::string error;

    // A .echodemo, or an EDITH clip's .json (its parts in order).
    bool Load(const std::filesystem::path& file);
    int PartAt(double t) const;
};

// Sampling one player at local time t (part time). False when the player isn't present then.
struct PlayerPose {
    V3 head;
    Q4 head_q;
    Q4 body_q;                     // the yaw-only body basis (the pose's hmdOri); head_q is the full head (BodyKey.ori)
    V3 lhand, rhand;
    Q4 l_q, r_q;
    float trig_l, trig_r;
    V3 vel;
    bool speaking, stunned;
    bool body_only;                // no pose recorded: head and hands are placed from the body
};
bool SamplePlayer(const PlayerTrack&, double t, PlayerPose& out);
bool SampleDisc(const Part&, double t, V3& pos, Q4& ori, V3& vel);
const State* StateAt(const Part&, double t);
// The team `track` is on at t (0 blue, 1 orange, 2 spectator), or -1 when they are not in the match then: from the
// roster's latest keyframe at or before t. A track the roster never names (the recorder) counts while it has samples.
int TeamAt(const Part&, const PlayerTrack& track, double t);

// What the recorder is called in the viewer (the file does not name them): "You" unless set (ini `me=`).
const std::string& RecorderName();
void SetRecorderName(const std::string&);

// Level id -> level name, from EchoConstruct's registry/level_roster.tsv.
std::string LevelName(uint64_t id);
std::string PrettyLevel(const std::string& name);   // "mpl_arena_a" -> "Arena"
std::vector<std::pair<uint64_t, std::string>> KnownMaps();   // the maps worth a menu preview

}  // namespace erv
