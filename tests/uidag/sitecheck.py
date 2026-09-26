"""W1-M's binary-level check: the byte windows probe_uidag.cpp carries really are what stands at those
addresses in echovr.exe, and the constants the probe decodes out of them are the ones it expects.

The probe does not take its offsets from the fact registry -- four of them are not in it -- so it
carries its own site tables and decodes every offset out of the bytes at run time. That is the right
shape for a probe (a recompiled game fails the comparison instead of reading a wrong offset), but it
puts unverified constants in a source file, and unverified constants rot. This is the check that stops
them: it parses the tables straight out of the .cpp, reads the same addresses out of the shipped
binary, and decodes the offsets the way the probe does.

A failing input is one byte changed in either place. A passing input is the pair agreeing.

Prints one PASS/FAIL line per assertion and a final `SITECHECK-CHECKED <n>` that check.ps1 folds into
the gate total.
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))

import gen_facts as G  # noqa: E402  -- ECHOVR_EXE, so the path is not written twice
from pe import PeImage  # noqa: E402

PROBE = os.path.join(HERE, "probe_uidag.cpp")

# What each site is expected to yield once decoded. Written here rather than imported from the probe so
# that this file is a second opinion: if someone edits the probe's expectations and its byte window
# together, this still fails.
#   name -> (offset in the window, width in bytes, expected value, what it is)
EXPECT = {
    "kGateSite": [
        (2, 4, 0x828, "the stage-0..3 gate word offset on the game object"),
        (9, 1, 23, "the bit index that disables the whole pass"),
    ],
    "kTlsSite": [
        (17, 4, 0x730, "the task-dispatch flag offset in the engine thread context"),
        (24, 4, 0xC38, "the scheme-index offset on a game space"),
        (31, 1, 0x78, "the 120-byte scheme stride"),
    ],
    "kListSite": [
        (3, 4, 0x1F50, "the game-space list base offset on the game object"),
    ],
    "kCountSite": [
        (3, 4, 0x1F80, "the game-space count offset on the game object"),
    ],
}

# The TLS index global is reached through the rip-relative displacement at kTlsSite+2, and the probe
# resolves it as (site + 6) + disp32. That resolution is itself worth checking against the value the
# disassembly gives, because an off-by-one in the instruction length would land on a plausible-looking
# neighbouring global rather than failing loudly.
TLS_INDEX_VA = 0x142098EE0

# ★ The double indirection. The runner does *(u32*)(*(void**)TlsGetValue(idx) + 0x730), and the `mov
# r8,[rax]` that performs the inner dereference sits at kTlsSite+11. A probe that dropped it would read
# 0x730 bytes past a pointer to a pointer and report a plausible number. Pinned as bytes.
INNER_DEREF_AT = 11
INNER_DEREF_BYTES = bytes((0x4C, 0x8B, 0x00))


def parse_sites(text):
    sites = {}
    pattern = re.compile(
        r"constexpr\s+Site\s+(\w+)\s*=\s*\{\s*"
        r'"[^"]*"\s*,\s*'
        r"(0x[0-9A-Fa-f]+)ull\s*,\s*"
        r"\{(.*?)\}\s*,\s*"
        r"(\d+)\s*\}\s*;",
        re.S,
    )
    for m in pattern.finditer(text):
        name, va, body, length = m.group(1), int(m.group(2), 16), m.group(3), int(m.group(4))
        raw = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", body))
        sites[name] = (va, raw, length)
    return sites


def main():
    checked = 0
    failed = 0

    def report(ok, what):
        nonlocal checked, failed
        checked += 1
        if not ok:
            failed += 1
        print(("PASS " if ok else "FAIL ") + what)

    if not os.path.exists(G.ECHOVR_EXE):
        print("SKIP echovr.exe not present at %s - cannot verify the probe's site tables" % G.ECHOVR_EXE)
        print("SITECHECK-CHECKED 0")
        return 0

    text = open(PROBE, "r", encoding="utf-8").read()
    sites = parse_sites(text)
    report(set(sites) == set(EXPECT),
           "probe_uidag.cpp declares exactly the %d expected sites (found %s)"
           % (len(EXPECT), ", ".join(sorted(sites)) or "none"))
    if set(sites) != set(EXPECT):
        print("SITECHECK-CHECKED %d" % checked)
        return 1

    pe = PeImage(G.ECHOVR_EXE)
    for name in sorted(EXPECT):
        va, raw, length = sites[name]
        report(len(raw) >= length, "%s declares %d bytes and a length of %d" % (name, len(raw), length))
        window = raw[:length]
        got = pe.read_va(va, length)
        report(got == window,
               "%s at 0x%X matches echovr.exe (%s)"
               % (name, va, "identical"
                  if got == window else "exe has %s, source has %s"
                  % (got.hex(" ").upper(), window.hex(" ").upper())))
        if got != window:
            continue
        for off, width, want, what in EXPECT[name]:
            if width == 1:
                value = window[off]
            elif width == 4:
                value = struct.unpack_from("<I", window, off)[0]
            else:
                raise AssertionError("width")
            report(value == want,
                   "%s+%d decodes to 0x%X, %s (expected 0x%X)" % (name, off, value, what, want))

    va, raw, length = sites["kTlsSite"]
    resolved = va + 6 + struct.unpack_from("<i", raw, 2)[0]
    report(resolved == TLS_INDEX_VA,
           "kTlsSite's rip-relative displacement resolves to 0x%X (expected the TLS index global 0x%X)"
           % (resolved, TLS_INDEX_VA))
    report(raw[INNER_DEREF_AT:INNER_DEREF_AT + 3] == INNER_DEREF_BYTES,
           "kTlsSite+%d is `mov r8,[rax]`, the inner dereference the flag read depends on"
           % INNER_DEREF_AT)

    # The probe watches for seven band arms it builds itself; the two ends are in the registry, and the
    # generated header is what the probe compiles against, so compare against that file rather than a
    # number retyped here.
    generated = os.path.join(ROOT, "src", "generated", "facts_generated.h")
    header = open(generated, "r", encoding="utf-8").read()
    for symbol, want in (("CS_PASS_WINDOW_BAND0", (0x1FFE << 16) | 0x0000),
                         ("CS_PASS_WINDOW_BAND6", (0xDFFE << 16) | 0xC000)):
        m = re.search(r"%s\s*=\s*(0x[0-9A-Fa-f]+)u" % symbol, header)
        report(m is not None and int(m.group(1), 16) == want,
               "%s in the generated header is 0x%08X, the arm the probe builds" % (symbol, want))

    print("SITECHECK-CHECKED %d" % checked)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
