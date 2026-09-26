// Every configuration key this build understands. Included once, by config.cpp, with the four
// REPLAY_CONFIG_* macros defined; also parsed by tests/ledgerkeys, which cross-checks the `row` and
// `stream` columns against the design and fails if the design declares one this file does
// not claim. The design is the architect's document and no packet can edit it, which is what makes it
// usable as the authority for "did you cover everything".
//
//   REPLAY_CONFIG_<TYPE>(key, default, ledgerRow, stream, flags, doc)
//
// Defaults come from the design, not from taste: S4.4 says rows 0.20 and 2.3 ship enabled and 2.1 does
// not; S6.6.1 sets the capture defaults; S5.4.7 makes ORACLE default-on under ruling Q-B.

// ---------------------------------------------------------------------------------------------------
// Level 0 - stock calls and writes to the engine's own data (S4.4).
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_BOOL("tick.slot_claim", true, "0.1", nullptr, kConfigNone,
                   "Per-frame tick by storing our pinned HMODULE in the free CNSProvider slot")
REPLAY_CONFIG_BOOL("control.replayviewer_arg", true, "0.2", nullptr, kConfigNone,
                   "Read -replayviewer <file> off the command line")
REPLAY_CONFIG_BOOL("session.enter_offline_match", true, "0.3", nullptr, kConfigNone,
                   "Enter an offline match at the recorded level and gametype")

// ENG-GAMETYPE-ENABLE (F2).  Captures written before the recorder kept the whole gametype qword
// carry its low 32 bits widened to 64, and a spectator capture carries the all-ones the engine field
// held at open.  Written verbatim into CR15Game before EnterOfflineMatch, either makes the inner
// replayed session start with a gametype CR15NetGameTypeNode does not recognize ("Match is not
// social, combat, or arena").  ON (the default): a recorded value that is not a whole gametype is
// resolved (foundation/gametype.h) -- low-dword match against the known gametypes, else the
// capture's own SNSLobbyOwnerState event, else session.gametype_override_name.  A whole recorded
// gametype is never touched, and nothing here runs unless a capture is being opened.  OFF: the
// recorded value is written exactly as recorded.
REPLAY_CONFIG_BOOL("session.gametype_override", true, nullptr, nullptr, kConfigEnvOverride,
                   "ENG-GAMETYPE-ENABLE: when a capture's recorded gametype is not a whole 64-bit "
                   "gametype, resolve it (known low dword, else the capture's owner-state event, "
                   "else session.gametype_override_name) before entering. Default ON; OFF writes "
                   "the recorded value verbatim")
REPLAY_CONFIG_STRING("session.gametype_override_name", "echo_arena", nullptr, nullptr, kConfigNone,
                   "ENG-GAMETYPE-ENABLE: the gametype NAME used when nothing in the capture "
                   "resolves its gametype (e.g. echo_arena, echo_arena_practice_ai). Only used "
                   "when session.gametype_override is ON")

REPLAY_CONFIG_BOOL("transport.clock_writes", true, "0.4", nullptr, kConfigNone,
                   "Drive the transport by writing the engine CClock's scale, flags, floor and step")
REPLAY_CONFIG_BOOL("transport.playhead_writes", true, "0.5", nullptr, kConfigNone,
                   "Move the playhead, including backwards, through the net-game registers")
REPLAY_CONFIG_BOOL("playback.entity_history_writes", true, "0.6", nullptr, kConfigNone,
                   "Drive entity transforms by filling the pools' history lists")
REPLAY_CONFIG_BOOL("playback.authority_owner_write", true, "0.7", nullptr, kConfigNone,
                   "Stop the local sim overwriting the recording by rewriting the PhysCI owner id")
REPLAY_CONFIG_BOOL("playback.pose_authoring", true, "0.8", nullptr, kConfigNone,
                   "Author an exact pose at an arbitrary time under authority")
REPLAY_CONFIG_BOOL("camera.write", true, "0.9", nullptr, kConfigNone,
                   "Camera and FOV, through the spectator API or the view state")
REPLAY_CONFIG_BOOL("identity.injection", true, "0.10", nullptr, kConfigNone,
                   "Names, profiles, loadouts, skins and team colours")
REPLAY_CONFIG_BOOL("audio.engine_calls", true, "0.11", nullptr, kConfigNone,
                   "Audio transport, seek and determinism through the game's own exported AK symbols")
REPLAY_CONFIG_BOOL("audio.rate_control", true, "0.12", nullptr, kConfigNone,
                   "Pause, slow motion and playback speed at or below 1.0x via SetFrequencyRatio")
REPLAY_CONFIG_BOOL("transport.watchdog_bracket", true, "0.13", nullptr, kConfigNone,
                   "Survive long seek and fast-forward bursts by bracketing the deadlock monitor")
// PERF-3's control. ON, nothing in the plugin is armed on an engine path: no netgame stage sweep,
// no host-netvar guard, no score gate, no in-frame UI draw, no capture taps and no engine clock
// writes. The playback tick still runs, so the pacing and `cost:` lines still measure the frame --
// which is the only frame-rate meter this machine has, and the reason MINIMAL is not simply "do not
// load the plugin". It exists to answer one question by measurement: how much of a frame is engine
// work the plugin PROVOKES rather than performs. Never a shipping configuration.
REPLAY_CONFIG_BOOL("bootstrap.minimal", false, nullptr, nullptr, kConfigEnvOverride,
                   "Diagnostic: arm nothing on an engine path, keep only the tick and its meters")
// Also PERF-3's, and also never a shipping configuration: it suspends the engine's frame thread a
// few hundred times a second to read where it is. It is the only instrument on this machine that
// can attribute the part of the frame the plugin does not run.
REPLAY_CONFIG_BOOL("bootstrap.profiler", false, nullptr, nullptr, kConfigEnvOverride,
                   "Diagnostic: sample the engine frame thread's instruction pointer")
REPLAY_CONFIG_BOOL("playback.lod_pin", true, "0.14", nullptr, kConfigEnvOverride,
                   "Bit-identical LOD by pinning the remote-player throttle state")
REPLAY_CONFIG_BOOL("audio.state_reconcile", true, "0.15", nullptr, kConfigNone,
                   "Correct audio state after a backwards seek by reconciling the sound player's memo")
REPLAY_CONFIG_BOOL("capture.event_tap", true, "0.15b", nullptr, kConfigNone,
                   "Event ingestion through BroadcasterListen; unverified until W1-L reports")
REPLAY_CONFIG_BOOL("control.http_routes", true, "0.16", nullptr, kConfigNone,
                   "Control API on the game's own localhost HTTP route registration")
// Default FALSE and it is the whole point of the key: the routes ride the
// GAME'S listener, `POST /replay/command` is unauthenticated, and in a live match `record stop`
// ends the owner's recording of it while `list` enumerates his replay folder. Contained offline
// playback registers them without this; a live session does not register them at all.
REPLAY_CONFIG_BOOL("control.http_routes_live", false, nullptr, nullptr, kConfigEnvOverride,
                   "Also register the plugin's HTTP control routes when the session is NOT "
                   "contained offline playback. Off by default: the in-game console and the "
                   "recorder surface are how recording is controlled in a live match. "
                   "ECHOREPLAY_CONTROL_HTTP_ROUTES_LIVE=1 turns it on")
REPLAY_CONFIG_BOOL("transport.raise_frame_clamp", true, "0.17", nullptr, kConfigNone,
                   "Raise the long-frame clamp for the duration of a burst")
// The pre-roll. The offline host will not resolve its own frisbee ref until its pre_match script
// has counted 45 s down on the engine clock this module masters, so a load holds the playhead where
// the owner left it and runs that clock forward until the disc binds. Row 0.4's writes, nothing new.
// All three carry kConfigEnvOverride: when the pre-roll is the suspect, a launch has to be able to
// turn it off, shorten its bound or drop it to 1x without an edit-and-rebuild between attempts.
REPLAY_CONFIG_BOOL("playback.preroll", true, nullptr, nullptr, kConfigEnvOverride,
                   "Run the host's pre-match and launch out on the engine clock at load, with the "
                   "playhead held, so the recorded disc is driven from the first second watched. "
                   "ECHOREPLAY_PLAYBACK_PREROLL=0 turns it off")
// PREROLL-FAST-1. Two scales, because the two halves of the wait are not the same thing. The 45 s
// pre-match is an empty countdown -- one script node subtracting graph->deltatime from a float --
// and runs at preroll_clock_scale; the 20 s behind it teleports and launches eight bodies, and runs
// at preroll_rate, the only rate a launch has been watched at. The engine clamps each heartbeat to
// 66666 us and clock_policy.h caps the commanded rate at maxElapsed/framePeriod, so asking for more
// than the frame rate allows is discarded in the cap rather than being dangerous.
REPLAY_CONFIG_DOUBLE("playback.preroll_clock_scale", 4.0, nullptr, nullptr, kConfigEnvOverride,
                     "Engine-clock scale while the host counts its 45 s pre-match down. Clamped to "
                     "1-8 and capped again at the engine's own ceiling (66666 us / frame period: 4x "
                     "at 60 fps, more above it). 4 is ~16 ms of sim per frame at this host's "
                     "windowed frame rate -- the step an ordinary 60 Hz session takes every frame. "
                     "ECHOREPLAY_PLAYBACK_PREROLL_CLOCK_SCALE=6 tries it faster")
