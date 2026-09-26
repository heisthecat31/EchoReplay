"""Corroborate a numeric fact against PC code, so libr15-sourced numbers cannot be trusted blind.

For each fact: scan a window of .text at a citing PC function and look for the value as
  - an imul imm32   (69 /r id, 48 69 /r id)   -> a STRIDE
  - a disp32 in a modrm memory operand         -> an OFFSET
  - any 4-byte immediate                       -> weak corroboration only

Three verdicts, and the middle one is the point:
  PC-CONFIRMED  the value appears in PC code at a citing site, in the expected encoding
  PC-ABSENT     it does not, and a rival candidate may - REFUSE until W0-2 re-derives
  UNCHECKABLE   this method cannot see it (small imm8 offsets, computed values)

Absence of a hit is not proof of absence; it is a refusal to trust, which is the correct default
for a number whose only provenance is an ARM64 read.
"""
import struct
import sys

import os  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from echovr_path import NOT_FOUND, find_echovr_exe  # noqa: E402

EXE = find_echovr_exe() or sys.exit(NOT_FOUND)
d = open(EXE, "rb").read()
_e = struct.unpack_from("<I", d, 0x3C)[0]
_coff = _e + 4
_nsec, = struct.unpack_from("<H", d, _coff + 2)
_szopt, = struct.unpack_from("<H", d, _coff + 16)
_opt = _coff + 20
BASE = struct.unpack_from("<Q", d, _opt + 24)[0]
_sec = _opt + _szopt
SECS = []
for _i in range(_nsec):
    _o = _sec + 40 * _i
    _vs, _va, _rs, _pr = struct.unpack_from("<IIII", d, _o + 8)
    SECS.append((d[_o:_o + 8].rstrip(b"\0").decode("latin1"), _va, _vs, _pr, _rs))


def off_of(va):
    r = va - BASE
    for _nm, sva, vs, pr, rs in SECS:
        if sva <= r < sva + max(vs, rs):
            return pr + (r - sva)
    return None


def find(va, span, val):
    """returns (imul_hits, disp32_hits, raw_hits)"""
    o = off_of(va)
    if o is None:
        return (0, 0, 0)
    b = d[o:o + span]
    pat = struct.pack("<I", val & 0xFFFFFFFF)
    raw = [i for i in range(len(b) - 3) if b[i:i + 4] == pat]
    imul = [i for i in raw if i >= 3 and (b[i - 3] == 0x69 or b[i - 2] == 0x69)]
    # disp32 modrm: mod=10 (0b10xxxxxx) byte immediately before the 4-byte displacement,
    # optionally preceded by a SIB. Accept either shape.
    disp = [i for i in raw
            if (i >= 1 and 0x80 <= b[i - 1] <= 0xBF)
            or (i >= 2 and 0x80 <= b[i - 2] <= 0xBF)]
    return (len(imul), len(disp), len(raw))


# fact, citing PC function, window, expected value, rival (usually the ARM value), kind
FACTS = [
    ("PHYSCI.STRIDE",        0x140D4FFC0, 0x1200, 504,    520,  "stride"),
    ("PHYSCI.STRIDE_2",      0x140D7DA20, 0x0900, 504,    520,  "stride"),
    ("PLAYERCI.STRIDE",      0x140D7DA20, 0x0900, 832,    None, "stride"),
    ("PHYSCI.HISTCOUNT",     0x140D4FFC0, 0x1200, 352,    None, "offset"),
    ("PHYSCI.HISTLIST",      0x140D4FFC0, 0x1200, 360,    None, "offset"),
    ("PHYSCI.TIMEOFFSET",    0x140D7DA20, 0x0900, 292,    None, "offset"),
    ("PLAYERCI.HISTLIST",    0x140D7DA20, 0x0900, 664,    None, "offset"),
    ("PLAYERCI.HISTCOUNT",   0x140D7DA20, 0x0900, 672,    None, "offset"),
    ("PLAYERCI.POSE_RENDER", 0x140D7DA20, 0x0900, 168,    None, "offset"),
    ("PLAYERCI.POSE_PHYS",   0x140D7DA20, 0x0900, 320,    None, "offset"),
    ("NETGAME.PEERTIME",     0x140D7DA20, 0x0900, 11688,  None, "offset"),
    ("NETGAME.SRVTIME",      0x1401AC280, 0x0200, 344432, None, "offset"),
    ("NETGAME.SRVSLEW",      0x1401AC280, 0x0200, 344440, None, "offset"),
    ("NETGAME.PEERSLOT",     0x140153A00, 0x0200, 403848, None, "offset"),
    ("CLOCK.MAXELAPSED",     0x1404F5870, 0x0400, 66666,  None, "imm"),
    ("USER.STRIDE",          0x1401539A0, 0x0C00, 592,    None, "stride"),
    ("PROFILE.STRIDE",       0x14016DD00, 0x0100, 19720,  None, "stride"),   # adjudicated site
    ("DISPLAY.BASE",         0x140146900, 0x0200, 406872, None, "offset"),
    ("PROVIDER.SLOT_A",      0x140127420, 0x0400, 0x8500, None, "offset"),
    ("PROVIDER.SLOT_C",      0x140127420, 0x0400, 0x8510, None, "offset"),
    ("NETGAME.PTR",          0x140127420, 0x0400, 0x8518, None, "offset"),
]

