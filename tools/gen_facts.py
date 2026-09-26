#!/usr/bin/env python3
"""Generate facts/facts_generated.h from facts/facts.tsv.

Values only.  The header never declares a struct, a class layout or a field the
constants describe -- a guard that generates both sides of its own comparison can
never fail.

LIBR15_VA rows are emitted as *incomplete types with no value at all*.  There is no
cast, no reinterpret_cast and no pointer arithmetic that recovers the number, because
the number is not in the file.  W1-I gets the addresses from facts/libr15_worklist.txt,
which no translation unit includes.

Usage:
    gen_facts.py                     regenerate the header
    gen_facts.py --seed-prologues    fill '?' bytes<N> rows from the real echovr.exe
    gen_facts.py --check             fail if the header on disk is not what we would emit
    gen_facts.py --check              regenerate nothing; report whether the generated header
                                      is stale AND validate every facts/pending/ row by the
                                      same per-row rules, failing on any row no reader can take
    gen_facts.py --merge-pending      merge facts/pending/*.tsv into facts.tsv, re-deriving
                                      each row's verdict and tier from guard evidence rather
                                      than from what the submitting packet claimed
"""

import argparse
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FACTS_TSV = os.path.join(ROOT, "facts", "facts.tsv")
PENDING_DIR = os.path.join(ROOT, "facts", "pending")


def rel_to_root(p):
    """`p` relative to ROOT -- or its absolute path when the two are on different mounts.

    os.path.relpath raises ValueError across Windows drive letters, so a drop channel or a
    build directory under a %TEMP% on C: with the repo on D: used to kill the loader before
    any row could be reported.  An absolute path names the file no packet owns it; that is
    the honest answer for one outside the tree."""
    try:
        return os.path.relpath(p, ROOT)
    except ValueError:
        return os.path.abspath(p)

# Emitted under src/ so the include the design specifies -- "generated/facts_generated.h"
# with the -Isrc every target already has -- actually resolves.  Living in facts/ meant
# no target could reach it, which made a registry eleven packets are told to cite
# unreachable from any of them.
OUT_HEADER = os.path.join(ROOT, "src", "generated", "facts_generated.h")
OUT_WORKLIST = os.path.join(ROOT, "facts", "libr15_worklist.txt")
ANCHORS_TSV = os.path.join(ROOT, "facts", "anchors.tsv")
RETIRED_TSV = os.path.join(ROOT, "facts", "retired.tsv")
# The anchor half of the contribution channel.  Packets drop anchors here for the same
# reason they drop facts in facts/pending/: so they never have to edit a file W0-2 owns.
PENDING_ANCHORS_DIR = os.path.join(ROOT, "facts", "pending", "anchors")


def anchor_sources():
    """facts/anchors.tsv plus every contributed drop, newest last."""
    paths = [ANCHORS_TSV]
    if os.path.isdir(PENDING_ANCHORS_DIR):
        paths += [os.path.join(PENDING_ANCHORS_DIR, n)
                  for n in sorted(os.listdir(PENDING_ANCHORS_DIR)) if n.endswith(".tsv")]
    return [p for p in paths if os.path.exists(p)]


# Anchor rows no reader can parse, whatever file they came from.  Kept apart from
# PENDING_COMPLAINTS because they are not the same kind of thing: a complaint is about a
# row's CONTENT and a drop's content is a proposal, while one of these is a row that never
# reached any guard at all.
UNREADABLE_DROPS = []       # (relative path, fact_id, why)


def complain_anchor(origin, fact_id, msg, unreadable=False):
    """A malformed anchor row: fatal in the registry, a warning while it is still a drop.

    EXCEPT when the row cannot be READ.  A drop whose content is wrong is a proposal and
    warns until its owner fixes it; a row whose SHAPE the reader cannot parse is not a
    proposal at all -- it is evidence that has silently left the system.  Twenty of them sat
    in one anchor file being counted as ordinary drop noise, so that packet's evidence had
    never once reached the gate and nobody knew.  Those refuse, wherever they came from."""
    rel = origin.replace(os.sep, "/")
    if unreadable:
        UNREADABLE_DROPS.append((rel, fact_id, msg))
        return
    sev = "warn" if rel.startswith("facts/pending/") else "fail"
    PENDING_COMPLAINTS.append((sev, os.path.join(ROOT, origin), 0, "%s: %s" % (fact_id, msg)))


_FROZEN = {}


def freeze_inputs():
    """Serve one snapshot of the anchor files for the rest of this process.

    For a long sweep only.  A run that takes an hour while a dozen packets drop files is
    not measuring one registry, and re-reading every anchor file inside every guard is
    most of what makes it take an hour."""
    _FROZEN["anchors"] = None
    _FROZEN["anchors_registry"] = None


def read_anchor_rows(registry_only=False):
    """(row, origin) for every anchor, mine and contributed.

    registry_only drops the contributed ones.  Checking wants them -- a contributed anchor
    should be validated before it is merged, not after -- but anything that declares a
    guard's SCOPE must not move because somebody dropped a file."""
    key = "anchors_registry" if registry_only else "anchors"
    if _FROZEN.get(key) is not None:
        return _FROZEN[key]
    out = []
    for path in ([ANCHORS_TSV] if registry_only else anchor_sources()):
        origin = rel_to_root(path)
        for line in open(path, "r", encoding="utf-8", newline="").read().splitlines():
            line = line.rstrip("\r")   # a CRLF drop must not import its CR into the last column
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "fact_id":
                continue
            if len(parts) < 6:
                if path != ANCHORS_TSV:
                    complain_anchor(origin, parts[0],
                                    "anchor row has %d columns, expected at least 6 "
                                    "(fact_id, kind, anchor_va, a, b, instr_bytes)"
                                    % len(parts), unreadable=True)
                    continue
                raise FactError("%s: anchor row has %d columns, expected at least 6"
                                % (origin, len(parts)))
            out.append((parts, origin))
    if key in _FROZEN:
        _FROZEN[key] = out
    return out


