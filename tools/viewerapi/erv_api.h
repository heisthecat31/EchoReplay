#pragma once
// echoreplay_reader.dll: a C API over the .echodemo reader, for EchoReplayViewer.
//
// The DLL links EchoReplay's own decoders (static CRT); this header is plain C so a program on any
// CRT can use it. One call decodes a whole file into flat arrays that stay valid until erv_close.
//
// Times are seconds from the file's first CORE chapter. World space is the engine's own.
//
// How a player is put together (EchoReplay import H1 + the wire layout):
//   head position   = the player's body com (a body sample with idTag 0 and idValue = the user slot)
//   head rotation   = ErvPlayerSample.head_q
//   hand positions  = head position + l_off / r_off (world axes, not rotated)
//   hand rotations  = l_q / r_q

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ERV_BUILD
#define ERV_API __declspec(dllexport)
#else
#define ERV_API __declspec(dllimport)
#endif

#define ERV_API_VERSION 3
#define ERV_TAG_LOCAL 0xFFFFu
// v2: a player id is the engine's SR15NetUserId, {u16 slot, u16 generation} (slot | generation << 16), so a slot
// that someone else takes later is a different player. The recorder's own samples (the local engine's
// per-frame sample, which has no user id) use ERV_SLOT_LOCAL.
#define ERV_SLOT_LOCAL 0xFFFFFFFEu

typedef struct ErvInfo {
    uint64_t level_id, gametype_id;
    uint64_t t0_us;          // wall clock (unix us) the recording started
    double   duration_s;
    uint32_t player_count, player_sample_count, body_sample_count, state_count, event_count, voice_count;
    uint32_t imported;       // converted from .echoreplay / .tape (lower rate, no voice, no cosmetics)
    uint32_t recovered;      // the file was never closed cleanly and was read by the recovery scan
} ErvInfo;

typedef struct ErvPlayer {
    uint32_t slot;           // user slot: what samples, holds and voice refer to
    uint32_t team;           // 0 blue, 1 orange, 2 spectator, 0xFFFF none
    char     name[40];
} ErvPlayer;

typedef struct ErvPlayerSample {
    double   t;
    uint32_t slot;
    float    head_q[4];      // x y z w
    float    l_q[4], r_q[4];
    float    l_off[3], r_off[3];
    float    trig_l, trig_r;
    uint32_t hold_l, hold_r; // slot held, 0xFFFFFFFF none
    uint32_t bits;           // the raw 28-bit state word
    uint32_t sampled;        // 1 the local engine's per-frame sample, 0 a network (wire) record
} ErvPlayerSample;

typedef struct ErvBodySample {
    double   t;
    uint32_t id_tag;         // 0 a user (id_value = slot), 1 a component, >=2 a net id,
                             // ERV_TAG_LOCAL a body with no net identity (id_value = capture pool handle):
                             // the recording player's own body, and anything else simulated only locally
    uint32_t sampled;
    uint64_t id_value;
    float    com[3], ori[4], vel[3];
} ErvBodySample;

typedef struct ErvState {
    double   t;
    uint32_t phase, round;
    uint32_t blue_points, orange_points;
    uint32_t disc_owner;     // frisbee owner id as recorded
    float    game_timer, countdown;
} ErvState;

enum { ERV_EVENT_STUN = 1, ERV_EVENT_GOAL = 2, ERV_EVENT_STAT = 3 };

typedef struct ErvEvent {
    double   t;
    uint32_t kind;
    uint32_t a, b;           // stun: victim, puncher. goal: scorer, assist
    uint32_t team, points;   // goal
    float    f0, f1;         // stun: duration. goal: speed, distance
    uint32_t goal_type;      // goal: the game's goal type (1 SLAM DUNK .. 10 SELF GOAL; 0 generic)
} ErvEvent;

typedef struct ErvVoice {
    double         t;
    uint32_t       slot;
    uint32_t       bytes;
    const uint8_t* payload;  // the game's own VOIP frame, verbatim
} ErvVoice;

// Every roster row of every keyframe, in file order: who held which entrant number when. The players list
// above is the last word per number; this is the history (joins, leaves, team changes, a number reused).
typedef struct ErvRosterRow {
    double   t;              // the keyframe's time
    double   first_seen, last_seen;   // the row's own first / last sighting (seconds on the same clock; 0 unknown)
    uint64_t platform_id;    // xplatform id (lobby rows: platform code)
    uint32_t entrant, generation, team, active;
    char     name[40];
} ErvRosterRow;

typedef struct ErvFile ErvFile;

ERV_API int      erv_version(void);
// NULL on failure; erv_last_error() says why.
ERV_API ErvFile* erv_open(const wchar_t* path);
ERV_API const char* erv_last_error(void);
ERV_API void     erv_close(ErvFile*);
// Fast: header, manifest and chapter index only (no decode). For a file browser. 0 on failure.
ERV_API int      erv_peek(const wchar_t* path, ErvInfo* out);

ERV_API const ErvInfo*         erv_info(const ErvFile*);
ERV_API const ErvPlayer*       erv_players(const ErvFile*);
ERV_API const ErvPlayerSample* erv_player_samples(const ErvFile*);   // sorted by t
ERV_API const ErvBodySample*   erv_body_samples(const ErvFile*);     // sorted by t
ERV_API const ErvState*        erv_states(const ErvFile*);
ERV_API const ErvEvent*        erv_events(const ErvFile*);
ERV_API const ErvVoice*        erv_voices(const ErvFile*);
ERV_API const ErvRosterRow*    erv_roster(const ErvFile*, uint32_t* count);

#ifdef __cplusplus
}
#endif
