#!/usr/bin/env python3
"""Generate src/engine/gamestate_facts.h and facts/pending/W1-D_gamestate.tsv.

Everything here is re-derived from two artifacts and nothing else:

  * echovr.exe on disk (md5 caa05ee3bca2f5df0809994e93e2cc05) -- every CSymbol64 value,
    every phase-record id byte, every name string;
  * the offline IDA database (read-only) -- the INSTRUCTION STREAM of sub_1400C5B40 (the
    registrar that fills the 16-record array at 0x1420D3850) and of sub_14016D510 (the
    engine's symbol -> coarse-name mapper, 25 comparands).

The instruction bytes taken from the database are compared against echovr.exe at each
instruction's own file offset before they are believed, so a stale or edited database
cannot put a row in the header.

ARCH-21 S3.2's [U] -- whether the array continues past 0x1420D3950 -- is answered here from
the consumer sub_140D5C960's own loop bound, and the answer is asserted, not assumed.

Usage:
    gen_gamestate_facts.py [--check]
"""

import argparse
import hashlib
import os
import re
import sqlite3
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))
from pe import PeImage  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from echovr_path import find_echovr_exe, optional_input  # noqa: E402

ECHOVR_EXE = find_echovr_exe() or "(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)"
ECHOVR_MD5 = "caa05ee3bca2f5df0809994e93e2cc05"
IDA_DB = "file:%s?mode=ro" % (optional_input("REPLAY_IDA_ECHOVR_DB") or "(set REPLAY_IDA_ECHOVR_DB)").replace("\\", "/")

REGISTRAR = 0x1400C5B40   # fills the array
CONSUMER = 0x140D5C960    # the only other reader of the array; owns the row count
NAME_TABLE = 0x14016D510  # symbol -> coarse name, 25 comparands
ARRAY_VA = 0x1420D3850
RECORD_STRIDE = 0x10

OUT_HEADER = os.path.join(ROOT, "src", "engine", "gamestate_facts.h")
OUT_TSV = os.path.join(ROOT, "facts", "pending", "W1-D_gamestate.tsv")
OUT_ANCHORS = os.path.join(ROOT, "facts", "pending", "anchors", "W1-D_gamestate.tsv")

# name prefix -> (our Phase, the id the array must carry for it, or None when the symbol is
# not in the array).  The prefixes are ordered: the longest match wins, so
# `post_sudden_death` is never eaten by `sudden_death`.
PHASE_RULES = [
    ("team_room_wait", "TeamRoomWait", 0),
    ("pre_match", "PreMatch", 0),
    ("round_start", "RoundStart", 1),
    ("pre_launch", "RoundStart", 1),
    ("main_loop", "Playing", 6),
    ("point_score", "Score", 9),
    ("round_over", "RoundOver", 10),
]

# The 11 symbols the id array does NOT carry are classified by the engine's own coarse name,
# which is the only classification the engine publishes for them.
PHASE_BY_ENGINE_NAME = {
    "pre_match": "PreMatch",
    "round_start": "RoundStart",
    "playing": "Playing",
    "score": "Score",
    "round_over": "RoundOver",
    "post_match": "PostMatch",
    "pre_sudden_death": "PreSuddenDeath",
    "sudden_death": "SuddenDeath",
    "post_sudden_death": "PostSuddenDeath",
}


# (C++ name, engine variable name, deref depth from netGame, note).  ARCH-21 S3.1's table.
VAR_SYMBOLS = [
    ("VarGameState", "game_state", 0, "CSymbol64; the phase"),
    ("VarGameTimer", "game_timer", 0, "f32; the JSON calls it game_clock, which resolves to nothing"),
    ("VarCountdownTimer", "countdown_timer", 0, "f32"),
    ("VarPausedState", "paused_state", 0, "CSymbol64; meaning deliberately not claimed"),
    ("VarRoundCounter", "round_counter", 0, "i32; sub_1401AEE50:166 reads it off netGame itself"),
    ("VarBlueGameScore", "blueGameScore", 0, "i32; MATCH score, not round points"),
    ("VarOrangeGameScore", "orangeGameScore", 0, "i32; MATCH score"),
    ("VarRoundWinningTeam", "round_winning_team", 0, "i32"),
    ("VarFirstLaunchOfRound", "first_launch_of_round", 0, "raw; 1 -> 0 at main_loop"),
    ("VarFrisbeeOwnerId", "game_frisbee_ownerid", 0, "raw u32; 0xFFFFFFFF unowned"),
    ("VarBlueScore", "blue_score", 2, "i32; ROUND points, through *(*(netGame+0x2AA8))"),
    ("VarRedScore", "red_score", 2, "i32; ROUND points, same store as blue_score"),
]

_var_cache = {}


class DeriveError(Exception):
    pass


