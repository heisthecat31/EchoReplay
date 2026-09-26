"""P7 (DESIGN-FORMATS §9.2, T1): score the converter's event inference against native ground truth.

Each native capture is decoded through `echodemo rows`, reduced to what the Spark API would have shown
(poses, velocity, holding_*, stunned spans, cumulative stats, score), written as a plain-text
.echoreplay at 30 Hz and at 15 Hz, and run through the real reader, Normalise and Infer
(`import_inferprobe`, every rule on). What the reduction deleted (bits28, triggers, the stun puncher)
is the truth the output is scored against.

Kinematic thresholds are tuned on capture A only; capture B is held out and only scored. A gated rule
is enabled when B meets precision >= 0.85 and recall >= 0.50 at both rates.

    python tools/import/validate_infer.py --echodemo build/bin/echodemo.exe --probe build/bin/import_inferprobe.exe
        --truth-dir <FORMATS-NATIVE-TRUTH> --work <scratch dir> --out <json> A.echodemo B.echodemo
"""

import argparse
import itertools
import json
import os
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timedelta

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_native  # noqa: E402

RATES = (30, 15)
MATCH_S = 0.100          # §9.2: an event matches within +-100 ms, a span also by IoU >= 0.3
IOU_MIN = 0.3
GOAL_MATCH_S = 0.5       # the score netvar trails PostScoreUpdate; see the report
DISC_IN_HAND_M = 0.35
PRECISION_GATE, RECALL_GATE = 0.85, 0.50
USER_BASE = 1000

# Per-rule truth: which native bits, which probe column.
KINEMATIC = {
    "I-7 boost": {"truth": ["boost"], "bit": [0x100]},
    "I-8 brake": {"truth": ["brake"], "bit": [0x200]},
    "I-9 thruster L": {"truth": ["thrustA"], "bit": [0x400]},
    "I-9 thruster R": {"truth": ["thrustB"], "bit": [0x800]},
    "I-10 punch": {"truth": ["punchL", "punchR"], "bit": [0x2, 0x4]},
    "I-2 moving attached": {"truth": ["movingAttached"], "bit": [0x80]},
}
DIRECT_BITS = {
    "I-1 geo attach": {"truth": ["attachA", "attachB"], "bit": [0x20, 0x40]},
}

GRID = {
    "boost": {"boostMinGainMps": [1.0, 1.5, 2.0, 2.5, 3.0], "boostMinPeakAccel": [6, 10, 14, 18],
              "boostWindowS": [0.1, 0.15, 0.2]},
    "brake": {"brakeMaxAccel": [-4, -6, -8, -10], "brakeMinFrames": [1, 2, 3], "brakeMaxSpeedRatio": [0.5, 0.6, 0.7, 0.8]},
    "thrust": {"thrustMinAccel": [0.5, 0.8, 1.2], "thrustMaxAccel": [4, 6], "thrustMinFrames": [3, 5],
               "thrustMinCos": [0.7, 0.8, 0.9]},
    "punch": {"punchMinHandSpeed": [3, 4, 5, 6, 8], "punchMinFrames": [1, 2, 3]},
    "moving": {"movingAttachedMinSpeed": [0.001, 0.5, 1, 2, 3, 4, 5, 6, 8, 10]},
}
RULE_GROUP = {"I-7 boost": "boost", "I-8 brake": "brake", "I-9 thruster L": "thrust", "I-9 thruster R": "thrust",
              "I-10 punch": "punch", "I-2 moving attached": "moving"}
DEFAULTS = {"boostMinGainMps": 1.5, "boostMinPeakAccel": 10, "boostWindowS": 0.15, "brakeMaxAccel": -6,
            "brakeMinFrames": 2, "brakeMaxSpeedRatio": 0.6, "thrustMinAccel": 0.8, "thrustMaxAccel": 4,
            "thrustMinFrames": 5, "thrustMinCos": 0.7, "punchMinHandSpeed": 5, "punchMinFrames": 2,
            "movingAttachedMinSpeed": 0.001}


def quat_axes(q):
    R = measure_native.quat_to_mat(q)
    return R[:, :, 0], R[:, :, 1], R[:, :, 2]  # x (left), y (up), z (forward)


def r3(v):
    return [round(float(x), 4) for x in v]


