"""W1-D's binary-level checks: the fallback's guard row against the real game, and our own export
surface against the names the engine actually resolves.

Two properties, both of which a unit test cannot reach because they are about shipped binaries:

  1. facts NETGAME_UPDATE_PROLOGUE really is what stands at NETGAME_UPDATE_RVA in echovr.exe. That row
     is the byte-guard MinHook's Create() consults before it patches; if the row drifted from the
     binary the fallback would either refuse forever or detour into the middle of another function.

  2. echoreplay.dll exports EXACTLY `Update` and `RadPluginShutdown`, undecorated, and NONE of the
     eleven CNSProvider method names the engine looks up against a provider handle.

Point 2 deliberately overlaps tests/exports, which is W0-1's. That check's deny-list was found inert in
audit -- every forbidden name was already caught by the allow-list, so the deny-list changed the message
and never the verdict -- and W1-D's canary is worthless if an unintended export answers an engine lookup.
This asserts the engine-facing property independently and from its own list, so a repair (or a
regression) in the other check cannot silently take this one with it.

Prints one PASS/FAIL line per assertion and a final `PECHECK-CHECKED <n>` that check.ps1 folds into the
gate total.
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gen_facts as G  # noqa: E402  -- ECHOVR_EXE and ECHOVR_MD5, so the path is not written twice

GENERATED = os.path.join(ROOT, "src", "generated", "facts_generated.h")

# The eleven names sub_140600820's census plus pnsdemo's export table say the engine can ask a provider
# for. Spelled here rather than read from tests/exports/allowed.txt on purpose: a check that reads the
# list it is meant to be a second opinion on is not a second opinion.
FORBIDDEN = [
    "AllowGuests", "InitGlobals", "ProviderID", "Social", "UserProviderID", "Users",
    "MicStart", "MicStop", "MicDestroy", "VoipDestroyEncoder", "VoipDestroyDecoder",
]
ALLOWED = ["Update", "RadPluginShutdown"]


class Checker(object):
    def __init__(self):
        self.checked = 0
        self.failed = 0

    def ok(self, condition, what, detail=""):
        self.checked += 1
        if condition:
            print("  PASS  %s" % what)
        else:
            self.failed += 1
            print("  FAIL  %s%s" % (what, (" -- " + detail) if detail else ""))
        return bool(condition)


def read_generated_constant(text, name):
    m = re.search(r"\b%s\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)" % re.escape(name), text)
    if not m:
        raise SystemExit("facts_generated.h has no %s; the fact registry row is missing" % name)
    return int(m.group(1), 0)


def read_generated_bytes(text, name):
    m = re.search(r"\b%s\s*\[\s*\d*\s*\]\s*=\s*\{([^}]*)\}" % re.escape(name), text)
    if not m:
        raise SystemExit("facts_generated.h has no %s" % name)
    return bytes(int(v, 0) for v in re.findall(r"0[xX][0-9A-Fa-f]+|\d+", m.group(1)))


class PE(object):
    """Just enough PE to turn an RVA into a file offset and to walk the export directory."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise SystemExit("%s is not a PE" % path)
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        optsz = struct.unpack_from("<H", self.data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", self.data, opt)[0]
        self.image_base = struct.unpack_from("<Q" if magic == 0x20B else "<I", self.data,
                                             opt + (24 if magic == 0x20B else 28))[0]
        dd = opt + (112 if magic == 0x20B else 96)
        self.export_rva, self.export_size = struct.unpack_from("<II", self.data, dd)
        self.sections = []
        for i in range(nsec):
            b = opt + optsz + 40 * i
            vsize, vaddr, rawsz, rawptr = struct.unpack_from("<IIII", self.data, b + 8)
            self.sections.append((vaddr, vsize, rawptr, rawsz))

    def offset(self, rva):
        for vaddr, vsize, rawptr, rawsz in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsz):
                d = rva - vaddr
                if d < rawsz:
                    return rawptr + d
        return None

    def at(self, rva, n):
        o = self.offset(rva)
        return self.data[o:o + n] if o is not None else None

    def export_names(self):
        if self.export_rva == 0 or self.export_size == 0:
            return []
        o = self.offset(self.export_rva)
        if o is None:
            return []
        count = struct.unpack_from("<I", self.data, o + 24)[0]
        names_rva = struct.unpack_from("<I", self.data, o + 32)[0]
        base = self.offset(names_rva)
        if base is None:
            return []
        out = []
        for i in range(count):
            rva = struct.unpack_from("<I", self.data, base + 4 * i)[0]
            start = self.offset(rva)
            end = self.data.index(b"\0", start)
            out.append(self.data[start:end].decode("ascii", "replace"))
        return out


def check_guard_row(c, text):
    import hashlib
    game = PE(G.ECHOVR_EXE)
    md5 = hashlib.md5(game.data).hexdigest()
    c.ok(md5 == G.ECHOVR_MD5, "echovr.exe is the pinned build",
         "md5 %s, registry pins %s" % (md5, G.ECHOVR_MD5))

    rva = read_generated_constant(text, "NETGAME_UPDATE_RVA")
    va = read_generated_constant(text, "NETGAME_UPDATE")
    prologue = read_generated_bytes(text, "NETGAME_UPDATE_PROLOGUE")
    c.ok(va - game.image_base == rva, "NETGAME_UPDATE's VA and RVA agree with the image base",
         "0x%X - 0x%X != 0x%X" % (va, game.image_base, rva))

    actual = game.at(rva, len(prologue))
    c.ok(actual == prologue, "CR15NetGame::Update holds the recorded 20-byte prologue",
         "found %s" % (actual.hex(" ").upper() if actual else "nothing"))

    # A known-answer control: the same read one byte along must NOT match, or a comparison that always
    # says yes would have passed the assertion above.
    shifted = game.at(rva + 1, len(prologue))
    c.ok(shifted != prologue, "the prologue comparison can still refuse (off-by-one control)")


def main():
    build = os.environ.get("REPLAY_BUILD_DIR")
    if not build:
        print("REPLAY_BUILD_DIR is not set")
        return 1

    c = Checker()
    text = open(GENERATED, "r", encoding="utf-8", errors="replace").read()

    # ---- 1. the fallback's byte guard, against the real game -------------------------------------
    if os.path.exists(G.ECHOVR_EXE):
        check_guard_row(c, text)
    else:
        print("  SKIP  echovr.exe not present at %s" % G.ECHOVR_EXE)
        print("        the guard-row check needs the real game file and cannot be faked")

    # ---- 2. our export surface, against the names the engine resolves ----------------------------
    dll = os.path.join(build, "bin", "echoreplay.dll")
    if not os.path.exists(dll):
        print("  FAIL  no %s -- build first" % dll)
        return 1

    names = PE(dll).export_names()
    print("  exports: %s" % (", ".join(sorted(names)) if names else "(none)"))

    for name in ALLOWED:
        c.ok(name in names, "exports %s, which the engine resolves by that exact string" % name)
    for name in FORBIDDEN:
        c.ok(name not in names,
             "does NOT export %s" % name,
             "exporting it makes us answer for a CNSProvider ABI we do not implement")
    c.ok(len(names) == len(ALLOWED), "the export table holds exactly %d names" % len(ALLOWED),
         "found %d: %s" % (len(names), ", ".join(sorted(names))))
    # GetProcAddress matches the literal string, so a decorated name would not be found at all.
    c.ok(all("?" not in n and "@" not in n for n in names),
         "every exported name is undecorated, as GetProcAddress needs")

    print("PECHECK-CHECKED %d" % c.checked)
    if c.failed:
        print("PECHECK-FAILED %d" % c.failed)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
