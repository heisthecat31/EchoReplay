#!/usr/bin/env python3
"""factcheck -- every fact in facts/facts.tsv, checked against a second derivation.

The guards below fall into two families and every row must be covered by one of them:

  * read it back out of the real echovr.exe        G_PROLOGUE G_EXPORT G_DISP G_IMM
                                                   G_RIPREL G_VTSLOT G_F32CONST G_PEHEADER
                                                   G_INSTR_AT G_RVA_PAIR
  * compare it against the hand transcript         G_CROSSNOTE G_ADDRSPACE_REFUSE
    (tests/factcheck/facts_hand.h, decimal)

A row with neither is a row nobody checked, so `guard_coverage` fails it.  The binary is
the stronger source and is preferred; the transcript exists for rows whose only other
witness is a document, which is where base-conversion slips live.

Guard bodies are hashed with their declared scope (S4.6 rule 6), so weakening an assertion
inside a guard, or quietly narrowing the set of rows it covers, fails on its own.
"""

import hashlib
import inspect
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import gen_facts as G
from pe import PeImage, load as pe_load

HAND_H = os.path.join(HERE, "facts_hand.h")
ANCHORS_TSV = os.path.join(ROOT, "facts", "anchors.tsv")
GUARD_HASHES = os.path.join(ROOT, "facts", "guard_hashes.tsv")
SCOPE_IDS = os.path.join(ROOT, "facts", "guard_scope_ids.tsv")

BINARY_GUARDS = {"G_PROLOGUE", "G_EXPORT", "G_DISP", "G_IMM", "G_RIPREL", "G_VTSLOT",
                 "G_F32CONST", "G_PEHEADER", "G_INSTR_AT", "G_RVA_PAIR", "G_FUNC_START"}
TRANSCRIPT_GUARDS = {"G_CROSSNOTE", "G_ADDRSPACE_REFUSE"}

HAND_RE = re.compile(r"^inline constexpr uint(?:32|64)_t (\w+) = (\d+)u?;", re.M)


class TrackedSet(set):
    """A set that tells the context which guard added each id."""

    def __init__(self, ctx):
        set.__init__(self)
        self._ctx = ctx

    def add(self, item):
        if self._ctx.running:
            self._ctx.touched.setdefault(self._ctx.running, set()).add(item)
        set.add(self, item)


class Ctx:
    def __init__(self, rows, pending_origin=None):
        # id -> drop file, for rows that are still proposals.  A failure about one of them
        # is reported and attributed but does not fail the gate: an unmerged drop is an
        # inbox item, and one packet's half-finished row must not stop the other twelve.
        # gen_facts.py --merge-pending applies the same checks and refuses there instead,
        # which is the point at which the row's correctness is actually load-bearing.
        self.pending_origin = pending_origin or {}
        self.warn = []
        self.rows = rows
        self.by_id = {r["id"]: r for r in rows}
        self.pe = pe_load(G.ECHOVR_EXE)
        # The header the generator WOULD emit for these rows, not whatever is on disk --
        # so a mutation sweep sees the same refusal a real regeneration would produce.
        self.header = G.emit_header(G.registry_only(rows))
        self.fail = []
        # Which guard is running, and what each one checked.  A guard whose declared scope
        # does not contain a row it checked has a scope expression that was never written:
        # the rule-6 hash then covers its body only, and every row could leave its coverage
        # with nothing detecting it.  An empty scope is honest for a guard that checks the
        # image rather than rows, and indistinguishable from a forgotten one without this.
        self.running = None
        self.touched = {}
        self.checked = TrackedSet(self)
        self.binary_checked = set()
        # binary_checked says "a guard read the file for this row".
        # identity_checked says "something other than the row itself says what it IS".
        self.identity_checked = set()

    def err_from(self, origin, guard, fact, msg):
        """Attribute by the ORIGIN OF THE EVIDENCE, not of the row.

        A contributed anchor that disagrees with the binary is the drop's defect even when
        the fact it names is a committed registry row -- and conversely, a registry anchor
        must still fail hard on a fact that a drop happens to also mention.  Routing on the
        row alone would get one of those two backwards."""
        if origin.replace(os.sep, "/").startswith("facts/pending/"):
            self.warn.append("%s %s: %s [unmerged, in %s]"
                             % (guard, fact, msg, origin.replace(os.sep, "/")))
        else:
            self.fail.append("%s %s: %s" % (guard, fact, msg))

    def err(self, guard, fact, msg):
        origin = self.pending_origin.get(fact)
        if origin:
            self.warn.append("%s %s: %s [unmerged, in %s]" % (guard, fact, msg, origin))
        else:
            self.fail.append("%s %s: %s" % (guard, fact, msg))

    def val(self, fid):
        return G.parse_value(self.by_id[fid])


# Kinds where `a` is a byte index INTO instr_bytes.  For vtslot it is a
# displacement from anchor_va, and for instr_at/u32_at/bytes_at it is unused.
INSTR_FIELD_KINDS = ("disp8", "disp16", "disp32", "imm8", "imm16", "imm32",
                     "riprel", "callee_arity", "callee_storearg", "callee_argmoveoff")


def load_anchors():
    """Mine and every contributed drop: a packet's anchor counts before it is folded in,
    or the gate would stay red until W0-2 happened to run a merge.

    `b` is the length of instr_bytes.  It had two documented meanings -- the header comment
    on facts/anchors.tsv said "width b" for disp32, which reads as the 4-byte field width,
    while the guard used it as the number of bytes to read back.  A contributor who
    followed the comment produced a row whose failure message blamed the binary ("bytes at
    0x1401C9948 are 48 8B 81 A0") for a defect in the row.  Both readings are gone: the
    guard now reads len(instr_bytes), and a `b` that disagrees is reported as what it is."""
    out = []
    for p, origin in G.read_anchor_rows():
        try:
            instr = bytes.fromhex(p[5])
            a, b = int(p[3]), int(p[4])
        except ValueError as exc:
            G.complain_anchor(origin, p[0], "unreadable anchor row (%s)" % exc,
                              unreadable=True)
            continue
        if b != len(instr):
            G.complain_anchor(origin, p[0],
                              "b=%d but instr_bytes is %d bytes. b is the LENGTH of "
                              "instr_bytes, not the field width -- the width comes from "
                              "kind. Set b=%d" % (b, len(instr), len(instr)),
                              unreadable=True)
            continue
        if p[1] in INSTR_FIELD_KINDS and not (0 <= a < len(instr)):
            G.complain_anchor(origin, p[0],
                              "a=%d is outside the %d-byte instruction" % (a, len(instr)),
                              unreadable=True)
            continue
        out.append(dict(fact_id=p[0], kind=p[1], anchor_va=int(p[2], 16),
                        a=a, b=b, instr=instr, origin=origin,
                        note=p[6] if len(p) > 6 else ""))
    return out


def load_hand():
    src = open(HAND_H, "r", encoding="utf-8").read()
    return {m.group(1): int(m.group(2)) for m in HAND_RE.finditer(src)}


# --------------------------------------------------------------------------- guards

def guard_binary_identity(c):
    """The whole registry is pinned to one build.  Everything else is meaningless if this
    is not the file the facts were read from."""
    import hashlib as h
    md5 = h.md5(c.pe.data).hexdigest()
    if md5 != G.ECHOVR_MD5:
        c.err("G_BUILD", "echovr.exe", "md5 %s, registry is pinned to %s" % (md5, G.ECHOVR_MD5))
    if c.pe.image_base != 0x140000000:
        c.err("G_BUILD", "echovr.exe", "image base 0x%X" % c.pe.image_base)


def guard_prologue(c):
    """Every hook site and every called PC function carries 20 bytes read from the real
    binary.  The address is NOT written down twice: it comes from the owning PC_VA row, so
    a prologue cannot drift onto a different function while still matching the file."""
    anchored = {a["fact_id"] for a in load_anchors()}
    for r in c.rows:
        if G.blob_len(r["type"]) is None:
            continue
        if not r["id"].endswith(".PROLOGUE"):
            # a byte string at its own site: its address is in anchors.tsv, so guard_anchors
            # reads it.  Only prologues take their address from an owning row.
            if r["id"] not in anchored:
                c.err("G_PROLOGUE", r["id"], "not a prologue and not anchored; its address is nowhere")
            continue
        owner = c.by_id.get(r["id"][: -len(".PROLOGUE")])
        if owner is None:
            c.err("G_PROLOGUE", r["id"], "no owning PC_VA row")
            continue
        want = G.parse_value(r)
        # Read as many bytes as the row holds: bytes20 and bytes32 rows both live here,
        # and a fixed 20 compared a 20-byte read against a 32-byte value.
        got = c.pe.read_va(G.parse_value(owner), G.blob_len(r["type"]))
        if got is None:
            c.err("G_PROLOGUE", r["id"], "owner 0x%X has no bytes on disk" % G.parse_value(owner))
        elif got != want:
            c.err("G_PROLOGUE", r["id"], "exe has %s at 0x%X, registry has %s"
                  % (got.hex(" ").upper(), G.parse_value(owner), want.hex(" ").upper()))
        # deliberately not identity evidence: see Ctx.identity_checked
        c.checked.add(r["id"]); c.binary_checked.add(r["id"])
        c.checked.add(owner["id"]); c.binary_checked.add(owner["id"])


def guard_export_table(c):
    """The Wwise surface is addressed by exported name.  Parsing the export directory out
    of the file proves the VA and the symbol together, which a byte compare cannot."""
    d = c.pe.data
    peoff = struct.unpack_from("<I", d, 0x3C)[0]
    opt = peoff + 24
    edir_rva = struct.unpack_from("<I", d, opt + 112)[0]
    eo = c.pe.rva_to_offset(edir_rva)
    nfunc, nname = struct.unpack_from("<II", d, eo + 20)
    af, an, ao = struct.unpack_from("<III", d, eo + 28)
    exports = {}
    for i in range(nname):
        nrva = struct.unpack_from("<I", d, c.pe.rva_to_offset(an) + 4 * i)[0]
        off = c.pe.rva_to_offset(nrva)
        end = d.index(b"\0", off)
        name = d[off:end].decode("ascii", "replace")
        ordi = struct.unpack_from("<H", d, c.pe.rva_to_offset(ao) + 2 * i)[0]
        frva = struct.unpack_from("<I", d, c.pe.rva_to_offset(af) + 4 * ordi)[0]
        exports.setdefault(c.pe.image_base + frva, []).append(name)

    if "PE.EXPORT_COUNT" in c.by_id and nname != c.val("PE.EXPORT_COUNT"):
        c.err("G_PEHEADER", "PE.EXPORT_COUNT", "file has %d named exports, registry has %d"
              % (nname, c.val("PE.EXPORT_COUNT")))
    if "PE.EXPORT_DIR_RVA" in c.by_id and edir_rva != c.val("PE.EXPORT_DIR_RVA"):
        c.err("G_PEHEADER", "PE.EXPORT_DIR_RVA", "file has 0x%X, registry has 0x%X"
              % (edir_rva, c.val("PE.EXPORT_DIR_RVA")))
    if "PE.IMAGE_BASE" in c.by_id and c.pe.image_base != c.val("PE.IMAGE_BASE"):
        c.err("G_PEHEADER", "PE.IMAGE_BASE", "file has 0x%X" % c.pe.image_base)
    for k in ("PE.EXPORT_COUNT", "PE.EXPORT_DIR_RVA", "PE.IMAGE_BASE"):
        if k in c.by_id:
            c.checked.add(k); c.binary_checked.add(k); c.identity_checked.add(k)

    for r in c.rows:
        if r["guard"] != "G_EXPORT":
            continue
        va = G.parse_value(r)
        if va not in exports:
            c.err("G_EXPORT", r["id"], "0x%X is not an exported entry point" % va)
        else:
            want = r["comment"].rsplit("export ", 1)[-1]
            if want and not any(n.startswith(want[:40]) for n in exports[va]):
                c.err("G_EXPORT", r["id"], "0x%X exports %r, registry says %r"
                      % (va, exports[va][0][:60], want[:60]))
        c.checked.add(r["id"]); c.binary_checked.add(r["id"]); c.identity_checked.add(r["id"])