def retired_ids():
    """id -> surviving id.  A retirement the merge does not read is undone by it."""
    out = {}
    if not os.path.exists(RETIRED_TSV):
        return out
    with open(RETIRED_TSV, "r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) >= 2 and p[0] != "retired_id":
                out[p[0]] = p[1]
    return out


def anchored_ids():
    """Fact ids with an anchor, from facts/anchors.tsv OR a contributed drop."""
    return {parts[0] for parts, _ in read_anchor_rows()}

sys.path.insert(0, HERE)
from echovr_path import find_echovr_exe  # noqa: E402

ECHOVR_EXE = find_echovr_exe() or "(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)"
ECHOVR_MD5 = "caa05ee3bca2f5df0809994e93e2cc05"

COLS = ["id", "value", "addr_space", "base", "provenance", "rival_arm", "type", "verdict",
        "tier", "verb", "observed", "effect", "evidence", "guard", "source_a", "source_b",
        "comment"]
# What the design needs the cited function to DO, and what the decompilation shows it doing.
# Kept on one row because the N-7 defect -- a row needing a wake, citing a function that
# sleeps -- was invisible while the requirement and the behaviour lived in two documents.
VERBS = ("mutate", "read", "compute", "construct", "-")
OBSERVED = ("mutates", "mutates_via_delegate", "mutates_via_vtable_thunk",
            "reads_const", "computes", "constructs", "-")
MUTATING = ("mutates", "mutates_via_delegate", "mutates_via_vtable_thunk")
# Drops are written by other packets on their own schedule, so a schema addition must not
# reject a file that was correct when it was written.  Columns added since are filled with
# `-`, which validates: a row that says nothing about a verb is not claiming one.
LEGACY_COLS = [c for c in COLS if c not in ("verb", "observed", "effect", "base")]
LEGACY_COLS_V2 = [c for c in COLS if c not in ("effect", "base")]
LEGACY_COLS_V3 = [c for c in COLS if c != "base"]
ADDR_SPACES = ("PC_VA", "PC_RVA", "LIBR15_VA", "OFFSET", "CONST")
PROVENANCES = ("PC", "ARM64", "UNKNOWN")
# A verdict is the assessment; the tier is what the project is allowed to do with it.
VERDICTS = ("PC_DECISIVE", "PC_CORROBORATED", "PC_PRESENT", "PC_ABSENT",
            "NOT_IN_CODE", "ARM64_ONLY", "NOT_ASSESSED",
            "PC_CANDIDATE")
VERDICT_MAY_BE_VERIFIED = ("PC_DECISIVE", "PC_CORROBORATED")
# Withheld = the generator emits no value at all.  Anything the project has no basis
# for belongs here: "not a pass" and "usable" cannot be the same state.
VERDICT_WITHHELD = ("PC_ABSENT", "ARM64_ONLY", "PC_PRESENT", "NOT_ASSESSED",
                    "PC_CANDIDATE")
# Withheld from `facts::`, but emitted into `facts::candidate::` where an experiment may
# use it.  A candidate is a QUESTION the harness exists to answer, not a claim.
VERDICT_CANDIDATE = "PC_CANDIDATE"
TIERS = ("V-PRIMARY", "V", "U")
# `str` holds ASCII read back from the image; `md5` holds the 32-hex module digest.
TYPES = ("u64", "u32", "bytes8", "bytes20", "bytes32", "bytes48", "str", "md5")
PROLOGUE_LEN = 20          # the default width --seed-prologues reads; NOT the only width
BYTES_TYPE = re.compile(r"^bytes(\d+)$")


def blob_len(type_name):
    """N for a bytes<N> row, None for anything else.  The length lives in the type name, so
    adding bytes48 is a row edit rather than a change to five hardcoded comparisons."""
    m = BYTES_TYPE.match(type_name or "")
    return int(m.group(1)) if m else None
SOURCES_TSV = os.path.join(ROOT, "facts", "sources.tsv")


# What a guard's name says about the KIND of claim its row makes.  Static is the default
# because most of the registry is layout; the other two are named explicitly so that adding
# a guard to either family is a decision somebody writes down.
WIRE_GUARDS = ("G_CORPUS", "G_WIRE")
RUNTIME_GUARDS = ("G_HARNESS",)

CLAIM_ACCEPTS = {
    "static":  ("static", "both"),
    "runtime": ("runtime", "both"),
    # The serialiser in the binary sees the same encoding the wire does, so static evidence
    # settles a wire claim as well as captured bytes do.  A runtime reading does not: what a
    # live struct holds is not what was transmitted.
    "wire":    ("wire", "static", "both"),
}

CLAIM_WHY = {
    "static": "A runtime reading corroborates a layout; it does not establish one.",
    "runtime": "This guard records what the running system DID, so a source that only "
               "describes the build on disk cannot certify it.",
    "wire": "Captured bytes or the code that serialises them; a reading of a live struct "
            "says nothing about what was transmitted.",
}


def claim_kind(guard):
    if guard in WIRE_GUARDS:
        return "wire"
    if guard in RUNTIME_GUARDS:
        return "runtime"
    return "static"


def load_sources():
    """name -> (artifact, fabricatable).  Two names that reduce to one artifact are one source."""
    out = {}
    with open(SOURCES_TSV, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "source":
                continue
            out[parts[0]] = (parts[1], parts[2] == "no",
                             tuple(k.strip() for k in parts[3].split(",") if k.strip()))
    return out


PENDING_COMPLAINTS = []   # (severity, path, lineno, message) from drop files

# Which drops --merge-pending is allowed to fold in.  Empty means the whole channel.
MERGE_ONLY = set()


def drop_selected(name):
    """Is this drop file in scope for the merge that is running?

    The channel is merged by ONE packet and committed under ONE packet's message, so a
    blanket merge lands every contributor's backlog -- 128 rows across eight packets, at
    the time this was added -- under whichever message the merger happened to be writing.
    That is the entanglement the dirty-tree refusal already names, arriving through the
    door the refusal does not cover.  `--only W2-D` merges one contributor's drop.

    The packet is the filename stem up to the first underscore, exactly as path_owner and
    the ALREADY-MERGED messages read it, so W1-D_R2.tsv belongs to W1-D."""
    if not MERGE_ONLY:
        return True
    return name[:-4].split("_")[0] in MERGE_ONLY


class FactError(Exception):
    pass


def write_atomic(path, text):
    """Write via a temp file and one rename, so a reader never sees a half-written file.

    Verification reads facts.tsv while another packet may be merging into it.  A plain
    truncate-and-write leaves a window where the file is short, and a reader that lands
    in it gets a row with one column and a traceback -- which happened, and reads as
    registry corruption rather than as a race.  os.replace is atomic on Windows and
    POSIX: the file is either the old one or the new one, never a prefix of either."""
    import tempfile
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".tmp_", suffix=".part")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise

def load(path, origin, tolerant=False):
    rows = []
    legacy = False
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "id":
                if parts == COLS:
                    legacy = False
                elif parts == LEGACY_COLS:
                    legacy = "v1"
                elif parts == LEGACY_COLS_V2:
                    legacy = "v2"
                elif parts == LEGACY_COLS_V3:
                    legacy = "v3"
                else:
                    if tolerant:
                        PENDING_COMPLAINTS.append(
                            ("fail", path, lineno,
                             "header is not any known schema, so no row in this file can be "
                             "read; the whole drop is skipped"))
                        return []
                    raise FactError("%s:%d header is %r, expected %r" % (path, lineno, parts, COLS))
                continue
            if legacy:
                cols = {"v1": LEGACY_COLS, "v2": LEGACY_COLS_V2, "v3": LEGACY_COLS_V3}[legacy]
                if len(parts) != len(cols):
                    if tolerant:
                        complain_anchor(
                            rel_to_root(path), parts[0],
                            "fact row has %d columns under this file's %d-column header, so "
                            "its columns cannot be identified" % (len(parts), len(cols)),
                            unreadable=True)
                        continue
                    raise FactError("%s:%d has %d columns, expected %d"
                                    % (path, lineno, len(parts), len(cols)))
                r = dict(zip(cols, parts))
                # Every column, not the three that had been noticed: a widened legacy row
                # must be shape-identical to a current one or some consumer eventually
                # indexes a key that is not there. guard_scope did, on `base`.
                for c in COLS:
                    r.setdefault(c, "-")
                r["effect"] = "-"
            else:
                if len(parts) != len(COLS):
                    if tolerant:
                        complain_anchor(
                            rel_to_root(path), parts[0],
                            "fact row has %d columns under this file's %d-column header, so "
                            "its columns cannot be identified" % (len(parts), len(COLS)),
                            unreadable=True)
                        continue
                    raise FactError("%s:%d has %d columns, expected %d" % (path, lineno, len(parts), len(COLS)))
                r = dict(zip(COLS, parts))
            r["_origin"] = origin
            r["_line"] = lineno
            rows.append(r)
    return rows


def load_all():
    rows = load(FACTS_TSV, "facts.tsv")
    seen = {r["id"] for r in rows}
    by_id = {r["id"]: r for r in rows}
    if os.path.isdir(PENDING_DIR):
        for name in sorted(os.listdir(PENDING_DIR)):
            if not name.endswith(".tsv"):
                continue
            _retired = retired_ids()
            for r in load(os.path.join(PENDING_DIR, name), "pending/" + name,
                          tolerant=True):
                if r["id"] in _retired:
                    continue
                if r["id"] in seen:
                    # Already merged into facts.tsv.  The drop file stays where its owner
                    # left it -- moving it would be a write to their path -- so a row that
                    # matches what was merged is simply done.  One that DIFFERS is a real
                    # conflict and still stops the build.
                    prev = by_id.get(r["id"])
                    # Compare NUMERICALLY: "4" and "0x4" are the same offset, and a
                    # notation fix in the registry must not read as a disagreement with
                    # every drop that predates it.
                    def _same(x, y):
                        try:
                            return parse_value(dict(x, value=x["value"])) ==                                    parse_value(dict(y, value=y["value"], type=x["type"]))
                        except (FactError, ValueError):
                            return x["value"] == y["value"]
                    if prev is not None and not _same(prev, r):
                        PENDING_COMPLAINTS.append(
                            ("fail", os.path.join(PENDING_DIR, name), r["_line"],
                             "%s = %s here, but facts.tsv holds %s. One of the two is wrong "
                             "and the disagreement is about the registry, not the drop."
                             % (r["id"], r["value"], prev["value"])))
                    elif prev is not None:
                        # Compare only the columns this drop actually filled: a widened
                        # legacy row reads "-" where its schema had no column, and treating
                        # that as a disagreement would flag every older drop in the channel.
                        # AMENDABLE only. derive_verdicts() deliberately rewrites verdict,
                        # tier, provenance, guard and the source pair on the way in, so the
                        # registry differing from the drop there is the system working --
                        # comparing them flagged 187 correct rows. What is left is exactly
                        # what a contributor edits when correcting a row: its prose.
                        diff = [c for c in AMENDABLE
                                if r.get(c, "-") != "-" and r[c] != prev[c]]
                        if diff:
                            amendable = [c for c in diff if c in AMENDABLE]
                            fix = ("facts/pending/amend/%s.tsv carries %s"
                                   % (name[:-4].split("_")[0], ", ".join(amendable))
                                   if amendable else
                                   "no channel can change %s on a merged row: that is a "
                                   "different claim, so anchor it or retire the id"
                                   % ", ".join(diff))
                            # WARN in factcheck, not FAIL: 63 drops across the channel
                            # already disagree with the registry here, and reddening
                            # everyone's gate over another packet's backlog is the block
                            # this changeset removed four times. The signal is what was
                            # missing -- it is named, attributed and repeated every run --
                            # and merge_pending refuses on it, which is the moment the edit
                            # is actually being asked to do something.
                            PENDING_COMPLAINTS.append(
                                ("warn", os.path.join(PENDING_DIR, name), r["_line"],
                                 "%s: this row ALREADY MERGED, so editing it here changes "
                                 "nothing -- %s still differs from the registry and the "
                                 "edit is being silently discarded. %s"
                                 % (r["id"], ", ".join(diff), fix)))
                    continue
                try:
                    parse_value(r)
                except (FactError, ValueError) as exc:
                    # "refused", not "warn": this row's VALUE could not be read, so it is
                    # not a proposal anybody can weigh -- it is evidence that leaves the
                    # channel silently.  --check fails on it; --merge-pending still skips
                    # it and merges the rest, which is unchanged (that path never read the
                    # severity).
                    PENDING_COMPLAINTS.append(
                        ("refused", os.path.join(PENDING_DIR, name), r["_line"],
                         "%s; row skipped" % (exc,)))
                    continue
                seen.add(r["id"])
                by_id[r["id"]] = r
                rows.append(r)
    return rows


def pending_report(complaints, unreadable):
    """--check's account of facts/pending/, one line per row the channel could not accept.

    Two populations, and they are not the same kind of thing.  REFUSED is a row no reader
    could take: a value that will not parse, a shape the schema cannot identify, or a value
    that contradicts the registry.  --merge-pending SKIPS those, so a refused row is evidence
    that never reaches any guard and whose owner is never told -- which is what --check
    existed to catch and did not.  WARNED is a proposal whose prose is merely out of date;
    that is its owner's to fix and must not redden every other packet's gate.
    """
    # Membership is decided by resolving the path against PENDING_DIR, not by a string
    # prefix: the drop channel is wherever this module says it is, and a control that points
    # it at a private copy must be reported by the same code the real run uses.
    chan = os.path.normcase(os.path.abspath(PENDING_DIR))

    def in_channel(p):
        return os.path.normcase(os.path.abspath(p)).startswith(chan)

    def show(p):
        return rel_to_root(p).replace(os.sep, "/")

    refused, warned = [], []
    for rel, fid, why in sorted(set(unreadable)):
        p = os.path.normpath(os.path.join(ROOT, rel))
        if in_channel(p):
            refused.append("%s  %s: %s" % (show(p), fid, why))
    for sev, path, lineno, msg in sorted(set(complaints)):
        if not in_channel(path):
            continue
        rel = show(path)
        line = "%s:%s  [%s]  %s" % (rel, lineno, path_owner(rel) or "unclaimed", msg)
        (refused if sev in ("fail", "refused") else warned).append(line)
    return refused, warned


def parse_value(r):
    """int for scalar rows, bytes for bytes20, str for str/md5."""
    v = r["value"]
    if r["type"] in ("str", "md5"):
        return v
    n = blob_len(r["type"])
    if n is not None:
        if v == "?":
            return None
        b = bytes(int(x, 16) for x in v.split())
        if len(b) != n:
            raise FactError("%s: %s row holds %d bytes" % (r["id"], r["type"], len(b)))
        return b
    return int(v, 16) if v.lower().startswith("0x") else int(v, 10)


def validate(rows):
    """Every S4.6 rule that can be decided from the registry alone."""
    sources = load_sources()
    anchored = anchored_ids()
    problems = []
    ids = {}
    ids_map = {}
    for r in rows:
        rid = r["id"]
        if rid in ids:
            problems.append("%s: duplicate id (also in %s)" % (rid, ids[rid]))
        ids[rid] = r["_origin"]
        ids_map[rid] = r

        if not re.match(r"^[A-Z][A-Z0-9]*(\.[A-Z][A-Z0-9_]*)+$", rid):
            problems.append("%s: id must be DOTTED.UPPER_SNAKE" % rid)
        if r["addr_space"] not in ADDR_SPACES:
            problems.append("%s: addr_space %r not in %r" % (rid, r["addr_space"], ADDR_SPACES))
        if r["provenance"] not in PROVENANCES:
            problems.append("%s: provenance %r not in %r" % (rid, r["provenance"], PROVENANCES))
        if r["verdict"] not in VERDICTS:
            problems.append("%s: verdict %r not in %r" % (rid, r["verdict"], VERDICTS))
        elif r["tier"] != "U" and r["verdict"] not in VERDICT_MAY_BE_VERIFIED:
            problems.append("%s: verdict %s cannot be tier %s -- docs/PC_DERIVATION.md is "
                            "explicit that this is not a pass" % (rid, r["verdict"], r["tier"]))
        if r["verdict"] == "ARM64_ONLY" and r["provenance"] != "ARM64":
            problems.append("%s: verdict ARM64_ONLY with provenance %r" % (rid, r["provenance"]))
        if r["provenance"] == "ARM64" and r["verdict"] != "ARM64_ONLY":
            problems.append("%s: provenance ARM64 with verdict %r" % (rid, r["verdict"]))
        if r["verb"] not in VERBS:
            problems.append("%s: verb %r not in %r" % (rid, r["verb"], VERBS))
        if r["observed"] not in OBSERVED:
            problems.append("%s: observed %r not in %r" % (rid, r["observed"], OBSERVED))
        # The N-7 class: a row that needs the function to change something, citing one that
        # does not.  A delegating or thunking function still mutates -- through a callee --
        # so those count; a const accessor does not, whatever the row is named.
        if r["verb"] == "mutate" and r["observed"] not in MUTATING + ("-",):
            problems.append("%s: needs a mutator but the cited function is %r"
                            % (rid, r["observed"]))
        # S4.6 rule 9: a row whose effect cannot be reduced to an operation stays U until
        # someone reads the body.  N-7 passed every other check with a verb that agreed with
        # a name that did the opposite.
        if r["verb"] == "mutate" and r["effect"] == "-" and r["tier"] != "U":
            problems.append("%s: verb mutate with no `effect` read from the body; rule 9 keeps "
                            "it at U until one is recorded" % rid)
        if r["verb"] == "read" and r["observed"] in MUTATING:
            problems.append("%s: recorded as a read but the cited function mutates" % rid)
        if r["tier"] not in TIERS:
            problems.append("%s: tier %r not in %r" % (rid, r["tier"], TIERS))
        if r["type"] not in TYPES:
            problems.append("%s: type %r not in %r" % (rid, r["type"], TYPES))

        # Rule 4, asserting what it means.  Two source names are two sources only if they
        # resolve to two different artifacts; `R6_decimal` and `R6_appendix` are one lane
        # wearing two names, and an unlisted name is a placeholder, not a citation.
        for col in ("source_a", "source_b"):
            if not r[col]:
                problems.append("%s: %s is empty" % (rid, col))
            elif r[col] not in sources:
                problems.append("%s: %s %r is not in facts/sources.tsv" % (rid, col, r[col]))
        if r["source_a"] in sources and r["source_b"] in sources:
            aa, ab = sources[r["source_a"]][0], sources[r["source_b"]][0]
            if aa == ab and r["tier"] != "U":
                problems.append("%s: source_a %r and source_b %r both reduce to %r -- one source, "
                                "so this cannot be tier %s"
                                % (rid, r["source_a"], r["source_b"], aa, r["tier"]))
            if r["tier"] != "U" and not (sources[r["source_a"]][1] or sources[r["source_b"]][1]):
                problems.append("%s: tier %s needs a source the code cannot fabricate; %r and %r are both documents"
                                % (rid, r["tier"], r["source_a"], r["source_b"]))
            # A row whose guard makes a STATIC claim -- an offset, an address, a byte
            # string -- needs the unfabricatable half to be static evidence.  A harness
            # reading may corroborate it, but it cannot establish where a field lives: the
            # mapped image it read can have been patched after load, and a wire byte says
            # what was sent rather than where the field sits.  This NARROWS rule 4 -- the
            # "at least one unfabricatable" requirement still holds, and now that one has
            # to match the kind of claim being made.
            kind = claim_kind(r["guard"])
            need = CLAIM_ACCEPTS[kind]
            if r["tier"] != "U" and not any(
                    sources[t][1] and set(sources[t][2]) & set(need)
                    for t in (r["source_a"], r["source_b"])):
                problems.append("%s: tier %s on a %s claim, but neither unfabricatable "
                                "source supports one (%r / %r). %s"
                                % (rid, r["tier"], kind, r["source_a"], r["source_b"],
                                   CLAIM_WHY[kind]))
        if not r["guard"]:
            problems.append("%s: no guard named" % rid)
        if r["tier"] != "U" and not r["evidence"]:
            problems.append("%s: tier %s with no evidence" % (rid, r["tier"]))

        # A number lifted from libr15 is unusable on PC whatever KIND of number it is.
        # PHYS.CI_STRIDE was `CONST 520` and was ARM64; addr_space could not see it.
        if r["provenance"] == "ARM64" and r["tier"] != "U":
            problems.append("%s: ARM64 provenance cannot be tier %s -- it is not a PC fact" % (rid, r["tier"]))
        if r["addr_space"] == "LIBR15_VA" and r["provenance"] != "ARM64":
            problems.append("%s: LIBR15_VA must have provenance ARM64, not %r" % (rid, r["provenance"]))
        # rival_arm is the discriminating evidence for anything libr15 also has.  It may be
        # a hex value, `none` (libr15 has no counterpart) or `-` (not looked up), but a row
        # whose value equals its own ARM rival cannot claim the rival test settled it.
        if r["rival_arm"] not in ("-", "none"):
            if not r["rival_arm"].lower().startswith("0x"):
                problems.append("%s: rival_arm %r must be hex, `none` or `-`" % (rid, r["rival_arm"]))
            elif r["provenance"] == "PC" and blob_len(r["type"]) is None                     and int(r["rival_arm"], 16) == parse_value(r)                     and "taint-proven" not in r["evidence"]:
                problems.append("%s: PC value equals its ARM rival %s, so the rival test settles "
                                "nothing; this row needs base-register evidence" % (rid, r["rival_arm"]))
        if r["provenance"] == "UNKNOWN" and r["tier"] != "U":
            problems.append("%s: provenance UNKNOWN cannot be tier %s -- nobody re-derived it on PC"
                            % (rid, r["tier"]))

        if r["type"] == "md5" and not re.fullmatch(r"[0-9a-f]{32}", r["value"]):
            problems.append("%s: md5 must be 32 lowercase hex digits" % rid)
        if r["type"] in ("str", "md5"):
            continue                       # the numeric rules below do not apply to text

        # rule 8: hex only for addresses and offsets
        if r["addr_space"] in ("PC_VA", "PC_RVA", "LIBR15_VA", "OFFSET"):
            if not r["value"].lower().startswith("0x"):
                problems.append("%s: %s value %r must be hex (S2 F4)" % (rid, r["addr_space"], r["value"]))

        try:
            v = parse_value(r)
        except (ValueError, FactError) as e:
            problems.append("%s: %s" % (rid, e))
            continue

        if v is None:
            continue
        if blob_len(r["type"]) is not None:
            continue

        # The comment is non-authoritative, but a wrong decimal in it is exactly what
        # misleads the next reader (S2 F4).  Any "dec N" or bare 0xN in the comment must
        # agree with the value.
        m = re.search(r"\bdec (\d+)", r["comment"])
        if m and int(m.group(1)) != v:
            problems.append("%s: value %s is %d, comment says dec %s" % (rid, r["value"], v, m.group(1)))
        m = re.search(r"(?<![\w.])0x([0-9A-Fa-f]+)\b", r["comment"])
        if m and not r["value"].lower().startswith("0x") and int(m.group(1), 16) != v:
            problems.append("%s: value %s is %d, comment says 0x%s" % (rid, r["value"], v, m.group(1)))
        if r["addr_space"] == "PC_VA" and not (0x140000000 <= v <= 0x142231000):
            problems.append("%s: PC_VA 0x%X is outside the image 0x140000000..0x142231000" % (rid, v))
        if r["addr_space"] == "PC_RVA" and not (0 <= v <= 0x2231000):
            problems.append("%s: PC_RVA 0x%X is outside the image" % (rid, v))
        if r["addr_space"] == "LIBR15_VA" and v >= 0x140000000:
            problems.append("%s: LIBR15_VA 0x%X looks like a PC VA" % (rid, v))
        if r["type"] == "u32" and v > 0xFFFFFFFF:
            problems.append("%s: value 0x%X does not fit its declared type u32" % (rid, v))

    # a PC_VA that equals a LIBR15_VA would defeat the addr_space split
    pc = {parse_value(r) for r in rows if r["addr_space"] == "PC_VA" and blob_len(r["type"]) is None}
    for r in rows:
        if r["addr_space"] == "LIBR15_VA" and parse_value(r) in pc:
            problems.append("%s: LIBR15_VA collides with a PC_VA row" % r["id"])

    # A prologue carries no address of its own: it names the PC_VA row it guards, and the
    # bytes are read at that row's value.  An address written down twice can drift, and a
    # drifted prologue is byte-valid against the file while pinning the wrong function.
    for r in rows:
        if blob_len(r["type"]) is None:
            continue
        if r["type"] in ("str", "md5"):
            continue
        if not r["id"].endswith(".PROLOGUE"):
            # a byte string at its own site: the address must still be recorded exactly once
            if r["id"] not in anchored:
                problems.append("%s: a byte-string row must either be named <OWNER>.PROLOGUE, so it "
                                "takes its address from that row, or carry an anchor in "
                                "facts/anchors.tsv.  It has neither, so its address is nowhere."
                                % r["id"])
            continue
        owner = ids_map.get(r["id"][:-len(".PROLOGUE")])
        if owner is None:
            problems.append("%s: no owning row %s" % (r["id"], r["id"][:-len(".PROLOGUE")]))
        elif owner["addr_space"] != "PC_VA":
            problems.append("%s: owner %s is %s, not PC_VA" % (r["id"], owner["id"], owner["addr_space"]))

    # Two ids holding one truth drift apart.  Same value in the same space is a merge error.
    by_value = {}
    for r in rows:
        if r["type"] in ("str", "md5") or blob_len(r["type"]):
            continue
        key = (r["addr_space"], parse_value(r))
        by_value.setdefault(key, []).append(r["id"])
    for (space, v), who in sorted(by_value.items()):
        if len(who) > 1 and space in ("PC_VA", "PC_RVA", "LIBR15_VA"):
            problems.append("%s 0x%X is carried by %d ids (%s); one fact, one id" %
                            (space, v, len(who), ", ".join(sorted(who))))
    return problems


def cname(rid):
    return rid.replace(".", "_")


def registry_only(rows):
    """A proposal is not a fact.

    An unmerged drop row was reaching facts_generated.h as a usable constexpr, on nothing
    but the verdict its own author wrote in it -- 33 of them, every one self-declared
    PC_DECISIVE.  That is the F1 shape exactly: a number marked verified by the document it
    arrived in.  Merging is what makes a row a fact, and emission follows merging."""
    return [r for r in rows if not r.get("_origin", "").startswith("pending/")]


def emit_header(rows):
    withheld = [r for r in rows if r["verdict"] in VERDICT_WITHHELD]
    _w = {r["id"] for r in withheld}
    scalars = [r for r in rows if r["id"] not in _w
               and not (r["type"] in ("str", "md5") or blob_len(r["type"]))]
    blobs = [r for r in rows if blob_len(r["type"]) is not None and r["verdict"] not in VERDICT_WITHHELD]
    unver = [r for r in rows if r["tier"] == "U"]

    L = []
    w = L.append
    w("// GENERATED by tools/gen_facts.py from facts/facts.tsv -- DO NOT EDIT.")
    w("//")
    w("// Values only.  This header never declares a struct, a class layout or a field that")
    w("// these constants describe.  Structs are hand-declared next to their derivation and")
    w("// compared against these constants: two derivations, one comparison (S4.6 rule 1).")
    w("#pragma once")
    w("")
    w("#include <stdint.h>")
    w("#include <stddef.h>")
    w("")
    w("namespace facts {")
    w("")
    w("// ---- PC_VA / PC_RVA / OFFSET / CONST ------------------------------------------")
    for r in sorted(scalars, key=lambda r: r["id"]):
        v = parse_value(r)
        suffix = "ull" if r["type"] == "u64" else "u"
        ctype = "uint64_t" if r["type"] == "u64" else "uint32_t"
        lit = ("0x%X" % v) if r["value"].lower().startswith("0x") else str(v)
        w("inline constexpr %s %s = %s%s;  // %s %s" % (ctype, cname(r["id"]), lit, suffix,
                                                        r["addr_space"], r["tier"]))
    w("")
    w("// ---- text read back out of the real echovr.exe -------------------------------")
    for r in sorted([x for x in rows if x["type"] in ("str", "md5")], key=lambda r: r["id"]):
        if r["verdict"] in VERDICT_WITHHELD:
            continue
        w('inline constexpr char %s[] = "%s";  // %s'
          % (cname(r["id"]), parse_value(r).replace('"', '\\"'), r["verdict"]))
    w("")
    w("// ---- prologue expectations, read from the real echovr.exe ---------------------")
    for r in sorted(blobs, key=lambda r: r["id"]):
        b = parse_value(r)
        if b is None:
            raise FactError("%s still holds '?'; run gen_facts.py --seed-prologues" % r["id"])
        body = ", ".join("0x%02X" % x for x in b)
        w("inline constexpr uint8_t %s[%d] = { %s };" % (cname(r["id"]), len(b), body))
    w("")
    w("// ---- withheld: declared, never defined, value deliberately absent -------------")
    w("//")
    w("// These numbers were read from libr15.so (Quest ARM64) and have")
    w("// no PC meaning.  Addresses are the obvious case -- casting one to a function")
    w("// pointer in this process lands in the PE header region -- but a stride, count, cap")
    w("// or mask carried across is just as wrong and does not announce itself:")
    w("// PHYS.CI_STRIDE was `CONST 520` and the PC stride is 0x1F8.")
    w("//")
    w("// The numbers are NOT in this file, so no cast recovers them.  Naming one of these")
    w("// in a PC translation unit is a compile error by construction.  W1-I owns the")
    w("// mapping; the values live in facts/libr15_worklist.txt, which no TU includes.")
    w("//")
    w("// PC_ABSENT rows are here for a different reason: the value is not at the site the")
    w("// design cites, so nobody knows what it is.  docs/PC_DERIVATION.md calls that REFUSE,")
    w("// and refusing means the constant does not exist rather than existing and being wrong.")
    w("namespace withheld_not_usable_on_pc {")
    # Candidates stay OUT of this list and out of facts:: -- they have their own block
    # below. Declaring them incomplete here too would tell W1-C the number it is meant to
    # read is unusable. They must remain in `withheld`, though: that list is what keeps
    # them from being emitted as ordinary constants, and dropping them from it put
    # LOBBY_MEMBER_ARRAY_OFF into facts:: as a usable u32. guard_verdict caught it.
    for r in sorted(withheld, key=lambda r: r["id"]):
        if r["verdict"] == VERDICT_CANDIDATE:
            continue
        w("struct %s;  // incomplete by design -- %s" % (cname(r["id"]), r["verdict"]))
    w("}  // namespace withheld_not_usable_on_pc")
    w("")
    cands = sorted([r for r in rows if r["verdict"] == VERDICT_CANDIDATE],
                   key=lambda r: r["id"])
    w("// ---- candidates: proposed numbers, for the harness that settles them -----------")
    w("//")
    w("// NOT facts.  Each of these is a hypothesis with no binary evidence behind it, and")
    w("// the only legitimate use is the runtime read that decides whether it is right.")
    w("// They live in their own namespace so that reaching one is deliberate, and")
    w("// gatecheck confines `facts::candidate::` to the harness so a guess cannot reach")
    w("// shipping code.  Writing the raw number instead would be the S4.6 rule 3")
    w("// transcription this registry exists to prevent -- and the harness is where a wrong")
    w("// number does the MOST damage, not the least, because it is what everyone believes")
    w("// afterwards.")
    w("//")
    w("// When the harness reports, cite harness_observation and the row promotes into")
    w("// `facts::` the ordinary way. Until then a candidate may be READ and never SHIPPED.")
    w("namespace candidate {")
    for r in cands:
        w("inline constexpr uint64_t %s = %s;  // UNVERIFIED -- %s"
          % (cname(r["id"]), parse_value(r), r["base"] or "base unstated"))
    if not cands:
        w("// (none)")
    w("}  // namespace candidate")
    w("")
    w("// ---- unverified facts, loud at startup (S4.6 rule 5) --------------------------")
    w("inline constexpr const char* UNVERIFIED_FACT_IDS[] = {")
    for r in sorted(unver, key=lambda r: r["id"]):
        w('    "%s",' % r["id"])
    w("};")
    w("inline constexpr size_t UNVERIFIED_FACT_COUNT = %d;" % len(unver))
    w("")
    w("inline constexpr size_t FACT_COUNT = %d;" % len(rows))
    w("")
    w("}  // namespace facts")
    return "\n".join(L) + "\n"


def emit_worklist(rows):
    withheld = sorted((r for r in rows if r["verdict"] in VERDICT_WITHHELD), key=lambda r: r["id"])
    L = ["# libr15.so numbers with no PC equivalent -- W1-I's work-item list.",
         "# This file is NOT a header and is included by no translation unit.  Nothing here is",
         "# usable on PC -- not the addresses, and not the strides either.  Mapping a row to PC",
         "# means adding a new provenance=PC row to facts.tsv, re-derived from PC code.",
         ""]
    for r in withheld:
        L.append("%-34s %-12s %-16s %s" % (r["id"], r["value"], r["verdict"], r["comment"]))
    return "\n".join(L) + "\n"


def seed_prologues(rows):
    sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))
    from pe import PeImage
    import hashlib

    with open(ECHOVR_EXE, "rb") as f:
        data = f.read()
    md5 = hashlib.md5(data).hexdigest()
    if md5 != ECHOVR_MD5:
        raise FactError("echovr.exe md5 is %s, expected %s" % (md5, ECHOVR_MD5))
    pe = PeImage(ECHOVR_EXE)

    by_id = {r["id"]: r for r in rows}
    filled = 0
    for r in rows:
        if blob_len(r["type"]) is None or r["value"] != "?":
            continue
        owner_id = r["id"][:-len(".PROLOGUE")]
        owner = by_id.get(owner_id)
        if owner is None:
            raise FactError("%s has no owning row %s" % (r["id"], owner_id))
        if owner["addr_space"] != "PC_VA":
            raise FactError("%s owner is %s, not PC_VA" % (r["id"], owner["addr_space"]))
        # Read the width the ROW declares, not the default: a bytes8 row seeded with
        # 20 bytes is written as a value parse_value() then refuses, and a bytes32 row
        # was silently seeded short.
        b = pe.read_va(parse_value(owner), blob_len(r["type"]))
        if b is None:
            raise FactError("%s: no file bytes at 0x%X" % (r["id"], parse_value(owner)))
        r["value"] = " ".join("%02X" % x for x in b)
        filled += 1

    # rewrite facts.tsv in place, preserving comments and order
    out = []
    with open(FACTS_TSV, "r", encoding="utf-8") as f:
        for line in f:
            s = line.rstrip("\n")
            if not s or s.startswith("#") or s.split("\t")[0] == "id":
                out.append(s)
                continue
            parts = s.split("\t")
            r = by_id.get(parts[0])
            if r is not None and r["_origin"] == "facts.tsv":
                parts[1] = r["value"]
            out.append("\t".join(parts))
    write_atomic(FACTS_TSV, "\n".join(out) + "\n")
    return filled


