"""Local gate for the legacy-format converter (DESIGN-FORMATS §11.3). Run on the operator machine over
the owner's recordings, which never enter the tree.

For each source: `echodemo convert` (timed, peak memory from the tool), `echodemo verify`, the decoded
tables through `echodemo rows` (the viewer's reader and windowbuild decode), the §5.4 statistical
chirality, and the counts P5's handoff reports. Exit 1 if any file fails or misses the §8 budgets.

    python tools/import/gate_local.py --echodemo build/bin/echodemo.exe --out-dir <dir> [--json <file>]
        [--perf-runs 5 --perf-file rec_2025-08-03_17-15-57 ...] <sources...>
"""

import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys

import numpy as np

# §8: a 10-minute 30 Hz file within 4 s and 200 MB; the budgets scale with the match length, and a
# miss of more than 25% fails. The bones file has its own 6 s ceiling.
BUDGET_S_PER_10MIN = 4.0
BUDGET_S_BONES = 6.0
BUDGET_MB = 200.0
TOLERANCE = 1.25

UP_MIN, FORWARD_MIN, LEFT_MIN = 0.8, 0.3, 0.9
HEX_COLUMNS = {"bits28", "bits11", "idValue", "owner"}
TAP_STUN, TAP_GOAL = "SR15StunPlayerMsg", "SR15NetReliablePostScoreUpdate"


def run(args):
    return subprocess.run(args, capture_output=True, text=True, errors="replace")


def table(exe, path, what, columns):
    """Selected numeric columns of one `rows` table; the census must report a clean decode."""
    proc = run([exe, "rows", path, "--what", what])
    if proc.returncode != 0:
        raise RuntimeError(f"rows {what}: rc={proc.returncode} {proc.stderr.strip()[:200]}")
    census = proc.stderr.strip() or proc.stdout.strip().splitlines()[-1]
    clean = all(f"{k}=0" in census for k in ("decode_failures", "blocks_bad", "chapter_faults"))
    lines = [l for l in proc.stdout.splitlines() if l and not l.startswith("ROWS ")]
    header = lines[0].split(",")
    idx = [header.index(c) for c in columns]
    text_cols = {c for c in columns if c in ("src", "tap")}
    out = {c: [] for c in columns}
    for line in lines[1:]:
        cells = line.split(",")
        for c, i in zip(columns, idx):
            out[c].append(cells[i])
    for c in columns:
        if c in text_cols:
            out[c] = np.array(out[c], dtype=object)
        elif c in HEX_COLUMNS:
            out[c] = np.array([int(v, 16) if v else -1 for v in out[c]], dtype=np.int64)
        else:
            out[c] = np.array([float(v) if v else math.nan for v in out[c]], dtype=np.float64)
    return out, clean, census


def rotate(q, v):
    x, y, z, w = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    t = 2 * np.stack([y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]], axis=1)
    return v + w[:, None] * t + np.stack([y * t[:, 2] - z * t[:, 1], z * t[:, 0] - x * t[:, 2],
                                          x * t[:, 1] - y * t[:, 0]], axis=1)


def chirality(players, bodies):
    """§5.4. The head basis rides the SimState body ori (H3), the yaw-only frame rides hmdOri (H2)."""
    own = bodies["idTag"] == 0
    q = np.stack([bodies[c][own] for c in ("oriX", "oriY", "oriZ", "oriW")], axis=1)
    v = np.stack([bodies[c][own] for c in ("velX", "velY", "velZ")], axis=1)
    up = rotate(q, np.array([0.0, 1.0, 0.0]))[:, 1]
    fwd = rotate(q, np.array([0.0, 0.0, 1.0]))
    speed = np.linalg.norm(v, axis=1)
    moving = speed > 2.0
    forward = float(np.mean(np.sum(fwd[moving] * v[moving], axis=1) / speed[moving])) if moving.any() else 0.0
    hq = np.stack([players[c] for c in ("hmdX", "hmdY", "hmdZ", "hmdW")], axis=1)
    ax = rotate(hq, np.array([1.0, 0.0, 0.0]))
    lo = np.stack([players[c] for c in ("lOffX", "lOffY", "lOffZ")], axis=1)
    ro = np.stack([players[c] for c in ("rOffX", "rOffY", "rOffZ")], axis=1)
    l, r = np.sum(lo * ax, axis=1), np.sum(ro * ax, axis=1)
    differ = l != r
    left = float(np.mean(l[differ] > r[differ])) if differ.any() else 0.0
    return {"up_median": float(np.median(up)), "forward_mean": forward, "moving": int(moving.sum()),
            "left_frac": left}


