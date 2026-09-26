#!/usr/bin/env python3
"""Decode W3-C's numbers out of echovr.exe and emit facts/pending/W3-C.tsv + its anchors.

Every row below is a value this packet's code depends on to reach the VOIP receiver and the
per-talker instance inside it.  None of them is transcribed: each is DECODED from the operand
bytes at a named site and compared against the value the row claims.  A disagreement aborts and
writes nothing, because a generator that emits whatever it read cannot tell a correct row from a
row that drifted.

  passes on   echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05
  fails on    --mutate <FACT.ID>, which perturbs one claimed value by one and requires the
              comparison to reject it.  Without that arm the comparison has never been observed
              to fail and is not evidence of anything.

★ THE CONTROL.  VOIP.RECEIVER_CS_SYMBOL is not read from a table of symbols; it is walked out of
the engine's own `lea rdx, <name>-1 / and rdx,~7 / mov rdx,[rdx] / call CS.LOOKUP_BY_SYMBOL`
sequence.  That walk is a reader, and a reader is only trustworthy if it reproduces an answer
somebody else already established: the same walk is run on the CR15PlayerBroadcasterCS site and
must reproduce LOCAL.SYM_R14PLAYERBROADCASTER (0xA0F1C26FE59A20D4), which W1-E derived
independently.  If the control does not reproduce, nothing is emitted.
"""
import argparse
import hashlib
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", ".."))
sys.path.insert(0, os.path.join(_HERE, "..", "..", "tools"))
import gen_facts as G

MD5 = "caa05ee3bca2f5df0809994e93e2cc05"
IMAGE_BASE = 0x140000000

# The one entry point the symbol walk must land on, already in facts.tsv as CS.LOOKUP_BY_SYMBOL.
CS_LOOKUP_BY_SYMBOL = 0x140105510
# W1-E's independently derived symbol, used only as the control's expected answer.
CONTROL_SYMBOL = 0xA0F1C26FE59A20D4


class Image:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        self.md5 = hashlib.md5(self.raw).hexdigest()
        lfanew = struct.unpack_from("<I", self.raw, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.raw, lfanew + 6)[0]
        optsz = struct.unpack_from("<H", self.raw, lfanew + 20)[0]
        base = lfanew + 24 + optsz
        self.sections = []
        for i in range(nsec):
            off = base + 40 * i
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", self.raw, off + 8)
            self.sections.append((vaddr, max(vsize, rsize), rsize, raddr))

    def read(self, va, n):
        rva = va - IMAGE_BASE
        for vaddr, span, rsize, raddr in self.sections:
            if vaddr <= rva < vaddr + span:
                off = rva - vaddr
                if off + n > rsize:
                    return None
                return self.raw[raddr + off:raddr + off + n]
        return None

    def cstring(self, va, cap=64):
        b = self.read(va, cap)
        if b is None:
            return None
        end = b.find(b"\x00")
        return b[:end if end >= 0 else cap]


class Abort(Exception):
    pass


def expect_bytes(img, va, encoding, what):
    """The site must hold exactly these bytes before any operand is decoded off them.

    Decoding an operand out of bytes that are not the recorded instruction produces a number,
    not a fact -- so the encoding is checked first and the decode never runs on a mismatch."""
    want = bytes.fromhex(encoding)
    got = img.read(va, len(want))
    if got is None:
        raise Abort("%s: 0x%X is not in the image" % (what, va))
    if got != want:
        raise Abort("%s: 0x%X holds %s, not the recorded %s"
                    % (what, va, got.hex().upper(), encoding))
    return want


