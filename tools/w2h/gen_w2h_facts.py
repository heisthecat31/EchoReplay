#!/usr/bin/env python3
"""Emits facts/pending/W2-H.tsv and facts/pending/anchors/W2-H.tsv.

Every row below names an instruction site. The generator reads those bytes out of the shipped
echovr.exe, decodes the field the way the anchor guard will, and compares that decode against
the value the row claims. A row whose decode disagrees ABORTS the generator instead of being
written, so a hand-typed offset cannot reach the drop.
"""
import hashlib
import os
import struct
import sys

# ★ THE SCHEMA COMES FROM THE PARSER, NOT FROM A LIST KEPT HERE.
#
# This generator used to carry its own twelve-column header whose first field was
# `fact_id` -- the ANCHOR channel's first column, copied across from the wrong sibling.
# gen_facts.load() recognises a header ONLY by parts[0] == "id" and compares the whole
# line against COLS and three legacy lists by EXACT EQUALITY, so that header parsed as a
# data row, every row under it was measured against the 17-column schema, and all of them
# were dropped on the way in. The values were right the whole time and it made no
# difference: nothing they establish had ever reached a guard.
#
# A checker or an emitter that holds its own copy of a format can only ever agree with
# itself. So the column list, the verdicts, the tiers and the observed vocabulary are all
# read out of gen_facts here, and a row this file builds is asserted against them before
# it is written.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import gen_facts as G

from echovr_path import NOT_FOUND, find_echovr_exe  # noqa: E402

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
EXE = sys.argv[1] if len(sys.argv) > 1 else (find_echovr_exe() or sys.exit(NOT_FOUND))
OUT_FACTS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "facts", "pending", "W2-H.tsv")
OUT_ANCH = sys.argv[3] if len(sys.argv) > 3 else os.path.join(REPO, "facts", "pending", "anchors", "W2-H.tsv")
MD5 = "caa05ee3bca2f5df0809994e93e2cc05"
IMAGE_BASE = 0x140000000

raw = open(EXE, "rb").read()
got = hashlib.md5(raw).hexdigest()
if got != MD5:
    sys.exit("wrong binary: md5 %s" % got)

