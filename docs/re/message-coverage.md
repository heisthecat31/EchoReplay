# Network message coverage

**Summary.** Which network message types an arena match sends, which ones the recorder taps, and what
is known about their layouts. Player identity lives in `SNSLobbyEntrantStatev2`; team changes, stats
framing and the id-space table are decoded below. Everything the recorder taps is stored verbatim, so
layouts that are still open can be decoded later from existing files.

## The reference capture and its limits

| | |
|---|---|
| What | A private capture of one UDP socket during one retail arena match (host + client), decoded to text |
| Size | 34,791 datagrams, 97,779 message records. The event decoders parse all 97,779 and drop 0 |
| Decode quality | 0 decode errors, 0 MAC failures |
| The match | One human, AI fills the other slots, one round, stops about 2 s into `main_loop` |
| Cannot answer | Anything that needs a second human, a full 3-round match, the login-service connection, or Combat |

A message type missing from this capture is not proof that it does not exist.

**Two sources.** `[wire]` is the capture above. `[lib]` is the Quest build `libr15.so`, which keeps
symbols; each `[lib]` row names the function it came from.

**How a message body is identified.** By computing `CMatSym::TypeId` of the struct name with the
engine's own 128-entry character table, and matching that symbol on the wire, not by recognising a
shape. `tests/wiretap/test_matsym.cpp` checks the computation against three known answers, and
`Hash("")` returning `0x8040201008040201` is its control. 428 struct names were extracted from
`echovr.exe` and hashed; 26 of the capture's 28 distinct symbols resolved directly, and the last two
(`SBroadcasterPingUpdate`, `SBroadcasterReliableBatchEvent`) once the name pattern was widened past
`SR15`/`SNS`.

## Open items

| Item | Status | Notes |
|---|---|---|
| `SR15NetReliablePostScoreUpdate` (goal event) | Open | Name exists (`B9F8831AB6E068A9`); 0 instances, the capture ends before any goal |
| `SR15StunMsg` / `SR15StunPlayerMsg` / `SR15RemoveStun` | Open | Names exist (`1FEB495968559BC3`, `CCD9ED2D3125FF67`, `0D82D66C7EBDC8F5`); 0 instances |
| Combat: damage / kill / respawn / teleport | Out of scope | All names are real: `SR15NetDamageableDealDamage`, `…DamageDealt`, `…HealthBroadcast`, `…PlayerKilled` (`BA9DB3F79FA8A511`), `…PlayerRespawnComplete` (`CAD5776E4AFC09D2`), `SR15NetGameplayTeleportPlayer` (`4D7FCB464E548B84`), plus `SR15NetDamageablePlayerRespawn` (`D2B758303E284785`) and `SR15NetExplosionPlayerDamage` (`C9BF5B1DBB888DEF`) |
| `SR15NetPlayEmote` | Open | Name exists (`7C63C5877B0EEE97`); 0 instances |
| Post-`main_loop` game states | Partly closed | The netvar stream carries every `game_state` transition up to `main_loop_update`: `pre_match_wait_for_ready_update` → `round_start` → `team_room_wait_enter/update` → `transition_to_launch_enter/update` → `pre_launch_enter/update` → `main_loop_enter` → `main_loop_update`. Later states (point_score, round_over, post_match, sudden_death) are not in the capture |
| Per-player stats | Layout closed, content open | See below. A solo match produces empty stat objects; populated bodies need 2+ humans |
| Is 44.02 Hz fixed or the client frame rate? | Open | Needs a headset capture. The format stores a timestamp per record, so it does not matter |
| `SimStateEvent` with more than one entry | Not seen | All 11,029 instances are 64 bytes; the reader handles N entries anyway |

## Decoded messages

Each row here is checked against real bytes in `tests/events/fixtures.h` and decoded by `src/events/`.
`events_corpus` runs the same decoders over the whole capture.

### `SNSLobbyEntrantStatev2`: player identity