def first_argmove(win):
    """Byte offset of the first REX.W `mov r64,r64` -- where the shared frame ends.

    None when there is none in the window: an offset guessed from the frame every getter
    shares is the one number this decode may not assume."""
    for i in range(len(win) - 2):
        if 0x48 <= win[i] <= 0x4F and win[i + 1] == 0x8B and (win[i + 2] & 0xC0) == 0xC0:
            return i
    return None


def callee_shape(pe, got, a):
    """Argument shape of the getter a CALL reaches, decoded from the callee's own bytes.

    The anchor sits on the call site, so the callee is recomputed from that call's rel32:
    an arity read out of a function nothing in the image calls says nothing about the
    function this tree binds, which is the identity half a prologue re-read cannot see.
    Returns the decoded number, or the sentence saying why it could not be decoded --
    never a plausible default."""
    target = a["anchor_va"] + a["b"] + struct.unpack_from("<i", got, a["a"])[0]
    win = pe.read_va(target, G.GETTER_WINDOW)
    if win is None:
        return ("the call at 0x%X reaches 0x%X, which has no file bytes"
                % (a["anchor_va"], target))
    off = first_argmove(win)
    if off is None:
        return ("no REX.W mov r64,r64 in the first %d bytes of 0x%X: the argument moves "
                "this anchor decodes are not there" % (G.GETTER_WINDOW, target))
    if a["kind"] == "callee_argmoveoff":
        return off
    chain, arity, _ = G._decode_argmoves(win, off)
    if a["kind"] == "callee_arity":
        return arity
    idx, base = G._find_store_deref(win, off)
    if idx < 0:
        return ("0x%X dereferences no store within %d bytes, so which argument carries the "
                "store cannot be read here" % (target, G.GETTER_WINDOW))
    at_deref, _a, _r = G._decode_argmoves(win, off, idx)
    slot = G.GETTER_ARG_REG.get(G._resolve_reg(at_deref, base), 0)
    if not slot:
        return ("the store dereference at 0x%X+0x%X is based on a register no argument "
                "move wrote" % (target, idx))
    return slot


def anchor_encodes(pe, got, a):
    """The scalar the anchored bytes carry, or None for a kind that carries none.

    One decoder, called by the guard here and by gen_facts.merge_pending_anchors, so an
    anchor cannot be folded into the registry on its bytes alone and then fail the value
    check from inside it -- which is a red gate on a row whose owner has gone."""
    kind, at = a["kind"], a["a"]
    if kind in ("disp8", "imm8"):
        return got[at]
    if kind in ("disp16", "imm16"):
        return struct.unpack_from("<H", got, at)[0]
    if kind in ("disp32", "imm32"):
        return struct.unpack_from("<I", got, at)[0]
    if kind == "riprel":
        return a["anchor_va"] + a["b"] + struct.unpack_from("<i", got, at)[0]
    if kind in ("instr_at", "funcstart"):
        # the fact IS the address; the bytes above already had to match the file
        return a["anchor_va"]
    if kind == "u32_at":
        return struct.unpack_from("<I", pe.read_va(a["anchor_va"], 4), 0)[0]
    if kind == "vtslot":
        return struct.unpack_from("<Q", got, 0)[0]
    if kind in ("callee_arity", "callee_storearg", "callee_argmoveoff"):
        return callee_shape(pe, got, a)
    return None


def anchor_mismatch(a, want, enc):
    """Why the anchor and the row disagree, in terms of the anchor's KIND.

    `instr_at` on a row whose value is not an address is the shape that keeps arriving:
    the bytes match, so the site is real, and the number the anchor certifies is the site's
    own address rather than the fact.  Saying "binary encodes 0x1400CF3D9, registry has
    0x2" reads as a defect in the binary, which is the one thing it is not."""
    # A blob-typed row anchored by a numeric kind makes `want` bytes, and %X then raises
    # instead of reporting the very mismatch this function exists to report.
    held = (want.hex(" ").upper() if isinstance(want, (bytes, bytearray))
            else "0x%X" % want)
    if a["kind"] in ("instr_at", "funcstart") and want != a["anchor_va"]:
        return ("anchor kind %s asserts the fact's VALUE is the address 0x%X, but this row "
                "holds %s. A value that is not an address needs an anchor over the field "
                "that ENCODES it -- imm8/imm16/imm32 for an immediate, disp8/disp16/disp32 "
                "for a displacement. If no instruction encodes it, it has no anchor and the "
                "row stays withheld."
                % (a["kind"], a["anchor_va"], held))
    return ("binary encodes 0x%X at 0x%X, registry has %s"
            % (enc, a["anchor_va"], held))


ANCHOR_IDENTITY = "identity"    # the site uses the value in its claimed role
ANCHOR_CONTENT = "content"      # the bytes hold the value; nothing names the slot


def anchor_verdict(pe, a, row):
    """(problem, evidence) for one anchor against the row it names.

    `problem` is None when the anchor agrees with both the file and the row; otherwise it
    is the sentence to report.  `evidence` is what the agreement is worth, and is None
    whenever there is a problem: EVIDENCE MAY NOT SURVIVE A FAILED CHECK.  It did -- a
    mismatching anchor still marked its row identity-checked, so a drop whose anchor the
    guard had just refused promoted the row to PC_DECISIVE on the way in.

    One decision, called by guard_anchors and by gen_facts.merge_pending_anchors, so an
    anchor cannot pass the merge on its bytes and then fail the gate from inside the
    registry -- which is a red gate on a row whose contributor has moved on."""
    read_at = a["anchor_va"] + a["a"] if a["kind"] == "vtslot" else a["anchor_va"]
    got = pe.read_va(read_at, len(a["instr"]))
    if got != a["instr"]:
        return ("bytes at 0x%X are %s, anchor recorded %s"
                % (read_at, (got or b"").hex(" ").upper(),
                   a["instr"].hex(" ").upper()), None)
    want = G.parse_value(row)
    if a["kind"] == "bytes_at":
        # the fact IS the byte string; `got` already holds the file's bytes at the site
        if G.blob_len(row["type"]) is None:
            # A numeric row anchored to N bytes at an address means "the value stored
            # there is this".  Well defined, so honour it rather than rejecting the
            # contributor -- but read it as a little-endian integer, and treat it as
            # CONTENT evidence only: reading a value back does not identify the slot it
            # came from, which is what 262,433 copies of 1.0f taught this registry.
            stored = int.from_bytes(got, "little")
            if stored != want:
                return ("0x%X holds %d, registry has %d"
                        % (a["anchor_va"], stored, want), None)
            return (None, ANCHOR_CONTENT)
        if got != want:
            return ("bytes at 0x%X are %s, registry has %s"
                    % (a["anchor_va"], got.hex(" ").upper(),
                       want.hex(" ").upper()), None)
        return (None, ANCHOR_IDENTITY)
    enc = anchor_encodes(pe, got, a)
    if enc is None:
        return ("unknown anchor kind %r" % a["kind"], None)
    if isinstance(enc, str):
        return (enc, None)
    if enc != want:
        return (anchor_mismatch(a, want, enc), None)
    # `u32_at` just reads a slot, and a slot's contents do not name it: 1.0f sits in
    # 262,433 aligned .rdata slots with 2708 instructions addressing one of them.  The
    # kinds that DO identify point at an instruction using the value in the claimed
    # role -- a displacement, an immediate, a RIP-relative reference, a vtable slot.
    return (None, ANCHOR_CONTENT if a["kind"] == "u32_at" else ANCHOR_IDENTITY)


def guard_anchors(c):
    """Each anchored fact names an instruction in the file that encodes it.  The guard
    re-reads that instruction from echovr.exe and pulls the number back out of the bytes,
    so the registry cannot hold a value the binary does not."""
    for a in load_anchors():
        fid = a["fact_id"]
        r = c.by_id.get(fid)
        if r is None:
            if fid in G.retired_ids():
                # deliberately retired; the drop that anchors it is not ours to edit
                continue
            c.err_from(a["origin"], "G_ANCHOR", fid,
                       "names a fact that is not in the registry")
            continue
        problem, evidence = anchor_verdict(c.pe, a, r)
        if problem:
            c.err_from(a["origin"], "G_ANCHOR", fid, problem)
            continue
        c.checked.add(fid)
        c.binary_checked.add(fid)
        if evidence == ANCHOR_IDENTITY:
            c.identity_checked.add(fid)


# The type name a CMatSym row hashes, written into the row as one explicit marker rather
# than scraped out of its prose.  A guard that tries several candidate names and takes the
# first that agrees is not a guard, it is several chances to agree -- the same reason
# guard_evidence_site refuses an ambiguous RIP-relative resolution.
MATSYM_MARK = re.compile(r"CMATSYM\(([A-Za-z_][A-Za-z0-9_]*)\)")

MATSYM_IDENTITY = 0x8040201008040201


def _mat_rows(m):
    return [(m >> (8 * i)) & 0xFF for i in range(8)]


def matsym_mul(a, b):
    """The 8x8 GF(2) matrix product echovr.exe's EVENT.MATSYM_COMBINE computes.

    That function masks each byte of `a` through the table at EVENT.MATSYM_TABLE, whose
    entry k is the byte-lane spread of k (0, 0xFF, 0xFF00, 0xFFFF, ...), and XOR-folds the
    lanes.  Masking by the spread of a row and folding IS "XOR the rows of b that this row
    of a selects", so the table is checked here for that property rather than indexed."""
    ra, rb = _mat_rows(a), _mat_rows(b)
    out = []
    for i in range(8):
        acc = 0
        for k in range(8):
            if (ra[i] >> k) & 1:
                acc ^= rb[k]
        out.append(acc)
    return sum(r << (8 * i) for i, r in enumerate(out))


def matsym_hash(name, char_table):
    """EVENT.MATSYM_HASH: empty string is the identity, and each character is applied
    OUTERMOST-first -- the function recurses on name+1 and combines the head with the
    result, so the first character multiplies on the left."""
    acc = MATSYM_IDENTITY
    for ch in reversed(name.encode("ascii")):
        acc = matsym_mul(char_table[ch if ch < 128 else ch - 256], acc)
    return acc


