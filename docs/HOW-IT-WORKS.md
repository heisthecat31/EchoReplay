# How it works

EchoReplay is one DLL that runs inside `echovr.exe`. In a live match it is a recorder; in an offline
match it is a viewer. Hooks are MinHook detours, and most of the work is done by calling the game's own
functions rather than re-creating their effects.

```
 LIVE MATCH (online)                          REPLAY (offline, localhost only)

 game network ──► engine state                .echodemo ──► decoder (3 chapters in memory)
                     │                                          │
                     ▼ copy each frame                          ▼ sample at playhead
                ring buffer                               apply to the engine
                     │                                    ├─ player pawns + poses
                     ▼ worker thread                      ├─ disc, clock, score, stats
                zstd chapters ──► .echodemo               ├─ goals, stuns, events
                                                          └─ voice, audio pitch
                                                                │
                                        player window ──► transport (play/seek/rate)
```

## Startup

- [EchoLoader](https://github.com/EchoTools/EchoLoader) loads every DLL in `bin\win10\plugins`. The
  plugin's first action is to write an "attached" line to `bin\win10\echoreplay.log`.
- `DllMain` does the minimum: single-instance check, config read, the few hooks that must run before
  the game parses its command line, then one worker thread.
- The worker installs the remaining hooks. Each hook site is checked against the expected bytes first.
  A mismatch (another game build, another mod's patch) refuses that hook and logs it.
- Per-frame work runs from a free engine "provider" slot, so the engine calls the plugin once per frame
  before its simulation. A hook is the fallback if the slot is taken.

Every game address, offset and byte signature the plugin uses lives in one registry,
`facts/facts.tsv`. `tools/gen_facts.py` turns it into `src/generated/facts_generated.h`.

## Offline mode

Replays need the game to host a match locally without logging in. Stock Echo VR cannot do that; the
EchoRelay project added it with `-offline -windowed` switches. EchoReplay now does the same thing
itself, so EchoRelay is not needed.

- It only happens in a replay session, which means `ECHOREPLAY_OPEN` is set. In a live session nothing
  is installed.
- The plugin waits for the moment the game applies its command line, sets the "not in VR" (windowed)
  flag, and rewrites four small code sites in memory: host the `-level` locally, take the offline host
  path, skip the login-failure handling, and route the tutorial through the same offline call.
- The file on disk is never written.
- If all four sites are already patched (EchoRelay did it), the plugin writes nothing. If any byte is
  unexpected, it writes nothing and logs every site.
- `session.offline_patch = false` turns it off.

`tools\watch_replay` passes only stock switches (`-level`, `-gametype`, the login hosts pinned to
`127.0.0.1`), because the game discards its whole command line when it sees a switch it does not know.

## Recorder

- Runs only in a live online match, played or spectated.
- Each frame the game thread only **copies** engine state into a ring buffer: no allocation, no locks,
  no file I/O. The engine kills the process if a frame stalls for about 10 s, so nothing there may block.
- A worker thread groups the data into chapters, compresses them with zstd and appends them to the
  file. A keyframe every 2 s makes seeking cheap.
- Recorded streams: engine tick, player state (as received and as sampled), bodies including the disc,
  networked variables, gameplay events (goals, stuns, joins, stats, emotes), world state, round
  markers, game state (phase, clock, score), per-player stats, voice, and each player's names and
  cosmetics whenever they change.
- Voice is recorded before your mute list is applied; mutes apply again at playback.
  `echodemo strip --voip` removes voice from a file before sharing it.

File layout: [FORMAT.md](FORMAT.md).

## Viewer

**Opening.** `tools\watch_replay` starts the game with `ECHOREPLAY_OPEN=<file>` set for that process
only. The plugin enters an offline match at the recorded level through the game's own offline-match
call. The in-game Open list and the `load` console command do the same from inside a session.

**Players.** Every recorded player gets a real pawn, created through the game's own player-join path.
You are seated as a spectator, which gives the free camera. A "dress" pass then applies names, numbers,
teams, colours and cosmetics, and checks the game accepted each one. When the replay closes, players
leave through the game's own leave handler.

**Feeding the engine.** One function drives everything at playhead time T, whether playing, paused,
reversed, scrubbing or seeking:

- Poses: each body gets one interpolated sample per frame, marked as owned by the recording so the local
  simulation does not overwrite it. Between the 45 Hz samples the path is a cubic curve through the
  recorded velocities.
- Recorded grab and thruster inputs are replayed, so the game runs its own grab logic and sounds.
- The recorded disc is bound to the arena's disc and driven, including who holds it.
- Clock, countdown, game state and score go through the game's own networked-variable setters, so the
  HUD and scoreboard redraw normally. Stat messages go through the game's own stat handlers.

**Transport.** The playhead is a recorded-tick index plus a fraction, so 1x forward and 1x reverse play
exactly one recorded tick per engine tick. Speeds run from 0.1x to 4x in either direction. Three
chapters stay decoded in memory. Every seek runs a cleanup pass through the game's own reset functions
(interpolation history, grab fix-ups, sleeping physics bodies) so nothing from before the jump lingers.
While you drag the timeline, cheaper "light" seeks run, and a full one happens when you let go.

**Preroll.** Before playback starts, the offline match's own pre-game countdown runs at 4x (launch at
2x) with the playhead held, so the arena is in play from the first frame.

**Opening another file.** The viewer waits for the current players to leave, then re-enters the level
for a clean match. If that fails it reloads in place.

## Motion smoothing

At slow speeds, straight interpolation between samples shows a small change of velocity at every
sample. Smoothing fixes that:

- Each sample is replaced by a centred, weighted fit of its neighbours (Gaussian width 18, 30 or 50 ms
  for Low, Med, High). A smooth curve is then drawn through the fitted points.
- Centred means no lag. Constant velocity and constant acceleration (a disc in flight) pass through
  unchanged.
- Breaks stay sharp: respawns, teleports, grabs, possession changes and gaps in the data.
- The default `slowmo` smooths only below 1x, so 1x, reverse and fast speeds stay exact. `off` is the
  unmodified path at every speed.
- Changing mode crossfades over 200 ms.

## Keeping the host's game mode quiet

The offline match is a real arena match, and its own rules keep running under the replay.

| Piece | Problem | Fix |
|---|---|---|
| Clock pin | The host's round timer runs out and ends the match mid-replay. | Hold the arena script's timer at 3600 s through its own setter. |
| Score gate | The replayed disc is a real object, so the host would score it and reset everyone. | While a replay is loaded, the host's scoring callback does nothing. |
| Disc shell | The disc's spinning shell followed the host's rounds, not the recording's. | Show it at each recorded round restart; clear it on the recorded grab. |

## Audio

- **Foley mix.** The game has two sound profiles for distant players: "headset" (what a live spectator
  hears; thrusters cut off past about 10 m) and "broadcast" (longer range). `audio.foley_mix` picks
  one; the default `headset` makes a replay sound like the live match.
- **Speed.** Below 1x the mix slows and is pitched down. Paused is silent, reverse is muted, above 1x
  plays at normal pitch.
- **Seeks** are muted by routing to a silent output the game ships with.
- **Voice.** Recorded voice goes back into the game's own voice receiver for the matching player, so it
  is positional. It plays at 1x forward only.

## Event feeds

Recorded messages are fed to the game's own handlers.

- **Goals:** banner, celebration and announcer run from the recorded score message. The explosion gets
  the scoring team's colour.
- **Stuns:** the effect and sound part of the stun handler runs; stats are replayed separately.
- **Other events:** only an allow-list passes (currently the launch-tube lights).

## Live safety

Nothing the viewer does may affect a real online match.

- Every playback path that writes game state goes through one check: the session must be offline and
  the offline flag locked. An unclear session is refused, so `load` during a live match does nothing.
- The offline patch is never installed in a live session.
- During replay, sockets bound to all interfaces are rebound to localhost, and outgoing connections to
  anything but localhost are refused. Neither is active in a live match.
- The plugin's control API on the game's local HTTP server is not registered in live sessions.

## Converting old replays

| Format | Source | Contents |
|---|---|---|
| `.echoreplay` | Spark | Zip of text lines: timestamp + the game's session JSON, at 10, 15 or 30 Hz. |
| `.tape` | [EchoTools/tape](https://github.com/EchoTools/tape) | zstd-compressed protobuf frames. |

The converter reads the file, repairs duplicates and gaps (gaps are kept and listed), resamples to
45 Hz and writes a normal `.echodemo` marked as imported. Some things are not in these formats and
are inferred from motion:

| Inferred | Status |
|---|---|
| Grabs, held players, hands attached to the arena | On |
| Stuns and goals | On (exact in tests) |
| Boost, brake, thrusters, punches | Off: not accurate enough, so these sounds are missing |

Voice, skeleton poses and cosmetics do not exist in these formats. Only `mpl_arena_a` matches convert.

In the game, a legacy file is converted once on a worker thread into `<capture dir>\imported\`; the
cache is trimmed to about 2 GB, oldest first. In-game conversion has a memory cap; longer files are
refused with a message pointing to `echodemo convert`, which has no cap and writes a report
(`<out>.import.json`) of repairs, gaps and inferred rows.
