#!/usr/bin/env python3
"""Read every engine value the codec depends on back out of echovr.exe.

Two families, and both are read from the shipped binary rather than compared against another document:

  CONSTANTS  the quantisation parameters in src/foundation/bitio/{engineconst,wirefields}.h.  The tables
             carry the .rdata address AND the IEEE-754 bit pattern; this reads the address and fails if
             the four bytes there are not the pattern.  A mistyped digit fails here, not in a match.

  PROLOGUES  the ten PC functions W1-A decompiled.  The design leaves BitPackQuat's encoding [U] and
             requires W1-A to close it by decompiling CBitWriter::BitPackQuat or to refuse the field.  It
             is closed -- and a decompilation is only as durable as the address it was read at, so the
             20 bytes at each are compared against the binary on every gate run.  W0-2's G_PROLOGUE guard
             would do the same for rows merged into facts/facts.tsv; this does it today, and doing it in
             both places is the point rather than duplication.

The design wants values cited, not transcribed.  Where a table cites `facts::NAME`, this
resolves the symbol out of the generated header first, so the citation itself is checked against the
binary -- a citation that resolves to the wrong slot would otherwise be invisible.

The number of registry rows actually merged is REPORTED, not assumed: 12.1 records a containment that
"named an empty set, which is worse than naming none", so "zero merged" has to stay visible.
"""

import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from echovr_path import find_echovr_exe  # noqa: E402

EXE = find_echovr_exe() or ""
MD5 = "caa05ee3bca2f5df0809994e93e2cc05"
IMAGE_BASE = 0x140000000

ENGINE_H = os.path.join(ROOT, "src", "foundation", "bitio", "engineconst.h")
WIRE_H = os.path.join(ROOT, "src", "foundation", "bitio", "wirefields.h")
GENERATED_H = os.path.join(ROOT, "src", "generated", "facts_generated.h")

CONST_ROW = re.compile(r'\{\s*"(?P<id>[A-Z][A-Z0-9_.]*)"\s*,\s*(?P<va>0x[0-9A-Fa-f]+ull|facts::\w+)\s*,\s*'
                       r'(?P<bits>0x[0-9A-Fa-f]+u|facts::\w+)\s*,\s*(?P<site>0x[0-9A-Fa-f]+ull)\s*\}')
SCALAR = re.compile(r"^inline constexpr uint(?:32|64)_t (\w+) = (0x[0-9A-Fa-f]+|\d+)u?(?:ll)?;", re.M)
BLOB = re.compile(r"^inline constexpr uint8_t (\w+)\[\d+\] = \{ ([^}]*)\};", re.M)


# Decode one SSE scalar instruction with a RIP-relative memory operand and return what it addresses.
#
# The length is DERIVED from the encoding, never searched. An earlier version tried lengths 7..10 and
# accepted whichever displacement happened to land on the expected slot -- four chances to agree, so a
# load site shifted by a byte still passed, and the check was demonstrated not to fail when it should.
# F3 [REX] 0F op ModRM disp32, with ModRM mod=00 rm=101, is exactly nine or eight bytes and nothing else.
def rip_target(b, site):
    i = 0
    if not b or b[0] != 0xF3:
        return None, "no F3 prefix"
    i = 1
    if i < len(b) and 0x40 <= b[i] <= 0x4F:
        i += 1
    if i + 2 >= len(b) or b[i] != 0x0F:
        return None, "no 0F escape"
    op = b[i + 1]
    if op not in (0x10, 0x11, 0x58, 0x59, 0x5C, 0x5E):
        return None, "opcode 0x%02X is not a scalar move or arithmetic" % op
    modrm = b[i + 2]
    if modrm & 0xC7 != 0x05:
        return None, "ModRM 0x%02X is not RIP-relative" % modrm
    end = i + 3 + 4
    if end > len(b):
        return None, "instruction runs past the bytes read"
    return site + end + struct.unpack_from("<i", b, i + 3)[0], ""


class Pe:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        self.image_base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
        self.sections = []
        off = pe + 24 + optsz
        for _ in range(nsec):
            name = data[off:off + 8].rstrip(b"\0").decode(errors="replace")
            vsize, vaddr = struct.unpack_from("<II", data, off + 8)
            raw = struct.unpack_from("<I", data, off + 20)[0]
            self.sections.append((name, vaddr, vsize, raw))
            off += 40

    def at_va(self, va, n):
        rva = va - self.image_base
        for name, vaddr, vsize, raw in self.sections:
            if vaddr <= rva < vaddr + vsize:
                return name, self.data[raw + (rva - vaddr):raw + (rva - vaddr) + n]
        return None, None


def load_generated():
    """name -> int for scalars, name -> bytes for prologue blobs."""
    scalars, blobs = {}, {}
    if not os.path.isfile(GENERATED_H):
        return scalars, blobs
    text = open(GENERATED_H, "r", encoding="utf-8").read()
    for name, lit in SCALAR.findall(text):
        scalars[name] = int(lit, 16) if lit.lower().startswith("0x") else int(lit)
    for name, body in BLOB.findall(text):
        blobs[name] = bytes(int(x, 16) for x in body.replace(" ", "").split(",") if x)
    return scalars, blobs


def resolve(token, scalars, problems, where):
    if token.startswith("facts::"):
        name = token[len("facts::"):]
        if name not in scalars:
            problems.append("%s cites facts::%s, which the generated header does not define" % (where, name))
            return None
        return scalars[name]
    return int(token.rstrip("ul"), 16)