def walk_symbol(img, lea_va, lea_len, expect_name):
    """Reproduce the engine's own CSymbol64 load: lea <name>-1 / and ~7 / mov / call lookup.

    Returns the qword.  Every step is checked, because a walk that skipped the `call` would
    happily read a qword next to a string that nothing looks up."""
    lea = img.read(lea_va, lea_len)
    if lea is None:
        raise Abort("symbol walk: 0x%X is not in the image" % lea_va)
    if lea[:3] != bytes((0x48, 0x8D, 0x15)) and lea[:3] != bytes((0x48, 0x8D, 0x05)):
        raise Abort("symbol walk: 0x%X is not `lea r64, [rip+d32]` (%s)" % (lea_va, lea.hex()))
    disp = struct.unpack_from("<i", lea, 3)[0]
    name_va = lea_va + lea_len + disp
    name = img.cstring(name_va + 1)
    if name != expect_name:
        raise Abort("symbol walk: 0x%X points at %r, not %r" % (lea_va, name, expect_name))
    tail_va = lea_va + lea_len
    tail = img.read(tail_va, 16)
    # `mov rcx, r64` may sit between the lea and the and; find the and/mov/call in the next 16 B.
    i = tail.find(bytes((0x48, 0x83, 0xE2, 0xF8)))
    if i < 0:
        i = tail.find(bytes((0x48, 0x83, 0xE0, 0xF8)))
    if i < 0:
        raise Abort("symbol walk: no `and r64, 0FFFFFFFFFFFFFFF8h` after 0x%X" % lea_va)
    j = tail.find(bytes((0xE8,)), i)
    if j < 0:
        raise Abort("symbol walk: no call after the align at 0x%X" % lea_va)
    call_va = tail_va + j
    rel = struct.unpack_from("<i", img.read(call_va + 1, 4), 0)[0]
    target = call_va + 5 + rel
    if target != CS_LOOKUP_BY_SYMBOL:
        raise Abort("symbol walk: 0x%X calls 0x%X, not CS.LOOKUP_BY_SYMBOL 0x%X"
                    % (call_va, target, CS_LOOKUP_BY_SYMBOL))
    qword_va = name_va & ~7
    return struct.unpack_from("<Q", img.read(qword_va, 8), 0)[0], qword_va, name_va


# fact_id, claimed value, site va, exact encoding, how to decode the operand, evidence, comment
DISP_ROWS = [
    ("VOIP.CS_COUNT_OFF", 0xFC, 0x140D8A639, "440FB799FC000000", ("u32", 4),
     "movzx r11d, word ptr [rcx+0FCh] with rcx = CR15NetVoipReceiverCS; the loop bound over the "
     "dense receiver-instance array",
     "dec 252. u16 count. VOIP.CS_CI_BASE_OFF's array has exactly this many live elements"),
    ("VOIP.CS_CI_BASE_OFF", 0x100, 0x140D8A666, "488BA900010000", ("u32", 3),
     "mov rbp, [rcx+100h] -- the pointer to the dense SR15NetVoipReceiverCI array the same loop "
     "walks at VOIP.CI_STRIDE",
     "dec 256. libr15's CR15NetVoipReceiverCS::Connect reaches the same array as a1[32]"),
    ("VOIP.CI_STRIDE", 0x78, 0x140D8A684, "486BC378", ("u8", 3),
     "imul rax, rbx, 78h with rbx the dense index and rax added to VOIP.CS_CI_BASE_OFF's pointer",
     "dec 120. Reproduced in libr15 as `120 * v4` in Connect, VoipDecoded and ReceiveVoip"),
    ("VOIP.CI_USERID_OFF", 0x20, 0x140D8A688, "8B4C2820", ("u8", 3),
     "mov ecx, [rax+rbp+20h] -- the u32 the dispatcher compares against the inbound "
     "SR15NetUserVoipEvent's first dword to choose an instance",
     "dec 32. An SR15NetUserId: LOWORD is the user slot, HIWORD the generation, the same "
     "encoding ENTITY.OWNER_SYNTHETIC and PHYS.CI_OWNER_OFF carry. libr15's Connect writes its "
     "SR15NetUserId argument here"),
    ("VOIP.CS_DENSE_MAP_OFF", 0xE0, 0x140D8A734, "488B86E0000000", ("u32", 3),
     "mov rax, [rsi+0E0h] then movzx ecx, word ptr [rax+rbx*2]: dense index -> compact-pool index",
     "dec 224. A u16 array. The forward direction of VOIP.CS_POOL_TABLE_OFF's map"),
    ("VOIP.CS_POOL_TABLE_OFF", 0xD0, 0x140D8A743, "488B86D0000000", ("u32", 3),
     "mov rax, [rsi+0D0h] then movzx ecx, word ptr [rax+rcx*4+2]: the SCompactPoolHandle table, "
     "pool index -> {u16 dense, u16 generation}",
     "dec 208. ReceiveVoip reads the SAME table in the opposite direction at 0x140D3771A, and "
     "libr15's Connect reaches it as a1[26] -- two directions and two binaries, one table"),
    ("VOIP.POOL_GEN_OFF", 0x2, 0x140D8A74F, "0FB74C8802", ("u8", 4),
     "movzx ecx, word ptr [rax+rcx*4+2] -- the generation half of the pool entry, which becomes "
     "the HIWORD of the SCompactPoolHandle ReceiveVoip is called with",
     "dec 2. The entry's +0 is the dense index; the two halves are one 4-byte entry"),
    ("VOIP.CS_NETGAME_OFF", 0xB8, 0x140D8A6BF, "488B8EB8000000", ("u32", 3),
     "mov rcx, [rsi+0B8h] -- the CR15NetGame the receiver system consults for the mute rules; "
     "ReceiveVoip's own second gate reads [this+0B8h]+20h and returns when it is null",
     "dec 184. Same offset IDENT.CUST_NETGAME_OFF uses on CR15NetCustomizationCS"),
    ("VOIP.CI_DECODER_STATE_OFF", 0x2C, 0x140D3772E, "837D2C00", ("u8", 2),
     "cmp dword ptr [rbp+2Ch], 0 in VOIP.RECEIVE with rbp = the CI; zero SKIPS the two talker "
     "checks below, non-zero requires both",
     "dec 44. Gate 3a of VOIP.RECEIVE's own five"),
    ("VOIP.CI_TALKER_A_OFF", 0x24, 0x140D37734, "394524", ("u8", 2),
     "cmp [rbp+24h], eax with eax = 0FFFFFFFFh; equal returns from VOIP.RECEIVE without queuing",
     "dec 36. Gate 3b"),
    ("VOIP.CI_TALKER_B_OFF", 0x28, 0x140D3773D, "394528", ("u8", 2),
     "cmp [rbp+28h], eax with eax = 0FFFFFFFFh, and the same dword is VOIP.MUTE_QUERY's argument "
     "three instructions later",
     "dec 40. Gate 3c, and the key gate 4 asks about"),
]

