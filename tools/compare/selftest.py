"""The comparator's own mutation battery: a planted defect per row, each of which must be caught.

Design section 12.0: a gate with no demonstrated failure is not evidence. So every rule in compare.py
is exercised here against a fixture built to trip it, and row 1 is the negative control - two identical
passes must come out PASS, or the comparator is simply always red and proves nothing either way.

Run directly, or through tests\\harnesscheck, which is what the gate discovers.
"""

import os
import shutil
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import compare                     # noqa: E402
import frames as frameio           # noqa: E402
import sidecar as sidecario        # noqa: E402

# Big enough that a 400 px2 region is still under 0.5 percent of the frame, so the region rule
# and the percentage rule can each be tripped without the other.
WIDTH, HEIGHT = 512, 384
CAMERA = "0123456789abcdef"
CLEAN_FLOOR = {"pct_outside": 0.0, "max_region_px": 0, "frames": 2, "a": "-", "b": "-"}

failures = []
checked = 0


def base_image(seed=0):
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    image[:, :, 0] = ((x * 4 + seed) % 256).astype(np.uint8)
    image[:, :, 1] = ((y * 5 + seed) % 256).astype(np.uint8)
    image[:, :, 2] = (((x + y) * 3 + seed) % 256).astype(np.uint8)
    image[8:20, 8:20] = 200
    return image


def frame_record(index, camera=CAMERA, rate=1.0, partial=0, since_seek=99, image=None):
    return {
        "frame_index": index, "sim_time_us": 1000 * index, "source_time_us": 16666 * index,
        "camera_hash": camera, "rate": rate, "steps_whole": 1, "steps_partial": partial,
        "frames_since_seek": since_seek, "image": image or ("f%03d.frame" % index),
    }


def header(which, width=WIDTH, height=HEIGHT, fmt="R8G8B8A8_UNORM_SRGB", space="srgb"):
    return {"sidecar": 1, "pass": which, "source": "selftest", "capture_route": "FR-C",
            "colour_space": space, "frame_format": fmt, "width": width, "height": height}


def write_pass(directory, which, images, records, **head):
    os.makedirs(directory, exist_ok=True)
    for record, image in zip(records, images):
        frameio.write_raw(os.path.join(directory, record["image"]), image)
    path = os.path.join(directory, "%s.sidecar.jsonl" % which)
    sidecario.write(path, header(which, **head), records)
    return sidecario.read(path)


def case(name, expected, actual, detail=""):
    global checked
    checked += 1
    if actual == expected:
        print("ok   %-52s %s" % (name, actual))
        return True
    failures.append("%s: expected %s, got %s %s" % (name, expected, actual, detail))
    print("FAIL %-52s expected %s, got %s %s" % (name, expected, actual, detail))
    return False


def verdict(root, ref_images, ref_records, rep_images, rep_records,
            regions=(), floor=CLEAN_FLOOR, ref_head=None, rep_head=None):
    """Build a two-pass fixture and return (verdict, report)."""
    shutil.rmtree(root, ignore_errors=True)
    reference = write_pass(os.path.join(root, "ref"), "reference", ref_images, ref_records,
                           **(ref_head or {}))
    replay = write_pass(os.path.join(root, "rep"), "replay", rep_images, rep_records, **(rep_head or {}))
    report = compare.run_comparison(reference, replay, list(regions), floor)
    return report["verdict"], report