def span_cover(spans, t):
    return any(s <= t <= e for s, e in spans)


def reduce_capture(exe, capture, truth, rate, path, seed):
    """Writes the API-like file; returns the per-slot sampled clocks (capture seconds) it wrote."""
    cap = measure_native.load_native(exe, capture)
    j = cap["joined"]
    disc = cap["disc"]
    team = {r["slot"]: r["team"] for r in truth["roster"]}
    slots = sorted({int(s) for s in j["slot"]})
    per = {}
    for s in slots:
        m = j["slot"] == s
        t = j["tUs"][m] / 1e6
        o = np.argsort(t, kind="stable")
        rows = {k: v[m][o] for k, v in j.items() if isinstance(v, np.ndarray) and len(v) == len(m)}
        bq = np.stack([rows[c] for c in ("b_oriX", "b_oriY", "b_oriZ", "b_oriW")], axis=1)
        hq = np.stack([rows[c] for c in ("hmdX", "hmdY", "hmdZ", "hmdW")], axis=1)
        lq = np.stack([rows[c] for c in ("lqX", "lqY", "lqZ", "lqW")], axis=1)
        rq = np.stack([rows[c] for c in ("rqX", "rqY", "rqZ", "rqW")], axis=1)
        per[s] = {"t": t[o], "rows": rows, "head": quat_axes(bq), "body": quat_axes(hq), "l": quat_axes(lq),
                  "r": quat_axes(rq)}
    dt = disc["tUs"] / 1e6
    dorder = np.argsort(dt)
    dt, dpos = dt[dorder], np.stack([disc[c][dorder] for c in ("comX", "comY", "comZ")], axis=1)
    owner_spans = [(s["start"] / 1e6, s["end"] / 1e6, s["slot"]) for s in truth["disc_owner_spans"]]
    owner_start = np.array([o[0] for o in owner_spans])
    stun_spans = defaultdict(list)
    for st in truth["stuns"]:
        stun_spans[st["victim"]].append((st["tUs"] / 1e6, st["tUs"] / 1e6 + st["duration"]))
    holds = {(int(k.split(":")[1]), side): [(h["start"] / 1e6, h["end"] / 1e6, h["target"]) for h in v[f"hold{side}"]]
             for k, v in truth["players"].items() if k.startswith("wire:") for side in ("L", "R")}
    stat_events = sorted((u["tUs"] / 1e6, u["entrant"], u["add"]) for u in truth["stat_updates"])
    gs = sorted((g["tUs"] / 1e6, g["bluePoints"], g["redPoints"]) for g in truth["game_state"])

    t0 = min(p["t"][0] for p in per.values())
    t1 = max(p["t"][-1] for p in per.values())
    rng = np.random.default_rng(seed)
    base = datetime(2026, 1, 1)
    stats = defaultdict(lambda: defaultdict(float))
    si = gi = 0
    blue = orange = 0
    k = 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        while True:
            t = t0 + k / rate + rng.uniform(-0.001, 0.001)
            k += 1
            if t > t1:
                break
            while si < len(stat_events) and stat_events[si][0] <= t:
                for name, val in stat_events[si][2].items():
                    stats[stat_events[si][1]][name] += val
                si += 1
            while gi < len(gs) and gs[gi][0] <= t:
                blue, orange = gs[gi][1], gs[gi][2]
                gi += 1
            di = max(0, np.searchsorted(dt, t, side="right") - 1)
            oi = np.searchsorted(owner_start, t, side="right") - 1
            owner = owner_spans[oi][2] if oi >= 0 and owner_spans[oi][0] <= t <= owner_spans[oi][1] + 0.05 else None
            teams = [[], []]
            for s, p in per.items():
                i = np.searchsorted(p["t"], t, side="right") - 1
                if i < 0 or t - p["t"][i] > 0.1 or team.get(s, 2) > 1:
                    continue
                r = p["rows"]
                com = np.array([r["b_comX"][i], r["b_comY"][i], r["b_comZ"][i]])
                loff = np.array([r["lOffX"][i], r["lOffY"][i], r["lOffZ"][i]])
                roff = np.array([r["rOffX"][i], r["rOffY"][i], r["rOffZ"][i]])
                bits = int(r["bits28"][i])

                # A held player sets the same arm bit as a geo grab, so the hold is checked first.
                def holding(side, off, bit):
                    for a, b, target in holds.get((s, side), []):
                        if a <= t <= b:
                            return str(target)
                    if bits & bit:
                        return "geo"
                    if owner == s and np.linalg.norm(dpos[di] - (com + off)) < DISC_IN_HAND_M:
                        return "disc"
                    return ""

                def basis(axes, i):
                    x, y, z = axes
                    return {"forward": r3(z[i]), "up": r3(y[i]), "left": r3(x[i])}

                st = stats[s]
                teams[team[s]].append({
                    "name": f"p{s}", "userid": USER_BASE + s, "playerid": s, "number": s, "level": 1,
                    "head": {"position": r3(com), **basis(p["head"], i)},
                    "body": {"position": r3(com), **basis(p["body"], i)},
                    "lhand": {"pos": r3(com + loff), **basis(p["l"], i)},
                    "rhand": {"pos": r3(com + roff), **basis(p["r"], i)},
                    "velocity": r3([r["b_velX"][i], r["b_velY"][i], r["b_velZ"][i]]),
                    "holding_left": holding("L", loff, 0x20), "holding_right": holding("R", roff, 0x40),
                    "stunned": span_cover(stun_spans.get(s, []), t),
                    "stats": {"points": int(st["Points"]), "goals": int(st["Goals"]), "stuns": int(st["Stuns"]),
                              "saves": int(st["Saves"]), "passes": int(st["Passes"]), "catches": int(st["Catches"]),
                              "steals": int(st["Steals"]), "blocks": int(st["Blocks"]),
                              "interceptions": int(st["Interceptions"]), "assists": int(st["Assists"]),
                              "shots_taken": int(st["ShotsOnGoal"]), "possession_time": round(st["PossessionTime"], 3)},
                })
            frame = {"map_name": "mpl_arena_a", "sessionid": "T1", "private_match": True, "game_status": "playing",
                     "game_clock": round(t, 4), "blue_points": int(blue), "orange_points": int(orange),
                     "disc": {"position": r3(dpos[di]), "velocity": [0, 0, 0], "forward": [0, 0, 1], "up": [0, 1, 0],
                              "left": [1, 0, 0]},
                     "teams": [{"team": "BLUE TEAM", "players": teams[0]}, {"team": "ORANGE TEAM", "players": teams[1]}]}
            stamp = base + timedelta(seconds=t)
            f.write(stamp.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + "\t" + json.dumps(frame, separators=(",", ":")) + "\n")
    return t0, t1