def guard_matsym(c):
    """A CMatSym is not IN the image and IS derivable from it, and those are different
    claims -- the note on these rows conflated them and parked four of them at U forever,
    because nobody re-examines a fact already marked unestablishable.

    TypeId(SFoo) = MatMul(EVENT.MATSYM_NS, Hash("Foo")), over the per-character matrix
    table at EVENT.MATSYM_CHAR_TABLE.  Every input is read out of echovr.exe here, so a
    row that verifies has had its value produced from the binary rather than transcribed.

    THE INPUTS ARE VERIFIED BY THEIR OWN OUTPUT: a wrong char-table address cannot yield
    four exact 64-bit matches for four different names, so the table rows are marked
    checked only when every row in scope agrees and there is more than one of them."""
    ids = [r["id"] for r in c.rows if MATSYM_MARK.search(r["comment"])]
    if not ids:
        return
    try:
        ns = c.val("EVENT.MATSYM_NS")
        table_va = c.val("EVENT.MATSYM_CHAR_TABLE")
        mask_va = c.val("EVENT.MATSYM_TABLE")
    except (KeyError, G.FactError, ValueError):
        c.err("G_MATSYM", "EVENT.MATSYM_NS",
              "%d row(s) name a CMatSym type, but the namespace or a table row is missing "
              "from the registry, so nothing can be recomputed" % len(ids))
        return
    blob = c.pe.read_va(table_va, 8 * 128)
    if blob is None or len(blob) != 8 * 128:
        c.err("G_MATSYM", "EVENT.MATSYM_CHAR_TABLE",
              "no file bytes for 128 qwords at 0x%X" % table_va)
        return
    char_table = list(struct.unpack("<128Q", blob))

    # EVENT.MATSYM_TABLE's whole content claim, checked exactly rather than sampled: entry
    # k must be the byte-lane spread of k.  This is what makes reading COMBINE as a matrix
    # product a fact about the image and not an interpretation of it.
    masks = c.pe.read_va(mask_va, 8 * 256)
    spread_ok = masks is not None and len(masks) == 8 * 256
    if spread_ok:
        vals = struct.unpack("<256Q", masks)
        for k, v in enumerate(vals):
            want = sum(0xFF << (8 * j) for j in range(8) if (k >> j) & 1)
            if v != want:
                c.err("G_MATSYM", "EVENT.MATSYM_TABLE",
                      "entry %d is 0x%016X, not the byte-lane spread 0x%016X" % (k, v, want))
                spread_ok = False
                break
    else:
        c.err("G_MATSYM", "EVENT.MATSYM_TABLE",
              "no file bytes for 256 qwords at 0x%X" % mask_va)

    agreed = 0
    for fid in ids:
        r = c.by_id[fid]
        name = MATSYM_MARK.search(r["comment"]).group(1)
        if not name.startswith("S"):
            c.err("G_MATSYM", fid,
                  "CMATSYM(%s) does not start with S; TypeId hashes the name WITHOUT its "
                  "leading S, so the marker has to carry the struct name as declared" % name)
            continue
        got = matsym_mul(ns, matsym_hash(name[1:], char_table))
        want = G.parse_value(r)
        if got != want:
            c.err("G_MATSYM", fid,
                  "MatMul(EVENT.MATSYM_NS, Hash(%r)) is 0x%016X, registry has 0x%016X"
                  % (name[1:], got, want))
            continue
        agreed += 1
        c.checked.add(fid)
        c.binary_checked.add(fid)
        c.identity_checked.add(fid)
    if agreed == len(ids) and agreed > 1 and spread_ok:
        for fid in ("EVENT.MATSYM_NS", "EVENT.MATSYM_CHAR_TABLE", "EVENT.MATSYM_TABLE"):
            if fid in c.by_id:
                c.checked.add(fid)
                c.binary_checked.add(fid)
                c.identity_checked.add(fid)


def guard_rva_pairs(c):
    """An RVA and its VA are one fact written twice; they must agree through the image base."""
    base = c.val("PE.IMAGE_BASE")
    # Derived, not listed.  A hardcoded pair list silently stops covering the next RVA row
    # somebody adds -- NETGAME.UPDATE_RVA was added and checked nothing until this changed.
    pairs = []
    for r in c.rows:
        if r["addr_space"] != "PC_RVA" or not r["id"].endswith("_RVA"):
            continue
        stem = r["id"][: -len("_RVA")]
        for cand in (stem, stem + "_VA", stem + "_PTR"):
            if cand in c.by_id and c.by_id[cand]["addr_space"] == "PC_VA":
                pairs.append((r["id"], cand))
                break
        else:
            # PE.EXPORT_DIR_RVA is read straight out of the PE header by guard_export_table
            # and has no VA counterpart; only an unchecked RVA is a finding.
            if r["id"] not in c.binary_checked:
                c.err("G_RVA_PAIR", r["id"], "no PC_VA row to check this RVA against")
    for rva_id, va_id in pairs:
        if rva_id not in c.by_id or va_id not in c.by_id:
            continue
        if c.val(rva_id) + base != c.val(va_id):
            c.err("G_RVA_PAIR", rva_id, "0x%X + base != %s 0x%X"
                  % (c.val(rva_id), va_id, c.val(va_id)))
        c.checked.add(rva_id)
        c.binary_checked.add(rva_id)
        # Identity is inherited, not created: adding a base to a number nobody has
        # identified gives another number nobody has identified.
        if va_id in c.identity_checked:
            c.identity_checked.add(rva_id)


def guard_func_starts(c):
    """A PC_VA the design calls must at least be readable code in .text.  This is the weak
    end of the ladder: it is a containment check, not an identity check, so every row that
    relies on it also carries a prologue."""
    for r in c.rows:
        if r["addr_space"] != "PC_VA" or G.blob_len(r["type"]) is not None:
            continue
        if r["id"] == "PE.IMAGE_BASE":
            continue
        if r["id"] + ".TEXT" in c.by_id:
            continue          # a string constant, not a function: .rdata is correct for it
        va = G.parse_value(r)
        sect = c.pe.section_of_rva(va - c.pe.image_base)
        if sect is None:
            c.err("G_FUNC_START", r["id"], "0x%X is outside every section" % va)
        elif r["id"] + ".PROLOGUE" in c.by_id and sect != ".text":
            c.err("G_FUNC_START", r["id"], "0x%X carries a prologue but sits in %s" % (va, sect))


def guard_hand_transcript(c):
    """The second derivation for every row whose other witness is a document.  facts.tsv
    holds hex, this holds decimal, and they were written from the corpus by different
    routes -- so a base-conversion slip fails here rather than in the game (S2 F4)."""
    hand = load_hand()
    used = set()
    for r in c.rows:
        if r["guard"] not in TRANSCRIPT_GUARDS:
            continue
        if r["verdict"] == "PC_ABSENT":
            # A row recording that the value is NOT at the cited site holds a placeholder,
            # not a value.  There is nothing to transcribe, and nothing is emitted.
            c.checked.add(r["id"])
            continue
        key = G.cname(r["id"])
        if key not in hand:
            c.err("G_TRANSCRIPT", r["id"], "no line in facts_hand.h; this row has no second derivation")
            continue
        used.add(key)
        if hand[key] != G.parse_value(r):
            c.err("G_TRANSCRIPT", r["id"], "registry %s = %d, hand transcript = %d"
                  % (r["value"], G.parse_value(r), hand[key]))
        c.checked.add(r["id"])
    # A line for a row that is already binary-guarded is welcome: it is a third witness.
    # Only a line naming a fact that no longer exists is stale.
    by_cname = {G.cname(r["id"]): r for r in c.rows}
    for key in sorted(set(hand) - used):
        r = by_cname.get(key)
        if r is None:
            c.err("G_TRANSCRIPT", key, "transcribed but no such fact; delete the line")
        elif hand[key] != G.parse_value(r):
            c.err("G_TRANSCRIPT", r["id"], "registry %s = %d, hand transcript = %d"
                  % (r["value"], G.parse_value(r), hand[key]))


def guard_provenance_evidence(c):
    """`provenance = PC` is a claim that someone read this number out of the PC binary.
    A row may only make that claim if a guard actually did: otherwise the column records
    a belief, which is how PHYS.CI_STRIDE spent the whole corpus looking like a PC stride.
    Flipping an unverified row to PC therefore fails here rather than passing quietly."""
    for r in c.rows:
        if r["provenance"] != "PC":
            continue
        if r["id"] in c.binary_checked:
            continue
        if G.blob_len(r["type"]) is None and r["addr_space"] == "PC_VA"                 and r["id"] + ".PROLOGUE" in c.by_id:
            continue
        c.err("G_PROVENANCE", r["id"],
              "claims PC provenance but no guard read it out of echovr.exe")


def guard_arm64_evidence(c):
    """`provenance = ARM64` is a claim too, and it is the claim that makes a value vanish
    from the generated header.  Left unchecked it is a way to make any inconvenient row
    disappear from the gate.  It must agree with the sources the row cites, in both
    directions: ARM64 requires a libr15 citation, and citing libr15 requires ARM64."""
    for r in c.rows:
        cites = "libr15_symbols" in (r["source_a"], r["source_b"])
        if r["provenance"] == "ARM64" and not cites:
            c.err("G_ARM64", r["id"], "claims ARM64 provenance but cites no libr15 source")
        if cites and r["provenance"] != "ARM64":
            c.err("G_ARM64", r["id"], "cites libr15_symbols but claims provenance %r" % r["provenance"])


def guard_verdict(c):
    """The verdict is the assessment; the tier is the permission it grants.  Keeping them in
    separate columns and never comparing them is how a row classified PC_PRESENT -- which
    docs/PC_DERIVATION.md states is NOT a pass -- came to be marked V-PRIMARY and emitted as
    a verified constant.  A classification nothing consumes is decoration, so it is consumed
    here and in the generator, which withholds the refused verdicts entirely."""
    header = c.header
    open_at = header.find("\nnamespace candidate {")
    close_at = header.find("}  // namespace candidate")
    # No block, or a malformed one, means every candidate is outside it -- the safe reading.
    CANDIDATE_BLOCK = (open_at, close_at) if 0 <= open_at < close_at else (0, 0)
    for r in c.rows:
        v = r["verdict"]
        if v in G.VERDICT_WITHHELD:
            name = G.cname(r["id"])
            hit = re.search(r"^inline constexpr \w+ %s\b" % re.escape(name), header, re.M)
            if hit and v == G.VERDICT_CANDIDATE:
                # A candidate belongs in facts::candidate and nowhere else.  What is checked
                # is the LOCATION, because "emitted at all" is what a candidate is for.
                if not (CANDIDATE_BLOCK[0] <= hit.start() < CANDIDATE_BLOCK[1]):
                    c.err("G_VERDICT", r["id"],
                          "PC_CANDIDATE emitted outside namespace candidate, where engine "
                          "code can reach it. A candidate is a question, not a fact.")
            elif hit:
                c.err("G_VERDICT", r["id"], "verdict %s but emitted as a usable constant" % v)
            if r["tier"] != "U":
                c.err("G_VERDICT", r["id"], "verdict %s cannot be tier %s" % (v, r["tier"]))
        elif v not in G.VERDICT_MAY_BE_VERIFIED and r["tier"] != "U":
            c.err("G_VERDICT", r["id"], "verdict %s is not a pass; tier %s claims it is"
                  % (v, r["tier"]))
        if v in G.VERDICT_MAY_BE_VERIFIED and r["provenance"] != "PC":
            c.err("G_VERDICT", r["id"], "verdict %s with provenance %s" % (v, r["provenance"]))

        # The verdict is a claim about evidence, so it is checked against the evidence in
        # both directions.  Upward: PC_DECISIVE says the value was read back out of the
        # binary at an attributable site, and may only be claimed when a guard did that.
        # Downward: a row a binary guard proves cannot also be recorded as absent from the
        # binary, or as merely present.  Without both halves the column records a belief.
        proven = r["id"] in c.binary_checked
        if v == "PC_DECISIVE" and r["id"] not in c.identity_checked:
            c.err("G_VERDICT", r["id"],
                  "verdict PC_DECISIVE, but the only binary evidence is a prologue seeded "
                  "from this row's own address -- that detects drift, not identity")
        if v in ("PC_ABSENT", "PC_PRESENT", "NOT_IN_CODE", "NOT_ASSESSED") and proven:
            c.err("G_VERDICT", r["id"],
                  "verdict %s, but a binary guard reads this value at an attributable site" % v)
        # DELIBERATELY NOT `c.checked.add(r["id"])`.  It was here, unconditional, for every
        # row -- and this guard runs before guard_coverage, so guard_coverage found every row
        # already checked and COULD NOT FIRE.  What this guard reads is the verdict column's
        # internal consistency; it never reads the VALUE out of the binary, which is what
        # `checked` means to everything downstream.  One line, and it hid 102 rows whose
        # declared guard had never run.


