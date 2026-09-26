#!/usr/bin/env python3
"""Redact credentials out of captured launch artifacts.

WHY THIS EXISTS
    The GAME writes a WebSocket peer URI to stdout with the owner's credentials in the query
    string ([TCP CLIENT] [R14NETCLIENT] connection to ws://host:80?k=v&k=v ...). We cannot change
    that - echovr.exe must stay byte-identical. But the test harness redirects stdout/stderr and
    copies the game's own log into runs/, which turns a transient console line into three durable
    files per launch. The durability is ours, so the containment is ours.
    See docs/spikes/SECURITY-CREDS.md.

THE DESIGN RULE, AND IT IS THE WHOLE POINT (DESIGN 9.4)
    Substring/prefix matching is this project's commonest false pass - five instances. A redactor
    built as a BLOCKLIST of secret key names ("password", "passwd", "pw", "token", ...) is exactly
    that shape: it is complete only until the next variant, and a redactor that misses a variant is
    WORSE than none, because the report then says the files are clean.

    So this is an ALLOWLIST. kSafeKeys starts EMPTY. Every query-string value is redacted unless
    somebody has argued a specific key into the allowlist in review. Unknown is the redacting case,
    so an unseen key cannot slip through. It is fail-safe by construction rather than by the
    completeness of a list.

    It also never matches on the VALUE. Matching the value would mean holding the value, and would
    miss any re-encoding, any second account, and any rotation.

WHAT IT DELIBERATELY DOES NOT DO
    - It does not encode the original length into the marker. "<redacted:len=17>" would leak a
      real fact about the secret. The marker is fixed.
    - It does not touch anything outside a URI query string. A line of prose containing the word
      "password" is left exactly alone: over-redaction destroys evidence and trains readers to
      distrust the marker.
    - It does not delete lines or files. Line count, ordering, timestamps, log tags and the
      established/failed verb all survive, so these artifacts stay usable as crash and probe
      evidence.

THREE OUTCOMES, NOT TWO (DESIGN 9.1)
    A tool that meets input it does not understand must refuse, not shrug. After rewriting, this
    re-runs the DETECTOR over what it actually wrote. If any query-string value survived, it
    restores the original bytes and reports REFUSED - it never leaves a half-redacted file behind
    while returning success.
        clean     - nothing to redact
        redacted  - n value(s) replaced, residue scan clean
        REFUSED   - residue remained; the file was left exactly as found

Byte-level on purpose: files are read and written as bytes via latin-1 (a lossless byte<->char
map), so BOMs, CRLF line endings and any non-UTF-8 bytes in the game's own log survive untouched.
"""

import argparse
import hashlib
import json
import os
import re
import sys

# ---------------------------------------------------------------------------------------------
# ONE definition of the rule, shared with the test harness, which applies it at capture time.
# redact_patterns.json holds the allowlist (EMPTY BY DESIGN - adding a key is a review decision,
# and a key listed there keeps its value in cleartext in every captured artifact, forever), the
# marker, the three patterns, and the near-miss corpus both engines must pass. A second copy of
# any of them here would drift from the harness's, which is design 9.10 - so there is none.
# ---------------------------------------------------------------------------------------------
RULES_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "redact_patterns.json")


def load_rules(path=RULES_PATH) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        rules = json.load(f)
    for key in ("marker", "safe_keys", "uri", "authority", "param", "controls"):
        if key not in rules:
            raise ValueError("%s: missing '%s'" % (path, key))
    return rules


_RULES = load_rules()
kSafeKeys = frozenset(k.casefold() for k in _RULES["safe_keys"])
MARKER = _RULES["marker"]

# Stage 1: the things that can carry a query string.
#
# TWO patterns, because one was not enough and the near-miss corpus proved it. A first cut anchored
# only on "<scheme>://" left a schemeless "host.name:80?k=v" completely untouched - the exact 9.4
# failure this tool exists to avoid, caught by negative control C11 before any real file was opened.
# So an authority form is matched too. It still demands a dotted host AND a "?" AND a "k=v" inside
# the query, so ordinary prose - "really?yes", "which one?" - cannot reach it.
URI_RE = re.compile(_RULES["uri"])
AUTHORITY_RE = re.compile(_RULES["authority"])

# Stage 2: inside a URI's query string only. Key must be a whole token bounded by ? & and = -
# never a substring of a longer key (9.4: word boundaries, not prefixes).
PARAM_RE = re.compile(_RULES["param"])


def _redact_uri(uri: str, counter: list) -> str:
    """Redact the values in one URI's query string. Returns the rewritten URI."""
    q = uri.find("?")
    if q < 0:
        return uri                      # no query string: nothing to do, host and path survive
    head, query = uri[: q + 1], uri[q + 1:]

    def one(m):
        key, separator, value = m.group(1), m.group(2), m.group(3)
        if key.casefold() in kSafeKeys:
            return m.group(0)           # explicitly argued for in review
        if value == "" or value == MARKER:
            return m.group(0)           # nothing there, or already done: idempotent
        counter[0] += 1
        return key + separator + MARKER

    # PARAM_RE's lookbehind needs a delimiter before the first pair, so feed it one and drop it.
    return head + PARAM_RE.sub(one, "&" + query)[1:]