def convert(exe, src, out, runs):
    times, private, working = [], [], []
    text = ""
    for _ in range(runs):
        proc = run([exe, "convert", src, out])
        text = proc.stdout + proc.stderr
        if proc.returncode != 0:
            return None, text
        times.append(float(re.search(r"converted in ([0-9.]+) s", text).group(1)))
        m = re.search(r"peak memory: private ([0-9.]+) MB, working set ([0-9.]+) MB", text)
        private.append(float(m.group(1)))
        working.append(float(m.group(2)))
    return {"seconds_median": statistics.median(times), "seconds_all": times, "peak_private_mb": max(private),
            "peak_working_set_mb": max(working)}, text


def gate_one(exe, src, out_dir, runs):
    stem = os.path.splitext(os.path.basename(src))[0]
    kind = os.path.splitext(src)[1].lstrip(".")
    out = os.path.join(out_dir, f"{stem}.{kind}.echodemo")
    row = {"file": os.path.basename(src), "source_bytes": os.path.getsize(src), "failures": []}
    perf, text = convert(exe, src, out, runs)
    if perf is None:
        row["failures"].append("convert: " + text.strip()[:300])
        return row
    row.update(perf)
    row["output_bytes"] = os.path.getsize(out)
    sidecar = json.load(open(out + ".import.json", encoding="utf-8"))
    row["inferred"] = sidecar["inferred"]
    row["gaps"] = sidecar["repairs"]["gaps"]
    row["bones_skipped"] = sidecar["source"]["bonesSkipped"]
    row["duplicate_frames"] = sidecar["source"]["duplicateFrames"]

    verify = run([exe, "verify", out])
    row["verify_rc"] = verify.returncode
    row["verify_known_gaps"] = verify.stdout.count("import gap at")
    if verify.returncode != 0:
        row["failures"].append("verify: " + " | ".join(l for l in verify.stdout.splitlines() if "FAIL" in l)[:300])

    players, ok_p, _ = table(exe, out, "player", ["tUs", "slot", "hmdX", "hmdY", "hmdZ", "hmdW", "lOffX", "lOffY",
                                                  "lOffZ", "rOffX", "rOffY", "rOffZ", "bits28"])
    bodies, ok_b, _ = table(exe, out, "body", ["idTag", "oriX", "oriY", "oriZ", "oriW", "velX", "velY", "velZ"])
    events, ok_e, _ = table(exe, out, "event", ["tap"])
    if not (ok_p and ok_b and ok_e):
        row["failures"].append("rows: decode faults")
    row["duration_s"] = float((players["tUs"].max() - players["tUs"].min()) / 1e6)
    row["players"] = int(len(np.unique(players["slot"])))
    row["goals"] = int(np.sum(events["tap"] == TAP_GOAL))
    row["stun_rows"] = int(np.sum(events["tap"] == TAP_STUN))
    row["chirality"] = c = chirality(players, bodies)
    if c["up_median"] <= UP_MIN or c["forward_mean"] <= FORWARD_MIN or c["left_frac"] < LEFT_MIN:
        row["failures"].append(f"chirality: {c}")

    minutes = row["duration_s"] / 60.0
    budget_s = BUDGET_S_BONES if row["bones_skipped"] else BUDGET_S_PER_10MIN * max(minutes, 1.0) / 10.0
    row["budget_s"] = budget_s
    if row["seconds_median"] > budget_s * TOLERANCE:
        row["failures"].append(f"perf: {row['seconds_median']:.2f} s over {budget_s:.2f} s")
    if row["peak_private_mb"] > BUDGET_MB * TOLERANCE:
        row["failures"].append(f"memory: {row['peak_private_mb']:.0f} MB over {BUDGET_MB:.0f} MB")
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--echodemo", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--json")
    ap.add_argument("--perf-runs", type=int, default=5)
    ap.add_argument("--perf-file", action="append", default=[],
                    help="stem timed --perf-runs times; the rest are converted once")
    ap.add_argument("sources", nargs="+")
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    rows = []
    for src in args.sources:
        stem = os.path.splitext(os.path.basename(src))[0]
        runs = args.perf_runs if stem in args.perf_file else 1
        row = gate_one(args.echodemo, src, args.out_dir, runs)
        rows.append(row)
        c = row.get("chirality", {})
        print(f"{row['file']:44s} {'FAIL' if row['failures'] else 'ok  '} "
              f"{row.get('duration_s', 0):7.1f}s p={row.get('players', 0):2d} g={row.get('goals', 0):2d} "
              f"t={row.get('seconds_median', 0):5.2f}s mem={row.get('peak_private_mb', 0):5.0f}MB "
              f"out={row.get('output_bytes', 0) / 1e6:6.1f}MB up={c.get('up_median', 0):.3f} "
              f"fwd={c.get('forward_mean', 0):.3f} left={c.get('left_frac', 0):.3f} {'; '.join(row['failures'])}",
              flush=True)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)
    return 1 if any(r["failures"] for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