def guard_source_sanity(c):
    """Two things that make a guard silently stop guarding, checked directly.

    A stray control character in a pattern -- a mangled `\\b` arriving as U+0008 -- makes the
    pattern match nothing while the file still reads correctly in every viewer, so the guard
    passes over everything. It happened twice in this packet's own source.

    And the withhold check is proved live rather than assumed: a synthetic header containing
    a withheld symbol must be detected. A regex that matches nothing reports the same "no
    failures" as a registry with nothing wrong in it."""
    for path in (os.path.abspath(__file__), os.path.join(ROOT, "tools", "gen_facts.py"),
                 os.path.join(HERE, "gatecheck.py"), os.path.join(HERE, "mutate.py")):
        src = open(path, "r", encoding="utf-8", newline="").read()
        bad = sorted({ord(ch) for ch in src
                      if ord(ch) < 9 or 11 <= ord(ch) <= 12 or 14 <= ord(ch) < 32})
        if bad:
            c.err("G_SANITY", os.path.basename(path),
                  "contains control character(s) %s; a mangled escape in a pattern matches nothing"
                  % ", ".join("U+%04X" % b for b in bad))

    withheld = [r for r in c.rows if r["verdict"] in G.VERDICT_WITHHELD]
    if not withheld:
        c.err("G_SANITY", "withhold", "no withheld rows, so the refusal check proves nothing")
        return
    name = G.cname(withheld[0]["id"])
    probe = "inline constexpr uint32_t %s = 1u;\n" % name
    if not re.search(r"^inline constexpr \w+ %s\b" % re.escape(name), probe, re.M):
        c.err("G_SANITY", "withhold",
              "the withhold pattern does not match a header line that plainly declares %s" % name)


LIBR15_DB = os.environ.get("REPLAY_IDA_LIBR15_DB") or "(IDA database not configured - set REPLAY_IDA_LIBR15_DB)"


def guard_libr15_symbols(c):
    """Read the source 27 rows cite.  `libr15_symbols` claims the value came from libr15.so;
    until this existed nothing opened it, so the claim rested on one hand-read and the decimal
    transcription of that same hand-read -- two names for one derivation.

    The row's comment names the C++ symbol; this checks the address is that symbol's exact
    start in the ARM64 extraction, which is a fact the PC binary cannot supply and this
    repository cannot fabricate."""
    rows = [r for r in c.rows if "libr15_symbols" in (r["source_a"], r["source_b"])]
    if not rows:
        return
    if not os.path.exists(LIBR15_DB):
        # Loud, and it does not pass: a cited source that cannot be read is not a source.
        c.err("G_LIBR15", "libr15.so",
              "%d rows cite libr15_symbols and the extraction is not at %s, so that citation "
              "is unverified on this machine" % (len(rows), LIBR15_DB))
        return
    import sqlite3
    db = sqlite3.connect("file:%s?mode=ro" % LIBR15_DB.replace("\\", "/"), uri=True, timeout=120)
    checked = 0
    for r in rows:
        if G.blob_len(r["type"]) is not None:
            continue
        va = G.parse_value(r)
        hit = db.execute("SELECT demangled_name, name FROM functions WHERE start_va=?",
                         (va,)).fetchone()
        # Every C++ symbol the row names, in evidence or comment.  The row must name the
        # function it points at SOMEWHERE -- requiring the first mention would fail rows
        # that correctly document a correction by naming both the old and new symbol.
        named = set(re.findall(r"[A-Za-z_]\w*(?:::[~\w]+)+", r["evidence"] + " " + r["comment"]))
        leaves = {n.split("::")[-1].split("(")[0] for n in named}
        if hit is None:
            # not every ARM64 row is a function -- an offset or a mask is not
            if r["addr_space"] == "LIBR15_VA":
                c.err("G_LIBR15", r["id"],
                      "0x%X is not a function start in libr15.so" % va)
            continue
        sym = hit[0] or hit[1]
        if leaves and not any(leaf and leaf in sym for leaf in leaves):
            c.err("G_LIBR15", r["id"],
                  "0x%X is %s in libr15.so, and the row names none of %s"
                  % (va, sym[:70], sorted(leaves)))
        checked += 1
        c.checked.add(r["id"])
    if checked == 0:
        c.err("G_LIBR15", "libr15.so", "opened the extraction and checked nothing")


EVIDENCE_VA = re.compile(r"0x1[0-9A-Fa-f]{8}")
EVIDENCE_MNEMONIC = re.compile(
    r"\b(mov|movzx|movsx|lea|imul|cmp|test|movss|movsd|add|sub|or|and|xor|dec|inc|"
    r"movaps|movdqa|shl|shr|push|call|jmp)\b")


def guard_evidence_site(c):
    """The ruling, mechanised: a row whose evidence names an instruction that encodes its
    value has identity evidence, wherever that address was written down.

    `guard_anchors` does exactly this for rows whose site lives in facts/anchors.tsv.  A
    contributor writes the site into `evidence` instead, and reading only the first is how
    13 of W1-I's offsets sat at PC_PRESENT while PHYS.CI_OWNER_OFF -- identical evidence
    shape -- sat at PC_DECISIVE.  A tier that means different things for different authors
    is not a tier."""
    for r in c.rows:
        # PC_PRESENT and NOT_ASSESSED are withheld, and they are precisely the rows the
        # ruling exists to promote -- skipping every withheld verdict would exclude them.
        # Only the assessed negatives are out of scope: a row recording that the value is
        # NOT at the cited site, or that it came from ARM64, is not awaiting promotion.
        if G.blob_len(r["type"]) is not None or r["verdict"] in ("ARM64_ONLY", "PC_ABSENT", "NOT_IN_CODE"):
            continue
        ev = r["evidence"]
        if not EVIDENCE_MNEMONIC.search(ev):
            continue
        try:
            v = G.parse_value(r)
        except Exception:
            continue
        if v == 0:
            # ZERO HAS NO ENCODING TO FIND.  Every form of it -- one 00 byte, two, four --
            # is a run of zeros that ordinary instructions carry anyway, so the search below
            # answers yes on a coincidence.  It did: CLOCK.CTOR_MINELAPSED_INIT reached
            # PC_DECISIVE because its 16-byte window held exactly one 00, and that byte was
            # the top of the NEXT instruction's 0x989680 immediate.  A `mov [rdi+58h], rax`
            # that stores zero says so through the REGISTER, which no guard here reads, and
            # a `mov rcx,[rcx]` whose offset is zero says so by having no displacement byte
            # at all.  Both are sites, not immediates: registered as a site they are
            # evidence, promoted as a constant they are a coincidence.  The registry already
            # ruled this way once, on CAM.MODE_POV_DEFAULT_SITE.
            continue
        for m in EVIDENCE_VA.finditer(ev):
            va = int(m.group(0), 16)
            b = c.pe.read_va(va, 16)
            if b is None:
                continue
            # Exactly once, not merely present.  A single 00 byte occurs in almost every
            # instruction, so "the value appears somewhere in these bytes" accepted a
            # mutation of 0x100 down to 0 and kept the row emitted.
            enc = [struct.pack("<I", v & 0xFFFFFFFF), struct.pack("<H", v & 0xFFFF)]
            if 0 <= v <= 0x7F:
                enc.append(bytes([v]))
            hit = any(b.count(e) == 1 for e in enc)
            if not hit:
                # A RIP-relative instruction encodes a DISPLACEMENT and the fact is the
                # TARGET, so searching the bytes for the value finds nothing even when the
                # citation is exactly right.  Resolving it means assuming an instruction
                # length, and "try several and take the first that agrees" is not a guard --
                # it is four chances to agree.  So: the predicate is equality against one
                # specific 64-bit address, and the resolution must be UNIQUE.  Two lengths
                # resolving to the same value means the citation is ambiguous, and an
                # ambiguous citation is refused rather than accepted.
                resolved = {ln for ln in range(6, 12) if ln <= len(b)
                            and va + ln + struct.unpack_from("<i", b, ln - 4)[0] == v}
                hit = len(resolved) == 1
            if hit:
                c.checked.add(r["id"])
                c.binary_checked.add(r["id"])
                c.identity_checked.add(r["id"])
                break


def guard_text_at(c):
    """Read a string fact back out of the image, and require it to be UNIQUE there.

    For a data fact, "how do I know this address holds that and not its neighbour" is
    answered by uniqueness: a byte string occurring exactly once identifies its own address.
    RAD_Version occurs once in 35 MB.  A string that occurred twice would be read back
    successfully from the wrong address, so uniqueness is what makes this identity evidence
    rather than a successful read."""
    for r in c.rows:
        if r["type"] != "str":
            continue
        owner_id = r["id"][: -len(".TEXT")] if r["id"].endswith(".TEXT") else None
        owner = c.by_id.get(owner_id or "")
        if owner is None:
            c.err("G_TEXT", r["id"], "a str row must be named <OWNER>.TEXT with a PC_VA owner")
            continue
        want = G.parse_value(r).encode("ascii")
        got = c.pe.read_va(G.parse_value(owner), len(want))
        if got != want:
            c.err("G_TEXT", r["id"], "0x%X holds %r, registry has %r"
                  % (G.parse_value(owner), (got or b"")[:40], want[:40]))
            continue
        # A prefix matches too: "RAD_Version: goldmaster63154" reads back cleanly from the
        # same address as the full string.  Require the terminator, so the row holds the
        # WHOLE string rather than however much of it someone happened to copy.
        after = c.pe.read_va(G.parse_value(owner) + len(want), 1)
        if after != b"\x00":
            c.err("G_TEXT", r["id"], "the byte after it is %r, not a terminator -- the row "
                                     "holds a prefix of the real string" % (after or b""))
            continue
        n = c.pe.data.count(want)
        # A string that is not unique can still name its own address, PROVIDED something
        # else already proved which address it is.  An owner already in identity_checked
        # was proved by a guard that reads a unique instruction referencing it -- a riprel
        # anchor over one `lea` site -- which is strictly stronger evidence than "these
        # bytes occur once": twenty loadout slot names occur 2 to 131 times each in the
        # image and every one of them is loaded by exactly one instruction.  Uniqueness
        # stays the requirement for an owner nothing else identifies.
        if n != 1 and owner["id"] not in c.identity_checked:
            c.err("G_TEXT", r["id"], "occurs %d times in the image and nothing identifies "
                                     "0x%X, so reading it back does not identify this "
                                     "address" % (n, G.parse_value(owner)))
            continue
        for fid in (r["id"], owner["id"]):
            c.checked.add(fid)
            c.binary_checked.add(fid)
            c.identity_checked.add(fid)


def guard_module_md5(c):
    """The md5 the whole registry is pinned to, held as a fact rather than a constant."""
    import hashlib
    for r in c.rows:
        if r["type"] != "md5":
            continue
        actual = hashlib.md5(c.pe.data).hexdigest()
        if actual != G.parse_value(r):
            c.err("G_MD5", r["id"], "echovr.exe is %s, registry says %s"
                  % (actual, G.parse_value(r)))
        elif actual != G.ECHOVR_MD5:
            c.err("G_MD5", r["id"], "registry row and gen_facts.ECHOVR_MD5 disagree")
        c.checked.add(r["id"])
        c.binary_checked.add(r["id"])
        c.identity_checked.add(r["id"])


ECHOVR_DB = os.environ.get("REPLAY_IDA_ECHOVR_DB") or "(IDA database not configured - set REPLAY_IDA_ECHOVR_DB)"


