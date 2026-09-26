"""The per-frame sidecar: the only channel that carries a captured frame's identity.

Design section 12.5.2 requires both passes to emit a per-frame record and frames to be matched by
source_time_us, never by wall clock, and section 12.5.3 forbids burning a marker into the image. So the
sidecar is what makes a frame identifiable at all, and a capture route that cannot produce one honestly
cannot serve the image oracle.

One file per pass, UTF-8, one JSON object per line. Line 1 is the header; every later line is a frame.
"""

import json

SIDECAR_VERSION = 1

HEADER_FIELDS = {
    "sidecar": int,          # format version
    "pass": str,             # "reference" (the live match) or "replay"
    "source": str,           # what was played: demo file name, or live session id
    "capture_route": str,    # FR-A / FR-B / FR-C, so a route change is visible in the artifact
    "colour_space": str,     # "srgb"
    "frame_format": str,     # DXGI format name of the captured surface
    "width": int,
    "height": int,
}

FRAME_FIELDS = {
    "frame_index": int,        # 0-based, strictly increasing within a pass
    "sim_time_us": int,        # the engine's simulation clock
    "source_time_us": int,     # the recorded source timeline: THE matching key
    "camera_hash": str,        # 16 lowercase hex of the camera pose+fov actually used
    "rate": float,             # commanded transport rate at this frame; O5 requires 1.0
    "steps_whole": int,        # whole recorded ticks consumed by this frame
    "steps_partial": int,      # partial steps; O5 requires 0
    "frames_since_seek": int,  # 0 on the first frame after a seek; 0..3 are observations only
    "image": str,              # frame file, relative to the sidecar
}

_CAMERA_HASH_DIGITS = set("0123456789abcdef")


class SidecarError(Exception):
    pass


class Sidecar:
    def __init__(self, header, frames, path):
        self.header = header
        self.frames = frames
        self.path = path

    @property
    def geometry(self):
        return (self.header["width"], self.header["height"],
                self.header["frame_format"], self.header["colour_space"])

    def by_source_time(self):
        index = {}
        for f in self.frames:
            index.setdefault(f["source_time_us"], []).append(f)
        return index


def _check_fields(obj, spec, where):
    for name, kind in spec.items():
        if name not in obj:
            raise SidecarError("%s: missing field '%s'" % (where, name))
        value = obj[name]
        if kind is float:
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                raise SidecarError("%s: field '%s' is not a number" % (where, name))
        elif kind is int:
            if not isinstance(value, int) or isinstance(value, bool):
                raise SidecarError("%s: field '%s' is not an integer" % (where, name))
        elif not isinstance(value, kind):
            raise SidecarError("%s: field '%s' is not a %s" % (where, name, kind.__name__))


def parse(text, path="<sidecar>"):
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        raise SidecarError("%s: empty" % path)

    try:
        header = json.loads(lines[0])
    except ValueError as exc:
        raise SidecarError("%s line 1: not JSON (%s)" % (path, exc))
    if not isinstance(header, dict):
        raise SidecarError("%s line 1: header is not an object" % path)
    _check_fields(header, HEADER_FIELDS, "%s line 1" % path)
    if header["sidecar"] != SIDECAR_VERSION:
        raise SidecarError("%s line 1: sidecar version %r, this reader speaks %d"
                           % (path, header["sidecar"], SIDECAR_VERSION))
    if header["pass"] not in ("reference", "replay"):
        raise SidecarError("%s line 1: pass '%s' is not 'reference' or 'replay'" % (path, header["pass"]))
    if header["width"] <= 0 or header["height"] <= 0:
        raise SidecarError("%s line 1: width/height must be positive" % path)

    frames = []
    previous_index = None
    for n, line in enumerate(lines[1:], start=2):
        where = "%s line %d" % (path, n)
        try:
            frame = json.loads(line)
        except ValueError as exc:
            raise SidecarError("%s: not JSON (%s)" % (where, exc))
        if not isinstance(frame, dict):
            raise SidecarError("%s: frame record is not an object" % where)
        _check_fields(frame, FRAME_FIELDS, where)

        h = frame["camera_hash"]
        if len(h) != 16 or not set(h) <= _CAMERA_HASH_DIGITS:
            raise SidecarError("%s: camera_hash '%s' is not 16 lowercase hex digits" % (where, h))
        if frame["steps_partial"] < 0 or frame["steps_whole"] < 0 or frame["frames_since_seek"] < 0:
            raise SidecarError("%s: step and seek counters cannot be negative" % where)
        if previous_index is not None and frame["frame_index"] <= previous_index:
            raise SidecarError("%s: frame_index %d does not follow %d; a pass with a repeated or "
                               "reordered index cannot be matched to anything"
                               % (where, frame["frame_index"], previous_index))
        previous_index = frame["frame_index"]
        frames.append(frame)

    if not frames:
        raise SidecarError("%s: header only, no frames" % path)
    return Sidecar(header, frames, path)


def read(path):
    with open(path, "r", encoding="utf-8") as fh:
        return parse(fh.read(), path)


def write(path, header, frames):
    """Used by the harness's own tests and by any tool that needs to author a pass."""
    record = dict(header)
    record.setdefault("sidecar", SIDECAR_VERSION)
    _check_fields(record, HEADER_FIELDS, "header")
    out = [json.dumps(record, sort_keys=True)]
    for n, frame in enumerate(frames):
        _check_fields(frame, FRAME_FIELDS, "frame %d" % n)
        out.append(json.dumps(frame, sort_keys=True))
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")