REGISTRY_PATHS = ("facts/facts.tsv", "facts/anchors.tsv", "facts/retired.tsv",
                  "facts/sources.tsv", "facts/guard_hashes.tsv",
                  "facts/guard_scope_ids.tsv",
                  "src/generated/facts_generated.h")
# The two files a blessing writes.  They are DERIVED -- factcheck.py --update-hashes and
# every merge rewrite them from the tree -- so uncommitted rows here are never in-flight
# hand work, and the advice that fits the rest of the registry does not fit them.
BLESSING_PATHS = ("facts/guard_hashes.tsv", "facts/guard_scope_ids.tsv")
# The all-rows guard whose registered scope IS the number of rows in facts.tsv.  Named, not
# inferred, and its absence is REPORTED rather than read as agreement: a check keyed on a
# string nobody writes any more passes in silence, which is the failure this branch exists
# to make impossible.
WHOLE_REGISTRY_GUARD = "guard_coverage"


# ---------------------------------------------------------------------------------------------
# THE ARG-SHAPE CHECK.  A prologue blob harvested from the address it guards can never fail.
#
# RESET.CI_PC was bound to 0x140D3EA40 -- the INNER BODY of ResetCI, which takes the CI row
# POINTER -- while every consumer of the fact calls it as (cs, uint16 handle).  G_PROLOGUE passed
# anyway, because the 20 bytes recorded for the row were read FROM 0x140D3EA40: the guard proved
# the address still holds the bytes we wrote down, which is a tautology about the address, not a
# claim about the function.  The handle 0x28 was dereferenced as a pointer and the client died at
# 0x140D3EA4A (DIAG-CRASH, L3 run 4).
#
# This check asks a question the prologue blob cannot: does the code at the address CONSUME the
# second argument the way the calling code DECLARES it?  It takes its two inputs from two places
# that cannot fabricate each other -- the consumer's C++ signature, read out of the source, and the
# shipped exe's instruction bytes, read off disk -- so neither side can certify itself.
ARGSHAPE_WINDOW = 20