def guard_tail_calls(c):
    """A terminal `jmp` is a continuation, not a return.

    An `effect` written from a function's own body is wrong whenever the function ends by
    jumping into another one -- sub_140127960 never writes clock+104 itself, and the store
    is in what it jumps to.  So a row whose function ends in an unconditional jump to a
    DIFFERENT function has to say where it goes; otherwise the sweep that filled `effect`
    read one body, found nothing, and recorded that as the answer."""
    rows = [r for r in c.rows
            if r["addr_space"] == "PC_VA" and G.blob_len(r["type"]) is None
            and (r["effect"] not in ("-", "") or r["verb"] == "mutate")]
    if not rows:
        return
    if not os.path.exists(ECHOVR_DB):
        c.err("G_TAIL", "echovr.db",
              "%d rows need their tail calls followed and the extraction is not at %s"
              % (len(rows), ECHOVR_DB))
        return
    import sqlite3
    db = sqlite3.connect("file:%s?mode=ro" % ECHOVR_DB.replace("\\", "/"), uri=True, timeout=120)
    for r in rows:
        try:
            va = G.parse_value(r)
        except Exception:
            continue
        last = db.execute("SELECT va, mnemonic, disasm FROM instructions WHERE func_va=? "
                          "ORDER BY va DESC LIMIT 1", (va,)).fetchone()
        if not last or last[1] != "jmp":
            continue
        tgt = db.execute("SELECT to_va FROM code_xrefs WHERE from_va=? LIMIT 1", (last[0],)).fetchone()
        # a jump back inside the same function is a loop or a branch, not a continuation
        if tgt and db.execute("SELECT 1 FROM functions WHERE start_va<=? AND end_va>? AND start_va=?",
                              (tgt[0], tgt[0], va)).fetchone():
            continue
        if "tail" not in r["effect"]:
            c.err("G_TAIL", r["id"],
                  "ends in `%s` -- an effect read from its own body stops at the jump; record "
                  "where it goes" % last[2].strip()[:40])
        c.checked.add(r["id"])


MNEMONICS = ("imul", "movzx", "movsx", "movss", "movsd", "movaps", "movdqa", "lea", "mov",
             "inc", "dec", "cmp", "add", "sub", "shl", "shr", "or", "and", "xor", "test",
             "call", "jmp", "push", "divss", "subss", "addss", "mulss")
ROLE_DEREF = ("HEAD", "LIST", "PTR", "ARRAY", "TABLE")
ROLE_SCALED = ("STRIDE", "SIZE")


def _mnemonic(note):
    """The first token in the note that is actually a mnemonic.

    Contributed notes are prefixed with the packet name -- "W1-E: imul rcx, rax, 250h" --
    so taking token zero makes `W1-E:` the opcode and rejects a correct row."""
    for tok in note.replace(",", " ").split():
        t = tok.lower().strip(":")
        if t in MNEMONICS:
            return t
    return None


def guard_role(c):
    """Does the citing instruction DO what the id claims the field is?

    W1-E's diagnosis made mechanical: `provenance = PC` records where a number was read,
    not whether the field it NAMES is the field it INDEXES.  PHYS.CI_LIST_HEAD_OFF was
    read correctly off the right element with honest provenance, and it was the counter --
    the taint checked the base register and nobody checked the operation.

    Only the unambiguous direction is fatal: nobody decrements a list head.  A counter read
    with `movzx` is ordinary, so requiring counters to be incremented at their anchor would
    reject true statements, and those advisories are reported instead."""
    anchors = {a["fact_id"]: a for a in load_anchors()}
    for r in c.rows:
        a = anchors.get(r["id"])
        if a is None or not a["note"]:
            continue
        mnem = _mnemonic(a["note"])
        if mnem is None:
            continue
        tail = r["id"].rsplit(".", 1)[-1]
        if any(k in tail for k in ROLE_DEREF) and mnem in ("inc", "dec"):
            c.err("G_ROLE", r["id"],
                  "the id calls this a pointer and its anchor is `%s` -- nobody decrements a "
                  "list head; this is the PHYS.CI_LIST_HEAD_OFF defect" % a["note"][:44])
        elif any(k in tail for k in ROLE_SCALED) and mnem in ("inc", "dec"):
            c.err("G_ROLE", r["id"],
                  "the id calls this a stride and its anchor increments it: %s" % a["note"][:44])
        c.checked.add(r["id"])


def guard_value_checked(c):
    """A row that is EMITTED must have had its value checked, not merely its address placed.

    guard_func_starts confirms a PC_VA lands inside a section, which says nothing about the
    value: +/-1 on such an address changed nothing any guard noticed.  Verification means
    the value was read back out of the binary or compared against the transcript; anything
    else is withheld, exactly as PC_PRESENT is."""
    hand = load_hand()
    for r in c.rows:
        if r["verdict"] in G.VERDICT_WITHHELD:
            continue
        if r["id"] in c.binary_checked:
            continue
        if G.cname(r["id"]) in hand:
            continue
        c.err("G_UNCHECKED", r["id"],
              "emitted as a %s constant with nothing verifying its value -- no anchor, no "
              "prologue, no transcript line" % r["addr_space"])


def guard_not_in_code(c):
    """NOT_IN_CODE is emitted, so the claim behind it has to be checkable AND checked.

    WHICH ENCODINGS THIS COVERS, stated rather than implied.  The previous version tested
    only the 4-byte little-endian form, and every value that needed judging was smaller
    than that -- so it skipped precisely where those rows live, and 17 mutations into this
    verdict survived a full sweep.  That is the same shape as hex-only literal scanning and
    imul-only stride sweeping: a check scoped to one encoding, blind where the subject is.

      COVERED    4-byte and 8-byte little-endian immediates in .text, for values that
                 require them.
      REFUSED    values that fit in 1 or 2 bytes.  Absence is not merely unchecked for
                 these, it is unprovable: every byte pair occurs in 23 MB of .text, so the
                 claim can never be true.  A verdict nobody can check must not be
                 assertable, so the row is failed rather than waved through.
      NOT COVERED, deliberately, and named so nobody reads silence as coverage:
                 .rdata and .data -- the claim is about CODE, and a Wwise id living in a
                 data blob is exactly the case this verdict exists for;
                 values assembled at runtime rather than encoded as an immediate;
                 an immediate split across two instructions.
    """
    text = None
    for name, va, vs, rp, rs in c.pe.sections:
        if name == ".text":
            text = c.pe.data[rp:rp + rs]
            break
    if text is None:
        c.err("G_NOTINCODE", ".text", "no .text section; the absence claim cannot be checked")
        return

    for r in c.rows:
        if r["verdict"] != "NOT_IN_CODE" or r["type"] in ("bytes20", "str", "md5"):
            continue
        v = G.parse_value(r)
        if v <= 0xFFFF:
            c.err("G_NOTINCODE", r["id"],
                  "verdict NOT_IN_CODE on 0x%X, which fits in %d byte(s). Absence is not "
                  "provable for a value that small -- every short encoding occurs somewhere "
                  "in .text -- so this verdict cannot be earned, only assumed." 
                  % (v, 1 if v <= 0xFF else 2))
            c.checked.add(r["id"])
            continue
        found = []
        for width in (4, 8):
            if v >= (1 << (width * 8)):
                continue
            if v.to_bytes(width, "little") in text:
                found.append(width)
        if found:
            c.err("G_NOTINCODE", r["id"],
                  "verdict NOT_IN_CODE, but the %s-byte encoding of 0x%X does occur in .text; "
                  "that verdict is a claim and this one is false"
                  % ("/".join(str(w) for w in found), v))
        c.checked.add(r["id"])


def guard_base_agrees(c):
    """An id that names an object must be measured from that object.

    AUDIO.SINK_REFCOUNT_OFF was read off the OnBufferEnd callback argument and called a
    SINK offset -- the seventh number here that was read correctly and named wrongly.  The
    `base` column is the answer to "0x88 from what?"; this checks the id against it."""
    for r in c.rows:
        if r["addr_space"] != "OFFSET" or r["base"] == "-":
            continue
        owner = r["id"].split(".")[0].lower()
        base = r["base"].lower()
        # only flag when the id names an object the base plainly contradicts
        if owner in ("sink", "audio") and "sink" in r["id"].lower():
            if "sink" not in base:
                c.err("G_BASE", r["id"],
                      "the id says sink-relative, the base says %r" % r["base"][:52])
        c.checked.add(r["id"])


NOT_DERIVED_PHRASES = ("not re-derived", "no attributable instruction",
                       "pseudocode-only", "not observed", "no pc counterpart",
                       "not seen off", "could not be", "never re-derived")


def guard_evidence_consistent(c):
    """A row may not claim derivation in its verdict and deny it in its evidence.

    derive_verdicts promotes verdict, tier and provenance from anchor evidence, and can
    repair source_a/source_b and guard alongside them.  It cannot repair `evidence`: that
    field is prose, it is the only place the WHY lives, and a sentence invented by a tool
    would be worse than a stale one.  So the contradiction is failed rather than papered
    over -- whoever added the anchor knows what the evidence now is."""
    for r in c.rows:
        if r["verdict"] not in ("PC_DECISIVE", "PC_CORROBORATED"):
            continue
        ev = r["evidence"].lower()
        for phrase in NOT_DERIVED_PHRASES:
            if phrase in ev:
                c.err("G_EVIDENCE", r["id"],
                      "verdict %s but the evidence still says %r -- a promotion updated the "
                      "verdict and left the sentence that contradicts it. Rewrite the evidence "
                      "to say what was actually read." % (r["verdict"], phrase))
                break
        c.checked.add(r["id"])


def guard_pending_drops(c):
    """Report every row the loader could not read, and who owns the file it was in.

    The loader skips malformed rows in facts/pending/ instead of raising, so that one
    packet's half-migrated drop cannot take the gate down for the other twelve.  Skipping
    silently would be the worse bug, so each one is named here.  Shape defects warn --
    the drop is a proposal and its owner fixes it.  A drop that contradicts a merged
    value fails, because that is a claim about the registry itself."""
    for sev, path, lineno, msg in sorted(set(G.PENDING_COMPLAINTS)):
        rel = os.path.relpath(path, G.ROOT).replace(os.sep, "/")
        owner = G.path_owner(rel) or "unclaimed"
        if sev == "fail":
            c.err("G_DROP", rel + ":" + str(lineno), "[%s] %s" % (owner, msg))
        else:
            print("WARN G_DROP %s:%s [%s] %s" % (rel, lineno, owner, msg))

    # One refusal per FILE, naming every row in it -- the shape the test harness uses for a
    # bad probe descriptor, and for its reason: reporting one offender per run costs as many
    # runs as there are offenders, and this file had twenty.
    by_file = {}
    for rel, fid, why in G.UNREADABLE_DROPS:
        by_file.setdefault(rel, []).append((fid, why))
    for rel in sorted(by_file):
        rows = sorted(set(by_file[rel]))
        owner = G.path_owner(rel) or "unclaimed"
        detail = "; ".join("%s (%s)" % (fid, why) for fid, why in rows[:4])
        consequence = ("every fact row they anchor is left looking merely un-anchored"
                       if "/anchors/" in rel else
                       "these facts have never entered the registry, and an anchor naming "
                       "one of them reports as naming a fact that does not exist")
        c.err("G_DROP_UNREADABLE", rel,
              "[%s] %d row(s) REFUSED, not skipped: no reader can parse them, so this "
              "packet's evidence has never reached a guard and %s. %s%s"
              % (owner, len(rows), consequence, detail,
                 ", ..." if len(rows) > 4 else ""))