def main():
    problems = []
    checked = 0

    if not os.path.isfile(EXE):
        print("RESULT constcheck status=NOT-RUN checked=0 reason=echovr.exe is not on this machine")
        return 0

    data = open(EXE, "rb").read()
    md5 = hashlib.md5(data).hexdigest()
    if md5 != MD5:
        print("RESULT constcheck status=FAIL checked=0 reason=md5 %s, pinned to %s" % (md5, MD5))
        return 1

    pe = Pe(data)
    if pe.image_base != IMAGE_BASE:
        problems.append("image base is 0x%X, expected 0x%X" % (pe.image_base, IMAGE_BASE))

    scalars, blobs = load_generated()

    rows = []
    for path in (ENGINE_H, WIRE_H):
        text = open(path, "r", encoding="utf-8").read()
        for m in CONST_ROW.finditer(text):
            rows.append((os.path.basename(path), m.group("id"), m.group("va"), m.group("bits"),
                         m.group("site")))
    if len(rows) < 20:
        print("RESULT constcheck status=FAIL checked=0 reason=parsed only %d constant rows from the two "
              "headers; the regex no longer matches the tables it guards" % len(rows))
        return 1

    identity = 0
    for where, rid, va_tok, bits_tok, site_tok in rows:
        checked += 1
        va = resolve(va_tok, scalars, problems, "%s:%s" % (where, rid))
        bits = resolve(bits_tok, scalars, problems, "%s:%s" % (where, rid))
        site = int(site_tok.rstrip("ul"), 16)
        if va is None or bits is None:
            continue
        sect, raw = pe.at_va(va, 4)
        if raw is None or len(raw) < 4:
            problems.append("%s: 0x%X is not in any section of echovr.exe" % (rid, va))
            continue
        got = struct.unpack_from("<I", raw, 0)[0]
        if got != bits:
            problems.append("%s: echovr.exe holds 0x%08X at 0x%X, the table says 0x%08X"
                            % (rid, got, va, bits))

        # ★ CONTENTS ARE NOT IDENTITY, and for a float they are nowhere near it. The compiler pools equal
        # literals: 1.0f sits in 0x1416CCEF8 and in a quarter of a million other aligned .rdata slots, and
        # thousands of instructions address that one. Reading the four bytes back proves only that SOME
        # slot holds the value -- a VA mistyped onto another slot with the same float passes it.
        #
        # Identity comes from the instruction that USES the slot. Each row names the load site inside the
        # codec W1-A decompiled; this decodes that instruction's RIP-relative displacement out of the
        # binary and requires it to land on the row's own VA. The instruction length is not assumed: the
        # one length whose displacement resolves to the VA is the answer, and none resolving is a failure.
        isect, ibytes = pe.at_va(site, 12)
        if ibytes is None or isect != ".text":
            problems.append("%s: load site 0x%X is not in .text" % (rid, site))
            continue
        target, why = rip_target(ibytes, site)
        if target is None:
            problems.append("%s: 0x%X is not an SSE scalar load with a RIP operand (%s)" % (rid, site, why))
        elif target != va:
            problems.append("%s: the instruction at 0x%X reads 0x%X, not 0x%X, so the row names a slot "
                            "the codec does not load" % (rid, site, target, va))
        else:
            identity += 1

    # The ten decompiled functions. Absent rows are reported, never silently skipped: a prologue family
    # that shrank to nothing would otherwise look exactly like one that passed.
    prologues = sorted(n for n in blobs if n.startswith("BITIO_") and n.endswith("_PROLOGUE"))
    for name in prologues:
        owner = name[:-len("_PROLOGUE")]
        if owner not in scalars:
            problems.append("%s has no owning address row %s" % (name, owner))
            continue
        checked += 1
        va = scalars[owner]
        sect, raw = pe.at_va(va, len(blobs[name]))
        if raw is None:
            problems.append("%s: 0x%X is not in any section of echovr.exe" % (owner, va))
        elif sect != ".text":
            problems.append("%s: 0x%X is in %s, not .text" % (owner, va, sect))
        elif raw != blobs[name]:
            problems.append("%s: echovr.exe has %s at 0x%X, the registry has %s"
                            % (owner, raw.hex().upper(), va, blobs[name].hex().upper()))

    merged = sum(1 for _w, rid, _v, _b, _s in rows if rid.replace(".", "_") + "_BITS" in scalars
                 or rid.replace(".", "_") in scalars)
    print("CONSTCHECK constants=%d identity_via_load_site=%d registry_rows_merged=%d prologues=%d "
          "(constants not in the registry are checked here only)"
          % (len(rows), identity, merged, len(prologues)))
    if identity < len(rows):
        problems.append("only %d of %d constants were tied to the instruction that loads them; the rest "
                        "are contents-only, which a pooled literal does not establish"
                        % (identity, len(rows)))
    if len(prologues) < 10:
        problems.append("only %d BITIO prologue rows are in the generated header; W1-A decompiled ten, "
                        "and a family that shrank would otherwise pass silently" % len(prologues))
    for p in problems:
        print("FAIL " + p)
    status = "FAIL" if problems else "PASS"
    print("RESULT constcheck status=%s checked=%d failures=%d" % (status, checked, len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
