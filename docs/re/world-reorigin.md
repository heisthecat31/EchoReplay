# World re-origin shift

**Summary.** Echo VR recentres its world when an object drifts more than 100 m from the origin. The
running total of those shifts is a float3 at `+0x2A98` in the `CGParticleEffectRenderer` singleton,
reached through the pointer `qword_1420A81C8`. It is written only by `CncaGame::UpdateEndTranslation`
and never reset. The recorder stores it per keyframe so recorded positions can be mapped to world
positions.

Sources: `echovr.exe` (MD5 `caa05ee3bca2f5df0809994e93e2cc05`) read byte for byte, a disassembler
database of the same file, and a database of the Quest build `libr15.so` (arm64, keeps mangled C++
symbols) for names. Image base `0x140000000`; the image is relocatable, so add the RVA to
`GetModuleHandleW(nullptr)`.

## Answer

| Question | Answer | Confidence |
|---|---|---|
| Base | `qword_1420A81C8` (RVA `0x20A81C8`): a **pointer** to the `NRadEngine::CGParticleEffectRenderer` singleton | verified on PC; class name from Quest strings |
| Layout | Two contiguous float3s. **Cumulative** at `+0x2A98 / 0x2A9C / 0x2AA0` (10904 / 10908 / 10912). **Pending** at `+0x2AA4 / 0x2AA8 / 0x2AAC` (10916 / 10920 / 10924) | verified on PC |
| Writer | `sub_140517E30` = `NRadEngine::CncaGame::UpdateEndTranslation`, once per re-origin, dispatched from function-pointer tables. Both triples are `+=` a per-event delta. The cumulative one is never reset; the pending one is zeroed by `CGParticleEffectRenderer::ComputeUpdate` | verified on PC; name from Quest |

Not the shift: `0x1420A27C0` is `NRadEngine::C3Vector::zero`, a constant `(0,0,0)` that sits next to
the real value in one render path (see below). Recording it would claim the world never moved.

## The writer

`sub_140517E30`, RVA `0x517E30`. Its first 20 bytes, read from the file:

```
40 53 48 83 EC 20 8B 81 28 08 00 00 48 8B D9 48 C1 E8 09 A8
```

Decompiled:

```c
unsigned __int64 __fastcall sub_140517E30(__int64 a1)          // a1 = CncaGame *this
{
  result = (unsigned __int64)*(unsigned int *)(a1 + 2088) >> 9;
  if ( (*(_DWORD *)(a1 + 2088) & 0x200LL) != 0 )               // CncaGame+0x828 bit 9
  {
    sub_140691FB0(*((_QWORD *)qword_1420A00F0 + 1) + 7704LL);   // CPhZone::EndTranslateContents
    // for each game space: sub_140515D00(space, a1 + 2076)
    // for each renderer camera: sub_1405C39E0(qword_1420A3248, i, 2)   // camera cut

    v9 = qword_1420A81C8;
    *(float *)(v9 + 10916) += *(float *)(a1 + 2076);   // pending
    *(float *)(v9 + 10920) += *(float *)(a1 + 2080);
    *(float *)(v9 + 10924) += *(float *)(a1 + 2084);
    *(float *)(v9 + 10904) += *(float *)(a1 + 2076);   // cumulative
    *(float *)(v9 + 10908) += *(float *)(a1 + 2080);
    *(float *)(v9 + 10912) += *(float *)(a1 + 2084);

    *(_QWORD *)(a1 + 2088) &= ~0x200uLL;
  }
  return result;
}
```

The cumulative block in the disassembly:

