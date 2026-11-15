# EchoReplay

Record Echo VR matches and watch them back inside the game.

EchoReplay is a plugin for the PC version of Echo VR. While you play or spectate a live match, it
records what the game receives into an `.echodemo` file. Later the game plays that file back offline,
in the real arena, with its own rendering, HUD and sound.

This project is not affiliated with Ready At Dawn or Meta.

## Features

- Records every live match you play or spectate. A small REC marker shows while it runs.
- Plays back in the game itself: players, disc, goals, stuns, scoreboard, clock and stats.
- Player window: play/pause, reverse, 0.1x to 4x, frame step, skip buttons, and a timeline with round,
  goal and bookmark markers.
- Motion smoothing for slow motion (optional; off gives exact 1:1 playback).
- Recorded voice chat, played from the player who spoke.
- Camera modes: player POV, follow, disc, side and free camera.
- Opens older replays: Spark `.echoreplay` and EchoTools `.tape` files are converted when opened.
- Replay sessions run offline and do not connect to any server. EchoRelay is not needed.

## Requirements

- Echo VR for PC, the build whose `echovr.exe` has MD5 `caa05ee3bca2f5df0809994e93e2cc05`.
- [EchoLoader](https://github.com/EchoTools/EchoLoader), the plugin loader.

## Install

1. Install EchoLoader.
2. Copy `EchoReplay.dll` into `<game folder>\bin\win10\plugins\`. Create `plugins` if it is missing.
   Install only this one DLL. `echoreplay_core.dll` in a source build is a test binary and is never installed.
   Upgrading from an earlier version: delete the old `probe_replay.dll` from `plugins`, or let `tools\watch_replay` move it out.
3. Start the game from inside `bin\win10`. The loader looks for `plugins` in the current folder, so a
   shortcut that starts the game from somewhere else loads nothing.

If it loaded, `bin\win10\echoreplay.log` appears within a second of launch. To uninstall, delete
`EchoReplay.dll` (and optionally `echoreplay.log` and the `echoreplay\` settings folder next to it).
The game's files on disk are never changed.

## Record

Join a live match as a player or spectator. Recording starts on its own, and each match becomes one
file in `Documents\EchoReplay\`. Offline sessions and replays are never recorded.

- F9 shows the player window. Its Record button starts or stops a recording by hand.
- To record only when you ask, set `capture.auto_record` to `false` (see [CONFIG](docs/CONFIG.md)).

### Clips

`echodemo clip <in> <out> --last 180` (or `--from 60 --to 240`, seconds) cuts a window out of a recording,
including one that is still being written. Names, numbers and cosmetics from before the window are carried
into the clip. Other plugins can ask for one in game: `EchoReplay_Clip(seconds)` / `EchoReplay_ClipStatus(job,
out, cap)` (exported by `EchoReplay.dll`) cut the last N seconds across the current and earlier match files
into `Documents\EchoReplay\clips\<time>.json`, which lists the parts oldest first. EDITH-HUD's
"Edith, clip the last 3 minutes" uses this.

## Watch

Close the game, then run (`-DryRun` shows what would run):

```
tools\watch_replay.bat                 pick from a list of recent recordings
tools\watch_replay.bat -Latest         open the newest recording
tools\watch_replay.bat -Path <file>    open a .echodemo, .echoreplay or .tape file
```

The script finds the game on its own: `-GameDir <folder>` if given, then the `REPLAY_ECHOVR_EXE`
(path to `echovr.exe`) or `ECHOVR_DIR` (game folder) environment variables, then the Oculus/Meta
library folders. It lists recordings from `Documents\EchoReplay` and old replays from
`Documents\Spark\replays`, then starts the game in a window with the replay loaded.

The game needs no extra switches. In a replay session the plugin applies the offline mode itself, in
memory. If EchoRelay is installed, that is fine: the plugin sees its patch and leaves it alone.

### Controls

| Key | Action |
|---|---|
| F9 | Show or hide the player window |
| F10 | Switch the mouse between the player window and the game camera |
| Space | Play / pause (cursor on the player) |
| Left / Right | Back / forward 1 s (Shift: 10 s) |
| Home / End | Jump to start / end |
| `,` / `.` | Step one recorded frame back / forward |

The `=` menu in the player window has speed, motion smoothing (Off / Slow-mo only / Always, strength
Low / Med / High), **Open replay...**, Unload, player width and timeline markers.

A console window opens with the game. Type `help` for the list, for example `seek 1:23`,
`speed 0.25`, `round 3`, `cam follow 2`, `mark`, `record stop`.

## Watch without the game (EchoReplayViewer)

EchoReplayViewer is a separate desktop app that plays recordings without starting Echo VR. It reads
`.echodemo` files and EDITH clips (`clips\*.json`) through the reader in `tools\viewerapi`.

- A library of your recordings, split into Arena, Combat, Social Lobby, EDITH clips and older formats.
  Each card shows a picture of its map, shared by every replay on that map.
- The real map, loaded from your Echo VR install.
  Opening a replay loads in the background, so the window stays responsive (combat maps and the lobby take
  up to half a minute).
- Players drawn with the game's own chassis and cosmetics. The body follows the head's pitch and roll.
- The arena's screens filled from the recording: score, clock, and the goal board with the goal type,
  scorer, assist, disc speed, distance and time, readable from both sides.
- Hover a player for their team, speed, goals and stuns. Hover the disc for its speed, the last goal's
  speed and who touched it last.
- A Players panel with Blue, Orange and Spectators, taken from the match roster, so a player who left
  and rejoined is listed once.
- Camera modes: free, follow, POV, disc and overhead. Recorded voice plays per player.

What the recording does not carry is left out, not made up: fingers only close with the trigger (no
grip or finger data), and the recorder's own team is worked out from who they played near.

Build it from its own folder with `build.bat` (Visual Studio, CMake and Ninja). `build.bat deps` also
rebuilds the libraries it links.

## Old replays (.echoreplay, .tape)

They are converted to `.echodemo` the first time you open them. The copy is kept in
`Documents\EchoReplay\imported\` and reused.

| Carries over | Does not |
|---|---|
| Player head, hand and body positions | Voice |
| Disc, score, clock, goals, stuns | Skeleton poses and cosmetics |
| Stats, names, teams | Boost, brake, thruster and punch sounds |
| Grabs and held players (inferred) | Any map except `mpl_arena_a` |

These files were recorded at 10 to 30 Hz, so motion is less detailed than a native recording. Very
long files are refused in game; convert them with `echodemo convert`.

## Command-line tool

`echodemo.exe` inspects, checks, converts and cleans recordings. Full list in
[BUILD.md](docs/BUILD.md#the-echodemo-tool).

```
echodemo info    <file>                  duration, players, level
echodemo convert <in.echoreplay|in.tape> <out.echodemo>
echodemo strip   <in> <out> --voip       remove voice before sharing
```

## Known limitations

- Only the one PC build above is supported. On another build the plugin turns off what it cannot verify.
- Arena is the tested mode. Combat maps are recognised but barely tested.
- Sound: slow motion is pitched down, reverse is silent, above 1x plays at normal pitch.
- Voice plays only at 1x forward.
- A short hitch (about 10 ms) happens every couple of seconds while the next part of the file is
  decoded. `playback.async_window_decode` moves it off the frame but is experimental.
- The offline mode writes to the game's code in memory. Antivirus tools have flagged this kind of
  write before.

## Credits

- **EchoReplay** was originally made by [C-Luddy](https://github.com/C-Luddy).
- **[EchoTools Demo Viewer](https://github.com/EchoTools/demo-viewer)** was originally made by David Robidas
  ([@robidasdavid](https://github.com/robidasdavid)) and Zzenith (2020). The EchoTools fork is maintained by
  Andrew Bates and babs-VR. EchoReplayViewer's player rig and its head-and-hands body solve follow the demo
  viewer's player prefab and IK controller.
- **Echo VR** and its maps, models and art are by Ready At Dawn. The map pictures in the viewer are from the
  game and belong to their owners.
- Older replay formats: Spark (`.echoreplay`) and EchoTools tapedeck (`.tape`).

## More

- [How it works](docs/HOW-IT-WORKS.md)
- [Configuration](docs/CONFIG.md)
- [Building and testing](docs/BUILD.md)
- [The .echodemo format](docs/FORMAT.md)
- [Reverse-engineering notes](docs/re/README.md)

MIT licence, see [LICENSE](LICENSE). Bundled libraries and test data: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