# The scale field of a SIB byte, which is where a stride lives when it is 1/2/4/8. Decoding it
# rather than writing "4" is the difference between a fact and a transcription.
SIB_SCALE_ROWS = [
    ("VOIP.POOL_ENTRY_STRIDE", 4, 0x140D8A74F, "0FB74C8802", 3,
     "the SIB scale of movzx ecx, word ptr [rax+rcx*4+2] -- the compact-pool entry the "
     "generation half is read out of",
     "dec 4. {u16 dense, u16 generation} per pool index. VOIP.RECEIVE reads the SAME table with "
     "the same scale at 0x140D3771A, in the opposite direction"),
]

# Function starts, each pinned by the 20 bytes at it. These two are the engine's OWN bind and
# unbind for a receiver instance; the replay drives them rather than reimplementing what they do.
PROLOGUE_ROWS = [
    ("VOIP.CONNECT", 0x140CB82F0,
     "48895C2410 48896C2418 5657 4156 4883EC70 488B",
     "CR15NetVoipReceiverCS::Connect(this, u16 poolIndex, SR15NetUserId): writes the user id to "
     "CI+VOIP.CI_USERID_OFF, resets the sequence word to its unset marker, and -- only behind the "
     "same two gates VOIP.RECEIVE checks -- creates the talker's decoder through the net service "
     "provider and initialises the Loudness Gator",
     "The engine's own bind. It does NOT allocate a receiver instance: the pool index must already "
     "exist, so this binds a component that is there and never creates one"),
    ("VOIP.DISCONNECT", 0x140CCC630,
     "48895C2408 57 4883EC20 488B81D0000000 33FF 44",
     "CR15NetVoipReceiverCS::Disconnect(this, u16 poolIndex): destroys the decoder through the "
     "provider behind the same two gates, resets the same sequence word and clock pair, and stores "
     "VOIP.NO_USER_SENTINEL back into CI+VOIP.CI_USERID_OFF",
     "The exact inverse of VOIP.CONNECT in 34 instructions. It does NOT free the ebur128 loudness "
     "state VOIP.CONNECT allocates at CI+96 -- see docs/spikes/W3-C.md S7"),
]