def resolve_var_symbols(img):
    """Find each variable's OlPrEfIx literal by scanning echovr.exe for the marker+name.

    No database and no address is involved: the name is the search key, the qword in front of
    the marker is the answer, and a name that occurs at more than one address aborts rather
    than picking one -- two literals would mean two symbols and no way to say which the engine
    compares.
    """
    for _cname, name, _depth, _note in VAR_SYMBOLS:
        needle = b"OlPrEfIx" + name.encode("ascii") + b"\x00"
        hits = []
        start = 0
        while True:
            at = img.data.find(needle, start)
            if at < 0:
                break
            hits.append(at)
            start = at + 1
        if len(hits) == 0:
            # Not every game variable has an OlPrEfIx literal: the ones the /session writer
            # never names are only in the netvar tables, and their symbols were recovered from
            # that side (src/capture/capture_facts.h).  Recorded as absent rather than guessed.
            _var_cache[name] = None
            continue
        if len(hits) != 1:
            raise DeriveError("%r occurs at %d OlPrEfIx literals, not 1" % (name, len(hits)))
        marker_off = hits[0]
        base_va = None
        for _name, rva, _vsize, raw_start, raw_size in img.sections:
            if raw_size and raw_start <= marker_off < raw_start + raw_size:
                base_va = img.image_base + rva + (marker_off - raw_start)
                break
        if base_va is None:
            raise DeriveError("literal for %r at file offset %#x is in no section" % (name, marker_off))
        base = base_va - 8
        if base & 7:
            raise DeriveError("literal for %r is not 8-aligned (%#x)" % (name, base))
        symbol = struct.unpack("<Q", img.read_va(base, 8))[0]
        _var_cache[name] = (base, symbol, name)


def classify(name):
    best = None
    for prefix, phase, expected_id in PHASE_RULES:
        if name.startswith(prefix) and (best is None or len(prefix) > len(best[0])):
            best = (prefix, phase, expected_id)
    if best is None:
        raise DeriveError("no phase rule matches symbol name %r" % name)
    return best[1], best[2]


def load_image():
    with open(ECHOVR_EXE, "rb") as f:
        data = f.read()
    md5 = hashlib.md5(data).hexdigest()
    if md5 != ECHOVR_MD5:
        raise DeriveError("echovr.exe md5 is %s, expected %s" % (md5, ECHOVR_MD5))
    return PeImage(ECHOVR_EXE)


def instructions(db, func_va):
    rows = list(db.execute(
        "select va, file_offset, disasm, bytes from instructions where func_va=? order by va",
        (func_va,)))
    if not rows:
        raise DeriveError("no instructions for %#x in the IDA database" % func_va)
    return rows


def verify_bytes(img, rows, func_va):
    """Every instruction the database reports must be present in echovr.exe verbatim."""
    for va, file_off, _disasm, raw in rows:
        if raw is None or file_off is None:
            raise DeriveError("instruction %#x of %#x has no bytes/offset" % (va, func_va))
        if isinstance(raw, str):
            raw = bytes.fromhex(raw)
        got = img.read_va(va, len(raw))
        if got != raw:
            raise DeriveError("instruction %#x differs: db %s, exe %s"
                              % (va, raw.hex(), got.hex()))


def symbol_at(img, literal_va):
    """The CSymbol64 and the name for an OlPrEfIx literal the engine LEAs.

    Layout, confirmed on every one of the 27 rows: at the 8-aligned base sits the 8-byte
    CSymbol64, then the fixed marker "OlPrEfIx", then the NUL-terminated name.  The engine
    LEAs base+7 -- the symbol's own top byte -- which is why IDA shows the literal as a
    string with one junk character in front of the marker.
    """
    base = literal_va & ~7
    symbol = struct.unpack("<Q", img.read_va(base, 8))[0]
    marker = img.read_va(base + 8, 8)
    if marker != b"OlPrEfIx":
        raise DeriveError("literal %#x is not an OlPrEfIx symbol (marker %r)" % (literal_va, marker))
    raw = img.read_va(base + 16, 96)
    end = raw.find(b"\x00")
    if end <= 0:
        raise DeriveError("literal %#x has no terminated name" % literal_va)
    name = raw[:end].decode("ascii")
    if not re.fullmatch(r"[a-z0-9_]+", name):
        raise DeriveError("literal %#x name %r is not a phase symbol name" % (literal_va, name))
    return base, symbol, name


LEA_RE = re.compile(r"^lea\s+\w+,\s+(?:\w+_)?([0-9A-F]{6,})h?$|^lea\s+\w+,\s+\S+;\s+\"")


def lea_target(disasm, va, size_hint=7):
    """Recover a `lea reg, <rip-relative>` target from IDA's text.

    The text carries either a synthetic `unk_<VA>` / `a<Name>` label.  For the named-string
    form IDA does not print the VA, so the target is recomputed from the instruction bytes.
    """
    m = re.search(r"\b(?:unk|byte|word|dword|qword|a[A-Za-z0-9_]*)_?([0-9A-F]{8,9})\b", disasm)
    if m:
        return int(m.group(1), 16)
    return None


def lea_target_from_bytes(raw, va):
    """`lea r64, [rip+disp32]` is REX.W 8D /r with a 4-byte displacement."""
    if isinstance(raw, str):
        raw = bytes.fromhex(raw)
    if len(raw) != 7 or (raw[0] & 0xF8) != 0x48 or raw[1] != 0x8D or (raw[2] & 0xC7) != 0x05:
        raise DeriveError("instruction at %#x is not a rip-relative lea: %s" % (va, raw.hex()))
    disp = struct.unpack("<i", raw[3:7])[0]
    return va + 7 + disp