# The consumer.  Bindings are read from the source rather than listed here, so a fact bound to a
# handle-taking signature tomorrow is checked tomorrow without anyone remembering to add it.
ARGSHAPE_CONSUMER = os.path.join(ROOT, "src", "playback", "residue", "residue.cpp")
ARGSHAPE_BINDING_RE = re.compile(
    r"Guarded\s*<\s*ResidueEnv::(\w+)\s*>\s*\(\s*\"([A-Z][A-Z0-9_.]*)\"")

# What each signature says about argument 2, which the Win64 ABI passes in RDX.
#   u16  -- declared uint16_t: the callee must read DX, the low 16 bits, as a value.
#   u32  -- declared uint32_t: the callee must read EDX as a 32-bit value, not as an address.
# A signature absent from this table is not checked and is REPORTED as unchecked; silently
# skipping it would make a new signature look like a pass.
ARGSHAPE_SIGS = {
    "CsHandleFn": ("u16", "(void* cs, uint16_t handle)"),
    "CsSnapFn":   ("u16", "(void* cs, uint16_t handle, float seconds)"),
    "BodyWakeFn": ("u32", "(void* body, uint32_t resetSleepFrame, uint32_t wakeContacts)"),
}


def _modrm_is_rdx_register(m):
    """mod == 11 and r/m == 010: the operand IS rdx/edx/dx, a value."""
    return (m & 0xC7) == 0xC2


def _modrm_is_rdx_memory(m):
    """r/m == 010 with mod != 11: the operand is [rdx] / [rdx+disp], a dereference.

    r/m 010 never selects a SIB byte, so no escape case has to be excluded here."""
    return (m & 0xC7) in (0x02, 0x42, 0x82)


def _scan_arg2(win, width):
    """(value_use_index, deref_index) for argument 2 inside `win`.

    value_use for u16 is `movzx r32, dx` (0F B7 /r with a register operand); for u32 it is
    `mov r32, edx` (8B /r with a register operand) that is NOT REX.W -- a REX.W `mov r64, rdx`
    copies the argument as a POINTER, which is exactly the shape under suspicion, so it is not
    accepted as evidence that the argument is a value.  A dereference is either width read
    THROUGH rdx.  Indices are -1 when the form does not occur."""
    value_at, deref_at = -1, -1
    for i in range(len(win) - 1):
        if win[i] == 0x0F and i + 2 < len(win) and win[i + 1] == 0xB7:
            m = win[i + 2]
            if width == "u16" and _modrm_is_rdx_register(m) and value_at < 0:
                value_at = i
            if _modrm_is_rdx_memory(m) and deref_at < 0:
                deref_at = i
            continue
        if win[i] == 0x8B:
            m = win[i + 1]
            rexw = i > 0 and 0x48 <= win[i - 1] <= 0x4F and (win[i - 1] & 0x08)
            if width == "u32" and _modrm_is_rdx_register(m) and not rexw and value_at < 0:
                value_at = i
            if _modrm_is_rdx_memory(m) and deref_at < 0:
                deref_at = i
    return value_at, deref_at


def arg_shape_report(rows):
    """(report_lines, failures).  Gates the facts our own code binds to a typed entry point,
    and sweeps every other unverified *_PC row for the same defect as a REPORT."""
    lines, failures = [], []

    sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))
    try:
        from pe import PeImage
        pe = PeImage(ECHOVR_EXE)
    except Exception as e:                                    # noqa: BLE001 -- reported, not raised
        failures.append("ARGSHAPE could not read %s (%s). A check that could not ask is not a "
                        "check that passed." % (ECHOVR_EXE, e))
        return lines, failures

    try:
        src = io.open(ARGSHAPE_CONSUMER, "r", encoding="utf-8").read()
    except Exception as e:                                    # noqa: BLE001
        failures.append("ARGSHAPE could not read the consumer %s (%s)"
                        % (rel_to_root(ARGSHAPE_CONSUMER), e))
        return lines, failures

    bindings = ARGSHAPE_BINDING_RE.findall(src)
    if not bindings:
        # An empty binding list would make every assertion below vacuously true.  The consumer
        # spells its signature into the Guarded<> call precisely so this list cannot go quiet.
        failures.append("ARGSHAPE found no Guarded<Sig>(\"FACT.ID\", ...) bindings in %s -- the "
                        "check has no subjects and would pass over anything"
                        % rel_to_root(ARGSHAPE_CONSUMER))
        return lines, failures

    by_id = {r["id"]: r for r in rows}
    checked = 0
    for sig, fid in bindings:
        r = by_id.get(fid)
        if r is None:
            failures.append("ARGSHAPE %s is bound in %s but is not a registry row"
                            % (fid, rel_to_root(ARGSHAPE_CONSUMER)))
            continue
        if sig not in ARGSHAPE_SIGS:
            lines.append("ARGSHAPE %s: signature %s is not in the shape table -- UNCHECKED"
                         % (fid, sig))
            continue
        width, decl = ARGSHAPE_SIGS[sig]
        va = parse_value(r)
        win = pe.read_va(va, ARGSHAPE_WINDOW)
        if win is None:
            failures.append("ARGSHAPE %s: no file bytes at 0x%X" % (fid, va))
            continue
        value_at, deref_at = _scan_arg2(bytearray(win), width)
        why = []
        if value_at < 0:
            why.append("nothing in the first %d bytes reads argument 2 as a %s value"
                       % (ARGSHAPE_WINDOW, width))
        if deref_at >= 0 and (value_at < 0 or deref_at < value_at):
            why.append("argument 2 is DEREFERENCED at byte %d, before any value use -- the callee "
                       "wants a pointer" % deref_at)
        if why:
            failures.append("ARGSHAPE %s at 0x%X is bound to %s %s but %s. Bytes: %s"
                            % (fid, va, sig, decl, "; ".join(why),
                               " ".join("%02X" % b for b in win)))
        else:
            checked += 1
            lines.append("ARGSHAPE %s at 0x%X consumes argument 2 as a %s value at byte %d -- ok"
                         % (fid, va, width, value_at))

    # The sweep.  Every OTHER *_PC row still at tier U, classified by the same two forms.  This is
    # a REPORT, not a gate: nothing here declares what those callers pass, so a row-taking shape is
    # a lead for its owner rather than a verdict this tool can reach.
    bound = {fid for _, fid in bindings}
    swept = 0
    for r in rows:
        fid = r["id"]
        if not fid.endswith("_PC") or fid in bound or r["tier"] != "U":
            continue
        if r["addr_space"] != "PC_VA":
            continue
        win = pe.read_va(parse_value(r), ARGSHAPE_WINDOW)
        if win is None:
            lines.append("ARGSHAPE-SWEEP %s: no file bytes at its address" % fid)
            continue
        swept += 1
        v16, deref = _scan_arg2(bytearray(win), "u16")
        v32, _ = _scan_arg2(bytearray(win), "u32")
        if deref >= 0 and (max(v16, v32) < 0 or deref < max(v16, v32)):
            lines.append("ARGSHAPE-SWEEP %s at 0x%X DEREFERENCES argument 2 at byte %d -- it takes "
                         "a POINTER there. Any caller passing a handle or an int would fault, the "
                         "way RESET.CI_PC did." % (fid, parse_value(r), deref))
        elif v16 >= 0 or v32 >= 0:
            lines.append("ARGSHAPE-SWEEP %s at 0x%X reads argument 2 as a %s value"
                         % (fid, parse_value(r), "u16" if v16 >= 0 else "u32"))
        else:
            lines.append("ARGSHAPE-SWEEP %s at 0x%X: argument 2 is not touched in its first %d "
                         "bytes -- shape undetermined, NOT a pass"
                         % (fid, parse_value(r), ARGSHAPE_WINDOW))
    lines.append("ARGSHAPE bound=%d ok=%d failed=%d swept-unverified=%d"
                 % (len(bindings), checked, len(failures), swept))
    return lines, failures

# GETTERSHAPE -- ARITY and ARGUMENT ORDER of the four GS.GET_* engine getters.
#
# ARGSHAPE above asks one question of one consumer: is argument 2 a value or a pointer.  The
# getters need a different question.  All four share a byte-identical stack frame, so the prologue
# cannot tell them apart; GS.GET_SYMBOL returns a CSymbol64 by value and MSVC returns that class in
# memory, so it takes THREE arguments with the store in argument 2, while the other three take
# (store, symbol).  Binding GET_SYMBOL as (store, symbol) put our symbol in RDX and the engine
# executed mov r15,[rdx+2E00h] on it -- the L2-RUN-6 0xC0000005 at 0x140D5CF01.
#
# Three inputs, no two of which can fabricate each other: the DECLARED shape is parsed out of the
# typedefs in src/engine/gamestate.h (arity = parameter count, store = the position of the
# parameter named `store`); the BINDINGS come from src/engine/gamestate_live.cpp; the OBSERVED
# shape is decoded from the shipped exe's own bytes.  The registry's GS.GET_*.ARGS / .STORE_ARG /
# .ARGMOVE rows are cross-checked against the declaration as well, so a row edited to match a wrong
# binding is still caught by the exe, and a wrong binding matching the rows by the declaration.
GETTER_WINDOW = 0x40
GETTER_CONSUMER = os.path.join(ROOT, "src", "engine", "gamestate_live.cpp")
GETTER_HEADER = os.path.join(ROOT, "src", "engine", "gamestate.h")
GETTER_BINDING_RE = re.compile(
    r"Guarded\s*<\s*(?:\w+::)?(\w+)\s*>\s*\(\s*\"(GS\.[A-Z][A-Z0-9_.]*)\"")
GETTER_TYPEDEF_RE = re.compile(
    r"typedef\s+[\w:*&\s]+\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)\s*;")

# x64 register number -> Win64 integer argument index, and back to a name for the report.
GETTER_ARG_REG = {1: 1, 2: 2, 8: 3, 9: 4}
GETTER_REG_NAME = {0: "RAX", 1: "RCX", 2: "RDX", 3: "RBX", 4: "RSP", 5: "RBP", 6: "RSI", 7: "RDI",
                   8: "R8", 9: "R9", 10: "R10", 11: "R11", 12: "R12", 13: "R13", 14: "R14",
                   15: "R15"}


def _resolve_reg(chain, reg):
    seen = 0
    while reg in chain and seen < 16:
        reg = chain[reg]
        seen += 1
    return reg


FRAME_STORE_BASES = (4, 5)              # rsp, rbp: home slots and shadow space


def _reg_name(reg):
    """A name for a register the chain resolved, including the one it cannot name.

    `_step_over` records a zeroed register as None, so the store dereference can resolve to
    "nothing an argument reached" -- which is a refusal with a reason, not a KeyError."""
    if reg is None:
        return "a register zeroed inside the argument-move run"
    return GETTER_REG_NAME.get(reg, "r%d" % reg)


def _step_over(win, i, chain, killed):
    """Length of the instruction at `i` when the argument-move scan may step over it, else 0.

    Two forms, and only two.  Both occur INSIDE a real argument-move run and neither can be an
    argument move, so ending the run at them reports an arity lower than the callee's.  Anything
    else still ends the run: stepping over an instruction this decoder cannot read would let it
    credit an argument to a register something else had already overwritten.

      1. a store into the frame -- `mov [rsp+disp]/[rbp+disp], reg` -- of a register carrying no
         argument.  sub_140152210 spills into its shadow space between `mov r9,rdx` and
         `mov rsi,r8`, and the run ended there, answering 2 for a three-argument callee.
         Spilling an ARGUMENT register is not noise and still ends the run.
      2. `xor reg,reg`, the zeroing idiom, which KILLS the register: whatever argument it held it
         does not hold afterwards.  The kill is recorded in `killed`, NOT as a chain entry --
         a register that was COPIED out of the zeroed one before the xor still holds the
         argument, and a chain entry would retroactively take it away from that copy too."""
    j = i
    rex = 0
    if 0x40 <= win[j] <= 0x4F:
        rex, j = win[j], j + 1
    if j + 1 >= len(win):
        return 0
    op, m = win[j], win[j + 1]
    mod = m >> 6
    reg = ((m >> 3) & 7) | ((rex & 0x04) << 1)
    rm = (m & 7) | ((rex & 0x01) << 3)
    if op == 0x89 and mod in (1, 2):
        sib = (m & 7) == 4
        if sib and j + 2 >= len(win):
            return 0
        if sib and ((win[j + 2] >> 3) & 7) != 4:
            return 0                        # an INDEXED store is not a home slot
        base = (win[j + 2] & 7) if sib else (m & 7)
        if (rex & 0x01) or base not in FRAME_STORE_BASES:
            return 0
        if reg not in killed and GETTER_ARG_REG.get(_resolve_reg(chain, reg), 0):
            return 0                        # spilling an argument says something; do not skip it
        n = (j - i) + 2 + (1 if sib else 0) + (1 if mod == 1 else 4)
        return n if i + n <= len(win) else 0
    if op in (0x31, 0x33) and mod == 3 and reg == rm:
        killed.add(rm)                      # zeroed: this register carries no argument now
        chain.pop(rm, None)
        return (j - i) + 2
    return 0