# A `guard` column naming a guard that never fires is a claim nothing tests.  115 rows were
# in that state: named, warned about individually, and carrying no statement of WHY -- so a
# reader could not tell a row nothing can check from a row somebody switched off, and a
# promotion out of the withheld set would have kept the same decorative guard name.
#
# These are the only two states in which a silent guard is legitimate, and both are DERIVED
# from the row's own columns rather than asserted by the token, so a row cannot keep the
# token once the condition stops holding.  The reason lives in `comment` and not in `guard`
# because the guard column is read by four other consumers -- guard_export_table's scope,
# TRANSCRIPT_GUARDS' scope, the BINARY_GUARDS prologue skip below, and gen_facts.claim_kind,
# which decides which SOURCES the row must cite.  Overwriting it for 115 rows would re-key
# their source requirements and narrow two registered scopes, which is the mechanism that
# produced silent guards in the first place.
COVERAGE_SITE_RE = re.compile(r"0x14[0-9A-Fa-f]{7}")
COVERAGE_TOKEN = re.compile(r"G_NONE\((\w+)\)")
COVERAGE_REASONS = {
    "WITHHELD_NO_SITE":
        "withheld, and the evidence records no instruction site to read the value back from",
    "WITHHELD_UNANCHORED":
        "withheld with a site recorded; an anchor would make this row binary-checked, which "
        "guard_verdict refuses for a withheld verdict, so the guard cannot fire until the "
        "row is promoted on identity evidence",
}
# checking + reasoned + silent + prologue_skip must account for every row; a bucket that
# stops being filled would otherwise shrink the total silently.
COVERAGE = {"checking": 0, "reasoned": 0, "silent": 0, "prologue_skip": 0}


def coverage_reason_code(r):
    """Why this row's named guard produces no check, read off the row, never off the token."""
    if r["verdict"] not in G.VERDICT_WITHHELD:
        return None
    return ("WITHHELD_UNANCHORED" if COVERAGE_SITE_RE.search(r["evidence"])
            else "WITHHELD_NO_SITE")


def guard_coverage(c):
    """Every row NAMES a guard in its `guard` column.  This asks whether that guard actually
    produced a check for the row, because a declared guard that never ran is a claim.

    IT COULD NOT FIRE.  guard_verdict runs before it and ended its per-row loop with an
    unconditional `c.checked.add`, so every row was already checked by the time this looked --
    and a registered scope of 0 rows made the emptiness read as deliberate rather than as a
    guard somebody had switched off.  Removing that one line exposed 102 rows.

    The two consequences are reported separately, because they are not the same defect.  An
    EMITTED row with no check is a usable constant nothing is watching, and that fails.  A
    WITHHELD row with no check is a row the generator emits no value for at all -- no
    translation unit can name it, so it cannot go wrong silently -- and reddening the gate
    over another packet's un-anchored backlog is how a guard gets switched off a second time.
    Those are named individually on every run and counted, never summarised away."""
    scope = set(guard_scope(c.rows, guard_coverage))
    if not scope:
        # The anti-vacuity arm, and it is the one that matters here: this guard spent its
        # whole life registered for nothing.  An empty scope is not "nothing to check", it is
        # "this guard has stopped asking", and it has to be as loud as a real finding.
        c.fail.append("G_COVERAGE registry: this guard's declared scope is EMPTY, so it "
                      "judges no row and passes whatever the registry holds. That is exactly "
                      "the state it was in while it was registered for 0 rows.")
        return
    for r in c.rows:
        tok = COVERAGE_TOKEN.search(r["comment"])
        if tok and r["verdict"] not in G.VERDICT_WITHHELD:
            # The re-check on promotion.  The token says "nothing checks this row"; a verdict
            # that emits the value makes that statement into an unwatched constant, so the
            # promotion has to bring an anchor with it and take the token away.
            c.err("G_COVERAGE", r["id"],
                  "carries G_NONE(%s) -- a recorded statement that no guard checks this row --"
                  " but the verdict is %s, which IS emitted. A promotion must anchor the row "
                  "and drop the token." % (tok.group(1), r["verdict"]))
        if r["id"] not in scope:
            continue
        if r["id"] in c.checked:
            COVERAGE["checking"] += 1
            continue
        if r["guard"] in BINARY_GUARDS and r["addr_space"] == "PC_VA" \
                and r["id"] + ".PROLOGUE" in c.by_id:
            COVERAGE["prologue_skip"] += 1
            continue
        if r["verdict"] in G.VERDICT_WITHHELD:
            want = coverage_reason_code(r)
            if tok is None:
                COVERAGE["silent"] += 1
                c.err("G_COVERAGE", r["id"],
                      "guard %s produced no check and the row records no reason. Anchor it, or "
                      "state G_NONE(%s) in the comment so the silence is a decision somebody "
                      "made rather than a guard that stopped asking." % (r["guard"], want))
            elif tok.group(1) != want:
                COVERAGE["silent"] += 1
                c.err("G_COVERAGE", r["id"],
                      "records G_NONE(%s), but this row's own columns say %s. The token is a "
                      "derivation, not a label." % (tok.group(1), want))
            else:
                COVERAGE["reasoned"] += 1
                c.warn.append("G_COVERAGE %s: guard %s produced no check -- G_NONE(%s): %s. "
                              "[%s]"
                              % (r["id"], r["guard"], want, COVERAGE_REASONS[want],
                                 c.pending_origin.get(r["id"], "registry")))
            continue
        COVERAGE["silent"] += 1
        c.err("G_COVERAGE", r["id"],
              "guard %s produced no check, and this row IS emitted: code can read the value "
              "and nothing would notice if it changed" % r["guard"])


def guard_arm64_withheld(c):
    """Nothing with ARM64 provenance may reach the generated header as a usable value."""
    header = c.header
    for r in c.rows:
        if r["provenance"] != "ARM64":
            continue
        name = G.cname(r["id"])
        if re.search(r"^inline constexpr \w+ %s\b" % re.escape(name), header, re.M):
            c.err("G_ADDRSPACE_REFUSE", r["id"], "emitted as a usable constant")
        # A raw-number scan would false-positive: LOD.THROTTLE_ENABLE_OFF is 0x208 and
        # PHYS.CI_STRIDE_ARM64 is 520, the same number for unrelated reasons.  Only flag a
        # literal that no PC-provenance row accounts for.
        v = G.parse_value(r)
        emitted_holds = any(rr["provenance"] != "ARM64" and G.blob_len(rr["type"]) is None
                            and G.parse_value(rr) == v for rr in c.rows)
        if not emitted_holds:
            for lit in ("0x%X" % v, "0x%x" % v, str(v)):
                if re.search(r"(?<![\w])%s(?![\w])" % re.escape(lit), header):
                    c.err("G_ADDRSPACE_REFUSE", r["id"],
                          "value %s appears in the generated header" % lit)
        c.checked.add(r["id"])


def guard_derived(c):
    """Facts that are only correct relative to each other.  The vtable slot ties three rows
    together -- table, slot offset and target -- and the .rdata file offset ties a VA to the
    section table.  Checking them apart would let the trio drift while each row looked fine."""
    if all(k in c.by_id for k in ("RENDER.VTABLE", "RENDER.VT_SLOT_OFF", "RENDER.FN")):
        slot = c.pe.read_va(c.val("RENDER.VTABLE") + c.val("RENDER.VT_SLOT_OFF"), 8)
        if slot is None:
            c.err("G_VTSLOT", "RENDER.VT_SLOT_OFF", "vtable slot is not readable on disk")
        else:
            tgt = struct.unpack_from("<Q", slot, 0)[0]
            if tgt != c.val("RENDER.FN"):
                c.err("G_VTSLOT", "RENDER.VT_SLOT_OFF",
                      "vtable 0x%X + 0x%X holds 0x%X, RENDER.FN is 0x%X"
                      % (c.val("RENDER.VTABLE"), c.val("RENDER.VT_SLOT_OFF"), tgt, c.val("RENDER.FN")))
        c.checked.add("RENDER.VT_SLOT_OFF"); c.binary_checked.add("RENDER.VT_SLOT_OFF")
        c.identity_checked.add("RENDER.VT_SLOT_OFF")

    # A float constant is two rows -- where it lives and what it is.  Reading the second
    # from the address in the first is what makes a wrong address visible.
    for va_id, bits_id in (("AUDIO.MAXFREQRATIO_VA", "AUDIO.MAXFREQRATIO_BITS"),
                           ("NET.REMOTE_RENDER_DELAY_VA", "NET.REMOTE_RENDER_DELAY_BITS")):
        if va_id not in c.by_id or bits_id not in c.by_id:
            continue
        raw = c.pe.read_va(c.val(va_id), 4)
        if raw is None:
            c.err("G_F32CONST", va_id, "0x%X is not readable on disk" % c.val(va_id))
        else:
            got = struct.unpack_from("<I", raw, 0)[0]
            if got != c.val(bits_id):
                c.err("G_F32CONST", va_id, "0x%X holds 0x%08X, %s is 0x%08X"
                      % (c.val(va_id), got, bits_id, c.val(bits_id)))
        c.checked.add(va_id); c.binary_checked.add(va_id)
        c.checked.add(bits_id); c.binary_checked.add(bits_id)
        # NOT identity.  A float constant is pooled: 0x1416CCEF8 holds 1.0f and so do
        # 262,432 other aligned .rdata slots, with 2708 instructions addressing that one.
        # Reading the value back confirms the slot's contents and says nothing about which
        # setting it is.  Identity for a pooled literal needs the instruction that USES it
        # in the role claimed, which is W1-G's to establish.

    if all(k in c.by_id for k in ("AUDIO.MAXFREQRATIO_VA", "AUDIO.MAXFREQRATIO_FILEOFF")):
        off = c.pe.rva_to_offset(c.val("AUDIO.MAXFREQRATIO_VA") - c.pe.image_base)
        if off != c.val("AUDIO.MAXFREQRATIO_FILEOFF"):
            c.err("G_F32CONST", "AUDIO.MAXFREQRATIO_FILEOFF",
                  "section table puts that VA at file offset 0x%X, registry has 0x%X"
                  % (off, c.val("AUDIO.MAXFREQRATIO_FILEOFF")))
        c.checked.add("AUDIO.MAXFREQRATIO_FILEOFF")
        c.binary_checked.add("AUDIO.MAXFREQRATIO_FILEOFF")
        c.identity_checked.add("AUDIO.MAXFREQRATIO_FILEOFF")


GUARDS = [guard_binary_identity, guard_prologue, guard_export_table, guard_anchors,
          guard_matsym,
          guard_rva_pairs, guard_derived, guard_func_starts, guard_hand_transcript,
          # Order matters: every guard that CONTRIBUTES identity evidence has to run before
          # the guards that JUDGE it.  guard_evidence_site ran after guard_verdict and its
          # rows were judged against an identity set it had not filled in yet.
          guard_evidence_site, guard_libr15_symbols, guard_text_at, guard_module_md5,
          guard_tail_calls, guard_role, guard_evidence_consistent,
          guard_pending_drops, guard_base_agrees, guard_value_checked,
          guard_not_in_code,
          guard_arm64_withheld, guard_provenance_evidence, guard_arm64_evidence,
          guard_verdict, guard_source_sanity, guard_coverage]

GUARDS_BY_NAME = {fn.__name__: fn for fn in GUARDS}

# guard_anchors checks a REGISTRY row whenever a CONTRIBUTED anchor names one, and its
# declared scope is registry anchors only -- on purpose, because counting contributed drops
# made every packet's drop move it and turned --update-hashes into the reflex answer to a
# red gate.  So it legitimately checks outside its scope, and only it does: the exemption is
# a named list rather than a rule, so adding to it is a decision somebody has to write down.
SCOPE_DETECTOR_EXEMPT = ("guard_anchors",)

# A guard defined but never registered runs zero times and passes silently -- the same
# failure as a test file with no test cases.  Registration is therefore checked, not
# trusted: anything named guard_* in this module must be in GUARDS.
_unregistered = sorted(
    n for n, o in list(globals().items())
    if n.startswith("guard_") and callable(o) and o not in GUARDS)
if _unregistered:
    raise SystemExit("factcheck: these guards are defined but never run: %s"
                     % ", ".join(_unregistered))