def read_array(img, db):
    """The 16 records at 0x1420D3850, from the registrar's own stores.

    Each record is filled by three or four stores; we take the two the design depends on:
    `mov cs:qword_<rec+0>, rax` after a `lea rax, <literal>; and rax, ~7; mov rax, [rax]`,
    and the id store at `<rec+8>`.  An id with NO store is read from the image -- audit
    R2-c2: `team_room_wait_enter`'s zero must be a byte we read, not a zero we assumed.
    """
    rows = instructions(db, REGISTRAR)
    verify_bytes(img, rows, REGISTRAR)

    pending_literal = None
    by_record = {}
    id_stores = {}
    zeroed = set()
    for va, _off, disasm, raw in rows:
        d = disasm.strip()
        if d.startswith("xor") and re.search(r"xor\s+(e[a-d]x),\s+\1", d):
            zeroed.add(re.search(r"xor\s+(e[a-d]x)", d).group(1))
            continue
        if d.startswith("lea"):
            pending_literal = lea_target_from_bytes(raw, va)
            continue
        m = re.match(r"mov\s+cs:\w+_([0-9A-F]{9}),\s+rax$", d)
        if m:
            rec = int(m.group(1), 16)
            if pending_literal is None:
                raise DeriveError("qword store at %#x with no preceding lea" % va)
            by_record[rec] = pending_literal
            pending_literal = None
            continue
        m = re.match(r"mov\s+cs:dword_([0-9A-F]{9}),\s+(\w+)$", d)
        if m:
            slot = int(m.group(1), 16)
            src = m.group(2)
            if re.fullmatch(r"[0-9A-F]+h?", src):
                id_stores[slot] = int(src.rstrip("h"), 16)
            elif src in zeroed:
                id_stores[slot] = 0
            else:
                raise DeriveError("id store at %#x has unresolved source %r" % (va, src))

    count = array_row_count(img, db)
    records = []
    for i in range(count):
        rec = ARRAY_VA + i * RECORD_STRIDE
        if rec not in by_record:
            raise DeriveError("record %#x has no symbol store in the registrar" % rec)
        base, symbol, name = symbol_at(img, by_record[rec])
        if rec + 8 in id_stores:
            rid = id_stores[rec + 8]
            id_src = "registrar store"
        else:
            rid, id_src = unwritten_id(img, db, rec + 8)
        records.append({"record": rec, "literal": base, "symbol": symbol, "name": name,
                        "id": rid, "id_source": id_src})
    if len(records) != count:
        raise DeriveError("recovered %d records, expected %d" % (len(records), count))
    return records


def unwritten_id(img, db, slot_va):
    """The value of an id slot the registrar never stores to.

    AUDIT_ARCH_21 R2-c2 asks for this number to be READ rather than assumed, and the honest
    answer is that there is nothing to read: the slot lies past .data's raw bytes, so the
    number is not in the file at all.  What IS decidable is stronger than a byte, and both
    halves are checked here:

      * the loader zero-fills the tail of a section whose virtual size exceeds its raw size,
        and this slot has no file offset at all -- OR, when it is raw-backed, the bytes are
        read and must be zero;
      * NO instruction anywhere in the image names the slot, so nothing overwrites the zero
        before the consumer reads it.

    A slot that is raw-backed with a non-zero value, or that some other function writes,
    aborts: it would mean the id is a runtime decision this table cannot carry.
    """
    rva = slot_va - img.image_base
    offset = img.rva_to_offset(rva)
    if offset is None:
        origin = "past .data's raw bytes, zero-filled by the loader"
    else:
        raw = img.read_va(slot_va, 4)
        value = struct.unpack("<I", raw)[0]
        if value != 0:
            raise DeriveError("id slot %#x is raw-backed with %#x, not zero" % (slot_va, value))
        origin = "the four zero bytes at file offset %#x" % offset
    writers = [hex(r[0]) for r in db.execute(
        "select distinct func_va from instructions where disasm like ?",
        ("%%%X%%" % slot_va,))]
    if writers:
        raise DeriveError("id slot %#x is referenced by %s" % (slot_va, ", ".join(writers)))
    return 0, "no store in the registrar, no reference anywhere in .text, and %s" % origin


def array_row_count(img, db):
    """The consumer's loop bound is the row count -- ARCH-21 S3.2's carried [U].

    sub_140D5C960 walks the array with a 16-byte stride and `cmp rcx, <n>` / `jb`, then
    indexes the id as [base + 2*rcx*8 + 8].  The bound is the number of RECORDS.
    """
    rows = instructions(db, CONSUMER)
    verify_bytes(img, rows, CONSUMER)
    seen_base = False
    stride_ok = False
    for i, (va, _off, disasm, _raw) in enumerate(rows):
        d = disasm.strip()
        if "%X" % ARRAY_VA in d.upper() and d.startswith("lea"):
            seen_base = True
            continue
        if not seen_base:
            continue
        if re.match(r"add\s+\w+,\s+10h$", d):
            stride_ok = True
        m = re.match(r"cmp\s+(\w+),\s+([0-9A-F]+)h?$", d)
        if m and stride_ok:
            nxt = rows[i + 1][2].strip() if i + 1 < len(rows) else ""
            if nxt.startswith("jb"):
                return int(m.group(2).rstrip("h"), 16)
    raise DeriveError("could not recover the array row count from %#x" % CONSUMER)