```
0x140517F98  488B052902B901   mov     rax, cs:qword_1420A81C8
0x140517F9F  F30F10831C080000 movss   xmm0, dword ptr [rbx+81Ch]     ; delta.x
0x140517FA7  F30F5880982A0000 addss   xmm0, dword ptr [rax+2A98h]
0x140517FAF  F30F1180982A0000 movss   dword ptr [rax+2A98h], xmm0    ; cumulative.x
0x140517FB7  F30F108B20080000 movss   xmm1, dword ptr [rbx+820h]     ; delta.y
0x140517FBF  F30F58889C2A0000 addss   xmm1, dword ptr [rax+2A9Ch]
0x140517FC7  F30F11889C2A0000 movss   dword ptr [rax+2A9Ch], xmm1    ; cumulative.y
0x140517FCF  F30F108324080000 movss   xmm0, dword ptr [rbx+824h]     ; delta.z
0x140517FD7  F30F5880A02A0000 addss   xmm0, dword ptr [rax+2AA0h]
0x140517FDF  F30F1180A02A0000 movss   dword ptr [rax+2AA0h], xmm0    ; cumulative.z
0x140517FE7  4881A328080000FFFDFFFF   and qword ptr [rbx+828h], 0FFFFFFFFFFFFFDFFh
```

The pending block (`0x140517F44 … 0x140517F90`) is the same shape on `+0x2AA4 / 0x2AA8 / 0x2AAC`. All
twelve float encodings were also found by byte search in `echovr.exe` itself. The RIP-relative loads
decode to the same global:

```
0x140517F44: 48 8B 05 7D 02 B9 01    -> 0x140517F4B + 0x01B9027D = 0x1420A81C8
0x140517F98: 48 8B 05 29 02 B9 01    -> 0x140517F9F + 0x01B90229 = 0x1420A81C8
0x140472870: 48 83 3D 50 59 C3 01 00 -> 0x140472878 + 0x01C35950 = 0x1420A81C8
```

`sub_140517E30` has no direct callers, only seven data references from function-pointer tables. It is
a registered update task, as `CncaGame::UpdateEndTranslation` is in the Quest build.

## What `qword_1420A81C8` points to

Only two instructions store to it: `0x1410325F7` at boot (`rbp = rdi + 0x289D8`, a sub-object inside
the render engine, initialised by `sub_140581AF0`) and `0x141030FA3` at shutdown (null). So it is a
pointer to a sub-object, published as a singleton.

The class name: PC `sub_140555D20` contains the string `"Particle graph '%s' is missing an 'Update'
graph"`. In the Quest build that string is used only by `CGParticleEffectRenderer::ComputeUpdate`,
`ComputeWarmUps` and `ComputeEmission`. PC `sub_140555D20` reads `+0x2A98` and `+0x2AA4` off the same
object that `sub_1405ADC70` loads from `qword_1420A81C8`.

## Which triple is which

`sub_140555D20` (`ComputeUpdate`) copies both triples into a GPU constant block, then zeroes only the
pending one:

```
0x1405561D3  movsd  xmm1, qword ptr [rsi+2A98h]    ; slot A <- cumulative.xy
0x1405561E3  mov    eax, [rsi+2AA0h]               ; slot A <- cumulative.z
0x1405561EF  movsd  xmm0, qword ptr [rsi+2AA4h]    ; slot B <- pending.xy
0x1405561FF  mov    eax, [rsi+2AACh]               ; slot B <- pending.z
...
0x1405565E0  movsd  qword ptr [rsi+2AA4h], xmm0    ; pending.xy = 0
0x1405565EC  mov    [rsi+2AACh], eax               ; pending.z = 0
```

The warm-up sibling `sub_140557C90` fills the same two slots, but puts `C3Vector::zero` in the pending
slot:

```
0x14055815A  movsd  xmm1, qword ptr [rsi+2A98h]              ; slot A: cumulative
0x140558176  movsd  xmm0, qword ptr cs:dword_1420A27C0       ; slot B: C3Vector::zero
```

`sub_140554D50` runs this path only when the pending triple is non-zero (`ucomiss` on all three), and
`sub_1405E29A0` reads the pending triple to shift live particles.