def probe(exe, path, params):
    args = [exe, path]
    for k, v in params.items():
        args += ["--set", f"{k}={v}"]
    proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"probe: {proc.stderr.strip()}")
    samples = defaultdict(list)
    stuns, goals = [], []
    for line in proc.stdout.splitlines():
        f = line.split(",")
        if f[0] == "S":
            samples[int(f[2]) - USER_BASE].append((float(f[1]), int(f[3]), int(f[4]), int(f[5]), int(f[6]), int(f[7])))
        elif f[0] == "T":
            stuns.append((float(f[1]), int(f[2]) - USER_BASE, int(f[3]) - USER_BASE))
        elif f[0] == "G":
            goals.append((float(f[1]), int(f[2])))
    return samples, stuns, goals


def spans_of(times, on, max_step):
    out, start = [], None
    for i, (t, v) in enumerate(zip(times, on)):
        if v and start is None:
            start = t
        broken = i + 1 < len(times) and times[i + 1] - t > max_step
        if start is not None and (not v or broken or i + 1 == len(times)):
            out.append((start, times[i - 1] if not v else t))
            start = None
    return out


def match(pred, truth):
    """Greedy one-to-one per player: onset within MATCH_S, or IoU >= IOU_MIN. Returns (tp, errors)."""
    used = set()
    errors = []
    for slot, ps in pred.items():
        ts = truth.get(slot, [])
        for a, b in ps:
            best = None
            for j, (c, d) in enumerate(ts):
                if (slot, j) in used:
                    continue
                inter = max(0.0, min(b, d) - max(a, c))
                union = max(b, d) - min(a, c)
                iou = inter / union if union > 0 else (1.0 if a == c else 0.0)
                if abs(a - c) <= MATCH_S or iou >= IOU_MIN:
                    if best is None or abs(a - c) < abs(a - ts[best][0]):
                        best = j
            if best is not None:
                used.add((slot, best))
                errors.append(a - ts[best][0])
    return len(used), errors