REPLAY_CONFIG_DOUBLE("playback.preroll_rate", 2.0, nullptr, nullptr, kConfigEnvOverride,
                     "Engine-clock rate for the last stretch of the pre-roll: the host's launch "
                     "sequence, from its round_start (which is where the disc binds) to the moment "
                     "it is playing. Left at the proven 2x while the pre-match runs faster, because "
                     "this is the stretch that teleports bodies into tubes and launches them")
REPLAY_CONFIG_INT("playback.preroll_bound_ms", 55000, nullptr, nullptr, kConfigEnvOverride,
                  "Hard wall-time bound on the pre-roll, from the load. 65 s of host time (45 s "
                  "pre-match at preroll_clock_scale + 20 s launch at preroll_rate) is 21.3 s at the "
                  "4x/2x defaults, plus up to 8 s of admission wait before the fast clock starts. "
                  "The bound is a give-up, not a schedule, so it keeps its slack. At the bound "
                  "playback starts anyway and the disc settles when the host launches, as before")
// RELOAD-6. A mid-session Open used to admit the new roster into the level the previous replay
// played in, whose space still holds that roster's attached nametag/heraldry children and script
// graphs (the engine destroys those only when a space is torn down). ON: after the paced leave, the
// level is left and entered again through SESSION.CREATE_OFFLINE_MATCH -- the stock call the first
// load uses -- so the new replay starts in a fresh space, with its pre-roll, like a first load.
REPLAY_CONFIG_BOOL("playback.reload_via_level_cycle", true, nullptr, nullptr, kConfigEnvOverride,
                   "Open a replay while another is playing by re-entering the level (a new match on "
                   "the same host) after the previous roster has left, so nothing of the previous "
                   "replay survives into the new one. Any failure reopens in place, as before. "
                   "ECHOREPLAY_PLAYBACK_RELOAD_VIA_LEVEL_CYCLE=0 turns it off")
// The stat feed. kConfigEnvOverride for the same reason the pre-roll keys carry it: it is the one
// switch that separates "the HUD's bottom row is wrong" from "the stat feed is what broke this
// load", and a launch has to be able to answer that without a rebuild.
REPLAY_CONFIG_BOOL("playback.stats", true, nullptr, nullptr, kConfigEnvOverride,
                   "Feed the recorded per-player and per-team stat rows back through the engine's "
                   "own two appliers, so the spectator HUD's bottom row and the scoreboard show "
                   "what the recording says. ECHOREPLAY_PLAYBACK_STATS=0 turns it off")
// The stun feed. Default ON: the owner's own capture holds eight SR15StunPlayerMsg rows that
// nothing on the playback side ever decoded, which is why he hears blocks and not stuns.
// kConfigEnvOverride for the same reason the goal feed has it -- the first time this runs is on his
// file, and it has to be switchable from a launch without a rebuild.
REPLAY_CONFIG_BOOL("playback.stun_events", true, nullptr, nullptr, kConfigEnvOverride,
                   "Feed each recorded stun back through the engine's own two punchable delegates "
                   "and the three fields its receive handler writes, so a replayed stun sounds, "
                   "looks and times out exactly as a live one does. The stat counters are replayed "
                   "separately and this does not touch them. "
                   "ECHOREPLAY_PLAYBACK_STUN_EVENTS=0 turns it off")
// The action-word delegate feed -- the grab, boost, brake and thruster sounds of a replayed player.
// Default ON: it is the engine's own EmitChangedDelegates and nothing else raises those delegates on
// a playback path. kConfigEnvOverride because the only honest measurement of whether it makes sound
// is the same content played twice, and the second run has to withhold it without a rebuild.
REPLAY_CONFIG_BOOL("playback.delegate_events", true, nullptr, nullptr, kConfigEnvOverride,
                   "Diff each replayed player's action word against the frame before and let the "
                   "engine's own EmitChangedDelegates raise the grab, boost, brake, thruster, punch "
                   "and block delegates the sound scripts listen on. "
                   "ECHOREPLAY_PLAYBACK_DELEGATE_EVENTS=0 turns it off")
// FAR-SOUND-2's instrument, and nothing but an instrument: it reads the four Wwise Query entry
// points the `census` verb already binds and writes log lines. Default 0 (off) so a normal session
// pays nothing; kConfigEnvOverride because a measurement launch has to turn it on without a
// rebuild and without editing a file the owner also edits.
REPLAY_CONFIG_INT("audio.voicewatch_ms", 0, nullptr, nullptr, kConfigEnvOverride,
                  "Sample Wwise's playing-voice set every N ms and log each voice's start, its end "
                  "and its age. 0 is off. ECHOREPLAY_AUDIO_VOICEWATCH_MS=1000 turns it on")
// AUDIO-FIX-2 item 3. Default ON, unlike voicewatch above, because this is the line that carries
// the number the whole cutoff question needs and it has to be in the log of a LIVE spectate that
// nobody thought to arm beforehand. Strictly AK::Query reads plus log lines: it posts nothing,
// stops nothing, registers nothing and opens no socket, which is why it runs above the load gate
// and takes no live-safety feature.
REPLAY_CONFIG_STRING("audio.foley_mix", "headset", nullptr, nullptr, kConfigNone,
                     "headset | broadcast | engine. Which foley mix a replay plays with: headset is "
                     "what a live spectator in the headset hears (far foley fades out), broadcast "
                     "is the spectator-stream mix (far foley stays audible), engine leaves the "
                     "state to the game. Any other word is refused and runs engine")
REPLAY_CONFIG_INT("audio.foley_census_ms", 30000, nullptr, nullptr, kConfigEnvOverride,
                  "Every N ms, log one line: how many voices are playing, on how many emitters, "
                  "and the nearest and farthest PLAYING emitter's distance to the listener. In a "
                  "live match that farthest distance is the foley cutoff radius. 0 is off")
// ENG-LIVE-CENSUS-2 item 1, and ON by default for exactly the reason the foley line above is: the
// session it exists to measure is a LIVE spectate that nobody armed beforehand. Strictly probed
// memory reads plus log lines -- no engine call, no write, no live-safety feature -- and it runs on
// a trigger, so this interval is the CEILING on how often it walks, not a per-frame cost.
REPLAY_CONFIG_INT("disc.diff_census_ms", 10000, nullptr, nullptr, kConfigEnvOverride,
                  "Snapshot every disc actor's presentation state -- holder, the engine's held "
                  "bits, its script graphs' data words -- and log the DIFF against the previous "
                  "snapshot. Taken on first sight, on every holder change, on a disc respawn, and "
                  "otherwise at most once per N ms. 0 is off")
REPLAY_CONFIG_INT("audio.foley_deep_at_s", 60, nullptr, nullptr, kConfigEnvOverride,
                  "How many seconds into a match the one-per-match deep read fires: every global "
                  "Wwise state and every per-emitter switch, as Wwise itself answers them. Also "
                  "taken at match end")
// The goal feed. Default ON, and kConfigEnvOverride because the goal path is the one part of the
// replay that has never been seen run: the only capture in hand carries zero goal rows, so the
// owner's first goal playback IS its first test and it has to be switchable from a launch.
REPLAY_CONFIG_BOOL("playback.goal_events", true, nullptr, nullptr, kConfigEnvOverride,
                   "Feed each recorded goal back through the engine's own two "
                   "SR15NetReliablePostScoreUpdate receive handlers, so the goal banner, the "
                   "celebration graphs and the announcer run as the engine runs them. The "
                   "scoreboard numbers are netvars and do not come from this. "
                   "ECHOREPLAY_PLAYBACK_GOAL_EVENTS=0 turns it off")
// The component-event feed -- the launch tubes' light rigs. Default ON, and kConfigEnvOverride for
// the same reason as the goal feed: the allow-list admits four rows in the only capture in hand, so
// the owner's next round start IS its first test and it has to be switchable from a launch without
// a rebuild.
REPLAY_CONFIG_BOOL("playback.component_events", true, nullptr, nullptr, kConfigEnvOverride,
                   "Feed each recorded SR15NetComponentEvent on the (script, event) allow-list back "
                   "through the engine's own CR15NetGame::ComponentEventCB, so the launch tubes' "
                   "light rigs run their launch and return sequences as the engine runs them. "
                   "Everything off the list is counted and logged, never dispatched. "
                   "ECHOREPLAY_PLAYBACK_COMPONENT_EVENTS=0 turns it off")