# --------------------------------------------------------------------------- guard hashing

def guard_scope(rows, fn):
    by_id = {r["id"] for r in rows}
    """The rows a guard claims.  Hashed with the body so narrowing the corpus is visible."""
    name = fn.__name__
    if name == "guard_prologue":
        # The prologue rows AND the functions they guard.  A prologue exists to say "the
        # bytes at THAT address still look like this", so the parent is as much in the
        # guard's coverage as the byte string is, and leaving it out left 189 rows able to
        # drop out of coverage without moving the hash.
        ids = [r["id"] for r in rows if G.blob_len(r["type"]) is not None]
        ids += [r["id"][: -len(".PROLOGUE")] for r in rows
                if r["id"].endswith(".PROLOGUE") and r["id"][: -len(".PROLOGUE")] in by_id]
    elif name == "guard_export_table":
        # It reads the PE export directory, so the three header facts it reads it THROUGH
        # are checked by it just as much as the exports are.
        ids = [r["id"] for r in rows if r["guard"] == "G_EXPORT"]
        ids += [i for i in ("PE.EXPORT_COUNT", "PE.EXPORT_DIR_RVA", "PE.IMAGE_BASE")
                if i in by_id]
    elif name == "guard_anchors":
        ids = [p[0] for p, _ in G.read_anchor_rows(registry_only=True)]
    elif name == "guard_hand_transcript":
        ids = [r["id"] for r in rows if r["guard"] in TRANSCRIPT_GUARDS]
    elif name == "guard_arm64_withheld":
        ids = [r["id"] for r in rows if r["provenance"] == "ARM64"]
    elif name == "guard_rva_pairs":
        ids = [r["id"] for r in rows if r["addr_space"] == "PC_RVA"]
    elif name == "guard_derived":
        ids = [i for i in ("RENDER.VTABLE", "RENDER.VT_SLOT_OFF", "RENDER.FN",
                           "AUDIO.MAXFREQRATIO_VA", "AUDIO.MAXFREQRATIO_BITS",
                           "AUDIO.MAXFREQRATIO_FILEOFF", "NET.REMOTE_RENDER_DELAY_VA",
                           "NET.REMOTE_RENDER_DELAY_BITS") if i in {r["id"] for r in rows}]
    elif name == "guard_source_sanity":
        ids = [r["id"] for r in rows if r["verdict"] in G.VERDICT_WITHHELD]
    elif name == "guard_value_checked":
        ids = [r["id"] for r in rows if r["verdict"] not in G.VERDICT_WITHHELD]
    elif name == "guard_not_in_code":
        ids = [r["id"] for r in rows if r["verdict"] == "NOT_IN_CODE"]
    elif name == "guard_base_agrees":
        ids = [r["id"] for r in rows if r["addr_space"] == "OFFSET" and r["base"] != "-"]
    elif name == "guard_evidence_consistent":
        ids = [r["id"] for r in rows
               if r["verdict"] in ("PC_DECISIVE", "PC_CORROBORATED")]
    elif name == "guard_role":
        ids = [r["id"] for r in rows]
    elif name == "guard_tail_calls":
        ids = [r["id"] for r in rows if r["addr_space"] == "PC_VA"
               and (r["effect"] not in ("-", "") or r["verb"] == "mutate")]
    elif name == "guard_text_at":
        ids = [r["id"] for r in rows if r["type"] == "str"]
        # The guard reads each .TEXT row's OWNER address, so every owner is inside its coverage.
        # Naming one owner by hand held only while the registry carried a single string fact.
        ids += [r["id"][: -len(".TEXT")] for r in rows
                if r["type"] == "str" and r["id"].endswith(".TEXT")
                and r["id"][: -len(".TEXT")] in by_id]
    elif name == "guard_module_md5":
        ids = [r["id"] for r in rows if r["type"] == "md5"]
    elif name == "guard_evidence_site":
        ids = [r["id"] for r in rows if EVIDENCE_MNEMONIC.search(r["evidence"])]
    elif name == "guard_libr15_symbols":
        ids = [r["id"] for r in rows if "libr15_symbols" in (r["source_a"], r["source_b"])]
    elif name == "guard_verdict":
        ids = [r["id"] for r in rows]
    elif name == "guard_matsym":
        ids = [r["id"] for r in rows if MATSYM_MARK.search(r["comment"])]
        ids += [i for i in ("EVENT.MATSYM_NS", "EVENT.MATSYM_CHAR_TABLE",
                            "EVENT.MATSYM_TABLE") if i in by_id]
    elif name == "guard_coverage":
        # Every row, because it judges every row's `guard` column.  It was falling through to
        # the empty default, which registered it for nothing and made its own vacuity look
        # like a decision somebody had made.
        ids = [r["id"] for r in rows]
    elif name == "guard_arm64_evidence":
        ids = [r["id"] for r in rows if r["provenance"] == "ARM64"
               or "libr15_symbols" in (r["source_a"], r["source_b"])]
    elif name == "guard_provenance_evidence":
        ids = [r["id"] for r in rows if r["provenance"] == "PC"]
    elif name == "guard_func_starts":
        ids = [r["id"] for r in rows if r["addr_space"] == "PC_VA" and G.blob_len(r["type"]) is None]
    else:
        ids = []
    return sorted(ids)


def guard_scopes(rows):
    """name -> the id set each guard declares it covers."""
    return {fn.__name__: set(guard_scope(rows, fn)) for fn in GUARDS}


def guard_hashes(rows):
    out = {}
    for fn in GUARDS:
        body = inspect.getsource(fn)
        scope = guard_scope(rows, fn)
        h = hashlib.sha256()
        h.update(body.encode("utf-8"))
        h.update(b"\x00SCOPE\x00")
        h.update(("\n".join(scope)).encode("utf-8"))
        out[fn.__name__] = (h.hexdigest(), len(scope))
    return out