TXT = [s for s in SECS if s[0] == ".text"][0]
_TB = d[TXT[3]:TXT[3] + max(TXT[2], TXT[4])]


def global_hits(val):
    pat = struct.pack("<I", val & 0xFFFFFFFF)
    return sum(1 for i in range(len(_TB) - 3) if _TB[i:i + 4] == pat)


RARE = 200   # below this, a hit at a cited site is itself strong evidence

# The discriminating test for an ARM-sourced number is NOT how rare it is - it is whether the
# ARM value is ABSENT at a PC site where the PC value is PRESENT. That is what caught B1.
# So every fact lifted from libr15 records its ARM value as the rival, and the gate demands
# the rival miss. Facts where ARM and PC genuinely agree record rival=None and say so.
print("%-22s %-8s %-7s %-6s %-7s %-6s %s"
      % ("fact", "PC value", "kind", "@site", "global", "rival", "verdict"))
dec = corr = pres = absent = cited = 0
for name, va, span, val, rival, kind in FACTS:
    im, ds, rw = find(va, span, val)
    strong = im if kind == "stride" else (ds if kind == "offset" else rw)
    g = global_hits(val)
    rtxt = "-"
    if rival is not None:
        rim, rds, rrw = find(va, span, rival)
        rstrong = rim if kind == "stride" else (rds if kind == "offset" else rrw)
        rtxt = "%d:%d" % (rival, rstrong)
    if not strong:
        # A value absent HERE but present elsewhere in .text is a wrong CITATION, not a missing
        # value. PROFILE.STRIDE was reported PC-ABSENT -> REFUSE because the window in this table
        # was 161,850 bytes short of the real site; the scanner handled the encoding fine. A false
        # refusal is a silent capability loss that reads as rigour, so it gets its own verdict.
        if g:
            verdict = "CITATION-WRONG (present in .text, absent at the cited site)"; cited += 1
        else:
            verdict = "PC-ABSENT -> REFUSE"; absent += 1
    elif rival is not None and rstrong == 0:
        verdict = "PC-DECISIVE (rival absent at site)"; dec += 1
    elif rival is None and g < RARE:
        verdict = "PC-CORROBORATED (rare value at cited site)"; corr += 1
    else:
        verdict = "PC-PRESENT (common value; not proof - W0-2 must re-derive)"; pres += 1
    print("%-22s %-8d %-7s %-6d %-7d %-6s %s" % (name, val, kind, strong, g, rtxt, verdict))
print()
print("checked %d facts: %d DECISIVE, %d CORROBORATED, %d PRESENT-only, %d CITATION-WRONG, %d ABSENT"
      % (len(FACTS), dec, corr, pres, cited, absent))
print()
print("PC-PRESENT is NOT a pass. It means the number is consistent with PC code but a")
print("different number could also be. Those rows stay ARM-provenance until W0-2 re-derives")
print("them from the decompiler and records rival_arm so this tool can reach DECISIVE.")