def _decode_argmoves(win, off, stop=None):
    """(chain, arity, highest-argument register) for the REX.W `mov r64,r64` run at byte `off`.

    Everything before `off` is the frame every getter shares; this run is the only thing that
    separates the families.  Arity is 1 + the highest Win64 argument index the run reads, which is
    what the callee saving its arguments into non-volatile registers spells out.  The run is not
    required to be contiguous: `_step_over` names the two instruction forms that may sit inside
    it, and every other byte pattern ends it.

    `stop` bounds the scan at a byte, which is how a caller asks what a register held AT a site
    rather than after the whole run: `chain` is order-free, so a move recorded past the site
    being asked about would answer for code that has not executed yet."""
    chain, killed, arity, arg_reg = {}, set(), 0, None
    i = off
    while i + 2 < len(win) and (stop is None or i < stop):
        rex = win[i]
        if not (0x48 <= rex <= 0x4F) or win[i + 1] != 0x8B or (win[i + 2] & 0xC0) != 0xC0:
            n = _step_over(win, i, chain, killed)
            if not n:
                break
            i += n
            continue
        m = win[i + 2]
        dst = ((m >> 3) & 7) | ((rex & 0x04) << 1)
        raw = (m & 7) | ((rex & 0x01) << 3)
        src = None if raw in killed else _resolve_reg(chain, raw)
        if GETTER_ARG_REG.get(src, 0) > arity:
            arity, arg_reg = GETTER_ARG_REG[src], src
        killed.discard(dst)
        chain[dst] = src
        i += 3
    return chain, arity, arg_reg


def _find_store_deref(win, off):
    """(byte index, base register) of the first `mov r64,[base+disp32]` with disp >= 0x1000 after
    byte `off` -- the getter's first load out of the store object.  (-1, None) when there is none;
    an absent dereference is reported, never treated as agreement."""
    i = off
    while i + 6 < len(win):
        rex, m = win[i], win[i + 2]
        if 0x48 <= rex <= 0x4F and win[i + 1] == 0x8B and (m & 0xC0) == 0x80 and (m & 7) != 4:
            disp = win[i + 3] | (win[i + 4] << 8) | (win[i + 5] << 16) | (win[i + 6] << 24)
            if disp >= 0x1000:
                return i, (m & 7) | ((rex & 0x01) << 3)
        i += 1
    return -1, None


def getter_shape_report(rows):
    """(report_lines, failures).  Gates the arity and argument order of every GS.GET_* getter this
    tree binds, against the exe and against the registry."""
    lines, failures = [], []

    sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))
    try:
        from pe import PeImage
        pe = PeImage(ECHOVR_EXE)
    except Exception as e:                                    # noqa: BLE001 -- reported, not raised
        failures.append("GETTERSHAPE could not read %s (%s). A check that could not ask is not a "
                        "check that passed." % (ECHOVR_EXE, e))
        return lines, failures

    try:
        src = io.open(GETTER_CONSUMER, "r", encoding="utf-8").read()
        hdr = io.open(GETTER_HEADER, "r", encoding="utf-8").read()
    except Exception as e:                                    # noqa: BLE001
        failures.append("GETTERSHAPE could not read %s / %s (%s)"
                        % (rel_to_root(GETTER_CONSUMER), rel_to_root(GETTER_HEADER), e))
        return lines, failures

    bindings = GETTER_BINDING_RE.findall(src)
    if not bindings:
        failures.append("GETTERSHAPE found no Guarded<Sig>(\"GS....\", ...) bindings in %s -- the "
                        "check has no subjects and would pass over anything"
                        % rel_to_root(GETTER_CONSUMER))
        return lines, failures

    typedefs = dict(GETTER_TYPEDEF_RE.findall(hdr))
    by_id = {r["id"]: r for r in rows}
    off_row = by_id.get("GS.GETTER_ARGMOVE_OFF")
    if off_row is None:
        failures.append("GETTERSHAPE GS.GETTER_ARGMOVE_OFF is not a registry row -- the argument "
                        "moves cannot be located, and the check refuses rather than guess an "
                        "offset into the shared frame")
        return lines, failures
    off = parse_value(off_row)

    checked = 0
    for sig, fid in bindings:
        r = by_id.get(fid)
        if r is None:
            failures.append("GETTERSHAPE %s is bound in %s but is not a registry row"
                            % (fid, rel_to_root(GETTER_CONSUMER)))
            continue
        params = typedefs.get(sig)
        if params is None:
            failures.append("GETTERSHAPE %s is bound as %s but %s declares no such typedef -- the "
                            "declared shape cannot be read, so nothing would be asserted"
                            % (fid, sig, rel_to_root(GETTER_HEADER)))
            continue
        names = [p.strip().split()[-1].lstrip("*&") for p in params.split(",") if p.strip()]
        if "store" not in names:
            failures.append("GETTERSHAPE %s: typedef %s (%s) has no parameter named `store` -- "
                            "which argument carries the store is exactly what this check compares, "
                            "and it refuses rather than assume argument 1" % (fid, sig, params))
            continue
        declared_arity, declared_store = len(names), names.index("store") + 1

        va = parse_value(r)
        win = pe.read_va(va, GETTER_WINDOW)
        if win is None:
            failures.append("GETTERSHAPE %s: no file bytes at 0x%X" % (fid, va))
            continue
        win = bytearray(win)
        chain, observed_arity, arg_reg = _decode_argmoves(win, off)
        deref_at, base = _find_store_deref(win, off)

        why = []
        if observed_arity == 0:
            why.append("no argument move at byte 0x%X -- the code there does not have the getter "
                       "shape this check knows how to read" % off)
        elif declared_arity != observed_arity:
            why.append("declared arity %d but argument %d (%s) is read at byte 0x%X"
                       % (declared_arity, observed_arity, GETTER_REG_NAME[arg_reg], off))
        if deref_at < 0:
            why.append("no large-displacement load out of an argument in the first 0x%X bytes -- "
                       "the store dereference this check keys on is not there" % GETTER_WINDOW)
        else:
            # what `base` held AT the dereference, not after the run: the moves below it are
            # not this load's provenance.
            at_deref, _a, _r = _decode_argmoves(win, off, deref_at)
            origin = _resolve_reg(at_deref, base)
            observed_store = GETTER_ARG_REG.get(origin)
            if observed_store is None:
                why.append("the store is dereferenced out of %s at 0x%X, which is not a Win64 "
                           "argument register" % (_reg_name(origin), va + deref_at))
            elif observed_store != declared_store:
                why.append("declared store is argument %d but the store is dereferenced out of "
                           "argument %d at 0x%X"
                           % (declared_store, observed_store, va + deref_at))

        for suffix, want, what in ((".ARGS", declared_arity, "arity"),
                                   (".STORE_ARG", declared_store, "store argument")):
            rr = by_id.get(fid + suffix)
            if rr is None:
                why.append("%s%s is not a registry row -- the declared %s has nothing to be "
                           "cross-checked against" % (fid, suffix, what))
            elif parse_value(rr) != want:
                why.append("%s%s says %d but %s declares %s %d"
                           % (fid, suffix, parse_value(rr), rel_to_root(GETTER_HEADER), what, want))
        mv = by_id.get(fid + ".ARGMOVE")
        have = " ".join("%02X" % b for b in win[off:off + 8])
        if mv is None:
            why.append("%s.ARGMOVE is not a registry row -- the bytes the live binder compares "
                       "before it will call the address are not recorded" % fid)
        elif " ".join(mv["value"].split()).upper() != have:
            why.append("%s.ARGMOVE says %s but the exe has %s at 0x%X"
                       % (fid, " ".join(mv["value"].split()), have, va + off))

        if why:
            failures.append("GETTERSHAPE %s at 0x%X is bound as %s (%s) but %s. Bytes at 0x%X: %s"
                            % (fid, va, sig, params.strip(), "; ".join(why), off, have))
        else:
            checked += 1
            lines.append("GETTERSHAPE %s at 0x%X: %d arguments, store is argument %d, dereferenced "
                         "at 0x%X -- ok" % (fid, va, observed_arity, declared_store, va + deref_at))

    lines.append("GETTERSHAPE bound=%d ok=%d failed=%d"
                 % (len(bindings), checked, len(failures)))
    return lines, failures