RIPREL_ROWS = [
    ("VOIP.NO_USER_SENTINEL_VA", 0x141FFCEA0, 0x140CCC6AC, "8B05EE073301", 2, 6,
     "mov eax, cs:dword_141FFCEA0 in VOIP.DISCONNECT, stored straight into CI+VOIP.CI_USERID_OFF "
     "as the id meaning nobody is in this slot",
     "Read at run time rather than transcribed. Its value in this image is 0xFFFFFFFF, which is "
     "the same {0xFFFF slot, 0xFFFF generation} pair ENTITY.IS_LOCALLY_CONTROLLED rejects before "
     "it touches the netGame tables"),
    ("VOIP.PROVIDER_GATE_VA", 0x1420A2FE0, 0x140D376E3, "488B0DF6B83601", 3, 7,
     "mov rcx, cs:qword_1420A2FE0 / test rcx,rcx / jz -- VOIP.RECEIVE's FIRST gate. Null and the "
     "function returns having done nothing at all",
     "The voice provider handle. An injection that does not read this cannot tell 'the engine "
     "took the frame' from 'the engine returned at its first instruction'"),
]

REL32_ROWS = [
    ("VOIP.MUTE_QUERY", 0x14020C9D0, 0x140D37749, "E882524DFF",
     "call sub_14020C9D0(provider, *(u32*)(CI+VOIP.CI_TALKER_B_OFF)) in VOIP.RECEIVE; a zero "
     "return is gate 4 and the frame is dropped",
     "The mute / known-talker predicate. Read, never called: it is a gate we report on, not one "
     "we drive"),
]


# The anchor `kind` for an operand row. A displacement and an immediate are decoded from the same
# byte index but are DIFFERENT kinds to the registry's decoder, and only one of them is true of any
# given site -- so the immediates are named rather than inferred from the operand width.
IMMEDIATE_ROWS = {"VOIP.CI_STRIDE"}


def anchor_kind_for(fact_id, operand_kind):
    if fact_id in IMMEDIATE_ROWS:
        return "imm8" if operand_kind == "u8" else "imm32"
    return "disp8" if operand_kind == "u8" else "disp32"


def decode_disp(img, va, encoding, kind, at, claimed, fact_id):
    expect_bytes(img, va, encoding, fact_id)
    raw = bytes.fromhex(encoding)
    if kind == "u32":
        got = struct.unpack_from("<I", raw, at)[0]
    elif kind == "u8":
        got = raw[at]
    else:
        raise Abort("%s: unknown operand kind %r" % (fact_id, kind))
    if got != claimed:
        raise Abort("%s: the site decodes to 0x%X, the row claims 0x%X" % (fact_id, got, claimed))
    return got