// Default OFF again (ENG-GOAL-SFX): the engine gamelog of the owner's 2026-09-25 session prints
// "[DEBUGPRINT] [GOALSCORE] Printing fanfare sound..." and TWO "Playing equipped Fanfare item" lines
// per replayed goal, where a live client prints one. The script fanfare path does run on this
// process once the goal FX raises reach it, so the direct call is a second horn.
REPLAY_CONFIG_BOOL("playback.goal_horn", false, nullptr, nullptr, kConfigEnvOverride,
                   "Also call the engine's PlayGoalSFX for the scorer on a replayed goal -- the "
                   "equipped goal_fx fanfare. The script path already plays it on a replayed goal. "
                   "ECHOREPLAY_PLAYBACK_GOAL_HORN=0 turns it off")
// ENG-PRESENT-B2. The goal rig animates off its OWN event, evt_glb_scored_matanimation, which the
// disc script raises on a real goal -- not off the score netvars and not off the banner, which is
// why a replayed goal had every other element and no explosion. Scanned by symbol hash across all
// 567 local script DBs, the event has one listener, and that script links only FX/sound/material/
// delay/enable-component nodes: no send-event, no netvar set, no teleport, no physics, no reset.
REPLAY_CONFIG_BOOL("playback.goal_explosion", true, nullptr, nullptr, kConfigEnvOverride,
                   "Raise evt_glb_scored_matanimation on a replayed goal, through the same deferred "
                   "named-event entry the engine's own goal receive handler tail-jumps to, so the "
                   "goal rig plays its explosion. ECHOREPLAY_PLAYBACK_GOAL_EXPLOSION=0 turns it off")
// GOAL-W1. The score panel flash, the score accrual SFX and the goal_fx explosion are all the
// engine's own reaction to a blue_score/red_score CHANGE, and every netvar setter calls the change
// notifier only when SetValue reports the stored value actually moved. Nothing had ever measured
// whether a replayed score write reaches it, so "no goal effects" could not be told apart from "no
// trigger". This writes a log line per score change and touches nothing.
REPLAY_CONFIG_BOOL("playback.goal_score_probe", true, nullptr, nullptr, kConfigEnvOverride,
                   "Log every blue_score/red_score/blue_round_score/red_round_score write with the "
                   "value before, the value after and whether the engine's change notifier ran -- "
                   "the trigger the goal flash and the goal_fx explosion hang off. Reporting only; "
                   "ECHOREPLAY_PLAYBACK_GOAL_SCORE_PROBE=0 turns it off")
// HOST-HOLD. The host's arena script runs its OWN round clock under the replay -- a script-local
// float counting `graph->deltatime` down from net_match_settings_round_time_in_seconds -- and at
// zero it leaves main_loop, parks the frisbee at the joust spawn for ~26 s, and on a match end
// returns to the lobby and ends the viewing outright. Its round is 5 minutes; the owner's captures
// are 10+. So the plugin owns the netint the clock is SEEDED from and holds it at an hour, which
// the script re-reads at each round_start. Nothing in the arena reads that key: it is the private
// match settings menu's, and only the gametype script's own seed node otherwise.
REPLAY_CONFIG_BOOL("playback.host_hold", false, nullptr, nullptr, kConfigEnvOverride,
                   "Keep the offline host parked in main_loop for the whole viewing by owning the "
                   "netint its round clock is seeded from and holding it at an hour. "
                   "ECHOREPLAY_PLAYBACK_HOST_HOLD=0 turns it off")
// DESIGN-HOST-SYNC 6: host_hold is off by default -- redundant under host_clock_pin, and it writes a
// key the settings menu reads.
// DESIGN-HOST-SYNC P1. The arena graph's first-round clock is seeded in pre-match, never from the
// netint host_hold owns, so the host still expired at ~720 s. The pin writes the graph's own
// game_timer through the graph's own setter every sweep while loaded, and leaves it at unload.
REPLAY_CONFIG_BOOL("playback.host_clock_pin", true, nullptr, nullptr, kConfigEnvOverride,
                   "Pin the host arena graph's own game_timer at 3600 through its own setter while "
                   "a replay is loaded, so the host never ends a round or goes to overtime under it. "
                   "ECHOREPLAY_PLAYBACK_HOST_CLOCK_PIN=0 turns it off")
// DESIGN-HOST-SYNC P4. The disc's floating shell follows the recording: forceSetFXState_Red/Blue at
// each recorded round restart, cleared by the engine's own grab arm.
REPLAY_CONFIG_BOOL("playback.disc_shell", true, nullptr, nullptr, kConfigEnvOverride,
                   "Drive the disc's untouched-shell FX from the recording's round restarts. "
                   "ECHOREPLAY_PLAYBACK_DISC_SHELL=0 turns it off")
// The stronger lever, and the reason it is OFF. Holding net_game_paused_state at `paused` freezes
// EVERY countdown in the script, sudden death included -- but the DB says the UI script that owns
// the strings "PAUSED" / "PAUSE REQUESTED" / "UNPAUSING IN " listens for a change of exactly this
// key, and a music script branches on it too. That is open item O-G, and only the owner's eyes can
// close it. Turn it on for one viewing and look at the screen: if nothing is drawn, this becomes
// the default, because it covers the sudden-death rounds host_hold cannot.
REPLAY_CONFIG_BOOL("playback.host_hold_pause", false, nullptr, nullptr, kConfigEnvOverride,
                   "Also hold net_game_paused_state at `paused`, which stops every host countdown "
                   "and not just the round clock. May draw a PAUSED overlay (open item O-G). "
                   "ECHOREPLAY_PLAYBACK_HOST_HOLD_PAUSE=1 turns it on")
// Carries kConfigEnvOverride for the same reason the pre-roll keys do: it changes how every
// replayed body looks and sounds, so a launch has to be able to A/B it without a rebuild.
REPLAY_CONFIG_BOOL("playback.grip_inputs", true, nullptr, nullptr, kConfigEnvOverride,
                   "Publish the recorded per-hand grab input so the engine runs its own grab state "
                   "machine for replayed players -- the grab sound and the hand attach, for "
                   "geometry and for players. ECHOREPLAY_PLAYBACK_GRIP_INPUTS=0 turns it off")
REPLAY_CONFIG_BOOL("playback.hermite", true, nullptr, nullptr, kConfigEnvOverride,
                   "Interpolate position between the 45 Hz samples with a cubic Hermite through "
                   "the recorded velocities instead of a straight line. "
                   "ECHOREPLAY_PLAYBACK_HERMITE=0 goes back to lerp")
REPLAY_CONFIG_STRING("playback.motion_smoothing", "slowmo", nullptr, nullptr, kConfigNone,
                     "Motion smoothing for slow motion: off | slowmo | on. slowmo smooths only while "
                     "abs(rate) < 1, so 1x, -1x and fast rates stay exact; off is the unmodified 1:1 "
                     "path at every rate; on smooths at every rate. Any other word runs off. The = "
                     "menu changes it for the session")
REPLAY_CONFIG_INT("playback.motion_smoothing_strength", 2, nullptr, nullptr, kConfigNone,
                  "1 low (18 ms), 2 medium (30 ms), 3 high (50 ms): the Gaussian width of the centred "
                  "fit smoothing draws through. Outside 1..3 is clamped. The = menu changes it for the "
                  "session")
REPLAY_CONFIG_BOOL("playback.motion_smoothing_trace", false, nullptr, nullptr, kConfigEnvOverride,
                   "One [smooth-trace] line per frame (at most 20000 per load) with the lowest bound "
                   "player's driven pose, for tools/smoothtrace.ps1. "
                   "ECHOREPLAY_PLAYBACK_MOTION_SMOOTHING_TRACE=1 turns it on")
REPLAY_CONFIG_BOOL("playback.async_window_decode", false, nullptr, nullptr, kConfigEnvOverride,
                   "Decode the next sample window on a worker thread instead of on the tick the "
                   "playhead crosses a chapter boundary -- the 9-13 ms single-frame hitch at every "
                   "window roll. OFF until an operator has driven it in game: with it off the "
                   "synchronous path runs exactly as before and the worker is never started. "
                   "ECHOREPLAY_PLAYBACK_ASYNC_WINDOW_DECODE=1 turns it on")
REPLAY_CONFIG_BOOL("audio.wav_capture", false, "0.18", nullptr, kConfigNone,
                   "WAV export of the exact mix; off until the user asks for a capture")
REPLAY_CONFIG_BOOL("ui.panel", true, "0.20", nullptr, kConfigPinnedTrue,
                   "The control panel, scrub bar and replay browser, through the stock registration route "
                   "W1-M found. Ruling Q-F and SC11: not refusable")
REPLAY_CONFIG_BOOL("audio.seek_mute", true, "0.19", nullptr, kConfigNone,
                   "Hard mute for a hidden seek burst, via the shipped dummy-sink shareset")

