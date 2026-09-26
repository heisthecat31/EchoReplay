"""O5, the frame-image comparator (design section 12.5). Skeleton: metric, validity rules, verdict.

W3-D owns the acceptance run and the mutation battery. What lives here is the part every one of those
rows needs and none of them should reimplement: match the two passes by source_time_us, refuse to
compare anything that cannot be compared, and report the three numbers SC1 is written in.

  SC1   at least 99.5 percent of unmasked pixels within 2/255 on the sRGB triple,
        no contiguous differing region larger than 256 px squared,
        and the threshold must exceed the replay-vs-replay noise floor by at least 3x.

Every rule below that produces NOT-RUN is a rule about the run being unable to measure anything -
mismatched cameras, a rate other than 1, a partial accumulator step, an unpaired mask, a noise floor
too close to the gate. NOT-RUN is never folded into PASS.

  compare.py compare     --reference REF.jsonl --replay REP.jsonl [--masks M.json]
                         [--noise-floor NF.json] [--report OUT.json]
  compare.py noise-floor --a A.jsonl --b B.jsonl --report NF.json

Exit: 0 PASS, 1 FAIL, 2 NOT-RUN, 3 the arguments or the inputs were unusable.
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import frames as frameio          # noqa: E402
import sidecar as sidecario       # noqa: E402

WITHIN = 2                # per-channel tolerance, in 8-bit sRGB steps
MIN_PCT_WITHIN = 99.5     # SC1
MAX_REGION_PX = 256       # SC1
NOISE_MARGIN = 3.0        # SC1: the gate must exceed the measured noise floor by this factor
OBSERVATION_FRAMES_AFTER_SEEK = 4   # frames_since_seek 0..3 report, they do not vote

PASS, FAIL, NOT_RUN = "PASS", "FAIL", "NOT-RUN"


# --------------------------------------------------------------------------------------- metrics

def _largest_region_unionfind(mask):
    height, width = mask.shape
    parent, size = [], []

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra == rb:
            return
        if size[ra] < size[rb]:
            ra, rb = rb, ra
        parent[rb] = ra
        size[ra] += size[rb]

    previous = []
    for y in range(height):
        padded = np.concatenate(([0], mask[y].astype(np.int8), [0]))
        edges = np.diff(padded)
        starts = np.flatnonzero(edges == 1)
        ends = np.flatnonzero(edges == -1)
        current = []
        for s, e in zip(starts, ends):
            parent.append(len(parent))
            size.append(int(e - s))
            current.append((int(s), int(e), len(parent) - 1))
        i = j = 0
        while i < len(previous) and j < len(current):
            ps, pe, pid = previous[i]
            cs, ce, cid = current[j]
            if pe <= cs:
                i += 1
            elif ce <= ps:
                j += 1
            else:
                union(pid, cid)
                if pe < ce:
                    i += 1
                else:
                    j += 1
        previous = current
    best = 0
    for i in range(len(parent)):
        if find(i) == i and size[i] > best:
            best = size[i]
    return best


def largest_region(mask):
    """Largest 4-connected run of differing pixels, in pixels squared."""
    if not mask.any():
        return 0
    try:
        from scipy import ndimage
    except ImportError:
        return _largest_region_unionfind(mask)
    labels, count = ndimage.label(mask, structure=np.array([[0, 1, 0], [1, 1, 1], [0, 1, 0]]))
    if count == 0:
        return 0
    return int(np.bincount(labels.ravel())[1:].max())


def _luma(rgb):
    f = rgb.astype(np.float64)
    return 0.2126 * f[:, :, 0] + 0.7152 * f[:, :, 1] + 0.0722 * f[:, :, 2]


def _box_mean(a, window):
    padded = np.zeros((a.shape[0] + 1, a.shape[1] + 1), dtype=np.float64)
    padded[1:, 1:] = a.cumsum(axis=0).cumsum(axis=1)
    total = (padded[window:, window:] - padded[:-window, window:]
             - padded[window:, :-window] + padded[:-window, :-window])
    return total / float(window * window)


def ssim(a, b, window=8):
    """Mean SSIM on luma with a uniform window.

    The original paper uses an 11x11 gaussian; this is the box-window variant, which runs on cumulative
    sums with no convolution dependency and reads a few thousandths higher on textured content. SSIM is
    SC1's secondary number, never the gate, so the deviation is recorded rather than corrected for.
    """
    ya, yb = _luma(a), _luma(b)
    if min(ya.shape) < window:
        return None
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mu_a, mu_b = _box_mean(ya, window), _box_mean(yb, window)
    saa = _box_mean(ya * ya, window) - mu_a * mu_a
    sbb = _box_mean(yb * yb, window) - mu_b * mu_b
    sab = _box_mean(ya * yb, window) - mu_a * mu_b
    numerator = (2 * mu_a * mu_b + c1) * (2 * sab + c2)
    denominator = (mu_a ** 2 + mu_b ** 2 + c1) * (saa + sbb + c2)
    return float(np.mean(numerator / denominator))


def build_mask(shape, regions):
    """True where a pixel takes part in the comparison."""
    keep = np.ones(shape, dtype=bool)
    for region in regions:
        x, y, w, h = region["rect"]
        keep[max(0, y):y + h, max(0, x):x + w] = False
    return keep


def compare_frames(reference, replay, keep):
    if reference.shape != replay.shape:
        raise ValueError("frame geometry differs: %s vs %s" % (reference.shape, replay.shape))
    delta = np.abs(reference.astype(np.int16) - replay.astype(np.int16)).max(axis=2)
    differing = (delta > WITHIN) & keep
    counted = int(keep.sum())
    if counted == 0:
        raise ValueError("the masks cover every pixel; nothing would be compared")
    outside = int(differing.sum())
    return {
        "unmasked_pixels": counted,
        "pixels_outside_tolerance": outside,
        "pct_within": 100.0 * (counted - outside) / counted,
        "pct_outside": 100.0 * outside / counted,
        "max_abs_channel_delta": int(delta[keep].max()),
        "largest_differing_region_px": largest_region(differing),
        "ssim": ssim(reference, replay),
    }


# ------------------------------------------------------------------------------- validity rules

def check_masks(regions):
    """Section 12.5.4: nothing may be masked without a numeric substitute."""
    problems = []
    for n, region in enumerate(regions):
        name = region.get("name", "mask %d" % n)
        if "rect" not in region or len(region["rect"]) != 4:
            problems.append("%s: needs a rect of [x, y, w, h]" % name)
        if not region.get("substitute"):
            problems.append("%s: masked with no numeric substitute. Every mask must name the O3 field "
                            "set that carries its data instead; an unpaired mask is a design defect."
                            % name)
    return problems


def check_pass(side, label):
    """Section 12.5.2: r must be 1 and every step a whole recorded tick, or the run measures nothing."""
    problems = []
    bad_rate = [f["frame_index"] for f in side.frames if abs(float(f["rate"]) - 1.0) > 1e-9]
    if bad_rate:
        problems.append("%s: %d frame(s) ran at a rate other than 1.0 (first at index %d)"
                        % (label, len(bad_rate), bad_rate[0]))
    partial = [f["frame_index"] for f in side.frames if f["steps_partial"] != 0]
    if partial:
        problems.append("%s: %d frame(s) consumed a partial accumulator step (first at index %d); "
                        "the original dt sequence was not delivered" % (label, len(partial), partial[0]))
    return problems


def check_noise_floor(floor):
    """Section 12.5.5: SC1 must exceed the replay-vs-replay floor by 3x, or the verdict is NOT-RUN."""
    if floor is None:
        return ["no noise floor was supplied. Without the replay-vs-replay control the gate cannot be "
                "shown to sit above the system's own non-determinism, so its verdict is not admissible."]
    problems = []
    tolerated = 100.0 - MIN_PCT_WITHIN
    if tolerated < NOISE_MARGIN * floor["pct_outside"]:
        problems.append("noise floor %.4f%% of pixels outside tolerance; SC1 tolerates %.4f%%, which is "
                        "less than %gx the floor" % (floor["pct_outside"], tolerated, NOISE_MARGIN))
    if MAX_REGION_PX < NOISE_MARGIN * floor["max_region_px"]:
        problems.append("noise floor's largest differing region is %d px2; SC1 allows %d px2, which is "
                        "less than %gx the floor" % (floor["max_region_px"], MAX_REGION_PX, NOISE_MARGIN))
    return problems


# --------------------------------------------------------------------------------------- the run

def _resolve(sidecar_path, relative):
    return os.path.join(os.path.dirname(os.path.abspath(sidecar_path)), relative)


def match_frames(reference, replay):
    """Match by source_time_us, exactly. Section 12.5.2 forbids matching on wall clock, and a tolerance
    would silently pair frames from different instants, which is the error it exists to prevent."""
    replay_index = replay.by_source_time()
    pairs, unmatched = [], []
    for frame in reference.frames:
        candidates = replay_index.get(frame["source_time_us"])
        if not candidates:
            unmatched.append(frame["source_time_us"])
            continue
        pairs.append((frame, candidates[0]))
    return pairs, unmatched


def run_comparison(reference, replay, regions, floor, gate=True):
    report = {
        "reference": {"path": reference.path, "frames": len(reference.frames), "header": reference.header},
        "replay": {"path": replay.path, "frames": len(replay.frames), "header": replay.header},
        "thresholds": {"within": WITHIN, "min_pct_within": MIN_PCT_WITHIN,
                       "max_region_px": MAX_REGION_PX, "noise_margin": NOISE_MARGIN},
        "invalid": [], "frames": [], "verdict": NOT_RUN,
    }

    if reference.geometry != replay.geometry:
        report["invalid"].append("the two passes differ in geometry or colour space: %s vs %s. Comparing "
                                 "across colour spaces measures the conversion, not the replay."
                                 % (reference.geometry, replay.geometry))
    report["invalid"].extend(check_pass(reference, "reference"))
    report["invalid"].extend(check_pass(replay, "replay"))
    report["invalid"].extend(check_masks(regions))
    if gate:
        report["invalid"].extend(check_noise_floor(floor))
    if floor is not None:
        report["noise_floor"] = floor

    pairs, unmatched = match_frames(reference, replay)
    report["matched"] = len(pairs)
    report["unmatched_reference_frames"] = len(unmatched)
    if not pairs:
        nearest = None
        if reference.frames and replay.frames:
            times = np.array([f["source_time_us"] for f in replay.frames], dtype=np.int64)
            first = reference.frames[0]["source_time_us"]
            nearest = int(np.min(np.abs(times - first)))
        report["invalid"].append("no frame pair shares a source_time_us"
                                 + ("" if nearest is None else "; the nearest replay frame to the first "
                                    "reference frame is %d us away" % nearest))

    if report["invalid"]:
        report["verdict"] = NOT_RUN
        return report

    keep = None
    counted, failed, observed, camera_mismatch = 0, 0, 0, 0
    worst = None
    for ref_frame, rep_frame in pairs:
        row = {"source_time_us": ref_frame["source_time_us"],
               "reference_frame": ref_frame["frame_index"], "replay_frame": rep_frame["frame_index"]}
        if ref_frame["camera_hash"] != rep_frame["camera_hash"]:
            # Section 12.5.2: comparing images from different viewpoints measures nothing.
            row["verdict"] = NOT_RUN
            row["detail"] = ("camera_hash %s vs %s" % (ref_frame["camera_hash"], rep_frame["camera_hash"]))
            camera_mismatch += 1
            report["frames"].append(row)
            continue

        a = frameio.read(_resolve(reference.path, ref_frame["image"]))
        b = frameio.read(_resolve(replay.path, rep_frame["image"]))
        if keep is None or keep.shape != a.shape[:2]:
            keep = build_mask(a.shape[:2], regions)
        metric = compare_frames(a, b, keep)
        row.update(metric)

        after_seek = min(ref_frame["frames_since_seek"], rep_frame["frames_since_seek"])
        if after_seek < OBSERVATION_FRAMES_AFTER_SEEK:
            # Temporal effects legitimately differ for a few frames after a jump (section 12.5.4).
            row["verdict"] = "OBSERVATION"
            row["detail"] = "frame %d after a seek; reported, does not vote" % after_seek
            observed += 1
        else:
            ok = (metric["pct_within"] >= MIN_PCT_WITHIN
                  and metric["largest_differing_region_px"] <= MAX_REGION_PX)
            row["verdict"] = PASS if ok else FAIL
            counted += 1
            if not ok:
                failed += 1
            if worst is None or metric["pct_outside"] > worst["pct_outside"]:
                worst = dict(row)
        report["frames"].append(row)

    report["counted"] = counted
    report["failed"] = failed
    report["observations"] = observed
    report["camera_mismatches"] = camera_mismatch
    report["worst_frame"] = worst

    if camera_mismatch:
        report["invalid"].append("%d matched pair(s) had different camera poses; the comparison of those "
                                 "frames measures nothing" % camera_mismatch)
        report["verdict"] = NOT_RUN
    elif counted == 0:
        report["invalid"].append("every matched pair fell inside the post-seek observation window, so no "
                                 "frame voted")
        report["verdict"] = NOT_RUN
    else:
        report["verdict"] = FAIL if failed else PASS
    return report


def measure_noise_floor(a, b, regions):
    """Run the same replay twice and compare: the system's own non-determinism, measured with the same
    metric the gate uses, because a floor measured a different way would not bound the gate."""
    report = run_comparison(a, b, regions, floor=None, gate=False)
    if report["verdict"] == NOT_RUN:
        return report, None
    rows = [f for f in report["frames"] if "pct_outside" in f]
    floor = {
        "pct_outside": max(f["pct_outside"] for f in rows),
        "max_region_px": max(f["largest_differing_region_px"] for f in rows),
        "frames": len(rows),
        "a": a.path, "b": b.path,
    }
    return report, floor


def _load_regions(path):
    if not path:
        return []
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    return data["masks"] if isinstance(data, dict) else data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="mode")

    gate = sub.add_parser("compare", help="live-vs-replay, gated on SC1")
    gate.add_argument("--reference", required=True)
    gate.add_argument("--replay", required=True)
    gate.add_argument("--masks")
    gate.add_argument("--noise-floor", dest="noise_floor")
    gate.add_argument("--report")

    floor = sub.add_parser("noise-floor", help="replay-vs-replay, measures the validity floor")
    floor.add_argument("--a", required=True)
    floor.add_argument("--b", required=True)
    floor.add_argument("--masks")
    floor.add_argument("--report", required=True)

    args = parser.parse_args(argv)
    if not args.mode:
        parser.print_help()
        return 3

    try:
        regions = _load_regions(args.masks)
        if args.mode == "compare":
            reference = sidecario.read(args.reference)
            replay = sidecario.read(args.replay)
            measured = None
            if args.noise_floor:
                with open(args.noise_floor, "r", encoding="utf-8") as fh:
                    measured = json.load(fh)
            report = run_comparison(reference, replay, regions, measured)
        else:
            report, measured = measure_noise_floor(sidecario.read(args.a), sidecario.read(args.b), regions)
            if measured is not None:
                report["noise_floor"] = measured
    except (sidecario.SidecarError, frameio.FrameError, OSError, ValueError) as exc:
        print("COMPARE-RESULT verdict=NOT-RUN reason=%s" % type(exc).__name__)
        print(exc)
        return 3

    destination = getattr(args, "report", None)
    if destination:
        with open(destination, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(report, fh, indent=2, sort_keys=True)

    for problem in report["invalid"]:
        print("NOT-RUN: %s" % problem)
    if report.get("worst_frame"):
        w = report["worst_frame"]
        print("worst frame: source_time_us=%d %.4f%% within, largest region %d px2, ssim %.5f"
              % (w["source_time_us"], w["pct_within"], w["largest_differing_region_px"], w["ssim"]))
    if args.mode == "noise-floor" and report.get("noise_floor"):
        f = report["noise_floor"]
        print("noise floor over %d frame(s): %.4f%% outside tolerance, largest region %d px2"
              % (f["frames"], f["pct_outside"], f["max_region_px"]))
    print("COMPARE-RESULT verdict=%s matched=%d counted=%d failed=%d observations=%d"
          % (report["verdict"], report.get("matched", 0), report.get("counted", 0),
             report.get("failed", 0), report.get("observations", 0)))

    return {PASS: 0, FAIL: 1, NOT_RUN: 2}[report["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