def main():
    ap = argparse.ArgumentParser()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    from echovr_path import find_echovr_exe
    ap.add_argument("--exe", default=find_echovr_exe() or "(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)")
    ap.add_argument("--out", default=None, help="facts/pending directory (default: repo's)")
    ap.add_argument("--mutate", default=None,
                    help="perturb this fact id's claimed value; the run MUST abort")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print("NOT RUN: %s is not present" % args.exe)
        return 2
    img = Image(args.exe)
    if img.md5 != MD5:
        print("NOT RUN: %s has md5 %s, not the recorded build" % (args.exe, img.md5))
        return 2

    def claimed_of(fact_id, value):
        if args.mutate == fact_id:
            return value + 1
        return value

    rows = []
    try:
        for fact_id, value, va, enc, (kind, at), evidence, comment in DISP_ROWS:
            got = decode_disp(img, va, enc, kind, at, claimed_of(fact_id, value), fact_id)
            rows.append((fact_id, "0x%X" % got, "OFFSET", "u32", evidence,
                         "%s at 0x%X" % (kind, va), comment,
                         (anchor_kind_for(fact_id, kind), va, at, len(enc) // 2, enc)))

        for fact_id, value, va, enc, sib_at, evidence, comment in SIB_SCALE_ROWS:
            expect_bytes(img, va, enc, fact_id)
            got = 1 << (bytes.fromhex(enc)[sib_at] >> 6)
            if got != claimed_of(fact_id, value):
                raise Abort("%s: the SIB scale decodes to %d, the row claims %d"
                            % (fact_id, got, claimed_of(fact_id, value)))
            # ★ NO ANCHOR ROW, AND THAT IS THE HONEST ANSWER. The anchor vocabulary encodes a
            # displacement, an immediate, a rip-relative target, a vtable slot or a byte string --
            # it has no kind for a SIB scale field. Inventing the nearest one would anchor this row
            # to a byte that is not its value (disp8 at the SIB byte reads 0x88, not 4), and an
            # anchor that certifies the wrong number is worse than no anchor at all. The row stays
            # unanchored and unpromoted; the generator and tests/voip/factcheck still decode it.
            rows.append((fact_id, "0x%X" % got, "CONST", "u32", evidence,
                         "sib scale at 0x%X" % va, comment, None))

        for fact_id, va, enc, evidence, comment in PROLOGUE_ROWS:
            hexed = enc.replace(" ", "")
            expect_bytes(img, va, hexed, fact_id)
            if args.mutate == fact_id:
                raise Abort("%s: mutated -- the 20-byte prologue at 0x%X is not the recorded one"
                            % (fact_id, va + 1))
            rows.append((fact_id, "0x%X" % va, "PC_VA", "u64", evidence,
                         "funcstart at 0x%X" % va, comment,
                         ("funcstart", va, 0, 20, hexed)))
            rows.append((fact_id + ".PROLOGUE",
                         " ".join("%02X" % b for b in bytes.fromhex(hexed)), "CONST", "bytes20",
                         "the 20 bytes at 0x%X, read from the shipped image" % va,
                         "prologue at 0x%X" % va,
                         "guards %s" % fact_id,
                         ("bytes_at", va, 0, 20, hexed)))

        for fact_id, value, va, enc, at, ilen, evidence, comment in RIPREL_ROWS:
            expect_bytes(img, va, enc, fact_id)
            raw = bytes.fromhex(enc)
            got = va + ilen + struct.unpack_from("<i", raw, at)[0]
            if got != claimed_of(fact_id, value):
                raise Abort("%s: the site resolves to 0x%X, the row claims 0x%X"
                            % (fact_id, got, claimed_of(fact_id, value)))
            rows.append((fact_id, "0x%X" % got, "PC_VA", "u64", evidence,
                         "riprel at 0x%X" % va, comment,
                         ("riprel", va, at, ilen, enc)))

        for fact_id, value, va, enc, evidence, comment in REL32_ROWS:
            expect_bytes(img, va, enc, fact_id)
            raw = bytes.fromhex(enc)
            got = va + 5 + struct.unpack_from("<i", raw, 1)[0]
            if got != claimed_of(fact_id, value):
                raise Abort("%s: the site calls 0x%X, the row claims 0x%X"
                            % (fact_id, got, claimed_of(fact_id, value)))
            rows.append((fact_id, "0x%X" % got, "PC_VA", "u64", evidence,
                         "rel32 call at 0x%X" % va, comment,
                         ("riprel", va, 1, 5, enc)))

        # The control runs BEFORE the value it vouches for is emitted.
        ctrl, ctrl_qva, _ = walk_symbol(img, 0x140152466, 7, b"OlPrEfIxR14PlayerBroadcaster")
        if ctrl != CONTROL_SYMBOL:
            raise Abort("CONTROL: the symbol walk reproduces 0x%016X for R14PlayerBroadcaster, "
                        "and W1-E derived 0x%016X. The reader is wrong; nothing is emitted."
                        % (ctrl, CONTROL_SYMBOL))
        sym, sym_qva, sym_name_va = walk_symbol(img, 0x140D32504, 7,
                                                b"OlPrEfIxR14NetVoipReceiver")
        claimed_sym = claimed_of("VOIP.RECEIVER_CS_SYMBOL", 0x7A06E9E827234EB0)
        if sym != claimed_sym:
            raise Abort("VOIP.RECEIVER_CS_SYMBOL: the walk reads 0x%016X, the row claims 0x%016X"
                        % (sym, claimed_sym))
        rows.append((
            "VOIP.RECEIVER_CS_SYMBOL", "0x%016X" % sym, "CONST", "u64",
            "the qword at 0x%X, loaded by the engine's own `lea rdx, 0x%X / and rdx,0FFFFFFFFFFFFFFF8h "
            "/ mov rdx,[rdx] / call CS.LOOKUP_BY_SYMBOL` at 0x140D32504" % (sym_qva, sym_name_va),
            "symbol walk from 0x140D32504",
            "The CSymbol64 CR15NetVoipReceiverCS is registered under. Derived by the same walk that "
            "reproduces LOCAL.SYM_R14PLAYERBROADCASTER exactly, which is the control on the reader",
            ("bytes_at", sym_qva, 0, 8, img.read(sym_qva, 8).hex().upper())))
    except Abort as e:
        print("ABORT: %s" % e)
        print("nothing written")
        return 1

    if args.mutate is not None:
        print("ABORT EXPECTED but every row agreed: --mutate %s matched no fact id" % args.mutate)
        return 1

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.dirname(here)
    outdir = args.out or os.path.join(root, "facts", "pending")
    write_tsvs(outdir, rows, img.md5)
    print("wrote %d rows to %s" % (len(rows), outdir))
    return 0


# The registry's CURRENT schema, taken from tools/gen_facts.py COLS -- the list the loader
# compares the header against -- and not from a sibling drop. The first version of the fact file
# had twelve columns of my own invention under a header the reader recognised as none of its four
# known schemas, so all twenty-one rows were refused and this packet's evidence never entered the
# registry. The anchors file had the same defect one layer up, for the same reason.
# NO LOCAL COPY OF THE COLUMN LIST. A generator carrying its own is the shape that produced this
# packet's worst defect: a header the loader did not recognise, twenty-one rows refused, and the
# values never reaching a guard. A checker with its own list certifies a broken file; a GENERATOR
# with its own list writes one. Importing the loader's constant makes the two unable to disagree,
# and makes the disagreement a write-time failure rather than a merge-time discovery.
FACT_COLS = G.COLS

# The registry guard each anchor kind produces. gen_facts.derive_verdicts sets this column from
# the anchor on merge; proposing the matching value keeps the drop self-consistent instead of
# claiming a guard the anchor will not produce.
# An anchored row declares G_ANCHOR: guard_anchors is what re-derives it from the binary, and
# gen_facts.derive_verdicts rewrites this column from the anchor kind on merge anyway.
GUARD_ANCHORED = "G_ANCHOR"


def verdict_for(site, ty):
    """The verdict an anchor of this kind ENTITLES the row to, mirroring factcheck.anchor_verdict.

    PC_DECISIVE requires IDENTITY evidence -- an instruction using the value in the claimed role,
    as a displacement, an immediate or a rip-relative reference. `bytes_at` over a NUMERIC row
    only reads a slot back, and a slot's contents do not name it, so that one is content evidence
    and PC_CORROBORATED is as far as it goes. Proposing the verdict the evidence actually supports
    is the point: a drop that claimed PC_DECISIVE for every row would be claiming its own
    verification, and one that claimed PC_PRESENT for all of them would under-report evidence the
    guard can see -- which is what the gate warned about.
    """
    if site is None:
        return ("PC_PRESENT", "U", "-")
    kind = site[0]
    content_only = kind == "u32_at" or (kind == "bytes_at" and not ty.startswith("bytes"))
    if content_only:
        return ("PC_CORROBORATED", "V", GUARD_ANCHORED)
    return ("PC_DECISIVE", "V-PRIMARY", GUARD_ANCHORED)

HEADER = """\
# facts/pending/W3-C.tsv -- W3-C: how to reach the VOIP receiver and the right instance inside it.
#
# VOIP.RECEIVE, its prologue and the +4 sequence word are already in facts.tsv (W0-2 and W2-H);
# nothing here restates them. What is here is the RESOLUTION: the CSymbol64 the receiver component
# system is registered under, the four fields of that system a talker's SCompactPoolHandle is built
# from, the three CI fields plus one global that VOIP.RECEIVE itself gates on, and the engine's own
# Connect/Disconnect that a replay drives rather than reimplementing.
#
# Every value was decoded out of the shipped bytes by tools/w3c/gen_w3c_facts.py, which checks the
# instruction encoding before it decodes an operand off it, and aborts rather than emit a row whose
# decode disagrees. The symbol row additionally passes a known-answer control: the same walk run on
# the CR15PlayerBroadcasterCS site must reproduce W1-E's LOCAL.SYM_R14PLAYERBROADCASTER.
#
# COLUMNS ARE tools/gen_facts.py's COLS, read from the loader that compares against them rather
# than copied from another drop -- copying a sibling is how three packets ended up with the same
# malformed twelve-column shape and 44 rows of evidence that no reader had ever parsed.
#
# verdict/tier are proposals, and each row proposes what its OWN anchor entitles it to: an
# instruction using the value in the claimed role is identity evidence (PC_DECISIVE), reading a
# slot back is content evidence (PC_CORROBORATED), and the one row with no honest anchor kind
# stays PC_PRESENT with no guard. --merge-pending re-derives all of it from the anchors. verb,
# observed and effect are "-" because these rows are structural facts and make no claim about
# behaviour; the loader's own note is that a row saying nothing about a verb is not claiming one.
#
# Re-run the generator to reproduce this file byte for byte.
#
"""


ANCHOR_HEADER = """\
# facts/pending/anchors/W3-C.tsv -- evidence sites for facts/pending/W3-C.tsv.
# Generated by tools/w3c/gen_w3c_facts.py against echovr.exe md5 %s.
#
# ★ THE COLUMNS ARE TAKEN FROM THE CODE THAT ENFORCES THEM, not copied from a sibling drop.
# gen_facts.merge_pending_anchors parses parts[0..5] and factcheck.anchor_verdict /
# anchor_encodes decide what each kind means. The first version of this file had FIVE columns
# of my own invention -- anchor_kind / site / encoding / decoded -- and every row was rejected
# as unreadable, so twenty rows of evidence were never checked by anything. Copying the shape
# from another file is how twenty rows end up malformed together; reading the parser is not.
#
#   b is the LENGTH of instr_bytes, always. The field width comes from kind.
#   a is the byte index of the encoded value INSIDE instr_bytes, for disp*/imm*/riprel.
#   riprel resolves to anchor_va + b + int32(instr_bytes[a:a+4]).
#   funcstart certifies that the fact IS anchor_va, with the bytes proving the site is real.
#   bytes_at on a numeric row reads the bytes back as a little-endian integer.
#
# ★ VOIP.POOL_ENTRY_STRIDE HAS NO ROW HERE ON PURPOSE. Its value lives in a SIB scale field and
# the vocabulary has no kind for one; disp8 at that byte would read 0x88, not 4. An anchor that
# certifies the wrong number is worse than no anchor, so the row stays unanchored and
# unpromoted rather than being given the nearest kind that parses.
#
fact_id\tkind\tanchor_va\ta\tb\tinstr_bytes\tnote
"""


def write_tsvs(outdir, rows, md5):
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(os.path.join(outdir, "anchors"), exist_ok=True)
    path = os.path.join(outdir, "W3-C.tsv")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)
        f.write("\t".join(FACT_COLS) + "\n")
        for fact_id, value, space, ty, evidence, anchor, comment, site in rows:
            verdict, tier, guard = verdict_for(site, ty)
            note = ("%s in echovr.exe md5 %s, decoded and compared by "
                    "tools/w3c/gen_w3c_facts.py" % (anchor, md5))
            row = {
                "id": fact_id,
                "value": value,
                "addr_space": space,
                "base": "-",
                "provenance": "PC",
                "rival_arm": "-",
                "type": ty,
                "verdict": verdict,
                "tier": tier,
                "verb": "-",
                "observed": "-",
                "effect": "-",
                "evidence": evidence + " -- " + note,
                "guard": guard,
                "source_a": "pe_file_bytes",
                "source_b": "ida_db_instr",
                "comment": comment,
            }
            if len(row) != len(FACT_COLS):
                raise Abort("%s builds %d columns, gen_facts.COLS has %d"
                            % (fact_id, len(row), len(FACT_COLS)))
            f.write("\t".join(row[c] for c in FACT_COLS) + "\n")
    apath = os.path.join(outdir, "anchors", "W3-C.tsv")
    with open(apath, "w", encoding="utf-8", newline="\n") as f:
        f.write(ANCHOR_HEADER % md5)
        for fact_id, _value, _space, _ty, evidence, _anchor, _comment, site in rows:
            if site is None:
                continue
            kind, va, a, b, hexbytes = site
            note = "W3-C: " + evidence.split(" -- ")[0].split(";")[0].strip()
            f.write("\t".join([fact_id, kind, "0x%X" % va, str(a), str(b),
                               hexbytes.upper(), note]) + "\n")


if __name__ == "__main__":
    sys.exit(main())