// ---------------------------------------------------------------------------------------------------
// UI-2, DESIGN-UI-2 S7.1 - the overlay bar and what it replaces. `ui.panel` above gates the UI
// SUBSYSTEM and stays pinned true; these two sit under it and choose which surface it draws. The
// old panel's default is `false` here and its consumer switch lands with the bar (P2): until then
// nothing reads ui.debug_panel and the panel is the visible UI, so a half-landed pair cannot leave
// the session with no surface at all.
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_BOOL("ui.bar", true, nullptr, nullptr, kConfigNone,
                   "The overlay transport bar: timeline, play/pause, rate, and the `=` menu")
REPLAY_CONFIG_BOOL("ui.debug_panel", false, nullptr, nullptr, kConfigNone,
                   "The old \"Echo Replay\" panel, unchanged, as its own window. Everything it "
                   "showed also reaches the console's `diag` and the log")
REPLAY_CONFIG_STRING("ui.bar_anchor", "bottom", nullptr, nullptr, kConfigNone,
                     "bottom | top. Which edge the bar sits against")
REPLAY_CONFIG_INT("ui.bar_margin_px", 96, nullptr, nullptr, kConfigNone,
                  "Gap between the bar and its anchored edge, in client pixels. The default "
                  "clears the game's own stat strip")
REPLAY_CONFIG_INT("ui.player_width", 0, nullptr, nullptr, kConfigNone,
                  "Replay player width in pixels. 0 sizes the window to its content, which is the "
                  "default; the = menu's width control writes this value. Replaces ui.bar_width_px, "
                  "whose declared 760 outlived the layout it was measured for")
REPLAY_CONFIG_DOUBLE("ui.player_autohide_s", 0.0, nullptr, nullptr, kConfigNone,
                     "Seconds of no input before the player hides itself. 0 -- the default -- never "
                     "hides: F9 is the switch. Opt in by setting it. Replaces ui.bar_autohide_s, "
                     "whose declared 3.0 hid the window under the owner four times in one test")
REPLAY_CONFIG_BOOL("ui.bar_keys", true, nullptr, nullptr, kConfigNone,
                   "The cursor-mode keyboard shortcuts for the bar")
REPLAY_CONFIG_BOOL("ui.bar_keys_global", false, nullptr, nullptr, kConfigNone,
                   "The always-on shortcut set. Off until a user has verified the bindings do not "
                   "collide with the game's own")
REPLAY_CONFIG_STRING("ui.bar_markers", "rounds,goals,bookmarks", nullptr, nullptr, kConfigNone,
                     "Comma-separated tick families drawn on the timeline: rounds, goals, "
                     "bookmarks")
REPLAY_CONFIG_BOOL("ui.bar_drag", true, nullptr, nullptr, kConfigNone,
                   "Win32-sampled mouse dragging on the timeline. false leaves the bar "
                   "click-to-seek only, which is what it does before calibration anyway")
// ★ The kill switch for S5.4. On, a drag step skips the bookkeeping a drag cannot see -- HUD
// netvars, audio pins, the player re-bind, TrimHistories and the build log lines -- and keeps the
// residue battery, which is what makes the bodies snap. Off, every step is today's full seek.
REPLAY_CONFIG_BOOL("playback.scrub_light", true, nullptr, nullptr, kConfigNone,
                   "Light per-step seeks while scrubbing. false makes every drag step a full seek, "
                   "exactly as before this key existed")

// ---------------------------------------------------------------------------------------------------
// Level 1 - the fallback tick. S4.4: "Config key tick.mode accepts auto (default, prefers the slot),
// slot, hook". The key is named by the design, so it is spelled the way the design spells it.
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_STRING("tick.mode", "auto", "1", nullptr, kConfigTextDetour,
                     "auto | slot | hook. auto prefers the zero-hook slot claim and falls back")

// ---------------------------------------------------------------------------------------------------
// Level 1X - interception of Windows APIs and COM vtables in our own process, never of game code.
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_BOOL("session.cmdline_hook", true, "1X.1", nullptr, kConfigNone,
                   "Hook GetCommandLineA/W so the viewer needs no special command line")
// ENG-OFFLINE-PATCH (owner request): the in-process -offline -windowed for REPLAY sessions only, so a
// viewer without rlay.dll can host the recorded level offline (src/bootstrap/offlinepatch.h). It writes
// four code sites and one mode bit, and only when ECHOREPLAY_OPEN is set; a live session installs
// nothing. Where rlay's -offline already wrote the sites it writes nothing. false turns it off entirely.
REPLAY_CONFIG_BOOL("session.offline_patch", true, nullptr, nullptr, kConfigNone,
                   "In a replay session, apply the -offline -windowed patch in memory so no rlay.dll is "
                   "needed. Skipped where rlay already applied it; never in a live session")
REPLAY_CONFIG_BOOL("audio.max_rate_hook", true, "1X.2", nullptr, kConfigNone,
                   "Hook IXAudio2::CreateSourceVoice to raise MaxFrequencyRatio above 1.0")
// DESIGN S4.4 row 2.6 (ARCH-22): an entry detour on CAkSinkXAudio2::Term (AUDIO.SINK_TERM,
// 0x141401E40) inside echovr.exe's .text -- so kConfigTextDetour, and SC7 reads the site's
// ledgerRow off THIS declaration via hookmgr.cpp:191. There is no second place the address lives.
//
// ★ IT SHIPPED false FOR ONE COMMIT; W2-H-LIFETIME-3 FLIPPED IT WITH THE SITE.
// sc7.cpp:1013-1034 puts every kConfigTextDetour key whose RowDeclarationState is Declared into
// SC7's expected set; a row declared with no site to match it is exactly the 2.5b defect
// (expected_rows 4 against declared_sites 3). false resolves to NotDeclared, so the window between
// the design row and its site left no launch-time RED. The site is registered now, in
// src/audio/voicelifetime.cpp's ArmVoiceLifetime, so the key now carries the DESIGN row's own
// shipping default: a crash fix that sits default-OFF is not a fix.
REPLAY_CONFIG_BOOL("audio.sink_term_hook", true, "2.6", nullptr, kConfigTextDetour,
                   "Detour CAkSinkXAudio2::Term so a voice teardown is published before "
                   "DestroyVoice runs. The stock device-status callback misses sub_14139A640 "
                   "(Suspend, stall watchdog), which is the L3 #9 crash window")
REPLAY_CONFIG_STRING("net.loopback_only", "auto", "1X.4", nullptr, kConfigNone,
                     "auto | on | off. Rewrite every wildcard bind in this process to loopback, so an "
                     "offline session has no endpoint a remote peer can reach. ARCH-18: the engine's "
                     "session transport binds INADDR_ANY on both of its bind sites, so -offline never "
                     "meant unreachable, and on L2 a CGNAT mesh peer joined one. 'auto' derives from "
                     "the SESSION MODE - armed iff the offline lock reads Locked, never off the "
                     "presence of -offline in the args, because an unregistered switch earlier on the "
                     "line voids the whole line silently. Online play is never-asked and untouched: "
                     "nothing is hooked at all. A string rather than a bool because a bool cannot say "
                     "'no opinion' - IConfig::GetBool ignores a caller's default for a declared key, "
                     "so there is no way to tell an absent key from a false one. A new net. namespace "
                     "rather than beside bootstrap.block_egress: inbound containment is not the egress "
                     "blocker, and the two halves of constraint L2 read better as net.* as the surface "
                     "grows")

// ---------------------------------------------------------------------------------------------------
// Level 2 - detours into game code. W1-M's spike moved the UI off row 2.2 and onto the stock call at
// row 0.20, but ARCH-15 found that a draw issued at the stage-0 drain is refused at the first widget
// call, so the SITING of that draw is row 2.5. What is left here is one refusable row and two that
// ship enabled.
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_BOOL("playback.render_suppress_hook", false, "2.1", nullptr, kConfigTextDetour,
                   "Detour CncaGame::Render to suppress presentation during a hidden seek pre-roll")
REPLAY_CONFIG_BOOL("capture.voip", true, "2.3", "VOIP", kConfigTextDetour,
                   "Capture and replay voice, downstream of the jitter buffer and the audibility gate")
REPLAY_CONFIG_BOOL("ui.gate_scope", true, "2.5b", nullptr, kConfigEnvOverride,
                   "Open the engine's GUI suppression gate around our draw, through its own "
                   "setters, and restore the byte we observed (ARCH-20 R1). DEFAULT TRUE since "
                   "packet W2-F-C3: run 20260904-132534Z drew the panel over the loaded level on "
                   "4679/4679 in-frame entries with scopeRestores == scopeEntries == 5102 and "
                   "changed=0, and it is the ONLY configuration in which the panel has ever been "
                   "seen. Set it false to get the retail gate back: no engine setter is called")

REPLAY_CONFIG_BOOL("ui.inframe_draw", true, "2.5", nullptr, kConfigTextDetour,
                   "Draw the panel inside the engine's own GUI frame. DESIGN S4.4 row 2.5: an entry "
                   "detour on CEngine::HeartbeatBefore (GUI.ENGINE_HEARTBEAT_BEFORE, 0x1415483B0), "
                   "installed through hookmgr and prologue-guarded, drawing on the way out after the "
                   "trampoline returns. W2-F registers the site with THIS key, which is how SC7 reads "
                   "the row off it: one 5-byte relative jump at 0x1415483B0 and no other site. Setting "
                   "it false costs the panel itself - with the frame closed every widget call is "
                   "refused at NRadGui::Begin - so it is default-ON in every launch with the UI on")