def load_registered_scopes():
    """The id sets recorded at the last blessing.  Empty when the file is not there yet,
    which makes every narrowing unexplained -- the safe direction."""
    out = {}
    try:
        with open(SCOPE_IDS, encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or "\t" not in line:
                    continue
                k, fid = line.rstrip("\n").rstrip("\r").split("\t", 1)
                out.setdefault(k, set()).add(fid)
    except IOError:
        pass
    return out


REGISTERED_SCOPES = load_registered_scopes()


def read_registered():
    """The registered hashes, plus the blessing ledger already in the file."""
    want, ledger = {}, []
    with open(GUARD_HASHES, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n").rstrip("\r")
            if line.startswith("# REGISTERED") or line.startswith("#     "):
                ledger.append(line)
                continue
            if not line or line.startswith("#"):
                continue
            p = line.split("\t")
            if p[0] == "guard":
                continue
            want[p[0]] = (p[2], int(p[1]))   # extra columns ignored
    return want, ledger


def scope_swap(name, scopes):
    """(ids that left, ids that joined) for a guard whose scope COUNT did not move.

    Renaming one registry id removes one member and adds one, so the count is identical and
    the hash is not.  That was being reported as "BODY changed with scope unmoved", which is
    false and expensive: the only way to bless it was --accept-body-change, the flag whose
    whole job is to mean "the assertion inside the guard changed".  Landing a rename with it
    spends the one signal that catches a weakened assertion.

    REGISTERED_SCOPES is the registered id set, by name, so the two cases are separable
    exactly.  When it is unavailable the answer is "no swap", which routes back to `body` --
    the conservative direction, the same one load_registered_scopes() already takes."""
    was = REGISTERED_SCOPES.get(name)
    if not was:
        return (), ()
    now = set(scopes.get(name, ()))
    return tuple(sorted(was - now)), tuple(sorted(now - was))


def classify_registration(have, want, scopes=None):
    """How each guard's registration would change, in the terms that decide the flags."""
    grew, shrank, body, new, gone, swapped = [], [], [], [], [], []
    for k in sorted(set(have) | set(want)):
        if k not in want:
            new.append(k)
        elif k not in have:
            gone.append(k)
        elif have[k][0] != want[k][0]:
            if have[k][1] > want[k][1]:
                grew.append((k, want[k][1], have[k][1]))
            elif have[k][1] < want[k][1]:
                shrank.append((k, want[k][1], have[k][1]))
            else:
                left, joined = scope_swap(k, scopes or {})
                if left or joined:
                    swapped.append((k, left, joined))
                else:
                    body.append(k)
    return grew, shrank, body, new, gone, swapped


def check_guard_hashes(rows, update=False, reason=None,
                       accept_narrowing=False, accept_body=False, amended=()):
    # Scope is the REGISTRY, never the merged-with-pending view.  A drop is a proposal:
    # if an unmerged one could move a guard's declared scope, every packet dropping a file
    # would red this check, --update-hashes would become the reflex answer to a red gate,
    # and a rule-6 hash that is re-blessed on reflex is not detecting anything.  Scope now
    # moves only when W0-2 merges -- which is a deliberate act, and the point at which a
    # --reason is a real one.  Pending rows are still CHECKED by every guard; they just do
    # not get a vote on what the guards are registered for.
    # ONE population, used for every comparison below.  `have` is deliberately the registry
    # and not the merged-with-pending view, so the scope sets compared against it have to be
    # the registry's too -- reading them off the caller's rows counted every unmerged drop row
    # as having "joined", which is a swap report about a merge that has not happened.
    registry_rows = G.load(G.FACTS_TSV, "facts.tsv")
    registry_scopes = guard_scopes(registry_rows)
    have = guard_hashes(registry_rows)
    if update:
        try:
            want, ledger = read_registered()
        except IOError:
            want, ledger = {}, []
        scopes = registry_scopes
        grew, shrank, body, new, gone, swapped = classify_registration(have, want, scopes)
        if not (grew or shrank or body or new or gone or swapped) and os.path.exists(SCOPE_IDS):
            print("guard hashes already match; nothing to re-register")
            return []
        refuse = []
        if not reason:
            refuse.append("re-registration needs --reason: it blesses whatever is there now, "
                          "and an unexplained blessing is how rule 6 stops meaning anything")
        if shrank and not accept_narrowing and amended:
            # A merge that amended rows can legitimately narrow a CONTENT-derived scope --
            # editing an evidence string moves that row out of guard_evidence_site.  That is
            # the amendment's own effect, and only for the rows it touched.  Any other
            # departure is the change rule 6 exists to catch, so name the rows, not a count.
            unexplained = []
            for k, w, h in shrank:
                left = REGISTERED_SCOPES.get(k, set()) - scopes.get(k, set())
                if not left or not left <= set(amended):
                    unexplained.append((k, w, h))
            if not unexplained:
                shrank = []
        if shrank and not accept_narrowing:
            for k, w, h in shrank:
                refuse.append("%s now covers FEWER rows (%d -> %d). That is the change rule 6 "
                              "exists to catch. Pass --accept-narrowing if it is intended."
                              % (k, w, h))
        if body and not accept_body:
            refuse.append("the assertion inside %s changed with its scope unmoved. Pass "
                          "--accept-body-change if that is intended."
                          % ", ".join(body))
        if gone and not accept_narrowing:
            refuse.append("registered guard(s) %s have disappeared; --accept-narrowing "
                          "acknowledges dropping them." % ", ".join(gone))
        if refuse:
            print("REFUSING TO RE-REGISTER", file=sys.stderr)
            for r in refuse:
                print("    " + r, file=sys.stderr)
            print("", file=sys.stderr)
            print("What would change:", file=sys.stderr)
            for k, w, h in grew:
                print("    %-28s scope %d -> %d" % (k, w, h), file=sys.stderr)
            for k, w, h in shrank:
                print("    %-28s scope %d -> %d  NARROWING" % (k, w, h), file=sys.stderr)
            for k in body:
                print("    %-28s body changed" % k, file=sys.stderr)
            for k, left, joined in swapped:
                print("    %-28s scope swapped: -%s +%s"
                      % (k, ",".join(left[:3]), ",".join(joined[:3])), file=sys.stderr)
            for k in new:
                print("    %-28s newly registered" % k, file=sys.stderr)
            return None
        stamp = ["# REGISTERED %s%s%s"
                 % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "  --accept-narrowing" if accept_narrowing else "",
                    "  --accept-body-change" if accept_body else "")]
        for k, w, h in grew:
            stamp.append("#     %s scope %d -> %d" % (k, w, h))
        for k, w, h in shrank:
            stamp.append("#     %s scope %d -> %d  NARROWING" % (k, w, h))
        for k in body:
            stamp.append("#     %s body changed" % k)
        for k, left, joined in swapped:
            stamp.append("#     %s scope swapped at %d rows: -%s +%s"
                         % (k, have[k][1], ",".join(left[:3]) or "(none)",
                            ",".join(joined[:3]) or "(none)"))
        for k in new:
            stamp.append("#     %s newly registered" % k)
        for k in gone:
            stamp.append("#     %s dropped" % k)
        stamp.append("#     reason: " + reason)
        with open(GUARD_HASHES, "w", encoding="utf-8", newline="\n") as f:
            f.write("# sha256 over each guard's BODY and its DECLARED SCOPE.\n")
            f.write("# Hashing the guard's name would let someone weaken the assertion inside it\n")
            f.write("# (`== 480` -> `> 0`) or drop rows from the set it covers, with nothing\n")
            f.write("# detecting either.  Regenerate deliberately: factcheck.py --update-hashes,\n")
            f.write("# which needs a --reason and refuses a narrowing or a body change without\n")
            f.write("# its own flag.  Every blessing since is recorded below, newest last.\n")
            for line in (ledger + stamp)[-120:]:
                f.write(line + "\n")
            f.write("guard\tscope_rows\tsha256\tscope_ids_sha256\n")
            scopes = guard_scopes(G.load(G.FACTS_TSV, "facts.tsv"))
            for k in sorted(have):
                f.write("%s\t%d\t%s\t%s\n"
                        % (k, have[k][1], have[k][0],
                           hashlib.sha256(("\n".join(sorted(scopes.get(k, ()))))
                                          .encode("utf-8")).hexdigest()))
            with open(SCOPE_IDS, "w", encoding="utf-8", newline="\n") as g:
                g.write("# Which rows each guard declares it covers, so that a NARROWING can "
                        "be checked\n# by name instead of by count.  Written by "
                        "factcheck.py --update-hashes.\n")
                for k in sorted(scopes):
                    for fid in sorted(scopes[k]):
                        g.write("%s\t%s\n" % (k, fid))
        print("re-registered %d guard(s): %s"
              % (len(grew) + len(shrank) + len(body) + len(new) + len(gone) + len(swapped),
                 reason))
        return []
    want, _ = read_registered()
    bad = []
    for k in sorted(set(have) | set(want)):
        if k not in want:
            bad.append("G_HASH %s: guard is not registered in facts/guard_hashes.tsv" % k)
        elif k not in have:
            bad.append("G_HASH %s: registered guard has DISAPPEARED -- re-registering it "
                       "away needs --accept-narrowing" % k)
        elif have[k][0] != want[k][0]:
            if have[k][1] > want[k][1]:
                what = ("scope GREW %d -> %d rows; rows entered its coverage"
                        % (want[k][1], have[k][1]))
            elif have[k][1] < want[k][1]:
                what = ("scope NARROWED %d -> %d rows; it now checks fewer than it was "
                        "registered for, and re-registering needs --accept-narrowing"
                        % (want[k][1], have[k][1]))
            else:
                left, joined = scope_swap(k, registry_scopes)
                if left or joined:
                    what = ("scope SWAPPED at %d rows -- %d id(s) left and %d joined (-%s +%s); "
                            "the body is unchanged, so re-registering needs only a --reason"
                            % (have[k][1], len(left), len(joined),
                               ",".join(left[:2]), ",".join(joined[:2])))
                else:
                    what = ("BODY changed with scope unmoved; the assertion itself differs, "
                            "and re-registering needs --accept-body-change")
            bad.append("G_HASH %s: %s (sha256 %s, registered %s)"
                       % (k, what, have[k][0][:16], want[k][0][:16]))
    return bad


# --------------------------------------------------------------------------- driver

def run(rows, check_hashes=True):
    """Returns a list of failures.  Empty means every fact was checked and agreed."""
    pending_origin = {r["id"]: r["_origin"] for r in rows
                      if r.get("_origin", "").startswith("pending/")}
    fails, warns = [], []
    for m in G.validate(rows):
        # validate() yields "FACT_ID: what is wrong"; an exact split, never a substring
        # match, or FOO would swallow the failures of FOO.BAR.
        (warns if m.split(":", 1)[0] in pending_origin else fails).append(m)
    c = Ctx(rows, pending_origin)
    for fn in GUARDS:
        c.running = fn.__name__
        try:
            fn(c)
        except Exception as e:
            c.err("G_CRASH", fn.__name__, "%s: %s" % (type(e).__name__, e))
    c.running = None
    for name, touched in sorted(c.touched.items()):
        if name in SCOPE_DETECTOR_EXEMPT:
            continue
        registry = {r["id"] for r in rows if not r.get("_origin", "").startswith("pending/")}
        outside = sorted((touched & registry) - set(guard_scope(rows, GUARDS_BY_NAME[name])))
        if outside:
            fails.append("G_SCOPE %s: checks %d row(s) its declared scope does not contain "
                         "(%s%s). The rule-6 hash then covers its body only, so those rows "
                         "could leave its coverage undetected -- add a branch in "
                         "guard_scope()." % (name, len(outside), ", ".join(outside[:4]),
                                             ", ..." if len(outside) > 4 else ""))
    fails.extend(c.fail)
    warns.extend(c.warn)
    # A warning about an UNMERGED drop is a third verdict, not a soft failure: the guard was
    # asked, and refused to score a row that is still a proposal.  Six G_TRANSCRIPT lines and
    # a dozen others have sat here reading like registry defects nobody was fixing.  They are
    # known, they are named, and they carry their reason -- so they get their own word.
    _pending_warn = re.compile(r"\[unmerged, in pending/[^\]]+\]$")
    pending_refused = 0
    for w in sorted(set(warns)):
        if _pending_warn.search(w):
            pending_refused += 1
            print("PENDING-REFUSED " + w)
        else:
            print("WARN " + w)
    if pending_refused:
        print("  pending-refused=%d guard/row pair(s) belong to unmerged drops: reported with "
              "their reason, not counted as gate failures" % pending_refused)
    if check_hashes:
        fails.extend(check_guard_hashes(rows))
    return fails, len(c.checked)




# Exit 2 means THE QUESTION WAS NOT ASKED.  Exit 1 means it was asked and the answer was
# no.  Collapsing them is how a missing prerequisite gets read as a registry defect and
# sends somebody hunting for a fault that is not there -- and how, in the other direction,
# a real failure hides behind "the environment is broken".  Not a pass either way.
NOTRUN = 2


def prerequisite_missing():
    """The reason this check cannot run, or None when it can."""
    if not os.path.exists(G.ECHOVR_EXE):
        return ("echovr.exe is not at %s, so no guard can read the binary the whole "
                "registry is pinned to" % G.ECHOVR_EXE)
    try:
        pe_load(G.ECHOVR_EXE)
    except Exception as exc:
        return "echovr.exe at %s could not be read: %s: %s" % (
            G.ECHOVR_EXE, type(exc).__name__, exc)
    return None


MIN_EXPECTED_FACTS = 250        # the seed registry.  A sudden collapse is a broken load,
MIN_EXPECTED_GUARDS = 12        # not an empty pass.
MIN_CHECKED_FACTS = 250         # nor is a run in which the guards touched almost nothing.


def main():
    update = "--update-hashes" in sys.argv
    rows = G.load_all()
    if update:
        reason = None
        if "--reason" in sys.argv:
            i = sys.argv.index("--reason")
            if i + 1 < len(sys.argv):
                reason = sys.argv[i + 1].strip()
        amended = []
        if "--amended" in sys.argv:
            i = sys.argv.index("--amended")
            if i + 1 < len(sys.argv):
                amended = [x for x in sys.argv[i + 1].split(",") if x]
        if check_guard_hashes(rows, update=True, reason=reason,
                              accept_narrowing="--accept-narrowing" in sys.argv,
                              accept_body="--accept-body-change" in sys.argv,
                              amended=amended) is None:
            return 2
        return 0

    why = prerequisite_missing()
    if why:
        print("NOT RUN factcheck: %s" % why)
        print("RESULT factcheck status=NOTRUN facts=%d checked=0 guards=%d failures=0 "
              "reason=prerequisite" % (len(rows), len(GUARDS)))
        return NOTRUN

    fails, checked = run(rows)
    try:
        if G.emit_header(G.registry_only(rows)) != open(G.OUT_HEADER, encoding="utf-8").read():
            fails.append("G_STALE facts_generated.h: does not match facts.tsv; run tools/gen_facts.py")
    except Exception as e:
        fails.append("G_STALE facts_generated.h: %s: %s" % (type(e).__name__, e))

    # A check that checked nothing must never be able to look like a check that passed.
    # An empty registry, a failed load and an emptied guard list all produce "no failures",
    # and every one of them is a broken gate rather than a clean one.
    if len(rows) < MIN_EXPECTED_FACTS:
        fails.append("G_EMPTY registry: loaded %d facts, expected at least %d -- refusing to "
                     "report a pass over a registry this small" % (len(rows), MIN_EXPECTED_FACTS))
    if len(GUARDS) < MIN_EXPECTED_GUARDS:
        fails.append("G_EMPTY guards: %d registered, expected at least %d"
                     % (len(GUARDS), MIN_EXPECTED_GUARDS))
    # NOT "every row": guard_coverage owns the per-row question now, and it distinguishes an
    # emitted row from a withheld one.  What is left here is the anti-vacuity floor -- a run
    # in which the guards touched almost nothing must never look like a clean one.
    if checked < MIN_CHECKED_FACTS:
        fails.append("G_EMPTY coverage: only %d of %d facts were checked, expected at least "
                     "%d -- refusing to report a pass over a run this thin"
                     % (checked, len(rows), MIN_CHECKED_FACTS))

    # The count the auditor asked for, in the line that reports it: a silent guard is one
    # that produced no check and gave no reason, and there must not be one.  The buckets are
    # summed against the registry size so that a bucket which stops being filled cannot make
    # the silent count fall to zero by arithmetic.
    seen = sum(COVERAGE.values())
    if seen != len(rows):
        fails.append("G_COVERAGE registry: the coverage buckets account for %d of %d rows, so "
                     "the guards: line is measuring a different population than the registry"
                     % (seen, len(rows)))
    if COVERAGE["silent"]:
        fails.append("G_COVERAGE registry: %d row(s) name a guard that produced no check and "
                     "record no reason" % COVERAGE["silent"])
    print("guards: checking=%d silent=%d reasoned=%d prologue-covered=%d"
          % (COVERAGE["checking"], COVERAGE["silent"], COVERAGE["reasoned"],
             COVERAGE["prologue_skip"]))

    for f in fails:
        print("FAIL " + f)

    # Machine-readable, because a runner that swallows exit codes will report a pass over
    # anything.  Whatever consumes this must treat a missing RESULT line, status=FAIL, or
    # checked=0 as a failure.
    print("RESULT factcheck status=%s facts=%d checked=%d guards=%d failures=%d"
          % ("PASS" if not fails else "FAIL", len(rows), checked, len(GUARDS), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