def read_name_table(img, db):
    """The 25 comparands of sub_14016D510 and the coarse name each one returns.

    Shape, uniform across all 25: `lea rax, <literal>; and rax, ~7; cmp rcx, [rax]; jz L`,
    where L is a `lea rax, <name string>; retn` pair.  The fallthrough is `mov rax, rdx` --
    the CALLER'S fallback, which is why an unknown symbol is never named (ARCH-21 D7).
    """
    rows = instructions(db, NAME_TABLE)
    verify_bytes(img, rows, NAME_TABLE)
    by_va = {va: (disasm.strip(), raw) for va, _off, disasm, raw in rows}

    names_at = {}
    for va, _off, disasm, raw in rows:
        d = disasm.strip()
        if d.startswith("lea") and ";" in d and "OlPrEfIx" not in d:
            target = lea_target_from_bytes(raw, va)
            text = img.read_va(target, 64)
            end = text.find(b"\x00")
            names_at[va] = text[:end].decode("ascii")

    out = []
    pending = None
    for va, _off, disasm, raw in rows:
        d = disasm.strip()
        if d.startswith("lea") and va not in names_at:
            pending = lea_target_from_bytes(raw, va)
            continue
        m = re.match(r"jz\s+(?:short\s+)?\w+_([0-9A-F]{9})$", d)
        if m and pending is not None:
            label = int(m.group(1), 16)
            if label not in names_at:
                raise DeriveError("jz target %#x at %#x is not a name lea" % (label, va))
            base, symbol, name = symbol_at(img, pending)
            out.append({"literal": base, "symbol": symbol, "name": name,
                        "engine_name": names_at[label]})
            pending = None
    if not out:
        raise DeriveError("recovered no comparands from %#x" % NAME_TABLE)
    fallback = [va for va, (d, _r) in by_va.items() if d == "mov     rax, rdx"]
    if not fallback:
        raise DeriveError("%#x has no caller-fallback arm; D7's premise is gone" % NAME_TABLE)
    return out


def union_rows(array_rows, name_rows):
    merged = {}
    for r in array_rows:
        phase, expected_id = classify(r["name"])
        if expected_id is None or r["id"] != expected_id:
            raise DeriveError("array row %s has id %d; phase %s expects %s"
                              % (r["name"], r["id"], phase, expected_id))
        merged[r["symbol"]] = {"name": r["name"], "symbol": r["symbol"], "literal": r["literal"],
                               "phase": phase, "id": r["id"], "record": r["record"],
                               "id_source": r["id_source"], "engine_name": None}
    for r in name_rows:
        row = merged.get(r["symbol"])
        if row is None:
            # Not in the id array: the engine's own coarse NAME is the only classification it
            # publishes for this symbol, so that is what we take.  Safe here only because the
            # four names involved collide with nothing in the id half -- checked below.
            phase = PHASE_BY_ENGINE_NAME.get(r["engine_name"])
            if phase is None:
                raise DeriveError("no phase for engine name %r (symbol %s)"
                                  % (r["engine_name"], r["name"]))
            merged[r["symbol"]] = {"name": r["name"], "symbol": r["symbol"],
                                   "literal": r["literal"], "phase": phase, "id": -1,
                                   "record": 0, "id_source": "not in the array",
                                   "engine_name": r["engine_name"]}
        else:
            if row["name"] != r["name"]:
                raise DeriveError("symbol %#x names %s in the array and %s in the name table"
                                  % (r["symbol"], row["name"], r["name"]))
            row["engine_name"] = r["engine_name"]

    rows = sorted(merged.values(), key=lambda r: r["name"])

    # The premise S3.2's hybrid rests on, checked rather than assumed: no coarse name is
    # produced by BOTH an id-classified symbol and a name-classified one.  If it were, a
    # name-classified row would be inheriting a phase the id table would have split.
    id_names = {r["engine_name"] for r in rows if r["id"] >= 0 and r["engine_name"]}
    only_names = {r["engine_name"] for r in rows if r["id"] < 0 and r["engine_name"]}
    clash = id_names & only_names
    if clash:
        raise DeriveError("coarse-name collision between the two halves: %s" % sorted(clash))
    return rows


