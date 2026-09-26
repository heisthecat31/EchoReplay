# As-seen playback and the two pose slots

**Summary.** Each remote player has two pose slots in the engine. Only the first is drawn; the second
slot's pose is written every frame and read by nothing. So "as seen" playback (the match as one client
saw it) does not need the two slots to differ. The viewer just samples each player at the time that
client rendered them. A second, more invasive route (a late engine update between the pose producer
and consumer) exists but is not needed.

All numbers are from the PC build (`echovr.exe` MD5 `caa05ee3bca2f5df0809994e93e2cc05`). The Quest
build was used for names and structure only. No game was launched. Tags: `[V]` read from instruction
bytes, `[I]` from decompiled pseudocode, `[U]` needs the game running.

## The two slots

`SR15RemotePlayerCI` (pool stride 832, base `CS+256`, handle map `CS+208`) holds two
`SR15NetPlayerHistoryState` slots of 152 bytes:

| Slot | CI offset | Filled with (online) |
|---|---|---|
| 1 | `+168` (`0xA8`) | `InterpolateState(history, PeerTime(owner) − 44,440 µs)` |
| 2 | `+320` (`0x140`) | `InterpolateState(history, PeerTime(owner) ± abs(*(float*)(PhysCI+292)))` |

`[V]` The producer is `sub_140D7DA20` (`CR15RemotePlayerCS::UpdateInterpolatedPlayerStates`). It is the
only function that indexes the 832 pool (`imul rdi, rdx, 340h` at `0x140D7DA3D`), calls `PeerTime`
(`sub_14018AF00`) and then `sub_140D064D0` twice with `ci+168` and `ci+320` as outputs. The Quest twin
has the full symbol and the same two-call shape.

## Who reads each slot

Method: disassemble every function that indexes the 832 pool (45 functions) and classify each memory
operand in `[168,320)` or `[320,472)` by whether its base register holds the CI or the CS. A plain
sweep of decompiled offsets does not work: `CS+184/208/224/252/256/320/336/344/360/424/432` are real
CS fields with the same numbers, so it reports about 30 false readers.

| Reader | What it reads | Evidence |
|---|---|---|
| `sub_140D85EC0` = `CR15RemotePlayerCS::UpdateRemoteBodyCI` | **Slot 1 pose**: `+208` quat, `+224` quat, `+240` vec3, `+256` quat, `+272` vec3 | `+208` passed to `sub_1408FEFD0`; the four vectors are loaded just before |
| same function | **Slot 2, four scalars only**: `+440`, `+444`, `+448`, `+452` | `0x140D862C0` `F3 0F 10 86 C4 01 00 00` and the three loads after it |
| `sub_140D48030` | Slot 2 vec3 at `+460` / `+468` | `0x140D483F0`, `0x140D483F9`; only when a hand is holding something (`ci+140 & 0x60`) |

`[V]` Nothing reads slot 2's quaternions or hand vectors (`+360`, `+376`, `+392`, `+408`, `+424`). They
are written every frame (`0x140D7DCD7 … 0x140D7DD1D`) and never used. Slot 2 is not a second rendered
pose; it carries four scalars and one hand vector.

`sub_140D85EC0` is identified by strings: its caller `sub_140D77050` passes
`"CR15RemotePlayerCS::UpdateRemoteBodyThrottled"` / `"CR15RemotePlayerCS::UpdateRemoteBody"` to the
parallel-for dispatcher, and both paths call `sub_140D85EC0`. The same caller reads `CS+520` (throttle
enable), `CS+536` (bodies per frame) and `CS+544` (round-robin cursor). `[V]`

The four scalars go to `sub_1408F89E0`, which writes them into the animation pool (stride 16240) at
`+15024`, `+15028`, `+168` and sets `0x40` at `+15142`. They are the thruster / boost visual state
(`[V]` offsets, `[I]` meaning).

## Offline, the slots are already identical

In an offline session `sub_140D7DA20` takes a short path (`(netflags & 0x44) == 0x40`). It decodes
`PlayerCI+60` into slot 1 and then copies slot 1 into slot 2. For every slot-2 field that anything
reads:

| Slot 2 field | Offline source | Slot 1 twin | Offline source |
|---|---|---|---|
| `+440` | `*(DWORD*)(ci+152)` | `+288` | `*(DWORD*)(ci+152)` |
| `+444` | `*(DWORD*)(ci+156)` | `+292` | `*(DWORD*)(ci+156)` |
| `+448` | `(*(DWORD*)(ci+140) >> 27) & 1` | `+296` | same expression |
| `+452` | `*(DWORD*)(ci+160)` | `+300` | `*(DWORD*)(ci+160)` |
| `+460` / `+468` | `*(QWORD*)(ci+308)` / `*(DWORD*)(ci+316)` | `+308` / `+316` | `*(QWORD*)(ci+120)` / `*(DWORD*)(ci+128)` |

`[V]` from the store list `0x140D7DCAD … 0x140D7DD24`. Writing one value per player per frame, as the
viewer does, reproduces the only state an offline client ever has.

## How as-seen playback works

The client rendered slot 1, one value per player per frame. As-seen and ground truth differ only in
the time at which the viewer samples each player's recorded stream:

```
for each player p, each frame:
    t_pub = playhead
            - (as_seen ? render_delay_us     : 0)   // the -44,440 µs the client applied
            + (as_seen ? time_offset_us[p]   : 0)   // the recorded PhysCI+292
    state = SampleStream(p, t_pub)
    publish state                                   // same path as ground truth
```

In as-seen mode the viewer also skips publishing a player on frames where the recording says the
client did not update them (occluded, or throttled by `CS+520/+536/+544`). The inputs come from the
file's ASSEEN section (`capture.section.asseen`). Console: `mode asseen` / `mode ground`.

Body physics needs no slot work at all: `UpdateTargetState` (`sub_140D76960` → `sub_140D87C80`)
interpolates the ring the viewer fills, so as-seen there is just a different playhead.

**Remaining difference.** The four thruster / boost scalars are sampled at the pose's time instead of
the pose's time ± `PhysCI+292`: at most about 45 ms of a visual thrust value. Nothing else in the image
can observe the two-slot difference. How visible those 45 ms are was not measured.

## The other route: a late engine update

If exact slot skew were ever needed, the engine supports registering an update between producer and
consumer.

- Every component-system update is installed by `CncaGameSpace::RegisterUpdate` (`0x140506810`). Its
  6th argument is a `u16` order key. The phase-name string argument (`r9`) is never read. `[V]`
- The pose producer runs at key `0x0002` (`"UpdateBeforeScripts"`, `sub_140D76970` → `sub_140D7DA20`).
  The consumer runs at `0x6002` (`"UpdateBody"`, `sub_140D77050` → `sub_140D85EC0`). Keys
  `0x0003 … 0x6001` are free ground between them, already used by many stock phases.
- `RegisterUpdate` appends a 72-byte record, writes the keys at `record+0x38` / `+0x3A`, and sets a
  dirty flag at `scheme+1496` (`0x140506857`). The per-frame runner (`sub_140505260`) checks that flag
  and re-sorts the table with a quicksort keyed on those two `u16`s (`sub_1404DB020`). So late
  registration works by design. `[V]`

Open points for that route, all `[U]`: how to get the `CncaGameSpace*` (it arrives as `rdx` of
`sub_14011CBA0`, `CR15Game::RegisterComponentSystemUpdates`); which thread the node runs on (the runner
can hand work to a task kernel); what `record+60` gates; and re-registering when the game space
changes. It is not built, because the sampling approach above costs nothing and the late update would
only improve four thruster scalars by up to 45 ms.

## Approaches that were ruled out

| Approach | Result |
|---|---|
| A late integrator that overwrites the slots after the viewer writes them | None. Within the 832-pool functions the only writer of either slot is `sub_140D7DA20`, once per frame |
| A `CTimerManager` callback as the late tick | Fires after `UpdateBody`; the next frame overwrites both slots before anything reads them |
| Naming slot fields from Quest symbols | Only typedefs exist. `CurrentState()` returns the raw block at `+60..+167`, confirming `+60` is the current state and not a slot |
| Making the engine take its online interpolation branch offline (clear `0x40`) | Refused: `IsDedicated`, `PeerTime`, the netvar gate and the lobby read the same flag word. It would only fill slot 2's unread pose |
| Patching the `(netflags & 0x44) == 0x40` test | Refused, and it gains nothing for the same reason |

## Known gaps in this analysis

1. The reader census covers functions that index the 832 pool. A function handed a CI pointer by
   someone else could be missed; only `sub_140D7DA20` and the CI initialiser `sub_140D4CBA0` take the
   address of a slot.
2. `sub_140D48030` is not named.
3. `netflags & 4` being clear offline is inferred. If it were set, the engine would interpolate from the
   history ring, which would make as-seen easier, not harder.
