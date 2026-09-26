#!/usr/bin/env python3
"""Is six bytes really all of ReceiveVoip's packet header that matters?

The S_VOIP replay path hands the engine a stub header with a sequence word at +4 and zeroes
everywhere else. That is only correct if VOIP.RECEIVE dereferences its third argument at no other
displacement, and this is the check that says so.

Two arms, and they answer different questions:

  BYTES   always runs. Reads echovr.exe and asserts the six known read sites are byte-exact. That
          proves the sites are where we think they are; it does NOT prove there are no others.
  SWEEP   runs only when the IDA extraction is present. Enumerates every instruction in the
          function and asserts that the complete set of displacements read off the register holding
          argument three is exactly {4}. That is the half that proves the negative, and it reports
          NOT RUN naming the path when the extraction is absent rather than passing without it.

  passes on   the shipped binary
  fails on    --mutate, which plants a read at +8 into the sweep's instruction list and requires the
              check to reject it
"""
import argparse
import hashlib
import os
import struct
import sys

MD5 = "caa05ee3bca2f5df0809994e93e2cc05"
IMAGE_BASE = 0x140000000
FUNC_VA = 0x140D376D0          # VOIP.RECEIVE
FUNC_END_VA = 0x140D37B80      # past the last instruction the extraction lists
ARG3_REG = "r13"               # mov r13, r8 at 0x140D376EA
EXPECTED_DISP = {4}

# The six sites, and the encoding at each. movzx <r32>, word ptr [r13+4].
SITES = [
    (0x140D377E3, "410FB74504"),
    (0x140D3782D, "410FB74504"),
    (0x140D3787A, "410FB74D04"),
    (0x140D378A7, "450FB74504"),
    (0x140D37AF0, "410FB74504"),
    (0x140D37B0D, "410FB74D04"),
]


def load_sections(raw):
    lfanew = struct.unpack_from("<I", raw, 0x3C)[0]
    nsec = struct.unpack_from("<H", raw, lfanew + 6)[0]
    opt = struct.unpack_from("<H", raw, lfanew + 20)[0]
    base = lfanew + 24 + opt
    out = []
    for i in range(nsec):
        off = base + 40 * i
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", raw, off + 8)
        out.append((vaddr, max(vsize, rsize), rsize, raddr))
    return out


def read_at(raw, sections, va, n):
    rva = va - IMAGE_BASE
    for vaddr, span, rsize, raddr in sections:
        if vaddr <= rva < vaddr + span:
            off = rva - vaddr
            if off + n > rsize:
                return None
            return raw[raddr + off:raddr + off + n]
    return None


def bytes_arm(exe):
    if not os.path.isfile(exe):
        print("NOT RUN: %s" % exe)
        return None
    raw = open(exe, "rb").read()
    got = hashlib.md5(raw).hexdigest()
    if got != MD5:
        print("NOT RUN: %s has md5 %s, not the recorded build" % (exe, got))
        return None
    sections = load_sections(raw)
    bad = 0
    for va, expected in SITES:
        actual = read_at(raw, sections, va, len(expected) // 2)
        if actual is None or actual.hex().upper() != expected:
            print("BYTES FAIL 0x%X: expected %s, read %s"
                  % (va, expected, actual.hex().upper() if actual else "<unreadable>"))
            bad += 1
    if bad:
        return False
    print("BYTES PASS: all %d known reads of arg3 are byte-exact and all are at +4" % len(SITES))
    return True


def sweep_arm(db, mutate):
    if not os.path.exists(db):
        print("NOT RUN: the IDA extraction is not at %s, so the NEGATIVE half of this check "
              "(that no OTHER displacement is read off arg3) was not established" % db)
        return None
    import sqlite3
    con = sqlite3.connect("file:%s?mode=ro&immutable=1" % db.replace("\\", "/"), uri=True)
    rows = list(con.execute(
        "SELECT va, disasm FROM instructions WHERE va >= ? AND va < ? ORDER BY va",
        (FUNC_VA, FUNC_END_VA)))
    if not rows:
        print("NOT RUN: the extraction holds no instructions for 0x%X" % FUNC_VA)
        return None
    if mutate:
        rows.append((FUNC_END_VA - 1, "movzx   eax, word ptr [%s+8]" % ARG3_REG))

    disps = {}
    for va, dis in rows:
        marker = "[%s" % ARG3_REG
        at = dis.find(marker)
        if at < 0:
            continue
        tail = dis[at + len(marker):]
        end = tail.find("]")
        if end < 0:
            continue
        operand = tail[:end].strip()
        if operand == "":
            disps.setdefault(0, []).append(va)
            continue
        if operand.startswith("+"):
            value = operand[1:].strip().rstrip("h")
            try:
                disps.setdefault(int(value, 16), []).append(va)
            except ValueError:
                disps.setdefault(operand, []).append(va)
        else:
            disps.setdefault(operand, []).append(va)

    found = set(disps.keys())
    print("SWEEP: %d instructions in VOIP.RECEIVE; displacements read off %s: %s"
          % (len(rows), ARG3_REG, sorted(str(d) for d in found)))
    if found != EXPECTED_DISP:
        print("SWEEP FAIL: the replay header stub is six bytes because +4 is the only "
              "displacement read; this build reads %s" % sorted(str(d) for d in found))
        return False
    print("SWEEP PASS: +4 is the only displacement read off arg3, at %d sites"
          % len(disps[4]))
    return True


def main():
    ap = argparse.ArgumentParser()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
    from echovr_path import find_echovr_exe, optional_input
    ap.add_argument("--exe", default=find_echovr_exe() or "(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)")
    ap.add_argument("--db", default=optional_input("REPLAY_IDA_ECHOVR_DB")
                    or "(IDA database not configured - set REPLAY_IDA_ECHOVR_DB)")
    ap.add_argument("--mutate", action="store_true")
    args = ap.parse_args()

    b = bytes_arm(args.exe)
    s = sweep_arm(args.db, args.mutate)

    if args.mutate:
        # The planted read at +8 must be rejected. A mutation that survives means the sweep is not
        # looking at what it claims to look at.
        if s is None:
            print("MUTATION NOT RUN: no extraction to plant into")
            return 2
        if s is False:
            print("MUTATION CAUGHT")
            return 0
        print("MUTATION SURVIVED: a read at +8 did not fail the sweep")
        return 1

    if b is False or s is False:
        return 1
    if b is None:
        return 2
    if s is None:
        print("PARTIAL: the byte arm passed; the negative half is NOT RUN")
        return 0
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
