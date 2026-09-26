#!/usr/bin/env python3
r"""Which echovr.exe addresses do the OTHER plugins in plugins\ resolve, and do any collide with ours?

Written after the 2026-08-28 sweep, where rlay.dll had already hooked a site W1-C guarded and the plugin
refused to load a correct build. Answering "who else is patching what" needed a one-off script; leaving it
as a one-off would mean the next packet re-derives it, so it is a check.

WHAT IT LOOKS FOR. A DLL that patches echovr.exe cannot store an absolute address - the image is relocated
- so it computes one at run time. Microsoft Detours' emitted shape, which rlay.dll uses, is:

    48 8B 05 <rel32>      mov  rax, [rip+X]     ; the module base it cached
    48 05    <imm32>      add  rax, <RVA>       ; the target's RVA within echovr.exe
    48 89 05 <rel32>      mov  [rip+Y], rax     ; store the resolved absolute address
    C3                    ret

The imm32 is the target RVA in the clear. Scanning for that idiom recovers the target list without
running anything, disassembling anything, or having the mod's source.

WHAT IT CANNOT SEE, stated because a scanner that implies completeness is worse than none:
  * a mod that resolves targets any other way - a signature scan, an export lookup, a different codegen,
    or arithmetic split across basic blocks;
  * anything computed at run time from data rather than from an immediate;
  * a mod written in a language whose codegen does not look like this at all.
A clean report therefore means "nothing found by this method", never "no collisions exist". The collision
that matters is checked a second way at run time, per site, by buildguard::VerifySite at hook install.

Usage:
    scan_foreign_detours.py [--plugins DIR] [--emit TSV]
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))

import gen_facts as G
from pe import PeImage

PLUGINS = os.path.join(os.path.dirname(G.ECHOVR_EXE), "plugins")
CENSUS = os.path.join(HERE, "foreign_detours.tsv")

# mov rax,[rip+X] ; add rax, imm32 ; mov [rip+Y], rax ; ret
IDIOM = re.compile(rb"\x48\x8b\x05....\x48\x05(....)\x48\x89\x05....\xc3", re.S)

IMAGE_BASE = 0x140000000
IMAGE_LIMIT = 0x2231000


def scan(path):
    """Every echovr RVA this DLL resolves with the base+RVA idiom."""
    pe = PeImage(path)
    text = next(((raw, size) for name, _, _, raw, size in pe.sections if name == ".text"), None)
    if text is None:
        return [], False
    raw, size = text
    detours = any(name.startswith(".detour") for name, _, _, _, _ in pe.sections)
    out = []
    for m in IDIOM.finditer(pe.data, raw, raw + size):
        rva = struct.unpack("<I", m.group(1))[0]
        if 0 < rva < IMAGE_LIMIT:
            out.append(rva)
    return sorted(set(out)), detours


def our_hook_sites(rows):
    """The sites this project actually PATCHES - DESIGN S4.4's Level 1, 2.1 and 2.3.

    Not the build-guard fingerprint sites: those are read, never written, so another mod owning one costs
    a log line. A collision here is different in kind and is what this check exists to surface."""
    wanted = ("NETGAME.UPDATE", "RENDER.FN", "VOIP.RECEIVE")
    return dict((n, G.parse_value(rows[n])) for n in wanted if n in rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugins", default=PLUGINS)
    ap.add_argument("--emit", default=None, help="rewrite the census TSV from what was found")
    args = ap.parse_args()

    rows = {r["id"]: r for r in G.load_all()}
    by_va = {}
    for rid, r in rows.items():
        if r["addr_space"] == "PC_VA" and r["type"] != "bytes20":
            by_va.setdefault(G.parse_value(r), []).append(rid)
    hooks = our_hook_sites(rows)

    if not os.path.isdir(args.plugins):
        print("SKIP no plugins directory at %s" % args.plugins)
        print("RESULT foreigndetours status=SKIP dlls=0 targets=0 collisions=0 rlay=0")
        return 0

    found = []
    dlls = 0
    rlay = 0
    for name in sorted(os.listdir(args.plugins)):
        path = os.path.join(args.plugins, name)
        if not os.path.isfile(path) or ".dll" not in name.lower():
            continue
        # Our own DLL resolves engine addresses too, and that is not a foreign detour.
        if name.lower() in ("echoreplay_core.dll", "probe_bootstrap.dll"):
            continue
        try:
            rvas, detours = scan(path)
        except Exception as e:
            print("WARN could not scan %s: %s" % (name, e))
            continue
        dlls += 1
        if name.lower() == "rlay.dll":
            rlay = 1
        for rva in rvas:
            found.append((name, rva, detours))

    collisions = []
    lines = []
    for name, rva, detours in found:
        va = IMAGE_BASE + rva
        ids = ",".join(sorted(by_va.get(va, []))) or "-"
        hit = [h for h, v in hooks.items() if v == va]
        if hit:
            collisions.append((name, rva, hit))
        lines.append("%s\t0x%X\t0x%X\t%s\t%s\t%s" % (name, rva, va, ids,
                                                     "detours" if detours else "unknown",
                                                     ",".join(hit) or "-"))
        print("  %-18s RVA 0x%-8X VA 0x%-11X %-28s %s" % (name, rva, va, ids,
                                                          ("COLLIDES WITH OUR HOOK " + ",".join(hit))
                                                          if hit else ""))

    if args.emit:
        with open(args.emit, "w", encoding="utf-8", newline="\n") as f:
            f.write("# Generated by tests/loaderprobe/scan_foreign_detours.py -- do not hand-edit.\n")
            f.write("# echovr.exe addresses that OTHER plugins in plugins\\ resolve, by the Detours\n")
            f.write("# base+RVA idiom. See that script for what this method cannot see.\n")
            f.write("dll\trva\tva\tfact_ids\tlinker\tcollides_with_our_hook\n")
            f.write("\n".join(lines) + "\n")
        print("wrote %s" % args.emit)

    for name, rva, hit in collisions:
        print("FAIL %s resolves 0x%X, which is %s - a site this project HOOKS. Two tools patching one "
              "site is a real contention, not a log line." % (name, IMAGE_BASE + rva, ",".join(hit)))

    print("  scanned %d foreign DLL(s), %d resolved target(s), %d of our %d hook site(s) collide"
          % (dlls, len(found), len(collisions), len(hooks)))
    print("RESULT foreigndetours status=%s dlls=%d targets=%d collisions=%d rlay=%d"
          % ("PASS" if not collisions else "FAIL", dlls, len(found), len(collisions), rlay))
    return 1 if collisions else 0


if __name__ == "__main__":
    sys.exit(main())