def pr(npred, ntruth, tp, errors):
    return {"n_pred": npred, "n_truth": ntruth, "tp": tp,
            "precision": round(tp / npred, 3) if npred else None, "recall": round(tp / ntruth, 3) if ntruth else None,
            "onset_err_ms_p50": round(float(np.median(np.abs(errors))) * 1000, 1) if errors else None,
            "onset_err_ms_p90": round(float(np.percentile(np.abs(errors), 90)) * 1000, 1) if errors else None}


def truth_spans(truth, names, window):
    out = defaultdict(list)
    for key, p in truth["players"].items():
        if not key.startswith("wire:"):
            continue
        s = int(key.split(":")[1])
        lo, hi = window.get(s, (None, None))
        if lo is None:
            continue
        for n in names:
            out[s] += [(a / 1e6, b / 1e6) for a, b in p["bits"].get(n, []) if lo <= a / 1e6 <= hi]
    return out


def score(truth, samples, stuns, goals, rate):
    window = {s: (v[0][0], v[-1][0]) for s, v in samples.items() if v}
    step = 2.5 / rate
    res = {}
    for rule, spec in {**KINEMATIC, **DIRECT_BITS}.items():
        pred = defaultdict(list)
        for s, v in samples.items():
            t = [x[0] for x in v]
            for bit in spec["bit"]:
                pred[s] += spans_of(t, [(x[1] & bit) != 0 for x in v], step)
        tr = truth_spans(truth, spec["truth"], window)
        tp, err = match(pred, tr)
        res[rule] = pr(sum(map(len, pred.values())), sum(map(len, tr.values())), tp, err)
    for rule, col, field in (("I-3 held player", 2, "hold"), ("I-4 grip", 4, "trig")):
        pred, tr = defaultdict(list), defaultdict(list)
        for s, v in samples.items():
            t = [x[0] for x in v]
            for off in (0, 1):
                pred[s] += spans_of(t, [x[col + off] != 0 for x in v], step)
        for key, p in truth["players"].items():
            s = int(key.split(":")[1]) if key.startswith("wire:") else None
            if s not in window:
                continue
            lo, hi = window[s]
            for side in ("L", "R"):
                src = p[f"hold{side}"] if field == "hold" else [{"start": a, "end": b} for a, b in p[f"trig{side}_spans"]]
                tr[s] += [(h["start"] / 1e6, h["end"] / 1e6) for h in src if lo <= h["start"] / 1e6 <= hi]
        tp, err = match(pred, tr)
        res[rule] = pr(sum(map(len, pred.values())), sum(map(len, tr.values())), tp, err)
    lo = min(w[0] for w in window.values())
    hi = max(w[1] for w in window.values())
    tstun = defaultdict(list)
    for st in truth["stuns"]:
        if lo <= st["tUs"] / 1e6 <= hi:
            tstun[st["victim"]].append((st["tUs"] / 1e6, st["tUs"] / 1e6, st["puncher"]))
    pstun = defaultdict(list)
    for t, victim, puncher in stuns:
        pstun[victim].append((t, t, puncher))
    tp, err = match({k: [(a, b) for a, b, _ in v] for k, v in pstun.items()},
                    {k: [(a, b) for a, b, _ in v] for k, v in tstun.items()})
    res["I-6 stun"] = pr(len(stuns), sum(map(len, tstun.values())), tp, err)
    right = sum(1 for v, ps in pstun.items() for t, _, p in ps
                if any(abs(t - a) <= MATCH_S and p == q for a, _, q in tstun.get(v, [])))
    res["I-6 stun"]["puncher_correct"] = right
    tgoal = [(g["tUs"] / 1e6, g["team"]) for g in truth["goals"] if lo <= g["tUs"] / 1e6 <= hi]
    used, gerr = set(), []
    for t, team in goals:
        for j, (a, tt) in enumerate(tgoal):
            if j not in used and tt == team and abs(t - a) <= GOAL_MATCH_S:
                used.add(j)
                gerr.append(t - a)
                break
    res["goal"] = pr(len(goals), len(tgoal), len(used), gerr)
    return res


