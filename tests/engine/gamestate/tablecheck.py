"""W1-D's gate check: the 27-row phase vocabulary, re-read out of the shipped echovr.exe.

Three sources, none derived from another.  The DOMAIN -- which rows exist -- is
src/engine/gamestate_facts.h, the header the sampler actually compiles against.  The SUBJECT is
echovr.exe on disk, read at each row's own literal VA.  The third party is
src/generated/facts_generated.h, which supplies the one row the header names symbolically
(GS_SYM_MAIN_LOOP_ENTER) and the array/offset constants the header compiles against.

Each row is checked twice over, because a symbol that matched by accident is the failure this
exists to catch: the qword AT the literal VA must equal the row's symbol, and the bytes at
literalVa+8 must be the marker "OlPrEfIx" followed by the row's own name.  A row whose name does
not follow its symbol is not that row.

Prints one line per failure and `TABLE <n> STATUS <PASS|FAIL|MISSING>`.  MISSING is a FAILURE, not
a skip: this is the only half of W1-D's gate that reads echovr.exe, and a gate that reports success
having skipped it is worse than no gate.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))

import pe  # noqa: E402

sys.path.insert(0, os.path.join(ROOT, "tools"))
from echovr_path import NOT_FOUND, find_echovr_exe  # noqa: E402

ECHOVR_EXE = find_echovr_exe()
HEADER = os.path.join(ROOT, "src", "engine", "gamestate_facts.h")
GENERATED = os.path.join(ROOT, "src", "generated", "facts_generated.h")

MARKER = b"OlPrEfIx"
EXPECTED_ROWS = 27

ROW_RE = re.compile(
    r"\{\s*(?P<sym>0x[0-9A-Fa-f]+ULL|::facts::[A-Z0-9_]+),\s*"
    r"(?P<va>0x[0-9A-Fa-f]+)ULL,\s*(?P<id>-?\d+),\s*Phase::(?P<phase>\w+),\s*"
    r'"(?P<name>[^"]+)"')
CONST_RE = re.compile(r"inline constexpr uint64_t (?P<name>[A-Z0-9_]+) = (?P<value>0x[0-9A-Fa-f]+)ull;")


def generated_constants():
    with open(GENERATED, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    return {m.group("name"): int(m.group("value"), 16) for m in CONST_RE.finditer(text)}


def header_rows(constants):
    with open(HEADER, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    rows = []
    for m in ROW_RE.finditer(text):
        raw = m.group("sym")
        if raw.startswith("::facts::"):
            key = raw[len("::facts::"):]
            if key not in constants:
                return rows, "gamestate_facts.h names ::facts::%s, which facts_generated.h does not define" % key
            symbol = constants[key]
        else:
            symbol = int(raw[:-3], 16)
        rows.append({
            "symbol": symbol,
            "va": int(m.group("va"), 16),
            "id": int(m.group("id")),
            "phase": m.group("phase"),
            "name": m.group("name"),
        })
    return rows, None


def main():
    if not ECHOVR_EXE:
        print("SKIPPED " + NOT_FOUND)
        print("TABLE 0 STATUS SKIPPED")
        return 0
    if not os.path.exists(ECHOVR_EXE):
        print("echovr.exe not found at %s" % ECHOVR_EXE)
        print("TABLE 0 STATUS MISSING")
        return 1

    constants = generated_constants()
    rows, err = header_rows(constants)
    if err is not None:
        print(err)
        print("TABLE 0 STATUS FAIL")
        return 1
    # An unreadable header parses as zero rows, and zero rows would otherwise pass vacuously.
    if len(rows) != EXPECTED_ROWS:
        print("gamestate_facts.h yielded %d phase rows, expected %d" % (len(rows), EXPECTED_ROWS))
        print("TABLE 0 STATUS FAIL")
        return 1

    image = pe.load(ECHOVR_EXE)
    checked = 0
    failures = 0
    seen = set()
    for row in rows:
        if row["va"] in seen:
            print("%s: literal VA 0x%X is claimed by two rows" % (row["name"], row["va"]))
            failures += 1
            continue
        seen.add(row["va"])

        want_name = row["name"].encode("ascii")
        blob = image.read_va(row["va"], 8 + len(MARKER) + len(want_name) + 1)
        if blob is None:
            print("%s: 0x%X is not backed by image bytes" % (row["name"], row["va"]))
            failures += 1
            continue
        got_symbol = int.from_bytes(blob[:8], "little")
        if got_symbol != row["symbol"]:
            print("%s: symbol at 0x%X is %016X, header says %016X"
                  % (row["name"], row["va"], got_symbol, row["symbol"]))
            failures += 1
            continue
        checked += 1
        if blob[8:8 + len(MARKER)] != MARKER:
            print("%s: 0x%X+8 is not the OlPrEfIx marker" % (row["name"], row["va"]))
            failures += 1
            continue
        tail = blob[8 + len(MARKER):]
        if tail[:len(want_name)] != want_name or tail[len(want_name):len(want_name) + 1] != b"\x00":
            print("%s: the name at 0x%X+16 is not this row's name" % (row["name"], row["va"]))
            failures += 1
            continue
        checked += 1

    print("TABLE %d STATUS %s" % (checked, "FAIL" if failures else "PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