// ---------------------------------------------------------------------------------------------------
// Sections (S5.4). MANIFEST and CORE are required to play, so their keys are pinned.
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_BOOL("capture.section.manifest", true, nullptr, "MANIFEST", kConfigPinnedTrue,
                   "Schemas, header state, identity and the round table. Required to play")
REPLAY_CONFIG_BOOL("capture.section.core", true, nullptr, "CORE", kConfigPinnedTrue,
                   "The tick spine, player and body state, netvars, events, keyframes. Required to play")
REPLAY_CONFIG_BOOL("capture.section.asseen", true, nullptr, "ASSEEN", kConfigNone,
                   "The as-seen inputs; absence means the file plays ground-truth only")

// ---------------------------------------------------------------------------------------------------
// Streams (S5.4). One key each, so the degradation ladder and the strip tool have something to name.
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_BOOL("capture.stream.tick", true, nullptr, "S_TICK", kConfigPinnedTrue,
                   "The spine: one record per engine tick. Required to play")
REPLAY_CONFIG_BOOL("capture.stream.player_wire", true, nullptr, "S_PLAYER_WIRE", kConfigNone,
                   "Player state and physics sim state as received on the wire")
REPLAY_CONFIG_BOOL("capture.stream.player_sampled", true, nullptr, "S_PLAYER_SAMPLED", kConfigNone,
                   "The same field set sampled from decoded state rather than the wire")
REPLAY_CONFIG_BOOL("capture.stream.body_wire", true, nullptr, "S_BODY_WIRE", kConfigNone,
                   "Non-player bodies as received on the wire")
REPLAY_CONFIG_BOOL("capture.stream.body_sampled", true, nullptr, "S_BODY_SAMPLED", kConfigNone,
                   "Non-player bodies sampled from decoded state")
REPLAY_CONFIG_BOOL("capture.stream.netvar", true, nullptr, "S_NETVAR", kConfigNone,
                   "Netvar changes, stored as raw SValue qwords and never re-encoded")
REPLAY_CONFIG_BOOL("capture.stream.event", true, nullptr, "S_EVENT", kConfigNone,
                   "Component events, ownership, stun/damage/kill, emotes, joins and leaves")
REPLAY_CONFIG_BOOL("capture.stream.world", true, nullptr, "S_WORLD", kConfigNone,
                   "The 22 networked world slots")
REPLAY_CONFIG_BOOL("capture.stream.key", true, nullptr, "S_KEY", kConfigPinnedTrue,
                   "Chapter keyframes. Required to play, and required for seek")
REPLAY_CONFIG_BOOL("capture.stream.mark", true, nullptr, "S_MARK", kConfigNone,
                   "Round and phase markers")
REPLAY_CONFIG_BOOL("capture.stream.asseen", true, nullptr, "S_ASSEEN", kConfigNone,
                   "Per-tick and per-player as-seen samples")
REPLAY_CONFIG_BOOL("capture.stream.gamestate", true, nullptr, "S_GAMESTATE", kConfigNone,
                   "The local game-state sample: phase, round counter, scores, clocks "
                   "(DESIGN S5.4.5b)")
REPLAY_CONFIG_BOOL("capture.stream.playerstats", true, nullptr, "S_PLAYER_STATS", kConfigNone,
                   "The live scoreboard per entrant: points, goals, stuns, saves, ping "
                   "(DESIGN S5.4.5c)")
REPLAY_CONFIG_BOOL("capture.stream.voip", true, nullptr, "S_VOIP", kConfigNone,
                   "Encoded voice frames. capture.voip is the master switch for the whole section")
REPLAY_CONFIG_BOOL("capture.oracle", true, nullptr, "S_ORACLE", kConfigNone,
                   "Session and player-bones snapshots for validation. Ruling Q-B: on by default")

// ---------------------------------------------------------------------------------------------------
// This packet's own keys. They claim no ledger row and no stream: they configure the bootstrap, they do
// not enable an intervention.
// ---------------------------------------------------------------------------------------------------
REPLAY_CONFIG_STRING("log.level", "info", nullptr, nullptr, kConfigNone,
                     "trace | info | warn | error")
REPLAY_CONFIG_INT("log.drain_interval_ms", 250, nullptr, nullptr, kConfigNone,
                  "How often the worker flushes the log ring to disk")
REPLAY_CONFIG_INT("log.chatter_period_ms", 10000, nullptr, nullptr, kConfigNone,
                  "Shortest gap between two prints of a status block that reprints itself "
                  "verbatim while nothing changes; 0 prints every pass")
// The identity dress pass's two timeouts. Both are TIMEOUTS, never gates: no value of either lets
// the dress write anything it could not write at the shipped values, and a harness sets them to
// zero to drive a retry and a give-up inside one test run. Declared here because IConfig refuses an
// undeclared key by name, and the launch logged `no such key` for both while the consumers read
// their compiled defaults.
// The dress pass's two kill switches, both env-overridable so a second launch can isolate a crash
// without editing a file: ECHOREPLAY_PLAYBACK_DRESS=0 stops the pass entirely (admission through
// the provisioner's pump and the body drive are unaffected), and
// ECHOREPLAY_PLAYBACK_DRESS_LOADOUT=0 keeps names, numbers, teams and colours while skipping the
// one step that writes into the engine's per-entrant loadout instance.
REPLAY_CONFIG_BOOL("playback.dress", true, nullptr, nullptr, kConfigEnvOverride,
                   "Dress the recorded roster: names, numbers, teams, colours and loadouts. Off "
                   "runs no dress pass at all")
REPLAY_CONFIG_BOOL("playback.dress_loadout", true, nullptr, nullptr, kConfigEnvOverride,
                   "Apply each entrant's twenty recorded item ids and both numbers. Off skips only "
                   "that step; every other identity field is still written")
REPLAY_CONFIG_INT("playback.dress_retry_ms", 250, nullptr, nullptr, kConfigNone,
                  "How often a deferred identity dress is retried, in ms")
REPLAY_CONFIG_INT("playback.dress_deferral_bound_ms", 30000, nullptr, nullptr, kConfigNone,
                  "How long a dress may stay deferred before it is given up on, loudly, in ms. "
                  "Measured on a clock that advances only while the engine can make progress: it "
                  "does not run while the session gate is shut or while playback is paused")
REPLAY_CONFIG_BOOL("capture.trap", false, "1X.3", nullptr, kConfigNone,
                   "S6.4's allocation, lock and syscall trap. Compiled into release, off by default. "
                   "DESIGN S4.4 row 1X.3: it detours nine kernel32 exports with MinHook in our own "
                   "process, so it patches no echovr.exe byte and can never enter SC7's unattributed[] "
                   "- the row is published for the ledger's completeness, not for a verdict")
REPLAY_CONFIG_STRING("bootstrap.block_egress", "auto", nullptr, nullptr, kConfigNone,
                     "auto | on | off. Refuse every non-loopback destination in this process. 'auto' "
                     "lets the SESSION MODE decide: armed for replay/playback, not armed during normal "
                     "play because there the traffic is the subject of the capture, and armed when the "
                     "mode cannot be read. A sweep arms regardless. A string rather than a bool because "
                     "a bool cannot say 'no opinion' - and IConfig::GetBool ignores a caller's default "
                     "for a declared key, so there is no way to tell an absent key from a false one")
// Read by src/capture. Declared here because this file is the single registry of every key the build
// understands, and a key the product reads but never declares is invisible to the config file the
// plugin writes on first run - the user cannot set what nothing advertises.
// RECORDER-2. The recorder had no session-type gate at all: it opened a file on the state-9 tick of
// EVERY session, offline playback launches included, which is where the owner's folder of empty
// demos came from. These three keys are the gate and its front door.
REPLAY_CONFIG_BOOL("capture.force_offline", false, nullptr, nullptr, kConfigEnvOverride,
                   "Record even when this is not a live online session. The dev escape hatch for "
                   "the workflow that relied on capture being unconditional")
REPLAY_CONFIG_BOOL("capture.auto_record", true, nullptr, nullptr, kConfigEnvOverride,
                   "Start a recording by itself for every live match. Off means `record start` in "
                   "the console, or the Record button, opens the file by hand")
REPLAY_CONFIG_BOOL("capture.rec_indicator", true, nullptr, nullptr, kConfigEnvOverride,
                   "Draw a small REC marker while a recording is running")
REPLAY_CONFIG_BOOL("capture.enabled", true, nullptr, nullptr, kConfigNone,
                   "Record at all. The master switch above every capture.section and capture.stream key")
