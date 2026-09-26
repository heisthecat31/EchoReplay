# The `.echodemo` format

An overview for reading the files. The authoritative definition is
`src/foundation/demofile/format.h` and `demotypes.h`. All integers are little-endian.

```
+----------------------+  offset 0
| file header (128 B)  |
+----------------------+
| chapter: MANIFEST    |
| chapter: CORE        |  one per ~2 s of match time
| chapter: ASSEEN      |  optional sections run alongside CORE
| chapter: VOIP        |
| chapter: CORE        |
| ...                  |
+----------------------+  trailer offset
| chapter: INDEX       |  one entry per chapter + a copy of the manifest
| footer (48 B)        |
+----------------------+
```

## File header (128 bytes)

| Field | Notes |
|---|---|
| magic | `ECHODEMO` |
| format version | major.minor, currently 1.0 |
| header CRC32 | over the header minus this field |
| flags | open, has voice, has as-seen, has oracle, imported (converted from a legacy file), truncated |
| trailer offset, first chapter offset | byte offsets |
| start time | microseconds |
| demo UUID | 16 bytes |
| `echovr.exe` MD5 | the game build it was recorded with |
| build hash, content hash, capture role, writer version, authority flags | |

The header is rewritten once, when the file is closed. A file that was never closed cleanly (game
crash) still has the "open" flag and is recovered by scanning chapters; a torn end loses at most one
chapter.

## Chapters

Every chapter starts with a 48-byte head:

| Field | Notes |
|---|---|
| tag | `CHPT` |
| section | 0 manifest, 1 core, 2 as-seen, 3 voice, 4 oracle, 5 index |
| codec | 0 raw, 1 zstd (level 3, with checksum; stored raw if compression does not help) |
| payload length, payload CRC32 | |
| begin / end time | microseconds |
| sequence number, uncompressed length, stream count | |
| head CRC32 | |

A payload is at most 1 MiB. Inside, it is a list of streams, each with a 16-byte block header:
stream id, schema id, byte length, and two record counts (records the engine produced vs. records
stored), so any loss is visible in the file itself.

| Section | Holds |
|---|---|
| MANIFEST | Schemas, players, round table, level, game mode, start time, keyframe interval. |
| CORE | Tick, players, bodies (incl. disc), networked variables, events, world, keyframes, round markers, game state, player stats. |
| ASSEEN | Per-player timing needed to play a match as each player saw it. |
| VOIP | Encoded voice frames. Remove with `echodemo strip --voip`. |
| ORACLE | Validation snapshots (currently empty). |
| INDEX | The trailer. |

## Trailer

The INDEX chapter holds one 32-byte entry per chapter (begin/end time, offset, length, section,
sequence) and a copy of the manifest. It is followed by a 48-byte footer: `ECHOFTR!`, trailer offset,
end time, total size, chapter count, flags, CRC32.

Because the whole index must fit in one 1 MiB payload, a file holds at most 32,767 chapters, about
4.5 hours of recording. The writer stops cleanly at that limit.

## Tools

`echodemo info`, `verify`, `dump` and `rows` read the format from the command line; see
[BUILD.md](BUILD.md#the-echodemo-tool).