`[wire]` 4 bodies (360 / 364 / 368 / 400 B). `[lib]` `CNSLobby::EntrantStateCB`.

| Offset | Width | Field | Evidence |
|---|---|---|---|
| 0 | u64 | platform code (DMO = 7) | `[lib]` `SNSUserID` is `[u64 code][u64 account]` |
| 8 | u64 | account id | cross-checked against the platform string |
| 16 | u64 | session/party id | `[wire]`; meaning unknown, carried as-is |
| 24 | char[36] | platform string, NUL-padded | ends exactly where the name begins |
| 60 | char[36] | display name, NUL-padded | `[lib]` `CSysString::Compare(slot+60, body+60, .., 0x24)` |
| 96 | u16 | transient flag marker | `[lib]` low 2 bits drive slot `+136` |
| 98 | u16 | global entrant index | `[lib]` re-stamped by the host on every forward |
| 100 | u16 | generation | `[lib]` must equal the slot's `+140` |
| 102 | u16 | team, `0xFFFF` = none | `[lib]` maps to slot `+142`; `[wire]` `0xFFFF, 0xFFFF, 1, 1` |
| 104 | | profile JSON, NUL-terminated | `[lib]` and `[wire]` |

Two independent checks:

- The platform string at `+24` spells the account id at `+8` (`"DMO|<16 hex digits>"` against the
  same u64). `events_corpus` formats the integer and compares the text on every instance.
- The team at `+102` matches the `team` field of the `SNSLobbyOwnerState` JSON, a different message
  from the other end of the connection.

`0xFFFF` in `team` is a real value (no team yet), not an error.

### `SNSLobbyEntrantTeamChanged`

`[wire]` 1 body, 16 B: `00×8 | 01 00 | ff ff | 00×4`. `[lib]` `EntrantTeamChangedCB` reads the entrant
index (u64) and team (u16) and writes `lobby+864 + 216*entrant + 142`. Layout:
`[u64 entrant index][u16 team][u16 0xFFFF][u32 0]`.

### `SNSLobbyEntrantRemoved` (from `[lib]` only)

No instances: nobody left that match. `[lib]` `EntrantRemovedCB` reads the slot as a u64 and resets
`lobby+864 + 216*slot`; the broadcaster's listen proxy drops a body under 16 bytes, so the wire body is
`[u64 slot][u64 pad]`. The test fixture for it is synthetic and labelled so.

### `SR15NetIdsResponse`: framing closed, id meaning not claimed

`[wire]` 2 bodies, 218 B and 278 B. Framing `[u64 space][n × u64 id][n × u16 slot]`, exactly `8 + n*10`
with `n = 21` and `n = 27`.

The 8-byte header is an id-space key, and there are two spaces:

1. Each `SR15NetIdsRequest` body (8 bytes) is byte-identical to one response's header.
2. The low u32 of one key appears at `body+4` of every client-to-server
   `SR15NetPhysicsSimStateEvent`; the low u32 of the other appears there on every server-to-client one.

| Space key | Slots | Entries | Distinct ids | Direction its low u32 tags |
|---|---|---|---|---|
| `0x3F9915D3001DC28E` | 0 … 26 | 27 | 4 (runs of 10 and 15) | client to server |
| `0x576ED3F8428EBC4B` | 27 … 47 | 21 | 21, all distinct | server to client |

The id column is unique in one space and heavily repeated in the other, so it is not an actor identity
and nothing should key an actor on it. The slot is the cross-message key, and only within a stated
space. A search for all 25 distinct id values at every qword offset of every `SimStateEvent`,
`PlayerStateMsg` and `ComponentEvent` (1,004,765 offsets over 20,165 bodies) finds 0; a planted value is
found first to prove the search works.

`SR15NetComponentEvent` is `[16 B actor GUID][u16 slot][u16 kind][u32 -1]`. Its slot takes only the
values 27 and 47 across all 3,616 instances, both inside the 27…47 space. That is consistent with the
slot being the actor key, but not proof.

### `SNSLobbyOwnerState`