REPLAY_CONFIG_STRING("capture.dir", "", nullptr, nullptr, kConfigNone,
                     "Where .echoreplay files are written. Empty means %USERPROFILE%\\Documents\\"
                     "EchoReplay, falling back to .\\EchoReplay when the profile is unreadable")

REPLAY_CONFIG_BOOL("bootstrap.offline_bit_lock", true, "2.4", nullptr, kConfigTextDetour,
                   "Keep the offline mode bit from being cleared by the session teardown. DESIGN S4.4 row 2.4: a MinHook detour on sub_1401042B0, the teardown routine both offline-bit clear sites share, located at run time from the image's own unwind data. L5 measured it as the 5-byte jump at 0x1042B0; SC7 reads this row off this key and subtracts the lock's site. Default-ON and armed in every offline launch, but inert unless the bit was already set, so it cannot affect a session that never went offline")
// Read by src/control. Declared here for the same reason capture.enabled and capture.dir
// are: this file is the single registry of every key the build understands, and a key the product
// reads but never declares is invisible to the config file the plugin writes on first run.
//
// A tri-state for the reason bootstrap.block_egress is one - a bool cannot say "no opinion", and
// IConfig::GetBool ignores a caller's default for a declared key, so an absent key and an explicit
// false are indistinguishable. Here "no opinion" is a real third answer: G12 names the console as
// one of the two ways a replay is launched, so it is ON for a person, but a console window
// appearing over a measured frame capture could disturb the run it is sitting on top of.
REPLAY_CONFIG_STRING("control.console", "auto", nullptr, nullptr, kConfigNone,
                     "auto | on | off. The console window that serves the owner's \"console "
                     "command to a file\". 'auto' is on for a normal launch and off during a sweep")

REPLAY_CONFIG_BOOL("bootstrap.log_coexistence", true, nullptr, nullptr, kConfigNone,
                   "List the other DLLs sharing plugins\\ in the log at startup")

// Read by src/engine/clock.cpp. Sub-switches under ledger row 0.4, which is the capability;
// these choose which parts of it are armed and claim no row of their own.
//
// ★ clock.write_floor is not a preference. CLOCK.MINELAPSED_OFF is 0 on a stock CClock -- the
// constructor at 0x1400CD8F0 sets it and nothing else in the image writes it -- and with a zero
// minimum the engine's own pause bit drives the scaled delta to 0, hence CLOCK.RECIP_OFF to
// 1.0f/0.0f = +inf. Turning this key off therefore turns clock.pause_bit off with it rather than
// leaving a switch whose off position is a crash.
REPLAY_CONFIG_BOOL("clock.write_floor", true, nullptr, nullptr, kConfigNone,
                   "Write the 1 us floor to CClock+0x58 so a zero scaled delta, and the +inf "
                   "reciprocal it produces, are unrepresentable anywhere in the engine")
REPLAY_CONFIG_BOOL("clock.pause_bit", true, nullptr, nullptr, kConfigNone,
                   "Pause by setting CClock+0x50 bit 1. Requires clock.write_floor")
REPLAY_CONFIG_BOOL("clock.frame_step", true, nullptr, nullptr, kConfigNone,
                   "Single-frame step by setting CClock+0x50 bit 2 with CClock+0x98 as the delta")
REPLAY_CONFIG_BOOL("clock.mirror_transport", true, nullptr, nullptr, kConfigNone,
                   "Follow IReplayTransport's state and rate every frame, so no caller has to "
                   "remember to tell the clock what the transport just did")

// Read by src/playback/core/playback_module.cpp. The kill switch for the reply route: with
// it false the netgame wiretap is never created, ReplyRouteArmWith is never called, and the drain
// driver below is never installed, so the direct insert stays the shipping path. It claims no ledger
// row and no stream. The route registers a listener on the netgame's own broadcaster; it ALSO drives
// InstallReplyDrainDriver (identity_access.cpp), a dedicated always-armed detour on NETGAME.UPDATE
// that runs the deferred admission drain every frame in-match. That detour is byte-anchored through
// hookmgr under the DEDICATED key pawn.reply_drain below (which carries ledger row 2.7 so SC7 can
// attribute the .text patch) and refuses cleanly if tick's own hook-mode fallback took the site. This
// key remains its master kill switch: pawn.reply_drain's row is NotDeclared whenever this is false.
//
// kConfigEnvOverride because the profile that needs to disarm this is a launch, and a launch cannot
// carry a config file. The name is DERIVED (config.cpp:606): ECHOREPLAY_PAWN_REPLY_ROUTE. Resolved
// centrally so the feature, the ledger dump and the launch record all read one answer -- a feature
// that parsed its own variable is exactly what put two sources of truth in AUDIT_L5_RUN7 H7.
REPLAY_CONFIG_BOOL("pawn.reply_route", true, nullptr, nullptr, kConfigEnvOverride,
                   "Arm the reply route: wiretap the netgame and register the entrant-added "
                   "listener on its broadcaster. False leaves the direct insert as the only path")

// The DEDICATED row for the always-armed NETGAME.UPDATE drain detour (sub_1401BBDB0 / RVA 0x1BBDB0),
// which runs the deferred reply-admission drain every frame in-match and, per DIAG-DRAIN-NOOP Q2, calls
// the engine's own EntrantAdded handler directly. It is a SEPARATE key from pawn.reply_route because the
// route (listeners) is independent of the drain detour: the route must stay Declared in every tick mode,
// but the detour is byte-anchored on NETGAME.UPDATE and installs ONLY in tick.mode=slot -- in hook mode
// tick's own fallback already owns the site and hookmgr::Create refuses by prologue. So SC7 must expect
// this row iff the detour actually installs, and RowDeclarationState (config.cpp) value-gates it to
// Declared only when tick.mode resolves to "slot" (the complement of tick.mode's own hook-only row 1);
// declaring it in hook mode would be the 2.5b "declared row with no detour" trap. kConfigTextDetour so
// SC7's GatherSites/RowPatchesText attribute the 0x1BBDB0 patch to row 2.7; kConfigEnvOverride so a launch
// can carry ECHOREPLAY_PAWN_REPLY_DRAIN with no config file.
REPLAY_CONFIG_BOOL("pawn.reply_drain", true, "2.7", nullptr,
                   kConfigTextDetour | kConfigEnvOverride,
                   "Always-armed NETGAME.UPDATE (sub_1401BBDB0 / RVA 0x1BBDB0) detour that runs the "
                   "deferred reply-admission drain each frame in slot-tick mode. Byte-anchored; refuses "
                   "when tick.mode=hook already owns the site")

// BLOCKER-2 (DIAG-REPLYPHASE section 3). Derived name (config.cpp:606): ECHOREPLAY_STAGEB_AUTHFLIP.
// kConfigEnvOverride so a launch can isolate the flip with no config file. Default OFF
// (ENG-DRESS-LOADOUT): DIAG-SEAT-DRIVE 2.3 proved the SPAWN_NOTIFY remote arm is the EQUIPMENT path
// (R14NetPlayerEquipment) and can never dress the body, so the authflip that forces it is not a
// dressing mechanism. Disabled so the clean loadout-apply route is the sole body-dress path and the
// section-6 probe can settle Lever A vs B in one run; spawnNotify still fires its host (equipment)
// arm and the drivegate line still reads the model CI before/after for the A/B probe.
REPLAY_CONFIG_BOOL("stageb.authflip", false, nullptr, nullptr, kConfigEnvOverride,
                   "Stage-B authority spoof: around the entrant's spawnNotify (UserSpawned) ONLY, "
                   "transiently set the session-authority word's non-host bit so the engine takes its "
                   "REMOTE spawn arm. That arm is the equipment path (DIAG-SEAT-DRIVE 2.3) and does NOT "
                   "dress the body; default OFF takes the host arm so the loadout-apply route dresses. "
                   "RAII/SEH restored, minimal window")

// ENG-DRESS-LOADOUT (DIAG-SEAT-DRIVE): the model-swapper hook -- SwapperResetSweep's pooled
// current-reset (IDENT via ResetSwapperCurrent) plus the direct BeginModelSwap re-trigger. Default
// OFF: the sanctioned body-dress mechanism is the engine's own loadout-apply (LoadoutApplyAlignment),
// not the model-swapper hook, and a competing swapper drive would keep the section-6 probe from
// isolating whether the name-aligned loadout-apply alone dresses (Lever A) or the enable/profile
// landing must also be driven (Lever B). kConfigEnvOverride so a launch can re-arm it without a
// config file if the probe shows the loadout-apply route needs it.
REPLAY_CONFIG_BOOL("stageb.swapper_hook", false, nullptr, nullptr, kConfigEnvOverride,
                   "Model-swapper hook: the pooled-actor swapper current-reset sweep and the direct "
                   "BeginModelSwap re-trigger. Default OFF -- the loadout-apply route dresses the body; "
                   "this competing swapper drive is disabled so the dressing probe stays clean")