def gated_ok(r):
    return (r["precision"] or 0) >= PRECISION_GATE and (r["recall"] or 0) >= RECALL_GATE


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--echodemo", required=True)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--truth-dir", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("tune")
    ap.add_argument("heldout")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)

    caps = {}
    for label, path in (("A", args.tune), ("B", args.heldout)):
        stem = os.path.splitext(os.path.basename(path))[0]
        truth = json.load(open(os.path.join(args.truth_dir, stem + ".truth.json"), encoding="utf-8"))
        files = {}
        for rate in RATES:
            out = os.path.join(args.work, f"{label}_{rate}hz.echoreplay")
            if not os.path.exists(out):
                reduce_capture(args.echodemo, path, truth, rate, out, seed=rate)
            files[rate] = out
        caps[label] = {"stem": stem, "truth": truth, "files": files}
        print(f"reduced {label} {stem}", flush=True)

    # Tune on A: every group's grid, joint over both rates. Groups are independent bits, so one probe
    # run carries one combination of every group.
    combos = {g: [dict(zip(grid, vals)) for vals in itertools.product(*grid.values())] for g, grid in GRID.items()}
    runs = max(len(c) for c in combos.values())
    trials = defaultdict(list)
    for k in range(runs):
        params = {}
        for g, c in combos.items():
            params.update(c[k % len(c)])
        per_rate = {rate: score(caps["A"]["truth"], *probe(args.probe, caps["A"]["files"][rate], params), rate)
                    for rate in RATES}
        for rule, g in RULE_GROUP.items():
            trials[rule].append((combos[g][k % len(combos[g])], per_rate[30][rule], per_rate[15][rule]))
        print(f"tune {k + 1}/{runs}", flush=True)

    chosen = {}
    for g in GRID:
        rules = [r for r, gg in RULE_GROUP.items() if gg == g]
        best = None
        for idx in range(len(combos[g])):
            rows = [t for r in rules for t in trials[r] if t[0] == combos[g][idx]]
            precise = all((x[1]["precision"] or 0) >= PRECISION_GATE and (x[2]["precision"] or 0) >= PRECISION_GATE
                          for x in rows)
            recall = min(min(x[1]["recall"] or 0, x[2]["recall"] or 0) for x in rows)
            prec = min(min(x[1]["precision"] or 0, x[2]["precision"] or 0) for x in rows)
            key = (precise, recall if precise else prec)
            if best is None or key > best[0]:
                best = (key, combos[g][idx])
        chosen[g] = {"params": best[1], "tune_meets_precision": best[0][0]}

    final = dict(DEFAULTS)
    for g in chosen.values():
        final.update(g["params"])
    results = {}
    for label in ("A", "B"):
        results[label] = {}
        for rate in RATES:
            results[label][rate] = score(caps[label]["truth"], *probe(args.probe, caps[label]["files"][rate], final),
                                         rate)
            results[label][f"{rate}_defaults"] = score(
                caps[label]["truth"], *probe(args.probe, caps[label]["files"][rate], DEFAULTS), rate)
    verdict = {}
    for rule in KINEMATIC:
        verdict[rule] = all(gated_ok(results["B"][rate][rule]) for rate in RATES)
    json.dump({"tune": caps["A"]["stem"], "heldout": caps["B"]["stem"], "gate": {"precision": PRECISION_GATE,
               "recall": RECALL_GATE}, "match": {"onset_s": MATCH_S, "iou": IOU_MIN, "goal_s": GOAL_MATCH_S},
               "chosen": chosen, "thresholds": final, "enabled": verdict,
               "results": {lab: {str(k): v for k, v in r.items()} for lab, r in results.items()}},
              open(args.out, "w", encoding="utf-8"), indent=1)
    for label in ("A", "B"):
        for rate in RATES:
            for rule, r in results[label][rate].items():
                print(f"{label} {rate:2d}Hz {rule:22s} P={r['precision']} R={r['recall']} "
                      f"n={r['n_pred']}/{r['n_truth']} err50={r['onset_err_ms_p50']}")
    print("enabled:", verdict)


if __name__ == "__main__":
    main()