`[wire]` 2 bodies. `[u8 tag][UTF-8 JSON][NUL]`. The tag byte is skipped: the capture has `0x25`, a copy
inside a batch event has `0xC0`, and a working host sends `0x00`. The JSON carries `gametype`,
`starttime`, `level`, `battlepassstatgroup`, `team[]` (per-team `alignment`) and `user[]` (per-entrant
`team` / `teamslot`).

### `SR15NetReliableStatUpdate` / `SR15NetReliableTeamStatUpdate`

- `StatUpdate` is `[u32 entrant][JSON][NUL]`. All 19 instances are 7 bytes: entrant 0 with `{}`.
- `TeamStatUpdate` is `[u8 team][u8 pad][n × 20 B row]`. All 38 instances are 242 B (2 + 12×20), sent in
  team-0 / team-1 pairs.
- The first 8 bytes of each row are a `CSymbol64` stat name (same names, same order in both teams). The
  other 12 bytes are zero in all 456 rows, so the row is not split further.

### `SR15NetTouchStateEvent`: framing only

613 instances, all 16 B, all client to server: `[u32 idspace-lo][u32 0][u32 counter][u32 0]`, the
counter going up by 1 each time. Nothing is claimed about what a touch is.

## What the recorder taps

The recorder's tap list is in `src/capture/manifestbuild.cpp`. Symbol ids and counts from the reference
capture:

| Name | `CMatSym::TypeId` | Instances | Body sizes |
|---|---|---|---|
| `SR15NetPlayerStateMsg` | `C6BB60E8D9E9A3D3` | 5,520 | 64 |
| `SR15NetPhysicsSimStateEvent` | `8F62ADBA23F15FE0` | 11,029 | 64 |
| `SR15NetReplicatedDeltaUnreliableVars` | `2DED551BABC46DBF` | 14,796 | 24 … 926 |
| `SR15NetComponentEvent` | `6E54D0D298BFFB17` | 3,616 | 24 |
| `SR15NetTouchStateEvent` | `6D9A367EE21A5642` | 613 | 16 |
| `SR15NetTouchOwnershipGranted` | `84745D47A4B31F27` | 0 | |
| `SR15NetTouchOwnershipTransferMailbox` | `60FD8780FE4017F5` | 0 | |
| `SR15NetTouchOwnershipTransferReliable` | `6F248C06A631D7CB` | 0 | |
| `SR15NetTouchOwnershipDeclined` | `8255A3B48E69FD70` | 0 | |
| `SR15NetPhysicsOwnershipTransfer` | `E45B3A91A64B201C` | 0 | |
| `SR15NetSetOwner` | `4A2953DC4D5FBAED` | 0 | |
| `SR15NetReliablePostScoreUpdate` | `B9F8831AB6E068A9` | 0 | |
| `SR15NetCancelGoal` | `6AF7784293E2EDAB` | 0 | |
| `SR15StunMsg` | `1FEB495968559BC3` | 0 | |
| `SR15StunPlayerMsg` | `CCD9ED2D3125FF67` | 0 | |
| `SR15RemoveStun` | `0D82D66C7EBDC8F5` | 0 | |
| `SR15NetPlayEmote` | `7C63C5877B0EEE97` | 0 | |
| `SR15NetPlayerPausedMsg` | `E9426CF2F6CFB73C` | 0 | |
| `SR15NetReliableStatUpdate` | `5D84B0C2D4BB6CFC` | 19 | 7 |
| `SR15NetReliableTeamStatUpdate` | `7BA444F7D419B1A2` | 38 | 242 |
| `SR15NetUnreliableStatUpdate` | `3FE43AFC2A73C712` | | |
| `SR15NetServerFrameMsg` | `C57EC38913BE8582` | 15,039 | 16 |
| `SNSLobbyOwnerState` | `2068C0F03B4B8F25` | 2 | 233, 241 |
| `SNSLobbyEntrantStatev2` | `EB6218834B3FA648` | 4 | 360 … 400 |
| `SNSLobbyEntrantRemoved` | `39156563932117D6` | 0 | |
| `SNSLobbyEntrantTeamChanged` | `CEB8759C36184B82` | 1 | 16 |
| `SNSLobbyEntrantDataChanged` | `8F05B6735E916658` | | |
| `SR15NetIdsResponse` | `8F8AF081AF21CBE7` | 2 | 218, 278 |
| `SR15NetCurrentLoadoutResponse` | `A52712EC66B2DB48` | 1 | 452 |
| `SR15NetPlayerEquipmentUpdate` | `71C59625B07711B9` | 1 | 56 |
| `SR15NetItemEquippedNotification` | `ED0FBA849BADC7BF` | | |
| `SR15NetLoadoutNumberNotification` | `80AF66BF1E67B474` | 1 | 12 |
| `SR15NetNewUnlocks` | `187008FE3940A327` | | |
| `SR15NetBalanceSettingsMassUpdate` | `E9A4B52CBA8B27E7` | 1 | 3,334 |
| `SNSRefreshProfileFromServer` | `06877B856E8D785F` | 0 | |
| `SR15NetServerInfo` | `FA563AE00EBFF2BA` | 1 | 96 |
| `SNSLobbyUpdatePings` | `03E01D2BD46A86C0` | 125 | 33 |