def redact_text(text: str) -> tuple:
    """Pure. Returns (rewritten_text, values_redacted)."""
    counter = [0]
    out = URI_RE.sub(lambda m: _redact_uri(m.group(0), counter), text)
    out = AUTHORITY_RE.sub(lambda m: _redact_uri(m.group(0), counter), out)
    return out, counter[0]


def detect_text(text: str) -> int:
    """THE DETECTOR, and it is deliberately a separate function from the rewriter (9.17: ask the
    consumer, do not let the producer grade itself). Counts query-string values that are still
    present in cleartext."""
    n = 0
    spans = list(URI_RE.finditer(text)) + list(AUTHORITY_RE.finditer(text))
    for m in spans:
        uri = m.group(0)
        q = uri.find("?")
        if q < 0:
            continue
        for pm in PARAM_RE.finditer("&" + uri[q + 1:]):
            key, value = pm.group(1), pm.group(3)
            if key.casefold() in kSafeKeys:
                continue
            if value == "" or value == MARKER:
                continue
            n += 1
    return n


def _md5(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


def process(path: str, apply: bool) -> dict:
    with open(path, "rb") as f:
        original = f.read()
    text = original.decode("latin-1")

    before = detect_text(text)
    rewritten, n = redact_text(text)
    residue = detect_text(rewritten)

    row = {
        "path": path,
        "before": before,
        "redacted": n,
        "residue": residue,
        "md5_before": _md5(original),
        "md5_after": None,
        "lines_before": text.count("\n"),
        "lines_after": rewritten.count("\n"),
        "status": None,
    }

    if residue != 0:
        # 9.1: refuse, do not shrug. Leave the file exactly as found and say so loudly.
        row["status"] = "REFUSED"
        return row
    if n == 0:
        row["status"] = "clean"
        row["md5_after"] = row["md5_before"]
        return row
    if row["lines_before"] != row["lines_after"]:
        row["status"] = "REFUSED"       # cannot happen by construction; assert it anyway
        return row

    payload = rewritten.encode("latin-1")
    if apply:
        tmp = path + ".redact.tmp"
        with open(tmp, "wb") as f:
            f.write(payload)
        os.replace(tmp, path)
    row["md5_after"] = _md5(payload)
    row["status"] = "redacted" if apply else "would-redact"
    return row


# The near-miss corpus lives in redact_patterns.json beside the patterns, so anyone who touches the
# matcher re-runs it from either engine and a future edit that reintroduces a gap fails immediately.
# Every value is synthetic (SYNTH-*); no real value has ever been in it, and none may ever be added.
#
# C11 is the reason the corpus exists: the first matcher anchored only on "<scheme>://" and left a
# schemeless "host:port?k=v" untouched - 9.4 inside the very tool written to prevent 9.4.
#
# Rows whose expected output equals their input are the OVER-redaction controls: a matcher that
# mangles those is as broken as one that misses a secret.
SELFTEST = [(row["label"], row["input"], row["expected"]) for row in _RULES["controls"]]


def selftest() -> int:
    failures = 0
    for label, src, want in SELFTEST:
        got, _ = redact_text(src)
        over = (src == want)
        if got != want:
            failures += 1
            kind = "OVER-REDACTED" if over else "MISSED"
            print("FAIL  %-13s %s" % (kind, label))
        else:
            print("ok    %-13s %s" % ("untouched" if over else "redacted", label))
        # the detector must agree with the rewriter, in both directions
        if detect_text(got) != 0:
            failures += 1
            print("FAIL  RESIDUE       %s" % label)
    print("---")
    print("selftest: %d control(s), %d failure(s)" % (len(SELFTEST), failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="files to process")
    ap.add_argument("--selftest", action="store_true",
                    help="run the near-miss control corpus and exit; changes nothing on disk")
    ap.add_argument("--apply", action="store_true",
                    help="rewrite in place; without it this is a dry run")
    ap.add_argument("--detect-only", action="store_true",
                    help="report the detector's count and change nothing")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.paths:
        ap.error("give at least one path, or --selftest")

    total_before = total_red = total_residue = 0
    refused = 0
    for p in args.paths:
        if args.detect_only:
            with open(p, "rb") as f:
                n = detect_text(f.read().decode("latin-1"))
            total_before += n
            print("%-9s %5d  %s" % ("detect", n, p))
            continue
        row = process(p, args.apply)
        total_before += row["before"]
        total_red += row["redacted"]
        total_residue += row["residue"]
        if row["status"] == "REFUSED":
            refused += 1
        print("%-12s before=%-4d redacted=%-4d residue=%-3d lines=%d->%d md5 %s -> %s  %s" % (
            row["status"], row["before"], row["redacted"], row["residue"],
            row["lines_before"], row["lines_after"],
            row["md5_before"][:8], (row["md5_after"] or "--------")[:8], row["path"]))

    print("---")
    print("files=%d  values_found=%d  values_redacted=%d  residue=%d  refused=%d"
          % (len(args.paths), total_before, total_red, total_residue, refused))
    return 1 if (refused or total_residue) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