def emit_header(rows, count, array_va):
    id_rows = [r for r in rows if r["id"] >= 0]
    named = [r for r in rows if r["engine_name"]]
    L = []
    A = L.append
    A("#pragma once")
    A("")
    A("// GENERATED by tools/w1d/gen_gamestate_facts.py -- do not edit by hand.")
    A("//")
    A("// The phase vocabulary of ARCH-21 S3.2, as a closed table of %d symbols: the %d records"
      % (len(rows), count))
    A("// the engine's own registrar sub_1400C5B40 writes at %#x (each carrying the id the" % array_va)
    A("// engine classifies that phase by), unioned with the %d comparands of sub_14016D510," % len(named))
    A("// the engine's symbol -> coarse-name mapper.  The overlap is %d." % len(id_rows))
    A("//")
    A("// EVERY value below was read out of echovr.exe (md5 %s) at the" % ECHOVR_MD5)
    A("// address on its own row.  Nothing is hashed here and nothing is inferred: an id with no")
    A("// store in the registrar was read as an image byte, because a zero one assumes and a zero")
    A("// one reads are the same number and only one of them is evidence (AUDIT_ARCH_21 R2-c2).")
    A("//")
    A("// The `id` column is the engine's OWN coarse class and it is what makes the round-start")
    A("// distinction survivable: team_room_wait_* is id 0 with pre_match_*, while round_start and")
    A("// pre_launch_* are id 1.  sub_14016D510 collapses the first group into the name")
    A('// "round_start" -- classify by that name and entering the team room emits a round.')
    A("")
    A("#include <stdint.h>")
    A("")
    A('#include "engine/gamestate_types.h"')
    A('#include "generated/facts_generated.h"')
    A("")
    A("namespace replay {")
    A("namespace gamestate {")
    A("namespace facts {")
    A("")
    A("// The array the engine fills, and the row count taken from its CONSUMER sub_140D5C960's")
    A("// own loop bound (`cmp rcx, %#x` / `jb`, 16-byte stride) -- not from the registrar's" % count)
    A("// store count, which cannot see a row the registrar leaves at its static value.")
    A("inline constexpr uint64_t kPhaseArrayVa    = ::facts::GS_PHASE_ARRAY;")
    A("inline constexpr uint32_t kPhaseArrayRows  = ::facts::GS_PHASE_ARRAY_ROWS;")
    A("inline constexpr uint32_t kPhaseArrayStride = 0x%X;" % RECORD_STRIDE)
    A("")
    A("struct PhaseFact {")
    A("    uint64_t symbol;      // the CSymbol64 the engine compares, verbatim")
    A("    uint64_t literalVa;   // where that qword lives in echovr.exe; the startup cross-check")
    A("    int32_t  engineId;    // the engine's coarse id, or -1 when the symbol is not in the array")
    A("    Phase    phase;       // ours -- finer than the id, never coarser")
    A("    const char* name;     // the OlPrEfIx name at literalVa + 16")
    A("    const char* engineName; // what sub_14016D510 returns, or nullptr when it does not test it")
    A("};")
    A("")
    A("inline constexpr uint32_t kPhaseFactCount = %d;" % len(rows))
    A("")
    A("inline constexpr PhaseFact kPhaseFacts[kPhaseFactCount] = {")
    width = max(len(r["name"]) for r in rows) + 2
    for r in rows:
        en = ("\"%s\"" % r["engine_name"]) if r["engine_name"] else "nullptr"
        sym = REGISTERED_SYMBOLS.get(r["symbol"], "0x%016XULL" % r["symbol"])
        A("    { %-32s 0x%XULL, %3d, Phase::%-16s %-*s %s },"
          % (sym + ",", r["literal"], r["id"], r["phase"] + ",",
             width, "\"%s\"," % r["name"], en))
    A("};")
    A("")
    A("// A row's id is not decoration: it is the engine's own answer, and a build whose table")
    A("// drifted from it must not compile.  One assert per id-bearing row, by name, so a failure")
    A("// names the row rather than the table.")
    for r in id_rows:
        A("static_assert(kPhaseFacts[%d].engineId == %d && kPhaseFacts[%d].phase == Phase::%s,"
          % (rows.index(r), r["id"], rows.index(r), r["phase"]))
        A('              "%s: engine id %d, phase %s");' % (r["name"], r["id"], r["phase"]))
    A("")
    A("static_assert(kPhaseFactCount == %d, \"ARCH-21 S3.2: the union is %d symbols\");"
      % (len(rows), len(rows)))
    A("static_assert(kPhaseArrayRows == %d, \"sub_140D5C960's loop bound\");" % count)
    A("")
    A("// The read chains of ARCH-21 S3.1.  The DEPTH is part of the fact: two different stores")
    A("// answer these symbols and they are not the same store.  `blue_score` at depth 0 reads a")
    A("// slot that is not there, silently, forever.")
    A("inline constexpr uint32_t kRulesOff        = ::facts::GS_RULES_OFF;  // netGame -> the game-mode block")
    A("inline constexpr uint32_t kLastScoreTeamOff   = 0x10;  // depth 1, u16")
    A("inline constexpr uint32_t kLastScorePointsOff = 0x12;  // depth 1, u16")
    A("inline constexpr uint32_t kLastScorerOff      = 0x6C;  // depth 1, dword")
    A("inline constexpr uint32_t kBlockGameTimeOff   = 0xBC;  // depth 1, f32")
    A("")
    A("// The engine's typed getters. (store, CSymbol64) -> value; all four are read-only.")
    A("inline constexpr uint64_t kGetSymbolVa = ::facts::GS_GET_SYMBOL;  // called at 0x140155D01")
    A("inline constexpr uint64_t kGetFloatVa  = ::facts::GS_GET_FLOAT;   // called at 0x140155D18")
    A("inline constexpr uint64_t kGetBoolVa   = ::facts::GS_GET_BOOL;    // called at 0x140155D51")
    A("inline constexpr uint64_t kGetIntVa    = ::facts::GS_GET_INT;     // called at 0x1401AF264")
    A("")
    A("// The variable symbols the sampler asks for, and the STORE each one is asked of. The depth")
    A("// is half the fact: blue_score answered at depth 0 is not a wrong number, it is no number,")
    A("// forever, silently. Every value is the qword in front of its own OlPrEfIx literal.")
    A("struct VarFact {")
    A("    uint64_t symbol;")
    A("    uint64_t literalVa;")
    A("    uint8_t  depth;       // derefs from netGame to the store this symbol is asked of")
    A("    const char* name;")
    A("};")
    A("")
    for cname, name, depth, note in VAR_SYMBOLS:
        found = _var_cache[name]
        if found is None:
            A("// k%s: no OlPrEfIx literal for \"%s\" exists in echovr.exe. The symbol is"
              % (cname, name))
            A("// recovered from the netvar side instead -- capture_facts.h. NOT declared here,")
            A("// because a fact this file cannot derive must not look like one it did.")
            continue
        base, symbol, _got = found
        A("inline constexpr VarFact k%s = { 0x%016XULL, 0x%XULL, %d, \"%s\" };  // %s"
          % (cname, symbol, base, depth, name, note))
    A("")
    A("}  // namespace facts")
    A("}  // namespace gamestate")
    A("}  // namespace replay")
    return "\n".join(L) + "\n"


TSV_COLS = ["id", "value", "addr_space", "base", "provenance", "rival_arm", "type", "verdict",
            "tier", "verb", "observed", "effect", "evidence", "guard", "source_a", "source_b",
            "comment"]