A scan of every instruction in the image for these displacements finds exactly two contiguous 12-byte
float runs, `0x2A98..0x2AA3` and `0x2AA4..0x2AAF`. The cumulative triple has one writer
(`sub_140517E30`), no `memset` or zero store targets it, and `+0x2A9C` is referenced nowhere else.
It is never reset, so a recording that starts mid-session sees a non-zero value on frame 0 if the
world has already moved.

## `0x1420A27C0` is `C3Vector::zero`

- 1,187 reads and 0 writes across the whole image.
- It lies in the uninitialised tail of `.data` (raw data ends at RVA `0x2098E00`), so it is
  zero-filled by the loader and stays `(0,0,0)`.
- PC `sub_140515C90` compares its argument against it; the Quest twin `CncaGame::Translate` compares
  against `NRadEngine::C3Vector::zero` (Quest `0x376E354`).

## The full chain

1. **Request.** `sub_140515C90` = `CncaGame::Translate(const C3Vector&)`. If the shift differs from
   zero by more than `9.5367431640625e-07`, it stores the delta at `CncaGame + 0x81C / 0x820 / 0x824`
   and sets bit `0x100` of the flag qword at `CncaGame + 0x828`. It is the only place that sets that
   bit. All four callers pass the negated position: "move the world so this point becomes the origin".
2. **Trigger.** `sub_140127A60` computes `max(|x|, |y|, |z|)` and calls `Translate(-p)` when it exceeds
   `dword_1416CD9E4`, which reads `42C80000` = `100.0f` from the file.
3. **Latch.** In `CncaGame::RunLoop` (`sub_14050BD00`, with `UpdateBeginTranslation` inlined): if bit
   `0x100` is set, bit `0x4` is clear and no loads are pending, it calls
   `CPhZone::BeginTranslateContents(delta)`, then `btr 8` / `bts 9` at `0x14050BF05` turns `0x100` into
   `0x200`.
4. **Apply.** `CncaGame::UpdateEndTranslation` (above) sees `0x200`, translates every game space,
   cuts every camera, accumulates the delta into both triples, and clears `0x200`.

The Quest build's `CncaGame` members sit uniformly 8 bytes lower than PC's across both functions
(`+0x820` vs `+0x828`, `+0x814` vs `+0x81C`, and so on), which supports matching the two functions.

## What the Quest build can and cannot confirm

The Quest revision does not have this feature: its `ComputeUpdate` writes zeros where PC writes the two
triples, and its `UpdateEndTranslation` has no float accumulation. So the offsets `0x2A98` / `0x2AA4`
are PC-only. The Quest build does have `CncaGame::WorldOffset()`, which returns one contiguous float3
from a singleton, confirming the concept but not the numbers.

## How the recorder uses it

Once per keyframe:

```
base = *(uintptr_t*)(module_base + 0x20A81C8)   // a pointer: dereference it
if (base == 0) -> not booted yet, field absent
offset.x = *(float*)(base + 0x2A98)
offset.y = *(float*)(base + 0x2A9C)
offset.z = *(float*)(base + 0x2AA0)
```

- Store the absolute value; the engine already accumulates. A chapter with no re-origin costs 12
  constant bytes that compress to almost nothing.
- Do not record the pending triple: it is zeroed on the next particle pass, so it reads `(0,0,0)`
  almost always.
- At playback, a recorded world position maps as `p_play = p_rec + (offset_play - offset_rec)`.
- The three floats are written without a fence on another thread, so a read can in theory see a
  half-updated triple on the one frame a re-origin happens. Bit `0x200` of `CncaGame + 0x828` marks that
  window.
- Whether arena play ever crosses 100 m is a runtime question. Recording the offset means it does not
  need an answer.

The registry rows are `WORLD.REORIGIN_GLOBAL`, `WORLD.REORIGIN_OFF_X/Y/Z`,
`WORLD.REORIGIN_PENDING_OFF_X/Y/Z`, `WORLD.REORIGIN_FN` and `WORLD.REORIGIN_FLAG_OFF` in
`facts/facts.tsv`.