def main():
    work = tempfile.mkdtemp(prefix="o5-selftest-")
    try:
        clean = [base_image(0), base_image(1)]
        records = [frame_record(0), frame_record(1)]

        # M11's shape: the negative control. Without it a gate that fails everything looks identical to
        # a gate that works.
        v, report = verdict(work, clean, records, [i.copy() for i in clean], list(records))
        case("identical passes are green", "PASS", v)
        case("identical passes have no differing pixels", 0,
             report["frames"][0]["pixels_outside_tolerance"])

        # Within tolerance: 2/255 is the gate's own step, so it must not trip.
        mutated = [np.clip(i.astype(np.int16) + 2, 0, 255).astype(np.uint8) for i in clean]
        v, _ = verdict(work, clean, records, mutated, list(records))
        case("a whole-frame shift of exactly 2/255 stays green", "PASS", v)

        mutated = [np.clip(i.astype(np.int16) + 3, 0, 255).astype(np.uint8) for i in clean]
        v, report = verdict(work, clean, records, mutated, list(records))
        case("a whole-frame shift of 3/255 goes red", "FAIL", v,
             "%.2f%% within" % report["frames"][0]["pct_within"])

        # The two region calibrators: 100 px2 passes, 400 px2 fails, at a pixel count far below 0.5%.
        small = [i.copy() for i in clean]
        for image in small:
            image[30:40, 30:40] = 0
        v, report = verdict(work, clean, records, small, list(records))
        case("a 100 px2 region is inside the region gate", "PASS", v,
             "region %d px2" % report["frames"][0]["largest_differing_region_px"])
        case("a 100 px2 region is measured as 100 px2", 100,
             report["frames"][0]["largest_differing_region_px"])

        big = [i.copy() for i in clean]
        for image in big:
            image[20:40, 20:40] = 0
        v, report = verdict(work, clean, records, big, list(records))
        case("a 400 px2 region trips the region gate", "FAIL", v,
             "%.2f%% within, region %d px2" % (report["frames"][0]["pct_within"],
                                               report["frames"][0]["largest_differing_region_px"]))
        # ...and it must be the region rule that tripped, not the percentage rule wearing its clothes.
        case("and it does so with the percentage rule still satisfied", True,
             report["frames"][0]["pct_within"] >= compare.MIN_PCT_WITHIN)

        # Camera poses that do not match: comparing those images measures nothing.
        moved = [frame_record(0, camera="fedcba9876543210"), frame_record(1)]
        v, report = verdict(work, clean, records, [i.copy() for i in clean], moved)
        case("a camera_hash mismatch is NOT-RUN, not FAIL", "NOT-RUN", v)

        # r must be 1 and every step whole, or the original dt sequence was not delivered.
        v, _ = verdict(work, clean, records, [i.copy() for i in clean],
                       [frame_record(0, rate=0.5), frame_record(1)])
        case("a rate other than 1.0 is NOT-RUN", "NOT-RUN", v)
        v, _ = verdict(work, clean, records, [i.copy() for i in clean],
                       [frame_record(0, partial=1), frame_record(1)])
        case("a partial accumulator step is NOT-RUN", "NOT-RUN", v)

        # No shared source_time_us at all.
        shifted = [dict(r, source_time_us=r["source_time_us"] + 7) for r in records]
        v, report = verdict(work, clean, records, [i.copy() for i in clean], shifted)
        case("passes that share no source_time_us are NOT-RUN", "NOT-RUN", v)
        case("and the nearest-frame distance is reported", True,
             any("us away" in p for p in report["invalid"]))

        # Masking: unpaired masks are refused, paired masks are honoured.
        unpaired = [{"name": "fps", "rect": [0, 0, 12, 6]}]
        v, _ = verdict(work, clean, records, [i.copy() for i in clean], list(records), regions=unpaired)
        case("a mask with no numeric substitute is NOT-RUN", "NOT-RUN", v)

        paired = [{"name": "fps", "rect": [0, 0, 24, 24], "substitute": "O3 /session fps"}]
        masked = [i.copy() for i in clean]
        for image in masked:
            image[0:24, 0:24] = 255
        v, report = verdict(work, clean, records, masked, list(records), regions=paired)
        case("a paired mask removes its region from the comparison", "PASS", v)
        case("and the unmasked pixel count drops by the mask area",
             WIDTH * HEIGHT - 24 * 24, report["frames"][0]["unmasked_pixels"])

        # Geometry and colour space.
        v, _ = verdict(work, clean, records, [i.copy() for i in clean], list(records),
                       rep_head={"space": "linear"})
        case("a colour-space difference between passes is NOT-RUN", "NOT-RUN", v)

        # The noise floor is the validity guard for the whole gate.
        v, _ = verdict(work, clean, records, [i.copy() for i in clean], list(records), floor=None)
        case("no noise floor means the verdict is not admissible", "NOT-RUN", v)
        v, _ = verdict(work, clean, records, [i.copy() for i in clean], list(records),
                       floor={"pct_outside": 0.4, "max_region_px": 0, "frames": 2})
        case("a noise floor within 3x of SC1 is NOT-RUN", "NOT-RUN", v)
        v, _ = verdict(work, clean, records, [i.copy() for i in clean], list(records),
                       floor={"pct_outside": 0.0, "max_region_px": 100, "frames": 2})
        case("a noise floor region within 3x of 256 px2 is NOT-RUN", "NOT-RUN", v)

        # The post-seek window reports rather than votes, and stops doing so at the 5th frame.
        broken = [i.copy() for i in clean]
        for image in broken:
            image[:, :] = 0
        v, report = verdict(work, clean,
                            [frame_record(0, since_seek=0), frame_record(1, since_seek=1)],
                            broken,
                            [frame_record(0, since_seek=0), frame_record(1, since_seek=1)])
        case("frames 1-4 after a seek do not vote", "NOT-RUN", v)
        case("and they are reported as observations", 2, report["observations"])
        v, _ = verdict(work, clean,
                       [frame_record(0, since_seek=4), frame_record(1, since_seek=5)],
                       broken,
                       [frame_record(0, since_seek=4), frame_record(1, since_seek=5)])
        case("the 5th frame after a seek does vote", "FAIL", v)

        # The measured floor comes out of the same code path as the gate.
        shutil.rmtree(work, ignore_errors=True)
        a = write_pass(os.path.join(work, "a"), "reference", clean, records)
        jittered = [i.copy() for i in clean]
        jittered[0][5, 5] = (0, 0, 0)
        b = write_pass(os.path.join(work, "b"), "replay", jittered, list(records))
        _, floor = compare.measure_noise_floor(a, b, [])
        case("the floor measures one differing pixel out of the frame",
             round(100.0 / (WIDTH * HEIGHT), 6), round(floor["pct_outside"], 6))
        case("and its largest region", 1, floor["max_region_px"])

        # Two independent implementations of the region measurement must agree, or the number is one
        # library's opinion rather than a measurement.
        rng = np.random.default_rng(20260827)
        blob = rng.random((HEIGHT, WIDTH)) < 0.35
        case("scipy and the fallback measure the same largest region",
             compare._largest_region_unionfind(blob), compare.largest_region(blob))
        case("both agree on an empty mask", 0, compare.largest_region(np.zeros((4, 4), dtype=bool)))

        # The sidecar reader is the only thing standing between a malformed pass and a wrong verdict.
        for name, text in [
            ("a bad camera_hash", '{"sidecar":1,"pass":"replay","source":"s","capture_route":"FR-C",'
                                  '"colour_space":"srgb","frame_format":"R8G8B8A8_UNORM_SRGB",'
                                  '"width":4,"height":4}\n'
                                  '{"frame_index":0,"sim_time_us":0,"source_time_us":0,'
                                  '"camera_hash":"XYZ","rate":1.0,"steps_whole":1,"steps_partial":0,'
                                  '"frames_since_seek":9,"image":"a.frame"}'),
            ("a repeated frame_index", '{"sidecar":1,"pass":"replay","source":"s","capture_route":"FR-C",'
                                       '"colour_space":"srgb","frame_format":"R8G8B8A8_UNORM_SRGB",'
                                       '"width":4,"height":4}\n'
                                       '{"frame_index":0,"sim_time_us":0,"source_time_us":0,'
                                       '"camera_hash":"0000000000000000","rate":1.0,"steps_whole":1,'
                                       '"steps_partial":0,"frames_since_seek":9,"image":"a.frame"}\n'
                                       '{"frame_index":0,"sim_time_us":1,"source_time_us":1,'
                                       '"camera_hash":"0000000000000000","rate":1.0,"steps_whole":1,'
                                       '"steps_partial":0,"frames_since_seek":9,"image":"b.frame"}'),
            ("a future sidecar version", '{"sidecar":2,"pass":"replay","source":"s",'
                                         '"capture_route":"FR-C","colour_space":"srgb",'
                                         '"frame_format":"R8G8B8A8_UNORM_SRGB","width":4,"height":4}\n'
                                         '{"frame_index":0,"sim_time_us":0,"source_time_us":0,'
                                         '"camera_hash":"0000000000000000","rate":1.0,"steps_whole":1,'
                                         '"steps_partial":0,"frames_since_seek":9,"image":"a.frame"}'),
            ("a header with no frames", '{"sidecar":1,"pass":"replay","source":"s",'
                                        '"capture_route":"FR-C","colour_space":"srgb",'
                                        '"frame_format":"R8G8B8A8_UNORM_SRGB","width":4,"height":4}'),
        ]:
            rejected = False
            try:
                sidecario.parse(text, name)
            except sidecario.SidecarError:
                rejected = True
            case("the sidecar reader rejects %s" % name, True, rejected)

        good = ('{"sidecar":1,"pass":"replay","source":"s","capture_route":"FR-C","colour_space":"srgb",'
                '"frame_format":"R8G8B8A8_UNORM_SRGB","width":4,"height":4}\n'
                '{"frame_index":0,"sim_time_us":0,"source_time_us":0,"camera_hash":"0000000000000000",'
                '"rate":1.0,"steps_whole":1,"steps_partial":0,"frames_since_seek":9,"image":"a.frame"}')
        case("and accepts a well-formed one", 1, len(sidecario.parse(good, "good").frames))

        # The frame reader must not quietly approximate a format it cannot return exactly.
        rgba = os.path.join(work, "rgba.frame")
        bgra = os.path.join(work, "bgra.frame")
        frameio.write_raw(rgba, clean[0], frameio.DXGI_R8G8B8A8_UNORM_SRGB)
        frameio.write_raw(bgra, clean[0], frameio.DXGI_B8G8R8A8_UNORM_SRGB)
        case("RGBA and BGRA dumps read back identically", True,
             bool(np.array_equal(frameio.read(rgba), frameio.read(bgra))))
        case("and both equal what was written", True,
             bool(np.array_equal(frameio.read(rgba), clean[0])))

        with open(rgba, "r+b") as fh:
            fh.seek(16)
            fh.write((24).to_bytes(4, "little"))     # R10G10B10A2_UNORM
        refused = False
        try:
            frameio.read(rgba)
        except frameio.FrameError:
            refused = True
        case("a 10-bit surface is refused rather than quantised", True, refused)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("SELFTEST checked=%d failures=%d subject=o5-comparator-mutations" % (checked, len(failures)))
    for problem in failures:
        print("  %s" % problem)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