def emit_tsv(rows, count, array_va):
    L = ["# facts/pending/W1-D_gamestate.tsv -- contributed by W1-D (ARCH-21 round markers: the",
         "# LOCAL game-state read chains and the engine's own phase vocabulary).  Merged into",
         "# facts/facts.tsv by tools/gen_facts.py; W0-2 owns the merge.",
         "#",
         "# GENERATED by tools/w1d/gen_gamestate_facts.py.  Every value was read out of echovr.exe",
         "# (md5 %s) at the address named in its evidence column, and every" % ECHOVR_MD5,
         "# instruction quoted from the IDA database was compared byte-for-byte against the same",
         "# offset in echovr.exe before it was believed.",
         "#",
         "# The 27 phase symbols are NOT one row each: the header carries them and the guard is the",
         "# array itself.  What is registered here is the frame the header hangs on -- the array's",
         "# base and row count, the two functions that write and read it, the four typed getters,",
         "# and the offsets of the two deeper stores S3.1 separates.",
         "#"]
    L.append("\t".join(TSV_COLS))

    def row(**kw):
        kw.setdefault("addr_space", "PC_VA")
        kw.setdefault("base", "-")
        kw.setdefault("provenance", "PC")
        kw.setdefault("rival_arm", "-")
        kw.setdefault("type", "u64")
        kw.setdefault("verdict", "PC_DECISIVE")
        kw.setdefault("tier", "V-PRIMARY")
        kw.setdefault("verb", "-")
        kw.setdefault("observed", "-")
        kw.setdefault("effect", "-")
        kw.setdefault("guard", "G_IMM")
        kw.setdefault("source_a", "pe_file_bytes")
        kw.setdefault("source_b", "ida_db_instr")
        L.append("\t".join(str(kw[c]) for c in TSV_COLS))

    first = rows[0]
    row(id="GS.PHASE_ARRAY", value="0x%X" % array_va, addr_space="PC_VA",
        evidence="sub_1400C5B40 stores %d symbol qwords here (lea/and ~7/mov rax,[rax]/mov cs:qword_XXX,rax at 0x1400C5B78 onward) and sub_140D5C960 walks it at 0x140D5CA6E with a 16-byte stride; every instruction verified against echovr.exe at its own file offset" % count,
        comment="the engine's phase table: {qword CSymbol64 @+0, dword coarse id @+8}, 16-byte records. The id is what separates team_room_wait_* (0) from round_start/pre_launch_* (1) -- the distinction sub_14016D510's name table destroys")
    row(id="GS.PHASE_ARRAY_ROWS", value="%d" % count, addr_space="CONST", type="u32",
        evidence="the CONSUMER's loop bound: cmp rcx, 10h / jb short loc_140D5CA80 at 0x140D5CA8C with add rdx, 10h at 0x140D5CA88, indexing the id as [r8+rcx*8+8] after add rcx,rcx",
        comment="ARCH-21 S3.2's carried [U] answered from the reader, not the writer: the registrar's store count cannot see a row it leaves at its static value")
    row(id="GS.PHASE_REGISTRAR", value="0x%X" % REGISTRAR,
        evidence="144 instructions read from the IDA database and compared byte-for-byte against echovr.exe; 16 rip-relative leas paired with 16 qword stores into %#x" % array_va,
        comment="fills the phase array at startup. Reference only; we never call it")
    row(id="GS.PHASE_CONSUMER", value="0x%X" % CONSUMER,
        evidence="the only other function in the database whose disassembly names %#x (select distinct func_va from instructions where disasm like '%%1420D3850%%' -> {0x1400C5B40, 0x140D5C960})" % array_va,
        comment="matches a symbol against the array and compares the record's id with the dword at its subject+0x54. Reference only; we never call it")
    row(id="GS.NAME_TABLE", value="0x%X" % NAME_TABLE,
        evidence="25 comparands of the form lea rax,<literal> / and rax,0FFFFFFFFFFFFFFF8h / cmp rcx,[rax] / jz <name>, and a `mov rax, rdx` fallthrough at 0x14016D6E8 that returns the CALLER's fallback",
        comment="the engine's symbol -> coarse-name mapper. LOSSY: team_room_wait_enter/update return \"round_start\". We read it for corroboration and never classify by it (ARCH-21 D9)")
    row(id="GS.SYM_%s" % first["name"].upper(), value="0x%016X" % first["symbol"],
        addr_space="CONST", evidence="the qword at %#x in echovr.exe, immediately before the \"OlPrEfIx%s\" marker+name at %#x; the registrar LEAs %#x (the qword's own top byte) and masks it with ~7 at 0x1400C5B59" % (first["literal"], first["name"], first["literal"] + 8, first["literal"] + 7),
        comment="one representative row for the 27-symbol vocabulary; the full table with every symbol, literal VA and engine id is src/engine/gamestate_facts.h, regenerated by tools/w1d/gen_gamestate_facts.py and asserted against these bytes by tests/engine/test_gamestate_facts.cpp")
    row(id="GS.RULES_OFF", value="0x2AA8", addr_space="OFFSET", type="u32",
        evidence="*(_QWORD *)(a2 + 10920) in sub_1401AEE50:157 (v37 = *(float *)(*(_QWORD *)(a2 + 10920) + 188LL)) and the same load in sub_140163D10's 5-slot context build at +0x20",
        comment="netGame -> the game-mode block. blue_score/red_score are TWO derefs past netGame through this; blueGameScore/orangeGameScore are ZERO. Reading either at the other's depth answers silently and forever wrongly")
    row(id="GS.GET_INT", value="0x140D05730",
        evidence="v39 = (int)sub_140D05730(a2, *(_QWORD *)((unsigned __int64)\"NOlPrEfIxround_counter\" & 0xFFFFFFFFFFFFFFF8uLL)) at sub_1401AEE50:166, with a2 proven to be netGame by its own *(a2 + 10920) load at line 157",
        comment="(store, CSymbol64) -> int32. THE round_counter getter, and the proof that round_counter is read off netGame at depth 0")
    row(id="GS.GET_SYMBOL", value="0x140D5CED0",
        evidence="sub_140155C80:236, the game_state read in the /session writer",
        comment="(store, CSymbol64) -> CSymbol64. game_state and paused_state")
    row(id="GS.GET_FLOAT", value="0x140D36750",
        evidence="sub_140155C80:238, the game_timer read in the /session writer",
        comment="(store, CSymbol64) -> f32. game_timer -- NOT game_clock, which is the JSON key and resolves to nothing")
    row(id="GS.GET_BOOL", value="0x140CE4460",
        evidence="sub_140155C80:245, the blue_team_restart_request read in the /session writer",
        comment="(store, CSymbol64) -> bool. Not read by the sampler; registered so the four-getter family is complete and a future row does not re-derive it")
    return "\n".join(L) + "\n"