A blank count means the tap was added after the reference capture was analysed.

**A name that does not exist.** `CMatSym::TypeId` returns an ordinary-looking symbol for any string,
so a tap on a name the game never sends registers fine and never fires. `SR15NetTouchOwnershipTransfer`
is such a name: it is not in the image. The real ownership messages are the Mailbox / Reliable /
Declined variants, `SR15NetPhysicsOwnershipTransfer` and `SR15NetSetOwner`, all tapped now. The
non-existent name is still in the list and does nothing.

**Not tapped:** `SNSNewUnlocksNotification` (`AE1131A24763F2C8`).

**`SNSRefreshProfileFromServer` with 0 instances means nothing.** `[lib]` it is
`[16 B SNSUserID][encoded profile]`, and the receiver drops any profile that is not its own. In the
reference capture the client fetched its profile over the login-service connection, which the capture
does not include. The tap sits on the engine's broadcaster, downstream of both.

## Where the event classes live

| Class | Carrier | Status |
|---|---|---|
| Component events | `SR15NetComponentEvent` | Confirmed; spawn/position transforms only. 3,616 instances, all 24 B, kind 5 on 3,614 and 13 on 2 |
| Touch | `SR15NetTouchStateEvent` | Carrier found; framing only |
| Ownership | the touch/physics ownership messages above | Names found; 0 instances (one player, no contested grabs) |
| Stun | `SR15StunMsg` / `SR15StunPlayerMsg` / `SR15RemoveStun` | Names found; 0 instances |
| Score | `SR15NetReliablePostScoreUpdate`, `SR15NetCancelGoal` | Names found; 0 instances |
| Emote | `SR15NetPlayEmote` | Name found; 0 instances |
| Team change | `SNSLobbyEntrantTeamChanged` | Closed |
| Join / leave | `SNSLobbyEntrantStatev2` / `SNSLobbyEntrantRemoved` | Closed |
| Stat updates | `SR15NetReliableStatUpdate` / `…TeamStatUpdate` / `SR15NetUnreliableStatUpdate` | Framing closed; content open |

## Re-running the checks

```
tools\build.ps1
build\bin\events_selftest.exe     decoders over the committed fixtures
build\bin\events_corpus.exe       the same decoders over a decoded capture
```

`events_corpus` needs a capture: pass its path, or set `REPLAY_EVENT_CORPUS` or `REPLAY_FRESHCAP_DIR`.
Without one it prints `NOT RUN`. The reference capture is private.

Both have been seen to fail on purpose: changing the display-name offset from 60 to 61 breaks 7
selftest assertions, and changing the id entry length from 10 to 5 breaks the corpus run.