e_lfanew = struct.unpack_from("<I", raw, 0x3C)[0]
nsec = struct.unpack_from("<H", raw, e_lfanew + 6)[0]
opt_size = struct.unpack_from("<H", raw, e_lfanew + 20)[0]
sec0 = e_lfanew + 24 + opt_size
SECTIONS = []
for i in range(nsec):
    off = sec0 + 40 * i
    name = raw[off:off + 8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", raw, off + 8)
    SECTIONS.append((name, vaddr, vsize, raddr, rsize))


def file_off(va):
    rva = va - IMAGE_BASE
    for name, vaddr, vsize, raddr, rsize in SECTIONS:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            if rva - vaddr >= rsize:
                sys.exit("VA 0x%X is in %s but past its raw data" % (va, name))
            return raddr + (rva - vaddr)
    sys.exit("VA 0x%X is in no section" % va)


def bytes_at(va, n):
    o = file_off(va)
    return raw[o:o + n]


# fact_id, value, kind, anchor_va, a, blen, type, note
ROWS = [
    # ★ SOUND.SYSTEMS_PTR_VA IS NOT EMITTED, and this is not an omission.
    #
    # 0x1420A0470 is already in the registry as ENGINE.CENGINE_PTR, merged and V-PRIMARY,
    # anchored at four other `mov rax, cs:qword_1420A0470` sites (0x140517A9F, 0x140517AEC,
    # 0x140D6AAAD, 0x1409291D4). The riprel at 0x14106A627 this generator decoded is a FIFTH
    # read of that same global -- one engine systems table, indexed at SOUND.PLAYER_SLOT_OFF
    # for CSoundPlayer and at a different slot for the camera node manager.
    #
    # One fact, one id: the merged row wins, and src/audio/audio_facts.h cites
    # facts::ENGINE_CENGINE_PTR rather than restating the address. The name is wrong for a
    # table that is not the camera's, but renaming a merged V-PRIMARY row belongs to whoever
    # owns that id -- and the registry's own comment on that row records that it already
    # absorbed a second submission of the same address once ("also submitted as
    # CAM.NODEMGR_GLOBAL; one fact, one id").
    #
    # The collision was invisible until this file's rows became readable at all: while the
    # header was malformed nothing here reached a guard, so nothing could notice that two
    # packets had named one address.
    ("SOUND.PLAYER_SLOT_OFF", 0x2CF80, "disp32", 0x14106A633, 3, 7, "u32",
     "mov rcx,[rax+2CF80h] -- CSoundPlayer* is *(*(ENGINE.CENGINE_PTR, the merged id for 0x1420A0470) + this)"),
    ("SOUND.MEMO_COUNT_OFF", 0x30, "disp8", 0x14020EE52, 3, 4, "u32",
     "mov rbp,[rsi+30h] in SOUND.SETSTATE with rsi = soundPlayer + SOUND.MEMO_CACHE_OFF; the live entry count"),
    ("SOUND.EMITTER_LOOKUP", 0x140208FD0, "funcstart", 0x140208FD0, 0, 20, "u64",
     "CSoundPlayer::FindEmitter(this, u32 gameObj, bool lock); SOUND.SETSWITCH calls it before touching any memo"),
    ("SOUND.EMITTER_MAP_OFF", 0x60, "disp8", 0x140209024, 3, 4, "u32",
     "mov rdi,[rbx+60h] in SOUND.EMITTER_LOOKUP -- the sorted 16-byte id/emitter array"),
    ("SOUND.EMITTER_MAP_COUNT_OFF", 0x90, "disp32", 0x140209018, 3, 7, "u32",
     "mov rdx,[rbx+90h] in SOUND.EMITTER_LOOKUP -- entries in SOUND.EMITTER_MAP_OFF"),
    ("SOUND.EMITTER_LOCK_OFF", 0x58, "disp8", 0x140208FE8, 3, 4, "u32",
     "mov r9,[rcx+58h] in SOUND.EMITTER_LOOKUP -- the spin lock its third argument takes"),
    ("SOUND.EMITTER_SWITCH_MEMO_OFF", 0x78, "disp8", 0x14020EFDD, 3, 4, "u32",
     "lea rsi,[rax+78h] in SOUND.SETSWITCH -- the per-emitter switch memo, same container shape as the global state memo"),
    ("SOUND.DISABLE_FLAG_VA", 0x14201F2E0, "riprel", 0x14020EE1B, 2, 7, "u64",
     "cmp cs:dword_14201F2E0,0 -- nonzero makes SOUND.SETSTATE and SOUND.SETSWITCH return SUCCESS without acting"),
    ("SOUND.SAVELOAD_GLOBAL", 0x140401630, "funcstart", 0x140401630, 0, 20, "u64",
     "CSoundCS::SaveLoadGlobal, csoundcs.cpp:360: the engine's own serialiser for the state memo, whose load arm replays every pair through SOUND.SETSTATE"),
    ("VOIP.RECV_SEQ_OFF", 0x4, "disp8", 0x140D377E3, 4, 5, "u32",
     "movzx eax, word ptr [r13+4] in VOIP.RECEIVE; r13 is the third argument and +4 is the ONLY displacement read off it at any of its six sites"),
    ("AUDIO.AKAUDIOSETTINGS_RATE_OFF", 0x4, "disp8", 0x14138D23E, 2, 3, "u32",
     "mov [rcx+4],eax in exported GetAudioSettings -- uNumSamplesPerSecond; +0 is uNumSamplesPerFrame"),
    ("XAUDIO2.VOICE_DESTROYVOICE_OFF", 0x90, "disp32", 0x141401F3D, 2, 6, "u32",
     "call qword ptr [rax+90h] on the sink mastering voice in CAkSinkXAudio2::Term; DestroyVoice is IXAudio2Voice slot 18, so the shared base is exactly 19 slots wide"),
    ("XAUDIO2.IUNKNOWN_RELEASE_OFF", 0x10, "disp8", 0x141401F53, 2, 3, "u32",
     "call qword ptr [rax+10h] on the object at sink+AUDIO.SINK_IFACE_OFF_A; IUnknown::Release is slot 2, which is what makes that field a COM interface"),
]

# The guard each row declares, keyed on the anchor kind that actually checks it.
# gen_facts.claim_kind reads every one of these as a STATIC claim, which is what they make.
KIND_GUARD = {"disp8": "G_DISP", "disp32": "G_DISP", "imm32": "G_IMM",
              "riprel": "G_RIPREL", "funcstart": "G_FUNC_START"}

# ★★ WHAT THE ANCHOR IS WORTH, MIRRORING factcheck.anchor_verdict RATHER THAN GUESSING.
#
# That function returns ANCHOR_IDENTITY for every kind whose instruction USES the value in
# its claimed role -- a displacement, an immediate, a RIP-relative reference, a vtable slot,
# a function start -- and ANCHOR_CONTENT only for `u32_at` and a numeric `bytes_at`, where
# the bytes hold the value but nothing names the slot. gen_facts.derive_verdicts then maps
# identity to PC_DECISIVE/V-PRIMARY and content to PC_CORROBORATED/V.
#
# UNDER-CLAIMING IS A DEFECT TOO. A first pass put PC_PRESENT/U on all of these and the gate
# answered "verdict PC_PRESENT, but a binary guard reads this value at an attributable site"
# once per row -- withholding a value the binary demonstrably encodes, which is the same
# class of wrong answer as claiming one it does not.
IDENTITY_KINDS = ("disp8", "disp16", "disp32", "imm8", "imm16", "imm32", "riprel",
                  "instr_at", "funcstart", "vtslot")


def verdict_and_tier(kind):
    if kind in IDENTITY_KINDS:
        return "PC_DECISIVE", "V-PRIMARY"
    return "PC_CORROBORATED", "V"

facts_lines, anch_lines, bad = [], [], []
for fid, value, kind, ava, a, blen, ftype, note in ROWS:
    verdict, tier = verdict_and_tier(kind)
    b = bytes_at(ava, blen)
    if kind == "disp8":
        dec = b[a]
    elif kind in ("disp32", "imm32"):
        dec = struct.unpack_from("<I", b, a)[0]
    elif kind == "riprel":
        dec = ava + blen + struct.unpack_from("<i", b, a)[0]
    elif kind == "funcstart":
        dec = ava
    else:
        sys.exit("unknown kind " + kind)
    if dec != value:
        bad.append("%s: row says 0x%X, %s at 0x%X decodes 0x%X" % (fid, value, kind, ava, dec))
        continue
    anch_lines.append("\t".join([fid, kind, "0x%X" % ava, str(a), str(blen), b.hex().upper(),
                                 "W2-H: " + note]))
    # NO .PROLOGUE ANCHOR. It named a fact id this generator never emitted, so guard_anchors
    # would refuse it with "names a fact that is not in the registry" -- and a prologue row
    # cannot honestly cite a second artifact anyway. gen_facts.py --seed-prologues is the
    # sanctioned way to add one, run by the registry owner after these rows merge.
    addr_space = "PC_VA" if ftype == "u64" else "OFFSET"
    facts_lines.append("\t".join([
        fid, "0x%X" % value, addr_space, "-", "PC", "-", ftype,
        verdict, tier,
        # No verb, no observed, no effect: these rows assert an address or a displacement, not a
        # behaviour, and gen_facts is explicit that a row saying nothing about a verb is not
        # claiming one. The old drop said verb=read observed=reads_field -- an over-claim, and
        # `reads_field` is not in gen_facts.OBSERVED at all.
        "-", "-", "-",
        "%s at 0x%X in echovr.exe md5 %s, decoded and compared by tools/w2h/gen_w2h_facts.py"
        % (kind, ava, MD5),
        KIND_GUARD[kind],
        # One artifact read directly, and one hand transcription of it: every value here is
        # restated as a literal in src/audio/audio_facts.h beside the site it was decoded from,
        # which is what hand_struct_decl names. It is fabricatable, so it corroborates and never
        # certifies; pe_file_bytes is the unfabricatable static half rule 4 requires.
        "pe_file_bytes", "hand_struct_decl",
        note]))

if bad:
    sys.exit("ABORT -- %d row(s) disagree with the binary:\n  %s" % (len(bad), "\n  ".join(bad)))

HDR_F = """# facts/pending/W2-H.tsv -- W2-H: the non-exported numbers the audio subsystem needs.
#
# The design says the audio control surface is offset-free because every AK:: entry point
# is a PE export resolved by name. That holds, and it is why this file is short: the exports are
# NOT listed here. A name is pinned by tests/audio's export check, which parses echovr.exe's
# export directory and fails when a name akbind asks for is absent -- an address in this file
# would be a second, weaker statement of the same fact.
#
# What is here is what is not exported and not already in facts.tsv: how to reach CSoundPlayer,
# the shape of the S10.2 / N-16 memo, the emitter table the per-object switch memo hangs off, the
# flag that makes both setters silently succeed, and the two XAudio2 vtable slots the sink itself
# calls -- which is what turns the SetFrequencyRatio slot into a checkable relationship instead of
# a transcribed 0xD0.
#
# Every value was decoded out of the shipped bytes by tools/w2h/gen_w2h_facts.py and compared
# against the row before the row was written. Re-run it to reproduce this file byte for byte.
#
# THE COLUMNS ARE gen_facts.COLS, READ OUT OF THE PARSER. This file previously carried a
# 12-column header of its own invention whose first field was `fact_id` -- the ANCHOR
# channel's first column. gen_facts.load() recognises a header only by parts[0] == "id",
# so that line parsed as DATA, every row under it was measured against the 17-column
# schema, and all 15 were dropped on the way in. The values were right the whole time and
# it made no difference: nothing they establish had ever reached a guard.
#
""" + "\t".join(G.COLS) + """
"""
HDR_A = """# facts/pending/anchors/W2-H.tsv -- evidence sites for facts/pending/W2-H.tsv.
# Generated by tools/w2h/gen_w2h_facts.py, which decodes each site out of echovr.exe md5
# caa05ee3bca2f5df0809994e93e2cc05 and aborts rather than emit a row whose decode disagrees
# with the value claimed.
#
# `b` is the LENGTH of instr_bytes; `a` is the byte index of the encoded field inside them.
#
fact_id\tkind\tanchor_va\ta\tb\tinstr_bytes\tnote
"""
# ★ ASK THE PARSER, do not trust this file. Every row is measured against gen_facts's own
# column list and vocabularies before anything is written, so a schema change there reddens
# this generator instead of silently producing another drop nothing can read.
for line in facts_lines:
    parts = line.split("\t")
    if len(parts) != len(G.COLS):
        sys.exit("ABORT -- row %s has %d columns, gen_facts.COLS has %d"
                 % (parts[0], len(parts), len(G.COLS)))
    r = dict(zip(G.COLS, parts))
    for col, allowed in (("verdict", G.VERDICTS), ("tier", G.TIERS), ("type", G.TYPES),
                         ("addr_space", G.ADDR_SPACES), ("provenance", G.PROVENANCES),
                         ("verb", G.VERBS), ("observed", G.OBSERVED)):
        if r[col] not in allowed:
            sys.exit("ABORT -- %s: %s %r is not in gen_facts.%s"
                     % (r["id"], col, r[col], col.upper()))
    for col in ("source_a", "source_b"):
        if r[col] not in G.load_sources():
            sys.exit("ABORT -- %s: %s %r is not in facts/sources.tsv" % (r["id"], col, r[col]))

open(OUT_FACTS, "w", newline="\n").write(HDR_F + "\n".join(facts_lines) + "\n")
open(OUT_ANCH, "w", newline="\n").write(HDR_A + "\n".join(anch_lines) + "\n")
print("wrote %d facts, %d anchors -- every value decoded from the binary and matched"
      % (len(facts_lines), len(anch_lines)))