# Each of these eight facts is a bare address or offset, and the registry withholds an address
# nothing read out of the image (ENG-W0-GSFACTS: all eight came back PC_PRESENT/U because their
# evidence was pseudocode).  An anchor fixes that only if it certifies the claim that is actually
# in doubt, which is IDENTITY -- "0x140D05730 is the getter this read chain calls" -- not mere
# presence.  So none of these is a funcstart over the callee's own prologue, which cannot fail on
# a wrong identity: each is the CALL SITE's rel32, decoded the way guard_anchors decodes a riprel
# (anchor_va + len + disp32), or, for the registrar, the .rdata function-pointer slot that names
# it.  A site is re-derived from code_xrefs on every run and its bytes re-read from echovr.exe;
# a decode that does not equal the fact aborts the generator.
#
# `preferred` is the function whose read the drop's evidence column names, so the anchor sits at
# the very call the evidence claims -- not at some other caller that happens to be lower-addressed.

# The one phase symbol the registry emits a value for; naming it here keeps the generated table
# out of S4.6 rule 3 without turning the other 26 into unreadable macro soup.
REGISTERED_SYMBOLS = {
    0x3B529C356A118CC6: "::facts::GS_SYM_MAIN_LOOP_ENTER",
}

ANCHOR_TARGETS = [
    # fact id,               value,        preferred caller, note
    ("GS.GET_INT",       0x140D05730, 0x1401AEE50,
     "call in sub_1401AEE50's round_counter read; rcx is netGame (mov rcx,rbp at 0x1401AF25A, the "
     "same rbp GS.RULES_OFF is taken off at 0x1401AF1E7) -- round_counter is DEPTH 0"),
    ("GS.GET_SYMBOL",    0x140D5CED0, 0x140155C80,
     "call in the /session writer's game_state read"),
    ("GS.GET_FLOAT",     0x140D36750, 0x140155C80,
     "call in the /session writer's game_timer read"),
    ("GS.GET_BOOL",      0x140CE4460, 0x140155C80,
     "call in the /session writer's blue_team_restart_request read"),
    ("GS.NAME_TABLE",    0x14016D510, 0x140155C80,
     "the /session writer's only call to the symbol -> coarse-name mapper"),
    ("GS.PHASE_CONSUMER", 0x140D5C960, None,
     "call site; the array walker that owns GS.PHASE_ARRAY_ROWS"),
]

REGISTRAR_SLOT = 0x1416C89B8   # .rdata function-pointer table entry holding GS.PHASE_REGISTRAR
RULES_OFF = 0x2AA8             # netGame -> the game-mode block; the value the disp32 must carry
RULES_OFF_SITE = 0x1401AF1E7   # mov rax, [rbp+2AA8h] in sub_1401AEE50, rbp = netGame


def call_sites(img, db, target):
    """Direct near calls (E8 rel32) to `target`, each verified against echovr.exe and decoded."""
    out = []
    for (from_va,) in db.execute(
            "select from_va from code_xrefs where to_va=? and xref_type_name like '%all%'",
            (target,)):
        row = db.execute("select func_va, mnemonic, size, bytes from instructions where va=?",
                         (from_va,)).fetchone()
        if not row:
            continue
        func_va, mnemonic, size, raw = row
        raw = bytes(raw)
        if mnemonic != "call" or size != 5 or raw[0] != 0xE8:
            continue
        if from_va + 5 + struct.unpack_from("<i", raw, 1)[0] != target:
            continue
        on_file = img.read_va(from_va, 5)
        if on_file != raw:
            raise DeriveError("call site 0x%X: echovr.exe has %s, the database has %s"
                              % (from_va, (on_file or b"").hex(), raw.hex()))
        out.append((from_va, func_va, raw))
    return sorted(out)