// ENG-BOT-BODY.  Route each recorded player through the engine's OWN bot body-build (AdmitUser over
// AddBotUser, IDENT.ADMIT_USER_VA) instead of the type-5 remote admission: AddBotUser applies the
// default profile+loadout and a visible default body locally with no peer, and the actor step it
// reaches (SET_PLAYER_ACTOR) installs the ncaPhysics/R14RemotePlayer pair the history-drive needs.
// The bot's AI is MUTED, not launched: the shipped movement decouple makes the history pose win over
// motor input (PRIMARY), the seeded account nibble stays 5 so SPAWN_USER never allocates the
// type-6-gated AI-brain record (SECONDARY), and a guarded best-effort write zeroes the NetBot drive
// level (TERTIARY).  Default OFF, flipped as a PAIR with pawn.equipment_recv (default ON): the
// emulate-a-player-join reply route plus the equipment RECV is the route that rendered the recorded
// players in LAUNCH-SPAWNED-SEED / LAUNCH-BIND-FIX.  ON selects this bot body-build instead.  Read only
// while a capture is loaded (the provisioner is armed and pumped behind playback's `loaded` gate), so
// neither value changes a live match.  kConfigEnvOverride so a launch can A/B the two admission paths
// without a rebuild.
REPLAY_CONFIG_BOOL("pawn.bot_body", false, nullptr, nullptr, kConfigEnvOverride,
                   "ON: route recorded players through the engine's own bot body-build (AddBotUser). "
                   "Default OFF: the emulate-a-player-join reply route, which with "
                   "pawn.equipment_recv (default ON) is the launch-verified visible-body path. "
                   "Only read while a capture is loaded")

// ENG-BOT-BODY appearance overlay.  The bot ships the DEFAULT bot look (guaranteed-visible humanoid)
// from AddBotUser's own default-profile apply; this optional overlay additionally attempts the
// recorded loadout on top, through the same profile/loadout hooks the remote dress ladder uses, but
// BEST-EFFORT -- a failed overlay never discards the visible default body.  Default OFF: the default
// look ships first and the overlay is opt-in until a launch confirms it dresses without regressing
// the body.  kConfigEnvOverride so a launch can arm it without a rebuild.
REPLAY_CONFIG_BOOL("pawn.bot_loadout", false, nullptr, nullptr, kConfigEnvOverride,
                   "ENG-BOT-BODY: also attempt the recorded loadout on the bot body (best-effort "
                   "overlay on top of the guaranteed default look). Default OFF")

// ENG-BOT-GATE-OFF (safety revert).  Whether the bot admission bridges AddBotUser's default loadout
// into the per-slot live table (HookPublishLoadout).  ON, the per-slot count reads > 0, the L3 #18
// crash guard passes, and the actor step (SET_PLAYER_ACTOR) runs -- which triggers the framework
// model-build out-of-band from our stage-0 detour and DEADLOCKS the engine (DIAG-BOT-DEADLOCK).
// Default OFF restores the prior non-crashing behavior: the count stays 0, the guard defers, no
// actor step, no model-build, no deadlock -- the bot is admitted without a driveable body.  Left as
// a config flip so the actor path can be re-enabled for controlled re-architecture testing.
// kConfigEnvOverride so that re-enable needs no rebuild.
REPLAY_CONFIG_BOOL("pawn.bot_publish_loadout", false, nullptr, nullptr, kConfigEnvOverride,
                   "ENG-BOT-GATE-OFF: bridge the bot's default loadout to the live table so the "
                   "actor step runs. Default OFF -- ON currently deadlocks the engine")

// ENG-NATIVE-ADMIT (echo-101).  On the engine's OWN remote-admission reply route (pawn.bot_body
// OFF), parse the recorded loadout into the per-user SOURCE container BEFORE the reply is
// dispatched, so the add-entrant handler's tail bridge (IDENT.LOADOUT_ASSIGN) copies count>0 into
// the live table and the framework's phase-16 model-build reads real instances instead of the
// zero-length array the deadlock monitor aborted on.  Default ON: the SOURCE fill is the deadlock
// protection for this route (it has no L3 #18 loadout guard of its own), so a launch that turns on
// the remote route must not silently reproduce the empty-loadout stall.  Harmless on the bot path
// (its direct insert has no bridge).  kConfigEnvOverride so a launch can A/B it off
// (ECHOREPLAY_PAWN_REMOTE_SOURCE_LOADOUT=0) as a control that reproduces the empty-loadout deadlock.
REPLAY_CONFIG_BOOL("pawn.remote_source_loadout", true, nullptr, nullptr, kConfigEnvOverride,
                   "ENG-NATIVE-ADMIT: parse the recorded loadout into the per-user SOURCE container "
                   "before remote admission so the engine's own bridge copies a non-empty loadout. "
                   "Default ON -- OFF reproduces the empty-loadout deadlock as a control")

// ENG-BOT-BODY-SOURCE (echo-102).  On the BOT body-build path (pawn.bot_body ON, default), fill the
// per-user loadout SOURCE with the RECORDED player's loadout and then bridge it into the per-slot
// live table before the L3 #18 crash guard and the SET_PLAYER_ACTOR step -- so the framework's
// phase-16 model-build runs over a NON-EMPTY loadout (no `Deadlock detected!`) and the bot's native
// actor+model instantiates into a visible, history-driven, AI-muted body.  Unlike the remote route,
// the bot insert (AddBotUser) has no engine bridge, and GenerateBotProfile CLEARS+reparses the SOURCE
// during admit (sub_14016C140 -> IDENT.LOADOUT_FROM_PROFILE on the container), so the recorded fill
// runs AFTER the insert (a new post-admit hook) and the SOURCE->LIVE copy (HookPublishLoadout) follows
// it -- both satisfying the guard with REAL parsed instances, never by disabling it.  Default OFF so
// the shipped bot-body default stays the prior non-deadlocking behavior (count 0, guard defers, no
// actor step).  kConfigEnvOverride so a launch arms it without a rebuild:
// ECHOREPLAY_PAWN_BOT_BODY_SOURCE_LOADOUT=1.
REPLAY_CONFIG_BOOL("pawn.bot_body_source_loadout", false, nullptr, nullptr, kConfigEnvOverride,
                   "ENG-BOT-BODY-SOURCE: on the bot body-build path, fill the loadout SOURCE with the "
                   "recorded loadout (after the insert) and bridge it into the live table before the "
                   "crash guard + actor step, so the bot builds a visible body with no deadlock. "
                   "Default OFF -- ON is the working-body experiment")

// ENG-EQUIPMENT-RECV (DIAG-MODEL-COMPONENT-LEVER).  On the engine's OWN remote-admission reply route
// (pawn.bot_body OFF), the entrant already has a native actor that binds and moves from history; the ONE
// missing thing for a visible body is the net-model component on that actor.  With this ON, the plugin
// drives the engine's own R15NetPlayerEquipmentUpdate RECV (sub_140D37300) once per settled entrant,
// carrying the RECORDED chassis, so the engine's own model-changed delegate installs the model through
// CModelSwapperCS and the framework's phase-16 rebuild seats it -- verified to stay mark-dirty/deferred and
// NEVER recurse into the flush sub_140323790 or the seat sub_14030F120.  Module couples it to !bot_body, so
// the bot-body route never fabricates a RECV.  Default ON, flipped as a PAIR with pawn.bot_body (default
// OFF): launch-verified in LAUNCH-SPAWNED-SEED / LAUNCH-BIND-FIX.  Only read while a capture is loaded.
// kConfigEnvOverride so a launch can A/B it without a rebuild: ECHOREPLAY_PAWN_EQUIPMENT_RECV=0.
REPLAY_CONFIG_BOOL("pawn.equipment_recv", true, nullptr, nullptr, kConfigEnvOverride,
                   "ENG-EQUIPMENT-RECV: on the remote route, drive the engine's own equipment RECV with "
                   "the recorded chassis so the engine installs the net-model component and the framework "
                   "seats a visible body. Default ON (with pawn.bot_body OFF); only read while a capture "
                   "is loaded")

// ENG-SPAWNED-SEED backstop (DIAG-ENABLE-INVALID-USER Fix B).  The reply seed now writes `spawned`
// before the entrant add, so the engine's own role transition enables the actor.  This is the
// backstop for an entrant that was added while the record still read 0: once per entrant, from the
// stage-0 NETGAME.UPDATE window, the plugin drives the engine's own NETGAME.PARTICIPATING_VA
// (sub_1401C9BA0) and then the dress half sub_1401C7BE0 -- NETGAME.USER_SETTINGS_APPLY's tail
// without its Team Slot reallocation.  Never for a recorded spectator, never for a local user, and
// only when the engine's own `spawned` reader already answers true.  Module couples it to
// !bot_body.  Default OFF; arm with ECHOREPLAY_PAWN_PARTICIPATE_BACKSTOP=1.
REPLAY_CONFIG_BOOL("pawn.participate_backstop", false, nullptr, nullptr, kConfigEnvOverride,
                   "ENG-SPAWNED-SEED backstop: for an admitted, spawned, still-disabled recorded "
                   "player, drive the engine's own participating dispatch and dress once from the "
                   "stage-0 window. Default OFF; arm with ECHOREPLAY_PAWN_PARTICIPATE_BACKSTOP=1")

