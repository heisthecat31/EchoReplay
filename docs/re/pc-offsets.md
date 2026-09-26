# PC offsets: checking Quest-derived numbers against PC code

**Summary.** Many structure offsets and strides were first read from the Quest build (ARM64), which
keeps its symbols. Some differ on PC. This note checks 21 of them against PC code and explains the
test used. It does not cover every number the plugin uses.

## Why

The physics pool stride was carried over from the Quest build as `520`. On PC it is `504`. The error
was silent: a wrong stride reads the wrong entity, it does not crash. So the rule is that any number
taken from the Quest build (strides, offsets, sizes, counts, masks, not just addresses) is unusable on
PC until it is re-derived from PC code. Each registry row records which architecture its value came
from.

## The test: look for the rival

Counting how often a value appears in `.text` does not work. `504` occurs 2,199 times, which would
mark the correct value "unverifiable". What settles it is the rival: at the PC site that uses the
field, does the PC value appear in the expected encoding while the Quest value does not?

```
PHYSCI.STRIDE  @ sub_140D4FFC0 (SimStateCB)             504: imul-imm32 hits = 1   520: hits = 0
PHYSCI.STRIDE  @ sub_140D7DA20 (UpdInterpPlayerStates)  504: imul-imm32 hits = 1   520: hits = 0
```

Two sites, decisive both times.

## Verdicts

| Verdict | Meaning | Trust on PC? |
|---|---|---|
| `PC-DECISIVE` | PC value present at the site in the expected encoding, and the Quest rival absent | yes |
| `PC-CORROBORATED` | present at the site, no rival recorded, and the value is rare in `.text` (<200 hits) | yes |
| `PC-PRESENT` | present at the site, but the value is common, so another number could also fit | no |
| `PC-ABSENT` | not present at the site in the expected encoding | no, refuse |
| `CITATION-WRONG` | present in `.text`, but not at the cited site | fix the citation first |

`CITATION-WRONG` exists because a wrong citation used to produce a confident refusal. "The value is not
there" and "you looked in the wrong place" are different problems with different fixes.

## Results

```
fact                   PC value kind    @site  global  rival  verdict
PHYSCI.STRIDE          504      stride  1      2199    520:0  PC-DECISIVE
PHYSCI.STRIDE_2        504      stride  1      2199    520:0  PC-DECISIVE
PLAYERCI.STRIDE        832      stride  3      781     -      PC-PRESENT
PHYSCI.HISTCOUNT       352      offset  5      3305    -      PC-PRESENT
PHYSCI.HISTLIST        360      offset  2      2108    -      PC-PRESENT
PHYSCI.TIMEOFFSET      292      offset  2      665     -      PC-PRESENT
PLAYERCI.HISTLIST      664      offset  3      739     -      PC-PRESENT
PLAYERCI.HISTCOUNT     672      offset  0      894     -      CITATION-WRONG
PLAYERCI.POSE_RENDER   168      offset  1      7025    -      PC-PRESENT
PLAYERCI.POSE_PHYS     320      offset  4      3441    -      PC-PRESENT
NETGAME.PEERTIME       11688    offset  1      29      -      PC-CORROBORATED
NETGAME.SRVTIME        344432   offset  3      7       -      PC-CORROBORATED
NETGAME.SRVSLEW        344440   offset  5      7       -      PC-CORROBORATED
NETGAME.PEERSLOT       403848   offset  1      8       -      PC-CORROBORATED
CLOCK.MAXELAPSED       66666    imm     1      1       -      PC-CORROBORATED
USER.STRIDE            592      stride  4      1232    -      PC-PRESENT
PROFILE.STRIDE         19720    stride  1      78      -      PC-CORROBORATED
DISPLAY.BASE           406872   offset  1      9       -      PC-CORROBORATED
PROVIDER.SLOT_A        34048    offset  1      7       -      PC-CORROBORATED
PROVIDER.SLOT_C        34064    offset  1      7       -      PC-CORROBORATED
NETGAME.PTR            34072    offset  2      35      -      PC-CORROBORATED

checked 21 facts: 2 DECISIVE, 10 CORROBORATED, 8 PRESENT-only, 1 CITATION-WRONG, 0 ABSENT
```

`PC-PRESENT` rows stay marked as Quest-derived until someone re-derives them from the decompiler and
records the Quest value as a rival.

## Notes on specific rows

| Fact | Note |
|---|---|
| `PLAYERCI.HISTCOUNT` (672) | No displacement hit at `sub_140D7DA20`. The player pool pairs list `+664` / count `+672`, while the physics pool orders the pair the other way (count `+352` / list `+360`). Both the value and the role need a PC read. |
| `PROFILE.STRIDE` (19720) | First reported absent. The value is present once at `0x14016DD3A`; the cited search window was 161,850 bytes short of the real site. |
| `PLAYERCI.STRIDE` (832) | Reproduces on PC (3 `imul` hits at `sub_140D7DA20`) while the physics stride did not. One confirmation says nothing about the next fact. |

## Coverage

These 21 are the values that index a pool, address the playhead, or select a provider slot, where a
wrong value fails silently per entity. Other numbers the plugin uses are not checked by this note;
see the evidence columns in `facts/facts.tsv` for those.