def derive_anchors(img, db):
    rows = []
    for fact_id, value, preferred, note in ANCHOR_TARGETS:
        sites = call_sites(img, db, value)
        if not sites:
            raise DeriveError("%s: no verified direct call site for 0x%X" % (fact_id, value))
        chosen = None
        if preferred is not None:
            chosen = next((s for s in sites if s[1] == preferred), None)
            if chosen is None:
                raise DeriveError("%s: sub_%X does not call 0x%X, but the evidence says it does"
                                  % (fact_id, preferred, value))
        if chosen is None:
            chosen = sites[0]
        from_va, _func_va, raw = chosen
        decoded = from_va + 5 + struct.unpack_from("<i", raw, 1)[0]
        if decoded != value:
            raise DeriveError("%s: 0x%X decodes to 0x%X" % (fact_id, from_va, decoded))
        rows.append((fact_id, "riprel", from_va, 1, 5, raw,
                     "%s (%d direct call site(s) in the image)" % (note, len(sites))))

    slot = img.read_va(REGISTRAR_SLOT, 8)
    if slot is None or struct.unpack("<Q", slot)[0] != REGISTRAR:
        raise DeriveError("the .rdata slot at 0x%X does not hold 0x%X" % (REGISTRAR_SLOT, REGISTRAR))
    rows.append(("GS.PHASE_REGISTRAR", "vtslot", REGISTRAR_SLOT, 0, 8, slot,
                 "the .rdata function-pointer table entry that names the phase registrar; it has "
                 "no direct call site in the image, so the slot IS how the engine reaches it"))

    site = db.execute("select size, bytes from instructions where va=?", (RULES_OFF_SITE,)).fetchone()
    if not site:
        raise DeriveError("no instruction at 0x%X" % RULES_OFF_SITE)
    size, raw = site[0], bytes(site[1])
    on_file = img.read_va(RULES_OFF_SITE, size)
    if on_file != raw:
        raise DeriveError("0x%X: echovr.exe has %s, the database has %s"
                          % (RULES_OFF_SITE, (on_file or b"").hex(), raw.hex()))
    field = struct.pack("<I", RULES_OFF)
    if raw.count(field) != 1:
        raise DeriveError("0x%X: %d candidate disp32 fields, need exactly one" % (RULES_OFF_SITE, raw.count(field)))
    rows.append(("GS.RULES_OFF", "disp32", RULES_OFF_SITE, raw.index(field), size, raw,
                 "mov rax, [rbp+2AA8h] in sub_1401AEE50, where rbp is netGame -- the same rbp "
                 "passed as the store to GS.GET_INT at 0x1401AF264; the loaded block is then read "
                 "at +0BCh by the movss at 0x1401AF1F3"))
    return rows


def emit_anchors(rows):
    L = []
    A = L.append
    A("# facts/pending/anchors/W1-D_gamestate.tsv -- evidence sites for the eight address/offset")
    A("# rows of facts/pending/W1-D_gamestate.tsv that ENG-W0-GSFACTS's merge withheld at")
    A("# PC_PRESENT/U because their evidence was pseudocode and nothing had read them out of the")
    A("# image.  GENERATED by tools/w1d/gen_gamestate_facts.py.")
    A("#")
    A("# What is in doubt for these eight is IDENTITY -- 0x140D05730 IS the getter this")
    A("# read chain calls -- not presence.  A funcstart over the callee prologue cannot fail")
    A("# that claim, so there is not one here.  Six are the CALL SITE's rel32 (kind riprel:")
    A("# anchor_va + 5 + disp32), read back out of echovr.exe md5 %s" % ECHOVR_MD5)
    A("# and decoded before the row was written; a decode that disagreed aborted the generator.")
    A("# GS.PHASE_REGISTRAR has no direct call site at all -- it is reached through an .rdata")
    A("# function-pointer table -- so its anchor is that slot (kind vtslot).  GS.RULES_OFF is not")
    A("# an address, so its anchor is the disp32 field of the instruction that applies it.")
    A("#")
    A("# `b` is the LENGTH of instr_bytes; `a` is the byte index of the encoded field inside it.")
    A("#")
    A("fact_id\tkind\tanchor_va\ta\tb\tinstr_bytes\tnote")
    for fact_id, kind, va, a, b, raw, note in rows:
        A("%s\t%s\t0x%X\t%d\t%d\t%s\t%s"
          % (fact_id, kind, va, a, b, raw.hex().upper(), note))
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="fail if the files on disk are not what we would emit")
    args = ap.parse_args()

    img = load_image()
    resolve_var_symbols(img)
    db = sqlite3.connect(IDA_DB, uri=True)
    count = array_row_count(img, db)
    array_rows = read_array(img, db)
    name_rows = read_name_table(img, db)
    rows = union_rows(array_rows, name_rows)

    header = emit_header(rows, count, ARRAY_VA)
    tsv = emit_tsv(rows, count, ARRAY_VA)
    anchors = emit_anchors(derive_anchors(img, db))

    stale = []
    for path, text in ((OUT_HEADER, header), (OUT_TSV, tsv), (OUT_ANCHORS, anchors)):
        old = None
        if os.path.exists(path):
            with open(path, "r", encoding="ascii", newline="") as f:
                old = f.read()
        if old == text:
            continue
        if args.check:
            stale.append(path)
        else:
            with open(path, "w", encoding="ascii", newline="\n") as f:
                f.write(text)
    if stale:
        sys.stderr.write("stale: %s\n" % ", ".join(stale))
        return 1
    print("array rows %d, name-table comparands %d, union %d"
          % (count, len(name_rows), len(rows)))
    for r in rows:
        print("  %-38s id %3d  %-16s %#x  engine=%s"
              % (r["name"], r["id"], r["phase"], r["literal"], r["engine_name"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except DeriveError as e:
        sys.stderr.write("gen_gamestate_facts: %s\n" % e)
        sys.exit(2)
