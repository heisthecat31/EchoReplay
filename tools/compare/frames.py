"""Reading a captured frame back as sRGB bytes, without resampling anything on the way in.

SC1 is specified at 2/255 per channel, so a reader that quantises, converts colour space or resamples
chroma destroys more signal than the gate is looking for. This module therefore reads only formats it
can return exactly, and refuses the rest by name instead of approximating them.

Two containers:
  .frame  raw back-buffer dump, 32-byte header, the canonical output of the in-process route (FR-C)
  .png    lossless, for hand-authored fixtures and for anything that arrives already encoded
"""

import struct

import numpy as np

RAW_MAGIC = b"ECHOFRM1"
RAW_HEADER = struct.Struct("<8sIIIIII")   # magic, width, height, dxgi_format, row_pitch, bpp, reserved
RAW_HEADER_SIZE = RAW_HEADER.size

# The only formats that can be returned exactly as 8-bit sRGB triples.
DXGI_R8G8B8A8_UNORM = 28
DXGI_R8G8B8A8_UNORM_SRGB = 29
DXGI_B8G8R8A8_UNORM = 87
DXGI_B8G8R8A8_UNORM_SRGB = 91

FORMAT_NAMES = {
    DXGI_R8G8B8A8_UNORM: "R8G8B8A8_UNORM",
    DXGI_R8G8B8A8_UNORM_SRGB: "R8G8B8A8_UNORM_SRGB",
    DXGI_B8G8R8A8_UNORM: "B8G8R8A8_UNORM",
    DXGI_B8G8R8A8_UNORM_SRGB: "B8G8R8A8_UNORM_SRGB",
}
_BGRA = (DXGI_B8G8R8A8_UNORM, DXGI_B8G8R8A8_UNORM_SRGB)


class FrameError(Exception):
    pass


def write_raw(path, rgb, dxgi_format=DXGI_R8G8B8A8_UNORM_SRGB):
    """Author a .frame. Used by fixtures and by the comparator's own mutation battery."""
    height, width, _ = rgb.shape
    pixels = np.zeros((height, width, 4), dtype=np.uint8)
    pixels[:, :, 3] = 255
    if dxgi_format in _BGRA:
        pixels[:, :, 0] = rgb[:, :, 2]
        pixels[:, :, 1] = rgb[:, :, 1]
        pixels[:, :, 2] = rgb[:, :, 0]
    else:
        pixels[:, :, :3] = rgb
    row_pitch = width * 4
    with open(path, "wb") as fh:
        fh.write(RAW_HEADER.pack(RAW_MAGIC, width, height, dxgi_format, row_pitch, 4, 0))
        fh.write(pixels.tobytes())


def _read_raw(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    if len(blob) < RAW_HEADER_SIZE:
        raise FrameError("%s: %d bytes, shorter than the frame header" % (path, len(blob)))
    magic, width, height, fmt, row_pitch, bpp, _ = RAW_HEADER.unpack_from(blob, 0)
    if magic != RAW_MAGIC:
        raise FrameError("%s: not a frame dump (magic %r)" % (path, magic))
    if fmt not in FORMAT_NAMES:
        raise FrameError("%s: DXGI format %d cannot be returned exactly as 8-bit sRGB; refusing to "
                         "approximate it, because a converted frame measures the converter" % (path, fmt))
    if bpp != 4:
        raise FrameError("%s: %d bytes per pixel, expected 4" % (path, bpp))
    if row_pitch < width * 4:
        raise FrameError("%s: row pitch %d is narrower than %d pixels" % (path, row_pitch, width))
    need = RAW_HEADER_SIZE + row_pitch * height
    if len(blob) < need:
        raise FrameError("%s: truncated, %d bytes of %d" % (path, len(blob), need))

    rows = np.frombuffer(blob, dtype=np.uint8, count=row_pitch * height,
                         offset=RAW_HEADER_SIZE).reshape(height, row_pitch)
    pixels = rows[:, :width * 4].reshape(height, width, 4)
    if fmt in _BGRA:
        return np.ascontiguousarray(pixels[:, :, 2::-1])
    return np.ascontiguousarray(pixels[:, :, :3])


def _read_png(path):
    from PIL import Image
    with Image.open(path) as image:
        if image.mode not in ("RGB", "RGBA", "L"):
            raise FrameError("%s: PIL mode %s is not an sRGB image" % (path, image.mode))
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def read(path):
    """Return the frame as an (h, w, 3) uint8 sRGB array."""
    lowered = str(path).lower()
    if lowered.endswith(".png"):
        return _read_png(path)
    if lowered.endswith(".frame"):
        return _read_raw(path)
    raise FrameError("%s: unknown frame container; expected .frame or .png" % path)