def blessing_residue():
    """Do the uncommitted blessing rows describe the registry that is on disk?

    Returns (verdict, detail).  The verdict is "residue" when they provably do not, and
    "unknown" otherwise: this test can prove a mismatch, never reproducibility.

    Written after 982ca5f, where the blessing files were left behind by a full
    --merge-pending whose data files were rolled back with `git checkout --` and whose
    blessing files were not.  They registered guard_coverage for 1159 rows over a facts.tsv
    holding 1072, and this tool's refusal told their owner to COMMIT them -- which would have
    landed a blessing for a registry that does not exist and reddened all fourteen all-rows
    guards."""
    try:
        on_disk = len(load(FACTS_TSV, "facts.tsv"))
    except Exception as exc:
        return "unknown", "facts.tsv could not be read (%s), so nothing was compared" % exc
    registered = None
    try:
        with open(os.path.join(ROOT, "facts", "guard_hashes.tsv"), encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("#") or not line.strip():
                    continue
                fields = line.rstrip("\n").split("\t")
                if fields[0] == WHOLE_REGISTRY_GUARD and len(fields) > 1:
                    registered = int(fields[1])
                    break
    except Exception as exc:
        return "unknown", ("facts/guard_hashes.tsv could not be read (%s), so nothing was "
                           "compared" % exc)
    if registered is None:
        return "unknown", ("%s is not registered in facts/guard_hashes.tsv, so the test "
                           "could not run -- do NOT read that as agreement"
                           % WHOLE_REGISTRY_GUARD)
    if registered != on_disk:
        return "residue", ("%s is registered for %d row(s) and facts.tsv on disk holds %d, "
                           "so the blessing describes a registry that is not here."
                           % (WHOLE_REGISTRY_GUARD, registered, on_disk))
    return "unknown", ("%s agrees with facts.tsv at %d row(s) -- necessary, not sufficient: "
                       "the hashes themselves are not recomputed here."
                       % (WHOLE_REGISTRY_GUARD, on_disk))


def path_owner(rel):
    """A routing hint: who to ask about a path.

    This exists only so a refusal can say "ask W0-2" instead of
    printing a path and leaving the reader to look it up, and a wrong hint costs a
    misdirected question rather than a wrong ownership decision.

    Under facts/pending/ the filename IS the owner -- that is the contribution channel's
    whole naming convention -- and "SHARED" from the directory marker would send a blocked
    reader looking for a person who does not exist.  Everywhere else, the nearest .owner."""
    rel = rel.replace("\\", "/")
    if rel.startswith("facts/pending/"):
        leaf = rel.rsplit("/", 1)[-1]
        if leaf.endswith(".tsv"):
            return leaf[:-4].split("_")[0]
    d = rel
    while True:
        d = d.rpartition("/")[0]
        marker = os.path.join(ROOT, d, ".owner")
        if os.path.isfile(marker):
            first = open(marker, encoding="utf-8").readline().strip()
            if first:
                return first
        if not d:
            return None


def uncommitted_ids(rel):
    """Fact ids appearing on either side of a registry file's uncommitted diff.

    Answers the only question a blocked packet actually has: are those my rows?  None
    when git cannot say, which is not the same as an empty set and must not read as one."""
    import subprocess
    try:
        out = subprocess.run(["git", "diff", "HEAD", "--unified=0", "--", rel],
                             cwd=ROOT, capture_output=True, text=True, timeout=30)
    except Exception:
        return None
    if out.returncode != 0:
        return None
    ids = set()
    for line in out.stdout.splitlines():
        if line[:1] in "+-" and line[:3] not in ("+++", "---"):
            first = line[1:].split("\t")[0].strip()
            if first and not first.startswith("#") and first != "id":
                ids.add(first)
    return ids


def pending_row_ids():
    """Ids the pending drops would introduce, read tolerantly -- this is for a message."""
    ids = set()
    if not os.path.isdir(PENDING_DIR):
        return ids
    for name in sorted(os.listdir(PENDING_DIR)):
        if not name.endswith(".tsv"):
            continue
        try:
            text = open(os.path.join(PENDING_DIR, name), encoding="utf-8",
                        newline="").read()
        except Exception:
            continue
        for line in text.splitlines():
            first = line.rstrip("\r").split("\t")[0].strip()
            if first and not first.startswith("#") and first != "id":
                ids.add(first)
    return ids


def gate_failures():
    """The FAIL lines factcheck reports right now, as a set. None if it cannot run."""
    import subprocess
    try:
        out = subprocess.run([sys.executable,
                              os.path.join(ROOT, "tests", "factcheck", "factcheck.py")],
                             cwd=ROOT, capture_output=True, text=True, timeout=300)
    except Exception:
        return None
    lines = {l for l in (out.stdout + out.stderr).splitlines() if l.startswith("FAIL ")}
    if not lines and "RESULT factcheck" not in out.stdout:
        return None          # it did not get far enough to have an opinion
    return lines


MERGE_OUTPUTS = ("facts/facts.tsv", "facts/anchors.tsv", "facts/retired.tsv",
                 "src/generated/facts_generated.h")


def snapshot_outputs():
    out = {}
    for rel in MERGE_OUTPUTS:
        full = os.path.join(ROOT, rel)
        if os.path.exists(full):
            out[full] = open(full, "rb").read()
    return out


def restore_outputs(snap):
    for full, blob in snap.items():
        with open(full, "wb") as f:
            f.write(blob)


def failure_fact_id(line):
    """The fact a FAIL line is about.  "FAIL G_ANCHOR CS.X: ..." and "FAIL CS.X: ..." both
    put the id immediately before the first colon; take the last token of that."""
    head = line.split(":", 1)[0]
    return head.split()[-1] if head.split() else ""


def drop_rows(ids):
    """Remove rows from facts.tsv, and any anchors that would be left pointing at nothing."""
    if not ids:
        return
    kept = [l for l in open(FACTS_TSV, encoding="utf-8", newline="").read().split("\n")
            if l.split("\t")[0] not in ids]
    write_atomic(FACTS_TSV, "\n".join(kept).rstrip("\n") + "\n")
    anch = [l for l in open(ANCHORS_TSV, encoding="utf-8", newline="").read().split("\n")
            if l.split("\t")[0] not in ids]
    write_atomic(ANCHORS_TSV, "\n".join(anch).rstrip("\n") + "\n")


def reregister_hashes(reason, amended=()):
    """Bless the scope this merge moved.  Returns False if it would be a narrowing or a
    body change -- neither of which a merge can legitimately cause, so it stops."""
    import subprocess
    out = subprocess.run([sys.executable,
                          os.path.join(ROOT, "tests", "factcheck", "factcheck.py"),
                          "--update-hashes", "--reason", reason,
                          "--amended", ",".join(sorted(amended))],
                         cwd=ROOT, capture_output=True, text=True, timeout=300)
    if out.returncode != 0:
        sys.stderr.write(out.stdout + out.stderr)
    return out.returncode == 0


def registry_is_dirty():
    """Uncommitted changes to the files a merge rewrites.

    A merge is a MAINTENANCE command: it deliberately writes to the registry, and any
    packet may run it -- restricting it to W0-2 would put a contributor back to dropping
    and waiting, which is the deadlock the drop channel exists to remove.  What it must not
    do is write over someone's in-flight edits, so it refuses while they are uncommitted.
    Returns the dirty paths, or [] when the tree is clean or git cannot answer."""
    import subprocess
    try:
        out = subprocess.run(["git", "status", "--porcelain", "--"] + list(REGISTRY_PATHS),
                             cwd=ROOT, capture_output=True, text=True, timeout=30)
    except Exception:
        return []
    if out.returncode != 0:
        return []
    dirty = []
    for line in out.stdout.splitlines():
        path = line[3:].strip().strip('"')
        if path:
            dirty.append(path)
    return dirty


def merge_pending():
    """Copy every new pending row into facts.tsv, leaving the drop files untouched."""
    if not os.path.isdir(PENDING_DIR):
        return 0
    lines = open(FACTS_TSV, "r", encoding="utf-8", newline="").read().rstrip("\n").split("\n")
    have = {l.split("\t")[0] for l in lines if l and not l.startswith("#")}
    merged_rows = {l.split("	")[0]: l.split("	")
                   for l in lines if l and not l.startswith("#")}
    retired = retired_ids()
    added, files = [], []
    for name in sorted(os.listdir(PENDING_DIR)):
        if not name.endswith(".tsv") or not drop_selected(name):
            continue
        path = os.path.join(PENDING_DIR, name)
        widen = False
        seen_header = False
        unreadable = False
        n = 0
        for lineno, line in enumerate(
                open(path, "r", encoding="utf-8", newline="").read().splitlines(), 1):
            line = line.rstrip("\r")   # a CRLF drop must not import its CR into the last column
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split("\t")
            if parts[0] == "id":
                if parts == COLS:
                    widen = False
                elif parts == LEGACY_COLS:
                    widen = "v1"
                elif parts == LEGACY_COLS_V2:
                    widen = "v2"
                elif parts == LEGACY_COLS_V3:
                    widen = "v3"
                else:
                    complain_anchor("facts/pending/" + name, parts[0],
                                    "header is not any known schema, so no row in this file "
                                    "can be merged; the whole drop is skipped")
                    unreadable = True
                    break
                seen_header = True
                continue
            if not seen_header:
                complain_anchor("facts/pending/" + name, parts[0],
                                "a data row appears above the `id` header, so its columns "
                                "cannot be identified; the whole drop is skipped")
                unreadable = True
                break
            if widen:
                # Widen by NAME, not by a hand-counted insert: each schema addition
                # silently broke the previous count, and a row short one column is
                # only caught later, by the loader, on a file this already wrote.
                cols = {"v1": LEGACY_COLS, "v2": LEGACY_COLS_V2, "v3": LEGACY_COLS_V3}[widen]
                got = dict(zip(cols, parts))
                # A drop predating `effect` may have used verb/observed for the prose
                # that field now holds.  Keep the body read, do not coerce it into a
                # vocabulary the author never chose.
                if got.get("verb", "-") not in VERBS or got.get("observed", "-") not in OBSERVED:
                    note = " ".join(x for x in (got.get("verb", ""), got.get("observed", ""))
                                    if x and x != "-")
                    got["effect"] = note or got.get("effect", "-")
                    got["verb"] = got["observed"] = "-"
                parts = [got.get(c, "-") for c in COLS]
                line = "	".join(parts)
            if parts[0] in retired:
                continue          # deliberately retired; see facts/retired.tsv
            if parts[0] in have:
                # Already merged.  A resubmitted row that MATCHES is done; one that DIFFERS
                # is an edit that this path cannot apply, and saying nothing about it is how
                # a correction evaporates without its author noticing.
                prev_row = merged_rows.get(parts[0])
                if prev_row is not None and len(parts) == len(COLS):
                    diffs = [COLS[k] for k in range(len(COLS))
                             if prev_row[k] != parts[k] and COLS[k] != "id"]
                    if diffs:
                        rel = "facts/pending/" + name
                        amendable = [d for d in diffs if d in AMENDABLE]
                        how = ("facts/pending/amend/%s.tsv can change %s"
                               % (name[:-4].split("_")[0], ", ".join(amendable))
                               if amendable else
                               "no channel can change %s on a merged row -- a different "
                               "value is a different claim: anchor it, or retire the id and "
                               "add a new one" % ", ".join(diffs))
                        PENDING_COMPLAINTS.append(
                            ("fail", os.path.join(ROOT, rel), 0,
                             "%s differs from the merged row in %s, and editing a drop that "
                             "has ALREADY MERGED changes nothing. %s"
                             % (parts[0], ", ".join(diffs), how)))
                continue
            # A row whose VALUE no reader can parse must never reach the registry.
            # load() refuses it and --check exits 1, but this writer read the drop raw,
            # appended the line verbatim, and the run then died regenerating the header --
            # leaving facts.tsv holding a value nothing downstream can read, while stderr
            # said the row had been skipped.  Refuse it here, by name, before the write.
            padded = dict((c, parts[j] if j < len(parts) else "-")
                          for j, c in enumerate(COLS))
            try:
                parse_value(padded)
            except (FactError, ValueError) as exc:
                PENDING_COMPLAINTS.append(
                    ("refused", os.path.join(PENDING_DIR, name), lineno,
                     "line %d: %s: %s; NOT written to facts.tsv"
                     % (lineno, parts[0], exc)))
                continue
            have.add(parts[0])
            added.append(line)
            n += 1
        files.append((name, n))
    if not added:
        return 0
    write_atomic(FACTS_TSV, "\n".join(lines + added) + "\n")
    return len(added)


def reconcile_duplicates(incumbents):
    """One fact, one id.  `incumbents` are the ids facts.tsv held before this merge."""
    lines = open(FACTS_TSV, "r", encoding="utf-8", newline="").read().rstrip("\n").split("\n")
    cols = [l for l in lines if not l.startswith("#") and l.split("\t")[0] == "id"][0].split("\t")
    ix = {c: i for i, c in enumerate(cols)}
    rows = {}
    order = []
    for l in lines:
        if l.startswith("#") or l.split("\t")[0] == "id":
            continue
        r = l.split("\t")
        rows[r[0]] = r
        order.append(r[0])

    groups = {}
    for fid, r in rows.items():
        if r[ix["type"]] in ("str", "md5") or blob_len(r[ix["type"]]):
            continue
        if r[ix["addr_space"]] not in ("PC_VA", "PC_RVA", "LIBR15_VA"):
            continue
        groups.setdefault((r[ix["addr_space"]], r[ix["value"]]), []).append(fid)

    retire = {}
    for key, ids in groups.items():
        if len(ids) < 2:
            continue
        keep = next((i for i in ids if i in incumbents), sorted(ids)[0])
        for other in ids:
            if other == keep:
                continue
            k, o = rows[keep], rows[other]
            if len(o[ix["evidence"]]) > len(k[ix["evidence"]]):
                k[ix["evidence"]] = o[ix["evidence"]]
            if k[ix["effect"]] in ("-", "") and o[ix["effect"]] not in ("-", ""):
                k[ix["effect"]] = o[ix["effect"]]
            k[ix["comment"]] = (k[ix["comment"]] + " | also submitted as " + other +
                                "; one fact, one id.")[:900]
            retire[other] = keep
            # a row's byte-string children take their address from it; orphaning them
            # leaves a prologue whose owner does not exist
            for child in list(rows):
                if child.startswith(other + "."):
                    retire[child] = keep + child[len(other):]

    if not retire:
        return 0
    out = []
    for l in lines:
        if l.startswith("#") or l.split("\t")[0] == "id":
            out.append(l)
            continue
        fid = l.split("\t")[0]
        if fid in retire:
            continue
        out.append("\t".join(rows[fid]))
    write_atomic(FACTS_TSV, "\n".join(out) + "\n")

    with open(RETIRED_TSV, "a", encoding="utf-8", newline="\n") as f:
        for dead, alive in sorted(retire.items()):
            f.write("%s\t%s\tduplicate of the same address; one fact, one id\n" % (dead, alive))


    # An anchor is verified evidence about an address, and retiring the row that cited
    # it must not throw it away or leave it naming an id that no longer exists.  Hand it
    # to the survivor when the survivor has none; drop it only when already covered.
    alines = open(ANCHORS_TSV, "r", encoding="utf-8", newline="").read().rstrip("\n").split("\n")
    anchored = {l.split("\t")[0] for l in alines if not l.startswith("#")}
    out_a, moved, dropped = [], 0, 0
    for l in alines:
        if l.startswith("#") or l.split("\t")[0] == "fact_id":
            out_a.append(l)
            continue
        parts = l.split("\t")
        alive = retire.get(parts[0])
        if alive is None:
            out_a.append(l)
            continue
        if alive in anchored:
            dropped += 1
            continue
        parts[0] = alive
        anchored.add(alive)
        moved += 1
        out_a.append("\t".join(parts))
    if moved or dropped:
        write_atomic(ANCHORS_TSV, "\n".join(out_a) + "\n")
        print("  anchors: %d moved to the surviving id, %d already covered" % (moved, dropped))
    return len(retire)


PENDING_AMEND_DIR = os.path.join(ROOT, "facts", "pending", "amend")

# Prose and behaviour classification.  Not `value`, and not anything that gates emission:
# an amendment corrects what a row SAYS, never what it claims to be or whether it is usable.
AMENDABLE = ("comment", "evidence", "verb", "observed", "effect")


# fact_id -> [(column, value before the amendment, drop file)], newest last.  Written by
# merge_pending_amendments and read only by revert_amendments.
AMENDMENT_UNDO = {}


def revert_amendments(fids):
    """Put the named rows back the way they were before this run amended them.

    An amendment is a separate act from a row addition, so a merge that cannot keep one
    must be able to give back that one -- not the whole merge.  Returns the notes."""
    fids = [f for f in fids if f in AMENDMENT_UNDO]
    if not fids:
        return []
    lines = open(FACTS_TSV, encoding="utf-8", newline="").read().rstrip("\n").split("\n")
    index = {}
    for i, l in enumerate(lines):
        if l and not l.startswith("#"):
            index.setdefault(l.split("\t")[0], i)
    notes = []
    for fid in fids:
        i = index.get(fid)
        for col, old, rel in reversed(AMENDMENT_UNDO[fid]):
            if i is None:
                continue        # the row itself was dropped; nothing to put back
            cells = lines[i].split("\t")
            cells[COLS.index(col)] = old
            lines[i] = "\t".join(cells)
            notes.append("%s %s reverted to its merged text  [%s]" % (fid, col, rel))
        AMENDMENT_UNDO.pop(fid, None)
    write_atomic(FACTS_TSV, "\n".join(lines) + "\n")
    return notes


def merge_pending_amendments():
    """Apply facts/pending/amend/<packet>.tsv to merged rows.  Returns (applied, [notes])."""
    if not os.path.isdir(PENDING_AMEND_DIR):
        return 0, [], set()
    lines = open(FACTS_TSV, encoding="utf-8", newline="").read().rstrip("\n").split("\n")
    index = {}
    for i, l in enumerate(lines):
        if l and not l.startswith("#"):
            index.setdefault(l.split("\t")[0], i)
    applied, notes, amended = 0, [], set()
    AMENDMENT_UNDO.clear()
    for name in sorted(os.listdir(PENDING_AMEND_DIR)):
        if not name.endswith(".tsv") or not drop_selected(name):
            continue
        rel = "facts/pending/amend/" + name
        path = os.path.join(PENDING_AMEND_DIR, name)
        for line in open(path, encoding="utf-8", newline="").read().splitlines():
            line = line.rstrip("\r")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "fact_id":
                continue
            if len(parts) < 4:
                complain_anchor(rel, parts[0],
                                "amendment needs fact_id, column, new_value, why; got %d "
                                "field(s). Not applied." % len(parts))
                continue
            fid, col, new, why = parts[0], parts[1], parts[2], "\t".join(parts[3:]).strip()
            if col not in AMENDABLE:
                complain_anchor(rel, fid,
                                "column %r is not amendable. Amendable: %s. A wrong number "
                                "is a new claim -- anchor it, or retire the id -- not a "
                                "correction to prose." % (col, ", ".join(AMENDABLE)))
                continue
            # `verb` and `observed` are amendable BECAUSE they are cheap to get wrong, but
            # they are enums the loader validates.  Checking them here -- against the
            # loader's own tuples, not a second copy of them -- refuses the one row with a
            # reason naming the column, instead of letting it through to redden the gate
            # where the only lever left was rolling the whole merge back.
            enum = {"verb": VERBS, "observed": OBSERVED}.get(col)
            if enum is not None and new not in enum:
                complain_anchor(rel, fid,
                                "amended %s %r is not one of (%s). `%s` is an enum the "
                                "registry validates, not free text -- an enumeration or a "
                                "sentence belongs in `comment` or `evidence`. Not applied."
                                % (col, new, ", ".join(enum), col))
                continue
            if not why:
                complain_anchor(rel, fid, "amendment of %s has no `why`. Not applied." % col)
                continue
            if fid not in index:
                complain_anchor(rel, fid, "no such row to amend. Not applied.")
                continue
            i = index[fid]
            cells = lines[i].split("\t")
            if len(cells) != len(COLS):
                complain_anchor(rel, fid, "registry row has %d columns; not amended"
                                % len(cells))
                continue
            j = COLS.index(col)
            if "\t" in new or "\n" in new:
                complain_anchor(rel, fid, "amended %s contains a tab or newline" % col)
                continue
            if cells[j] == new:
                notes.append("%s %s already reads as amended (%s); no change" % (fid, col, rel))
                continue
            notes.append("%s %s\n        was: %s\n        now: %s\n        why: %s  [%s]"
                         % (fid, col, cells[j], new, why, rel))
            AMENDMENT_UNDO.setdefault(fid, []).append((col, cells[j], rel))
            cells[j] = new
            lines[i] = "\t".join(cells)
            amended.add(fid)
            applied += 1
    if applied:
        write_atomic(FACTS_TSV, "\n".join(lines) + "\n")
    return applied, notes, amended


INSTR_FIELD_ANCHOR_KINDS = ("disp8", "disp16", "disp32", "imm8", "imm16",
                            "imm32", "riprel", "callee_arity", "callee_storearg",
                            "callee_argmoveoff")


def merge_pending_anchors():
    """Fold contributed anchors into facts/anchors.tsv, after the file agrees with them."""
    if not os.path.isdir(PENDING_ANCHORS_DIR):
        return 0
    sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))
    from pe import PeImage
    import factcheck
    pe = PeImage(ECHOVR_EXE)

    mine = open(ANCHORS_TSV, "r", encoding="utf-8", newline="").read().rstrip("\n")
    have = {l.split("\t")[0] for l in mine.split("\n") if not l.startswith("#")}
    known = {r["id"]: r for r in load(FACTS_TSV, "facts.tsv")}
    added = 0
    for name in sorted(os.listdir(PENDING_ANCHORS_DIR)):
        if not name.endswith(".tsv") or not drop_selected(name):
            continue
        path = os.path.join(PENDING_ANCHORS_DIR, name)
        for line in open(path, "r", encoding="utf-8", newline="").read().splitlines():
            line = line.rstrip("\r")   # a CRLF drop must not import its CR into the last column
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "fact_id" or parts[0] in have:
                continue
            rel = "facts/pending/anchors/" + name
            try:
                fid, kind, va = parts[0], parts[1], int(parts[2], 16)
                a, n, hexs = int(parts[3]), int(parts[4]), parts[5]
                want = bytes.fromhex(hexs)
            except (ValueError, IndexError) as exc:
                complain_anchor(rel, parts[0], "unreadable (%s); not merged" % exc)
                continue
            if n != len(want):
                complain_anchor(rel, fid,
                                "b=%d but instr_bytes is %d bytes; not merged. b is the "
                                "LENGTH of instr_bytes -- the field width comes from kind."
                                % (n, len(want)))
                continue
            if kind in INSTR_FIELD_ANCHOR_KINDS and not (0 <= a < len(want)):
                complain_anchor(rel, fid, "a=%d is outside the %d-byte instruction; not merged"
                                % (a, len(want)))
                continue
            if fid not in known:
                complain_anchor(rel, fid,
                                "no such fact in the registry; not merged. An anchor for a "
                                "row nobody merged is evidence for nothing -- drop the fact "
                                "row first, or with it.")
                continue
            # The BYTES matching only says the site is real.  What makes an anchor evidence
            # is that the field it points at carries the row's value, and that is checked
            # here rather than after the fold: an anchor merged on its bytes alone fails
            # guard_anchors from inside facts/anchors.tsv, where the drop's owner can no
            # longer withdraw it and the merge that imported it has already returned.
            problem, _evidence = factcheck.anchor_verdict(
                pe, dict(kind=kind, anchor_va=va, a=a, b=n, instr=want), known[fid])
            if problem:
                complain_anchor(rel, fid, problem + "; not merged")
                continue
            mine += "\n" + "\t".join(parts[:6] + [(parts[6] if len(parts) > 6 else "") +
                                                  " [from pending/anchors/%s]" % name])
            have.add(fid)
            added += 1
    if added:
        write_atomic(ANCHORS_TSV, mine + "\n")
    return added


