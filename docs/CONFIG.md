# Configuration

## Where settings live

| What | Where |
|---|---|
| Config file | `bin\win10\echoreplay\config.json` (inside the game folder) |
| Log | `bin\win10\echoreplay.log` |
| Recordings | `Documents\EchoReplay` (change with `capture.dir`) |

`config.json` is created on the first launch with every key, its default and a one-line comment.
Delete it to get the defaults back. It is read when the game starts.

- Keys are dotted: `"capture.voip": false`. Nested objects work too: `"capture": { "voip": false }`.
- `//` line comments are allowed.
- A key the plugin does not know, or a value of the wrong type, is ignored and named in the log.

**Environment variables.** Keys marked *env* below can also be set per launch with an environment
variable: `ECHOREPLAY_` + the key in upper case with dots as underscores. For example
`ECHOREPLAY_PLAYBACK_HOST_CLOCK_PIN=0`. The variable wins over the file.

`ECHOREPLAY_OPEN=<file>` is not a config key: it tells the plugin which replay to open at startup.
`tools\watch_replay` sets it for you.

## Recording

| Key | Default | What it does |
|---|---|---|
| `capture.enabled` | `true` | Master switch. `false` records nothing. |
| `capture.auto_record` | `true` (env) | Record every live match you play or spectate. `false` means you start recordings yourself (Record button or `record start`). |
| `capture.dir` | `""` | Output folder. Empty means `Documents\EchoReplay`. |
| `capture.rec_indicator` | `true` (env) | Small REC marker in the corner while recording. |
| `capture.voip` | `true` | Record and replay voice chat. |
| `capture.oracle` | `true` | Store extra session/skeleton snapshots used to validate playback. Makes files slightly larger. |
| `capture.section.asseen` | `true` | Store the per-player "as seen" timing, needed for `mode asseen`. |
| `capture.force_offline` | `false` (env) | Also record sessions that are not live online matches. For development. |

The `capture.stream.*` keys switch individual data streams off. Leave them on unless you know you
need a smaller file: whatever a stream carries is simply missing from the replay.

## Replay player

| Key | Default | What it does |
|---|---|---|
| `ui.bar` | `true` | The player window (timeline, play/pause, speed, `=` menu). |
| `ui.bar_anchor` | `"bottom"` | `bottom` or `top`: which edge of the game window the player sits on. |
| `ui.bar_margin_px` | `96` | Gap between the player and that edge, in pixels. |
| `ui.player_width` | `0` | Player width in pixels. `0` fits the content. The `=` menu's width buttons change it. |
| `ui.player_autohide_s` | `0.0` | Hide the player after this many idle seconds. `0` never hides (use F9). |
| `ui.bar_markers` | `"rounds,goals,bookmarks"` | Which markers the timeline draws. |
| `ui.bar_drag` | `true` | Drag on the timeline to scrub. `false` is click-to-seek only. |
| `ui.bar_keys` | `true` | Keyboard shortcuts while the cursor is on the player (see the README). |
| `ui.bar_keys_global` | `false` | Also accept `K` (play/pause) and `,` `.` (frame step) without cursor mode. |
| `ui.debug_panel` | `false` | The old detailed "Echo Replay" panel as a separate window. |
| `control.console` | `"auto"` | `auto`, `on` or `off`: the console window for typed commands. |

## Playback

| Key | Default | What it does |
|---|---|---|
| `playback.motion_smoothing` | `"slowmo"` | `off`: exact 1:1 motion at every speed. `slowmo`: smooth only below 1x. `on`: smooth at every speed. The `=` menu changes it for the session. |
| `playback.motion_smoothing_strength` | `2` | `1` low, `2` medium, `3` high. Higher is smoother but rounds off fast movements more. |
| `playback.scrub_light` | `true` | Cheap seeks while dragging the timeline, a full seek when you let go. |
| `playback.reload_via_level_cycle` | `true` (env) | Opening another replay reloads the level so nothing carries over. `false` reloads in place. |
| `playback.preroll` | `true` (env) | Run the game's pre-match countdown at load (sped up) so the match is in play from the first second. |
| `playback.stats` | `true` (env) | Feed recorded stats to the HUD and scoreboard. |
| `playback.goal_explosion` | `true` (env) | Play the goal explosion on replayed goals. |
| `playback.goal_horn` | `false` (env) | Also trigger the scorer's goal fanfare directly. Off because the game already plays it. |
| `playback.async_window_decode` | `false` (env) | Decode the next part of the file on a worker thread, removing the short hitch at each chapter boundary. Experimental. |
| `session.gametype_override_name` | `"echo_arena"` | Game mode used when a recording does not say which mode it was. |

## Audio

| Key | Default | What it does |
|---|---|---|
| `audio.foley_mix` | `"headset"` | Sound mix for distant players. `headset`: what a live spectator hears (far sounds fade out). `broadcast`: the spectator-stream mix (far sounds stay audible). `engine`: leave it to the game. |
| `audio.seek_mute` | `true` | Mute while seeking so a jump does not play a burst of sound. |
| `audio.wav_capture` | `false` | Also write the replay's audio mix to a WAV file. |

## Networking and safety

These keep a replay session offline and unable to talk to the internet. The defaults are correct; change
them only for debugging.

| Key | Default | What it does |
|---|---|---|
| `session.offline_patch` | `true` | In a replay session, apply the offline mode in memory so EchoRelay is not needed. Skipped if EchoRelay already applied it; never used in a live session. |
| `net.loopback_only` | `"auto"` | During replay, bind every socket to localhost only. |
| `bootstrap.block_egress` | `"auto"` | During replay, refuse every non-localhost connection. Never active in normal play. |
| `control.http_routes` | `true` | Control API on the game's local HTTP server (replay sessions only). |
| `control.http_routes_live` | `false` (env) | Also register that API during live play. |

## Logging

| Key | Default | What it does |
|---|---|---|
| `log.level` | `"info"` | `trace`, `info`, `warn` or `error`. |
| `bootstrap.minimal` | `false` (env) | Load but hook nothing except the frame tick. Use it to check whether a problem is caused by the plugin. |

## Troubleshooting switches

Each of these turns off one piece of the replay so you can tell whether it causes a problem. All
default to on and all take an environment variable.

| Key | Turns off |
|---|---|
| `playback.host_clock_pin` | Holding the game's own round clock so it cannot end the match mid-replay. |
| `playback.disc_score_gate` | Stopping the game from scoring goals on its own during replay. |
| `playback.disc_shell` | Showing the disc's spinning shell after each recorded round restart. |
| `playback.gamestate_clock`, `playback.gamestate_score` | Showing the recorded clock and score on the HUD. |
| `playback.goal_events` | Goal banner, celebration and announcer on replayed goals. |
| `playback.stun_events` | Stun sound, effect and timeout on replayed stuns. |
| `playback.component_events` | Launch-tube light sequences. |
| `playback.dress`, `playback.dress_loadout` | Recorded names, teams, colours and cosmetics. |
| `playback.hermite` | Curved interpolation through recorded velocities (off = straight lines between 45 Hz samples). |
| `playback.lod_pin` | Holding remote players' level-of-detail steady. |

Every other key in `config.json` is an internal switch for individual engine hooks. They are listed
with comments in the generated file; leave them at their defaults.
