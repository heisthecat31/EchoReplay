# Reverse-engineering notes

Research notes on how the PC build of Echo VR (`echovr.exe` MD5 `caa05ee3bca2f5df0809994e93e2cc05`)
does things the plugin depends on. They are dense and full of addresses. For how the plugin itself
works, read [HOW-IT-WORKS.md](../HOW-IT-WORKS.md).

| Note | Question it answers |
|---|---|
| [message-coverage.md](message-coverage.md) | Which network message types an arena match sends, which ones the recorder captures, and where player identity and team changes live on the wire. |
| [world-reorigin.md](world-reorigin.md) | Where the engine keeps its world re-origin shift, its layout, and what writes it. |
| [as-seen-poses.md](as-seen-poses.md) | How the engine's two per-player pose slots are filled and read, and why "as seen" playback can be done by sampling each player at their own delay. |
| [pc-offsets.md](pc-offsets.md) | A cross-check of structure offsets and strides from the Quest build against the PC code (they differ). Covers 21 values. |

## Conventions

- Addresses are virtual addresses at the image's preferred base `0x140000000`. Subtract the base to get
  the RVA.
- `sub_XXXXXXXX` names are disassembler default names for PC functions. Real C++ names come from the
  Quest build (`libr15.so`), which kept its symbols, and are matched to PC functions by strings and
  structure.
- `[V]` means read from the disassembly bytes. `[I]` means inferred from decompiled pseudocode. `[U]`
  means it needs the game running to confirm.
- Numbers from the Quest build are never trusted on PC until they are re-derived from PC code.

The values the plugin actually uses are in `facts/facts.tsv`, with the evidence for each one.