// ENG-CLOCK-STATE. Make the HUD clock and rendered match phase follow the playback timeline: while a
// replay is loaded in a contained process, the NETGAME.UPDATE stage-3 tail writes the recorded
// game_timer / countdown_timer / game_state netvars, resolved at the playhead, through the engine's own
// typed setters (store + change event), so play/pause/rate/reverse/seek all track.
// ★ SHIP-DEFAULTS: default ON. The owner launches through tools\watch_replay.ps1 with NO environment
// variable, so anything that ships off is a feature his own run does not have. The writer itself is
// inert outside a contained process -- HudNetVarsArm's first argument is ProcessContained()
// (playback_module.cpp:1229, netvarhud.cpp:327) -- and row 2.8's detours resolve to `off` for a live
// online match (bootstrap.cpp:627-631), so ON here changes nothing about a live capture.
REPLAY_CONFIG_BOOL("playback.gamestate_clock", true, nullptr, nullptr, kConfigEnvOverride,
                   "Drive the HUD clock netvars (game_timer, countdown_timer, game_state) from the "
                   "playback timeline through the engine's typed netvar setters, so the in-arena "
                   "clock follows scrub/seek/pause/reverse. Default ON in a contained playback "
                   "process; ECHOREPLAY_PLAYBACK_GAMESTATE_CLOCK=0 turns it off")

// ENG-GAMESTATE-SCORE / ENG-CLOCK-STATE. Drive the scoreboard netvars (blue_score/red_score and
// blue_round_score/red_round_score) from the timeline through the engine's typed setters. Either
// gamestate flag also binds the netvar table over the engine's cached route, so every other recorded
// netvar resolves last-write-<=-T through StateApplier::Apply -> the engine's own store decoder.
// ★ SHIP-DEFAULTS: default ON, same containment argument as playback.gamestate_clock above.
REPLAY_CONFIG_BOOL("playback.gamestate_score", true, nullptr, nullptr, kConfigEnvOverride,
                   "Drive the scoreboard netvars (blue/red_score, blue/red_round_score) from the "
                   "playback timeline through the engine's typed netvar setters. Default ON in a "
                   "contained playback process; ECHOREPLAY_PLAYBACK_GAMESTATE_SCORE=0 turns it off")

// HOST-NV, ledger row 2.8: entry detours on the six CR15NetVarCS setters (sub_140D45820, 46790,
// 47D60, 47E50, 48DD0, 48EC0), all six under this one row -- SC7 compares rows, and the six are
// installed or refused together. A STRING resolved by a provider, like tick.mode: bootstrap publishes
// `hook` only for a contained process (never a live online match) that has a gamestate flag on, and
// `off` otherwise, so the row is Declared exactly when the sites exist and Unresolved before the
// provider has spoken. `off` in the file keeps the detours out; nothing in the file can ask for them
// in a live match.
REPLAY_CONFIG_STRING("playback.host_netvar_hook", "auto", "2.8", nullptr, kConfigTextDetour,
                     "auto | off. Detour the six CR15NetVarCS netvar setters so host writes to the "
                     "keys a replay is driving can be counted or filtered. auto installs them only "
                     "in a contained process with playback.gamestate_clock or _score on")
// What the armed detours DO. Off, they count the host's writes to the driven keys and pass every one
// (the `hud netvar host writes (observe)` line); on, those writes are swallowed. Derived name:
// ECHOREPLAY_PLAYBACK_HOST_NETVAR_FILTER.
// ★ SHIP-DEFAULTS: default ON. This flag only decides what an ALREADY-INSTALLED row 2.8 detour does,
// and row 2.8 installs nowhere but a contained, offline-locked process (bootstrap.cpp:627-631). With
// the detours absent the flag reaches no code at all, so there is no live surface to turn on here.
REPLAY_CONFIG_BOOL("playback.host_netvar_filter", true, nullptr, nullptr, kConfigEnvOverride,
                   "Swallow the host's own writes to the (slot, symbol) netvars the replay is "
                   "driving -- the arena script's game_timer/game_state/score republish and the "
                   "native game_frisbee_ownerid write -- so the HUD stops flickering to the host's "
                   "values. Default ON; ECHOREPLAY_PLAYBACK_HOST_NETVAR_FILTER=0 counts them instead")

// DISC-BIND. Bind the recorded tag-1 disc track (an SComponentID of CR15NetPhysicsCS, checked against
// the live frisbee's own SR15FrisbeeCI+0x18) and drive it like a replayed player's body: synthetic
// remote owner at CI+0x1C, recorded history into the interpolation window.
// ★ SHIP-DEFAULTS: default ON. `g.disc_drive_on` ANDs this with ProcessContained() and with the score
// gate having installed (playback_module.cpp:3708-3710), so outside a contained process the drive --
// and the pre-roll that hangs off it -- stays off whatever this says. Derived name
// ECHOREPLAY_PLAYBACK_DISC_DRIVE.
REPLAY_CONFIG_BOOL("playback.disc_drive", true, nullptr, nullptr, kConfigEnvOverride,
                   "Bind and drive the recorded disc body during a replay. Default ON in a "
                   "contained playback process; ECHOREPLAY_PLAYBACK_DISC_DRIVE=0 turns it off")

// DISC-RING-4: the third field of the disc's possession, beside the owner dword and the touch pair.
// The engine's remote holder arm (sub_140CED640) ends on bit 0x200 of the newest physics history
// node's state word; the recording carries that bit in bits11 only on the tick possession changed,
// so without this level the net-touch update reads the disc as unheld every other frame and its
// grab edge -- the one the disc script clears its `bubble` ring on -- is never raised. Derived name
// ECHOREPLAY_PLAYBACK_DISC_NET_TOUCH_HELD.
REPLAY_CONFIG_BOOL("playback.disc_net_touch_held", false, nullptr, nullptr, kConfigEnvOverride,
                   "Re-apply the recorded holder as bit 0x200 of every physics history node pushed "
                   "for the disc, so the engine's own holder gate agrees with the owner dword. "
                   "ECHOREPLAY_PLAYBACK_DISC_NET_TOUCH_HELD=1 turns it on")

// DISC-BIND, ledger row 2.9: one entry detour on CR15NetGame::OnScoreCB (sub_140185DF0). A driven
// disc crosses the goal volume and the host's own detector scores it -- stats, score netvars,
// rules+0xC := 3, a reliable send, then the arena script's disc reset and player teleports. A STRING
// resolved by bootstrap exactly like playback.host_netvar_hook: `hook` only for a contained process
// (never a live online match) with playback.disc_drive on, `off` otherwise. `off` in the file keeps
// the detour out, and the disc is then not driven either unless playback.disc_score_gate is off too.
REPLAY_CONFIG_STRING("playback.host_score_hook", "auto", "2.9", nullptr, kConfigTextDetour,
                     "auto | off. Detour CR15NetGame::OnScoreCB so a replayed disc cannot score a "
                     "real goal on the host. auto installs it only in a contained process with "
                     "playback.disc_drive on")
// What the armed detour DOES while a replay is loaded and in a match: on, the score callback
// returns without running; off, it is counted and passed. Derived name
// ECHOREPLAY_PLAYBACK_DISC_SCORE_GATE.
REPLAY_CONFIG_BOOL("playback.disc_score_gate", true, nullptr, nullptr, kConfigEnvOverride,
                   "Swallow the host's OnScoreCB while a replay is loaded and in a match, so the "
                   "replayed disc scores nothing. Default ON; ECHOREPLAY_PLAYBACK_DISC_SCORE_GATE=0 "
                   "counts and passes instead")
REPLAY_CONFIG_BOOL("playback.clear_actor_on_leave", true, nullptr, nullptr, kConfigEnvOverride,
                   "After each entrant leaves, restore the pawn records the engine vacated to 'no "
                   "actor', so the next load's admission is not handed a freed actor guid")
REPLAY_CONFIG_BOOL("playback.leave_reverse_order", true, nullptr, nullptr, kConfigEnvOverride,
                   "Remove the replay's entrants from the highest engine slot down instead of the "
                   "lowest up. Pure ordering -- the same slots go through the same engine handler. "
                   "Default ON: the engine's own teardown COMPACTS the user table, so removing from "
                   "the bottom up moves every remaining entrant under each removal that follows, "
                   "and ENG-RELOAD-5 measured that as what leaves one pooled actor record "
                   "un-re-keyed after an 8-player unload")
REPLAY_CONFIG_BOOL("playback.leave_one_per_pass", true, nullptr, nullptr, kConfigEnvOverride,
                   "Remove at most one entrant per engine update instead of all of them in one "
                   "tick. Default ON, measured together with playback.leave_reverse_order: with "
                   "both, every pooled record comes back at the generation the pool stamps, nothing "
                   "is retired, and the reload admits all eight entrants into live handles")
