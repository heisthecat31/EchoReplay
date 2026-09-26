"""Minimal PE reader: virtual address -> file bytes, for guards that must read the real binary."""

import struct


_CACHE = {}


def load(path):
    """A shared PeImage per (path, size, mtime).  Callers must treat it as read-only."""
    import os
    st = os.stat(path)
    key = (os.path.abspath(path), st.st_size, st.st_mtime_ns)
    img = _CACHE.get(key)
    if img is None:
        _CACHE.clear()          # a rebuilt binary must not leave the old one reachable
        img = _CACHE[key] = PeImage(path)
    return img


class PeImage:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:2] != b"MZ":
            raise ValueError("not a PE image: bad MZ signature")
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE image: bad PE signature")
        machine, nsections = struct.unpack_from("<HH", self.data, pe + 4)
        opt_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", self.data, opt)[0]
        if magic != 0x20B:
            raise ValueError("expected PE32+ image, got magic 0x%X" % magic)
        self.machine = machine
        self.image_base = struct.unpack_from("<Q", self.data, opt + 24)[0]
        self.size_of_image = struct.unpack_from("<I", self.data, opt + 56)[0]
        self.sections = []
        sec = opt + opt_size
        for i in range(nsections):
            off = sec + i * 40
            name = self.data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", self.data, off + 8)
            self.sections.append((name, vaddr, vsize, rawptr, rawsize))

    def rva_to_offset(self, rva):
        for name, vaddr, vsize, rawptr, rawsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                delta = rva - vaddr
                if delta >= rawsize:
                    return None  # inside virtual padding; no bytes on disk
                return rawptr + delta
        return None

    def read_rva(self, rva, n):
        off = self.rva_to_offset(rva)
        if off is None or off + n > len(self.data):
            return None
        return self.data[off:off + n]

    def read_va(self, va, n):
        if va < self.image_base:
            return None
        return self.read_rva(va - self.image_base, n)

    def section_of_rva(self, rva):
        for name, vaddr, vsize, rawptr, rawsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return name
        return None
