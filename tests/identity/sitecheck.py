"""Every engine site the cosmetic path touches, read back out of the installed echovr.exe.

The domain is the identity anchors in facts/anchors.tsv; each one is re-read from the binary and
checked against its row in facts/facts.tsv.  src/engine/identity_access_facts.h, the header
identity_access.cpp compiles against, is compared with that set: constants it carries with no
anchor are listed as UNANCHORED.

Prints one line per failure and `SITES <n> STATUS <PASS|FAIL|MISSING|SKIPPED>`.  SKIPPED means no
echovr.exe was found; MISSING means one was configured but is not there, which is a failure.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))

import factcheck  # noqa: E402
from pe import PeImage  # noqa: E402

sys.path.insert(0, os.path.join(ROOT, "tools"))
from echovr_path import NOT_FOUND, find_echovr_exe  # noqa: E402

ECHOVR_EXE = find_echovr_exe()
FACTS = os.path.join(ROOT, "facts", "facts.tsv")
HEADER = os.path.join(ROOT, "src", "engine", "identity_access_facts.h")


def rows(path):
    out = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        head = None
        for line in f.read().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if head is None:
                head = parts
                continue
            out.append(dict(zip(head, parts)))
    return out


def main():
    if not ECHOVR_EXE:
        # SKIPPED, not PASS: check.ps1 counts no sites for it, so the binary half is visibly absent
        # from the verdict rather than folded into a pass.
        print("SKIPPED " + NOT_FOUND)
        print("SITES 0 STATUS SKIPPED")
        return 0
    if not os.path.exists(ECHOVR_EXE):
        print("FAIL echovr.exe is not at %s, so the binary half of this gate cannot run"
              % ECHOVR_EXE)
        print("SITES 0 STATUS MISSING")
        return 2

    facts = {r["id"]: r for r in rows(FACTS)}
    anchors = [a for a in factcheck.load_anchors()
               if a["fact_id"].startswith("IDENT.") or a["fact_id"] == "NETGAME.ALIGNMENT_BASE_OFF"]
    pe = PeImage(ECHOVR_EXE)

    # What the shipped code compiles against.  Names are the fact id minus its `IDENT.` prefix,
    # which is exactly how the generator emits them.
    header = "\n".join(l for l in open(HEADER, "r", encoding="utf-8").read().splitlines()
                       if not l.startswith("#define"))
    carried = set(re.findall(r"IDENT_FACT\((\w+),", header))
    carried |= set(re.findall(r"inline constexpr uint8_t k(\w+)\[\d+\]", header))
    carried = {n for n in carried if not n.endswith("Prologue")}

    failures = []
    checked = 0
    seen = set()

    for a in anchors:
        fid = a["fact_id"]
        row = facts.get(fid)
        if row is None:
            # An anchor with no registry row still has its bytes verified.
            got = pe.read_va(a["anchor_va"], len(a["instr"]))
            problem = None if got == a["instr"] else (
                "bytes at 0x%X are %s, anchor recorded %s"
                % (a["anchor_va"], (got or b"").hex(" ").upper(), a["instr"].hex(" ").upper()))
        else:
            problem, _ = factcheck.anchor_verdict(pe, a, row)
        if problem:
            failures.append("%s: %s" % (fid, problem))
            continue
        seen.add(fid)
        checked += 1

    # Other modules read IDENT facts straight from the generated registry, so an anchored fact
    # this header does not carry is normal.  A constant it carries with no anchor is an unverified
    # transcription: listed so the gap stays visible, but most were never anchored and anchoring
    # them needs the disassembly, which a test run cannot do.
    anchored_short = {f.split(".", 1)[1] for f in seen if f.startswith("IDENT.")}
    unanchored = sorted(carried - anchored_short)
    for name in unanchored:
        print("UNANCHORED k%s" % name)
    checked += len(carried & anchored_short)
    print("%d of %d header constants are anchored" % (len(carried) - len(unanchored), len(carried)))

    for f in failures:
        print("FAIL " + f)
    # The count is reported whether or not the gate passed, because the two say different
    # things: the status is "did anything disagree", the count is "how much was looked at".
    # Collapsing the count to zero on a failure would hide a shrinking domain behind a red run.
    print("SITES %d STATUS %s" % (checked, "FAIL" if failures else "PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