def derive_verdicts():
    """Set verdict and tier from the evidence the guards produce, in both directions.

    PC_DECISIVE requires IDENTITY evidence -- a source that names the value independently.
    A prologue is seeded from the address it guards, so it proves the bytes have not moved
    but not that the address is the right function; that is corroboration, not identity."""
    sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))
    import factcheck

    anchor_kind = {a[0]: a[1] for a, _o in read_anchor_rows()}
    rows = load_all()
    c = factcheck.Ctx(rows)
    for fn in factcheck.GUARDS:
        if fn.__name__ in ("guard_verdict", "guard_coverage", "guard_provenance_evidence",
                           "guard_source_sanity", "guard_arm64_evidence"):
            continue
        fn(c)
    identity, binary = set(c.identity_checked), set(c.binary_checked)

    ASSESSED = ("PC_ABSENT", "NOT_IN_CODE", "ARM64_ONLY")
    lines = open(FACTS_TSV, "r", encoding="utf-8", newline="").read().rstrip("\n").split("\n")
    out, changed, touched = [], 0, set()
    for l in lines:
        _was = l
        if l.startswith("#") or l.split("\t")[0] == "id":
            out.append(l)
            continue
        r = l.split("\t")
        fid = r[0]
        # normalise the libr15 citation first: it applies whatever the verdict is
        if r[COLS.index("provenance")] != "ARM64":
            for col in ("source_a", "source_b"):
                if r[COLS.index(col)] == "libr15_symbols":
                    r[COLS.index(col)] = "libr15_names"
                    changed += 1
        if r[COLS.index("verdict")] in ASSESSED:
            _now = "	".join(r)
            if _now != _was:
                touched.add(fid)
            out.append(_now)
            continue
        if fid in identity:
            v, t, prov = "PC_DECISIVE", "V-PRIMARY", "PC"
        elif fid in binary:
            v, t, prov = "PC_CORROBORATED", "V", "PC"
        else:
            # Nothing read this value out of the binary.  Withheld, whatever the drop
            # claimed -- an address nobody checked is an address that can be off by one
            # with every guard still green.
            #
            # PC_CANDIDATE keeps its name.  Both states mean "no binary evidence", but a
            # candidate additionally means "and the experiment that settles it is the
            # point", which is what puts it in facts::candidate where a harness may read
            # it.  Collapsing the two would take the number back out of the one place it
            # is allowed to be used, silently, on the merge that was meant to deliver it.
            if r[COLS.index("verdict")] == VERDICT_CANDIDATE:
                v, t, prov = VERDICT_CANDIDATE, "U", "UNKNOWN"
            else:
                v, t, prov = "PC_PRESENT", "U", "UNKNOWN"
        # S4.6 rule 9.  A row that claims to change something, with nobody having read what
        # it changes, is the N-7 shape exactly: correct address, correct build, correct
        # guard, inverse behaviour.  It holds at U until an effect is recorded.
        if r[COLS.index("verb")] == "mutate" and r[COLS.index("effect")] == "-":
            t = "U"
        # libr15 may legitimately supply a NAME for a PC row -- that is not a number, so it
        # must not force ARM64 provenance.  The distinction is in the source vocabulary:
        # `libr15_symbols` contributed the value, `libr15_names` contributed only the name.
        if prov != "ARM64":
            for col in ("source_a", "source_b"):
                if r[COLS.index(col)] == "libr15_symbols":
                    r[COLS.index(col)] = "libr15_names"
                    changed += 1
        if (r[COLS.index("verdict")], r[COLS.index("tier")], r[COLS.index("provenance")]) != (v, t, prov):
            r[COLS.index("verdict")] = v
            r[COLS.index("tier")] = t
            r[COLS.index("provenance")] = prov
            # A promotion updates one half of several coupled pairs.  Leaving the other
            # half stale makes the row internally inconsistent BY A SUCCESSFUL OPERATION:
            # W2-C anchored NETGAME.ALIGNMENT_BASE_OFF, derive promoted it to V-PRIMARY,
            # and both source columns still named documents -- a rule-4 violation that
            # blocked regeneration for every packet.  Every legacy U row that later gains
            # an anchor reproduces it, so the promotion fixes its partners here.
            if prov == "PC" and v in ("PC_DECISIVE", "PC_CORROBORATED"):
                sa, sb = COLS.index("source_a"), COLS.index("source_b")
                srcs = load_sources()
                ok = any(srcs.get(r[c], ("", False, ""))[1]
                         and srcs.get(r[c], ("", False, ""))[2] in ("static", "both")
                         for c in (sa, sb))
                if not ok:
                    r[sa], r[sb] = "pe_file_bytes", "ida_db_taint"
                kind = anchor_kind.get(fid)
                if kind:
                    g = COLS.index("guard")
                    r[g] = {"disp8": "G_DISP", "disp16": "G_DISP", "disp32": "G_DISP",
                            "imm8": "G_IMM", "imm16": "G_IMM", "imm32": "G_IMM",
                            "riprel": "G_RIPREL", "vtslot": "G_VTSLOT",
                            "u32_at": "G_F32CONST", "bytes_at": "G_PROLOGUE",
                            "instr_at": "G_INSTR_AT", "funcstart": "G_INSTR_AT"}.get(kind, r[g])
            changed += 1
        _now = "\t".join(r)
        if _now != _was:
            touched.add(fid)
        out.append(_now)
    write_atomic(FACTS_TSV, "\n".join(out) + "\n")
    return changed, touched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed-prologues", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--merge-pending", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="merge over uncommitted registry changes, and NOTHING else; "
                         "for the owner of those edits")
    ap.add_argument("--only", default="",
                    help="merge only these packets' drops, comma-separated (e.g. W2-D). "
                         "One contributor per merge keeps one commit to one packet's rows.")
    ap.add_argument("--out", default=OUT_HEADER)
    args = ap.parse_args()

    if args.merge_pending:
        MERGE_ONLY.update(x.strip() for x in args.only.split(",") if x.strip())
        if MERGE_ONLY:
            skipped = sorted({n[:-4].split("_")[0]
                              for n in os.listdir(PENDING_DIR) if n.endswith(".tsv")}
                             - MERGE_ONLY)
            print("merging only: %s" % ", ".join(sorted(MERGE_ONLY)))
            print("  left in the channel for their own merge: %s"
                  % (", ".join(skipped) if skipped else "(none)"))
        del PENDING_COMPLAINTS[:]
        load_all()
        if PENDING_COMPLAINTS:
            # NOT a refusal.  Refusing the whole merge over one unreadable row would mean
            # architect's half-migrated drop blocks W2-C's verified one -- the same block
            # the dirty-tree check was just narrowed to stop causing, rebuilt one level up.
            # An unreadable row simply does not merge; the readable ones do.
            print("NOT MERGED -- these rows could not be read, and were skipped:",
                  file=sys.stderr)
            for sev, path, lineno, msg in sorted(set(PENDING_COMPLAINTS)):
                rel = rel_to_root(path).replace(os.sep, "/")
                print("    %s:%s  [%s]  %s" % (rel, lineno, path_owner(rel) or "unclaimed", msg),
                      file=sys.stderr)
            print("Everything else in the drop channel merged normally.", file=sys.stderr)
            print("", file=sys.stderr)
        # A drop this merge SELECTED whose anchors no reader can parse: refuse, rather than
        # merge its fact rows and leave every one of them unverified-looking-un-anchored.
        # Only the selected ones -- another packet's unreadable file must not block yours,
        # which is the block --only exists to remove.
        read_anchor_rows()      # populates UNREADABLE_DROPS; load_all() reads facts, not anchors
        blocked = sorted({rel for rel, _fid, _why in UNREADABLE_DROPS
                          if drop_selected(rel.rsplit("/", 1)[-1])})
        if blocked:
            e = lambda t: print(t, file=sys.stderr)
            e("REFUSING TO MERGE: a drop this merge would fold in has rows no reader can")
            e("parse. They were being SKIPPED, which is how a packet's evidence stops")
            e("reaching the gate without anybody noticing.")
            e("")
            for rel in blocked:
                bad_rows = [(f, w) for r, f, w in UNREADABLE_DROPS if r == rel]
                e("    %-40s owned by %s" % (rel, path_owner(rel) or "unclaimed"))
                for fid, why in sorted(set(bad_rows)):
                    e("        %-38s %s" % (fid, why))
            e("")
            e("Fix the rows, or drop them; then merge again.")
            return 2
        dirty = registry_is_dirty()
        if dirty and not args.force:
            e = lambda t: print(t, file=sys.stderr)
            e("REFUSING TO MERGE: a file this merge WRITES has uncommitted changes.")
            e("")
            owners = set()
            incoming = pending_row_ids()
            for d in dirty:
                own = path_owner(d) or "unclaimed"
                owners.add(own)
                e("    %-34s owned by %s" % (d, own))
                touched = uncommitted_ids(d)
                if touched is None:
                    continue
                clash = sorted(touched & incoming)
                if clash:
                    e("        %d uncommitted row(s), %d of them also in your drop: %s"
                      % (len(touched), len(clash), ", ".join(clash[:4])))
                elif touched:
                    e("        %d uncommitted row(s), none of them rows you are adding"
                      % len(touched))
            e("")
            e("The harm is entanglement, not corruption: the merge reads this file from disk,")
            e("so those edits survive it -- but `git add` afterwards cannot separate them from")
            e("your drop, and they would land under your commit message and your name.")
            e("")
            blessing = [d for d in dirty if d.replace(os.sep, "/") in BLESSING_PATHS]
            if blessing:
                verdict, detail = blessing_residue()
                e("%s %s DERIVED: factcheck.py --update-hashes and every"
                  % (", ".join(blessing), "are" if len(blessing) > 1 else "is"))
                e("merge rewrite them from the tree, so uncommitted rows there are not hand")
                e("work in flight -- they are the output of a merge that already ran here.")
                e(detail)
                e("")
                if verdict == "residue":
                    e("These rows are RESIDUE of an uncommitted merge: its data files were")
                    e("rolled back and its blessing files were not. Committing them registers")
                    e("a scope for a registry that does not exist, and every all-rows guard")
                    e("reds on the next factcheck run. Do not commit them -- their owner runs")
                    e("    git checkout -- " + " ".join(blessing))
                    e("and then merges again.")
                else:
                    e("Commit them ONLY if they reproduce from HEAD: revert them with")
                    e("`git checkout --`, re-run whatever wrote them, and confirm")
                    e("`python tests/factcheck/factcheck.py` says status=PASS with the rows")
                    e("back. If it does not, they are residue and must be reverted, not landed.")
                e("")
            if owners == {"W0-2"}:
                e("Ask W0-2 to commit -- or, for a derived file the test above calls residue,")
                e("to revert it -- then merge again. Do NOT stash or revert anything else:")
                e("those are not your edits, and in-flight registry work is not yours to move.")
            else:
                e("Ask the owner(s) above to commit, then merge again -- those names are a hint")
                e("from .owner markers. Only stash or")
                e("revert a path you own; in-flight registry work is not yours to move.")
            e("(--force overrides THIS, and only this. It is for the owner of the listed")
            e(" edits, nobody else, and it does not accept rows the merge would reject.)")
            e("")
            e("Not a cause of this refusal, so do not go looking there: untracked drops under")
            e("facts/pending/ are INPUTS to the merge. They are supposed to arrive untracked")
            e("and the check has never read them. Only the files listed above can block you.")
            return 2
        _early = set(PENDING_COMPLAINTS)
        _gate_before = gate_failures()
        _snapshot = snapshot_outputs()
        _before = {r["id"] for r in load(FACTS_TSV, "facts.tsv")}
        n = merge_pending()
        print("merged %d rows from facts/pending/" % n)
        na = merge_pending_anchors()
        if na:
            print("merged %d contributed anchor(s) from facts/pending/anchors/" % na)
        nam, amend_notes, amended_ids = merge_pending_amendments()
        if amend_notes:
            print("amendments from facts/pending/amend/:")
            for note in amend_notes:
                print("    " + note)
        if nam:
            print("applied %d amendment(s) to merged rows" % nam)
        _late = sorted(set(PENDING_COMPLAINTS) - _early)
        if _late:
            print("NOT APPLIED -- rejected while merging:", file=sys.stderr)
            for sev, path, lineno, msg in _late:
                rel = rel_to_root(path).replace(os.sep, "/")
                print("    %s  [%s]  %s" % (rel, path_owner(rel) or "unclaimed", msg),
                      file=sys.stderr)
        d = reconcile_duplicates(_before)
        if d:
            print("retired %d duplicate id(s); see facts/retired.tsv" % d)
        n, promoted = derive_verdicts()
        print("verdict/tier re-derived from guard evidence on %d rows" % n)
        _reason = ("merged %d row(s) and %d anchor(s) from the drop channel" % (n, na))

        def _regen():
            r = load_all()
            os.makedirs(os.path.dirname(args.out), exist_ok=True)
            write_atomic(args.out, emit_header(registry_only(r)))
            write_atomic(OUT_WORKLIST, emit_worklist(registry_only(r)))

        def _introduced():
            # G_HASH is excluded: a merge adds rows, added rows grow scope, and grown scope
            # is a G_HASH failure by construction.  It is re-registered below instead --
            # and a merge that narrowed a scope or changed a body would be refused there,
            # because neither has a flag and neither is something a merge can cause.
            after = gate_failures()
            if _gate_before is None or after is None:
                return None
            return sorted(l for l in after - _gate_before if " G_HASH " not in l)

        _regen()
        bad = _introduced()
        _merged_ids = {r["id"] for r in load(FACTS_TSV, "facts.tsv")} - _before
        # NOT `and not args.force`.  --force is documented as "merge over uncommitted
        # registry changes", and it silently also switched off this -- the path that merges,
        # re-runs the gate, and removes any row that reddened it.  One --force passed to get
        # past a dirty tree let a row through that the merge had already judged and would
        # have rejected, and the gate then failed with a message about the row rather than
        # about the flag.  A flag means the one thing its help text says.
        if bad:
            culprits = {failure_fact_id(l) for l in bad}
            # Three populations, and they are not the same kind of thing.  A row this merge
            # ADDED can be removed; an amendment this merge APPLIED can be put back; a
            # failure naming neither is one the merge cannot undo, and only that last one
            # justifies rolling the merge back.  Folding the second into the third is what
            # made one bad cell in one packet's amendment file block every other packet's
            # rows indefinitely -- the merge had already identified all seven culprits.
            new_rows = culprits & _merged_ids
            bad_amends = {c for c in culprits - new_rows if c in AMENDMENT_UNDO}
            orphans = culprits - new_rows - bad_amends
            if culprits and not orphans:
                drop_rows(new_rows)
                undone = revert_amendments(bad_amends)
                _regen()
                still = _introduced()
                if still:
                    restore_outputs(_snapshot)
                    print("ROLLED BACK: dropping the %d offending row(s) did not clear it."
                          % len(culprits), file=sys.stderr)
                    if nam:
                        print("    DISCARDED with it: the %d amendment(s) printed above."
                              % nam, file=sys.stderr)
                    for line in still:
                        print("    " + line, file=sys.stderr)
                    return 2
                if new_rows:
                    print("REJECTED %d row(s) -- they merged, red the gate, and were removed:"
                          % len(new_rows), file=sys.stderr)
                    for fid in sorted(new_rows):
                        print("    %s" % fid, file=sys.stderr)
                if undone:
                    print("REVERTED %d amendment(s) counted above -- they red the gate:"
                          % len(undone), file=sys.stderr)
                    for note in undone:
                        print("    %s" % note, file=sys.stderr)
                    nam -= len(undone)
                    amended_ids = set(amended_ids) - bad_amends
                for line in bad:
                    print("        " + line, file=sys.stderr)
                print("Everything else merged. Fix these in your drop and merge again.",
                      file=sys.stderr)
                n -= len(new_rows)
                _reason += ", rejected %d" % len(new_rows)
                if undone:
                    _reason += ", reverted %d amendment(s)" % len(undone)
            else:
                restore_outputs(_snapshot)
                print("ROLLED BACK: %d gate failure(s) name %d fact(s) this merge neither"
                      % (len(bad), len(orphans)), file=sys.stderr)
                print("added nor amended, so there is nothing for it to take back. Registry",
                      file=sys.stderr)
                print("unchanged. Unattributable: %s" % ", ".join(sorted(orphans)),
                      file=sys.stderr)
                for line in bad:
                    print("    " + line, file=sys.stderr)
                return 2
        if not reregister_hashes(_reason, set(amended_ids) | promoted | _merged_ids):
            restore_outputs(_snapshot)
            print("ROLLED BACK: re-registering the guard scopes this merge moved was refused",
                  file=sys.stderr)
            if nam:
                print("    DISCARDED with it: the %d amendment(s) printed above. They were "
                      "applied and" % nam, file=sys.stderr)
                print("    are now undone -- the output above shows them landing, so read "
                      "this line as", file=sys.stderr)
                print("    the correction to it.", file=sys.stderr)
            print("(see above). A merge adds rows; it cannot narrow a scope or change a guard",
                  file=sys.stderr)
            print("body, so a refusal here means something other than a merge happened.",
                  file=sys.stderr)
            return 2

    rows = load_all()

    if args.seed_prologues:
        n = seed_prologues(rows)
        print("seeded %d prologue rows from echovr.exe" % n)
        rows = load_all()

    # A defect in an unmerged drop is its owner's, and generation no longer emits that row
    # at all, so failing here would stop everyone's regeneration over a proposal.
    _pending = {r["id"]: r["_origin"] for r in rows
                if r.get("_origin", "").startswith("pending/")}
    problems, proposals = [], []
    for msg in validate(rows):
        fid = msg.split(":", 1)[0]
        (proposals if fid in _pending else problems).append(
            msg if fid not in _pending else "%s [unmerged, in %s]" % (msg, _pending[fid]))
    for msg in proposals:
        # Under --check these are not warnings: --check is the contributor's own pass over
        # the drop channel, and a drop that breaks a per-row rule is exactly what it is for.
        # They are reported as PENDING-REFUSED below instead of twice.
        if not args.check:
            print("WARN %s" % msg, file=sys.stderr)
    if problems:
        for p in problems:
            print("FAIL %s" % p, file=sys.stderr)
        print("%d registry violations" % len(problems), file=sys.stderr)
        return 1

    header = emit_header(registry_only(rows))
    worklist = emit_worklist(registry_only(rows))

    if args.check:
        bad = False

        # facts/pending/ is a channel --check used to walk straight past: load_all() reads it,
        # records every unreadable row in PENDING_COMPLAINTS / UNREADABLE_DROPS, and nothing
        # outside --merge-pending ever printed them.  So `gen_facts.py --check` was silent and
        # exited 0 over a drop holding 0xZZZ, and anybody told to validate a drop with it was
        # running a vacuous check.  It reports the channel now, per row, and fails on a row no
        # reader could take.  It still does not merge.
        refused, warned = pending_report(PENDING_COMPLAINTS, UNREADABLE_DROPS)
        for msg in proposals:
            refused.append("%s  [the per-row rule the registry itself is held to]" % msg)
        pending_files = ([n for n in sorted(os.listdir(PENDING_DIR)) if n.endswith(".tsv")]
                         if os.path.isdir(PENDING_DIR) else [])
        pending_rows = sum(1 for r in rows if r.get("_origin", "").startswith("pending/"))
        for line in warned:
            print("PENDING-WARN %s" % line)
        for line in refused:
            print("PENDING-REFUSED %s" % line, file=sys.stderr)
        # An empty channel is not a pass: say what was read, so a run that covered nothing
        # cannot be mistaken for a run that found nothing.
        print("PENDING files=%d rows=%d refused=%d warned=%d"
              % (len(pending_files), pending_rows + len(refused), len(refused), len(warned)))
        if refused:
            print("%d pending row(s) REFUSED. --merge-pending skips exactly these, so they "
                  "are evidence that would never reach a guard." % len(refused),
                  file=sys.stderr)
            bad = True

        shape_lines, shape_fail = arg_shape_report(rows)
        for line in shape_lines:
            print(line)
        for line in shape_fail:
            print("FAIL %s" % line, file=sys.stderr)
        if shape_fail:
            bad = True

        getter_lines, getter_fail = getter_shape_report(rows)
        for line in getter_lines:
            print(line)
        for line in getter_fail:
            print("FAIL %s" % line, file=sys.stderr)
        if getter_fail:
            bad = True

        for path, want in ((args.out, header), (OUT_WORKLIST, worklist)):
            have = open(path, "r", encoding="utf-8").read() if os.path.exists(path) else None
            if have != want:
                print("FAIL %s is stale; re-run tools/gen_facts.py" % path, file=sys.stderr)
                bad = True
        return 1 if bad else 0

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    write_atomic(args.out, header)
    write_atomic(OUT_WORKLIST, worklist)
    # Two packets read a withheld row as permanent and stopped.  This path regenerates the
    # header and deliberately does NOT re-derive -- verification must not mutate tracked
    # state (S4.6 rule 0) -- so it has to say so, or the silence reads as a verdict.
    withheld_now = [r for r in rows if r["verdict"] in VERDICT_WITHHELD]
    if withheld_now and not args.merge_pending:
        print("note: %d row(s) are withheld. This path does NOT re-derive verdicts. If you have "
              "just added evidence, run:" % len(withheld_now))
        print("        tools/gen_facts.py --merge-pending      (merges, then re-derives)")
        print("      Anchors first, then merge: an anchored row is promoted on the way in. "
              "Contribute anchors via facts/pending/anchors/<packet>.tsv.")
    print("%s: %d facts (%d ARM64 withheld, %d unverified)" % (
        args.out, len(rows), sum(1 for r in rows if r["provenance"] == "ARM64"),
        sum(1 for r in rows if r["tier"] == "U")))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FactError as e:
        print("FAIL %s" % e, file=sys.stderr)
        sys.exit(1)
