#!/usr/bin/env python3
"""Enumerate every networking import in the install, and say which ones the egress blocker covers.

Written because "the complete networking surface" had been asserted over 18 of echovr.exe's 37 WS2_32
imports, with WININET omitted entirely while BugSplat64.dll statically imports ten of its functions.
That is an IMPORT DIRECTORY READ - enumerable in a second - so "we cannot enumerate every path" never
applied to it.

Two outputs, and the second is the point:
  * the surface, read from the PE import directories rather than remembered;
  * a COVERAGE verdict per entry point, so the blocker's report can state what it does not cover instead
    of letting a reader infer that "no refusals" means "no egress".

Ordinal imports are resolved through ws2_32.dll's own export table, because echovr.exe imports 27 of its
37 by ordinal and a name-only reading simply would not see them. That also settles a question the hooks
rest on: an ordinal import and a named import resolve to the SAME address, so hooking by name through
GetProcAddress covers both forms.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests", "factcheck"))

import gen_facts as G
from pe import PeImage

BIN = os.path.dirname(G.ECHOVR_EXE)
TRACKED_OUT = os.path.join(HERE, "network_surface.tsv")

NET_DLLS = ("ws2_32.dll", "wininet.dll", "winhttp.dll", "mswsock.dll", "dnsapi.dll",
            "iphlpapi.dll", "urlmon.dll", "wsock32.dll")

# What the blocker hooks today. Written here rather than parsed out of egress.cpp: a table generated from
# the code it audits proves nothing, and these have to be two statements that agree.
HOOKED = {"connect", "WSAConnect", "sendto", "WSASendTo", "WSAIoctl", "send", "WSASend",
          "socket", "WSASocketW", "WSASocketA", "InternetConnectW", "InternetConnectA",
          "HttpSendRequestW", "HttpSendRequestA", "InternetOpenUrlW", "InternetOpenUrlA"}

# Entry points that cannot carry data off this machine. Naming them is what stops the coverage figure
# being padded with functions that never needed a hook.
NOT_EGRESS = {
    "accept", "listen", "bind", "closesocket", "shutdown", "select", "__WSAFDIsSet",
    "recv", "recvfrom", "WSARecv", "WSARecvFrom", "WSAGetOverlappedResult",
    "getsockname", "getpeername", "getsockopt", "setsockopt", "ioctlsocket",
    "htonl", "htons", "ntohl", "ntohs", "inet_addr", "inet_ntoa", "inet_ntop", "inet_pton",
    "WSAGetLastError", "WSASetLastError", "WSAStartup", "WSACleanup",
    "getprotobynumber", "getprotobyname", "getservbyname", "gethostname",
    "InternetGetConnectedState", "InternetAttemptConnect", "InternetCloseHandle",
    "InternetQueryOptionW", "InternetSetOptionW", "HttpQueryInfoW", "InternetReadFile",
    "HttpOpenRequestW", "InternetOpenW", "InternetCheckConnectionW",
    "if_nametoindex", "FreeMibTable", "GetIfEntry2", "GetIfTable2Ex",
}


def data_dir_base(pe):
    peoff = struct.unpack_from("<I", pe.data, 0x3C)[0]
    opt = peoff + 24
    magic = struct.unpack_from("<H", pe.data, opt)[0]
    return opt + (112 if magic == 0x20B else 96)


def export_ordinals(path):
    pe = PeImage(path)
    d = pe.data
    rva, _ = struct.unpack_from("<II", d, data_dir_base(pe))
    off = pe.rva_to_offset(rva)
    if off is None:
        return {}
    base = struct.unpack_from("<I", d, off + 16)[0]
    count = struct.unpack_from("<I", d, off + 24)[0]
    names_off = pe.rva_to_offset(struct.unpack_from("<I", d, off + 32)[0])
    ords_off = pe.rva_to_offset(struct.unpack_from("<I", d, off + 36)[0])
    out = {}
    for i in range(count):
        n = pe.rva_to_offset(struct.unpack_from("<I", d, names_off + i * 4)[0])
        name = d[n:d.index(b"\x00", n)].decode("ascii", "replace")
        out[struct.unpack_from("<H", d, ords_off + i * 2)[0] + base] = name
    return out


def thunk_entries(pe, thunk_rva):
    d = pe.data
    off = pe.rva_to_offset(thunk_rva)
    if off is None:
        return []
    out, i = [], 0
    while True:
        v = struct.unpack_from("<Q", d, off + i * 8)[0]
        if v == 0:
            break
        if v >> 63:
            out.append(("ordinal", v & 0xFFFF))
        else:
            h = pe.rva_to_offset(v & 0x7FFFFFFF)
            if h is not None:
                out.append(("name", d[h + 2:d.index(b"\x00", h + 2)].decode("ascii", "replace")))
        i += 1
    return out


def imports(path):
    pe = PeImage(path)
    d = pe.data
    base = data_dir_base(pe)
    out = {}
    for idx, kind, stride in ((1, "static", 20), (13, "delay", 32)):
        rva, _ = struct.unpack_from("<II", d, base + idx * 8)
        off = pe.rva_to_offset(rva) if rva else None
        if off is None:
            continue
        i = 0
        while True:
            e = off + i * stride
            if idx == 1:
                oft, _, _, name_rva, first = struct.unpack_from("<IIIII", d, e)
            else:
                _, name_rva, _, first, oft = struct.unpack_from("<IIIII", d, e)
            if name_rva == 0:
                break
            n = pe.rva_to_offset(name_rva)
            if n is None:
                break
            dll = d[n:d.index(b"\x00", n)].decode("ascii", "replace").lower()
            out.setdefault((dll, kind), []).extend(thunk_entries(pe, oft or first))
            i += 1
    return out


def main():
    # With no argument this regenerates the tracked table; the check passes a path in its build dir.
    out = sys.argv[1] if len(sys.argv) > 1 else TRACKED_OUT
    if not os.path.isfile(G.ECHOVR_EXE):
        print("SKIP network surface: %s" % G.ECHOVR_EXE)
        print("RESULT networksurface status=SKIP rows=0 covered=0 uncovered=0 wininet_static=NOT-RUN")
        return 0
    ws2 = export_ordinals(os.path.join(os.environ.get("SystemRoot", "C:\\Windows"),
                                       "System32", "ws2_32.dll"))
    rows, uncovered, covered = [], [], []
    for root, _, files in os.walk(BIN):
        for fn in sorted(files):
            if not fn.lower().endswith((".exe", ".dll")):
                continue
            path = os.path.join(root, fn)
            try:
                imp = imports(path)
            except Exception:
                continue
            for (dll, kind), entries in sorted(imp.items()):
                if dll not in NET_DLLS:
                    continue
                for form, value in entries:
                    if form == "ordinal" and dll == "ws2_32.dll":
                        name = ws2.get(value, "#%d" % value)
                    elif form == "name":
                        name = value
                    else:
                        name = "#%d" % value
                    if name in HOOKED:
                        verdict = "covered"
                        covered.append((fn, dll, name))
                    elif name in NOT_EGRESS:
                        verdict = "not-egress"
                    else:
                        verdict = "NOT-COVERED"
                        uncovered.append((fn, dll, name))
                    rows.append((os.path.relpath(path, BIN), dll, kind, form, name, verdict))

    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Generated by tests/loaderprobe/scan_network_surface.py -- do not hand-edit.\n")
        f.write("# Every networking import in the install, and whether replay::egress covers it.\n")
        f.write("# NOT-COVERED is not a defect on its own; it is the list the blocker's report must not\n")
        f.write("# let a reader forget. Ordinals resolve through ws2_32.dll's own export table.\n")
        f.write("module\tdll\tlink\tform\tentry\tcoverage\n")
        for r in rows:
            f.write("\t".join(r) + "\n")

    # The WININET half of the coverage claim rests on wininet.dll being mapped before our DllMain runs.
    # That is not an assumption: echovr.exe statically imports BugSplat64.dll which statically imports
    # wininet, AND echovr.exe imports wininet directly. Static, not delay-loaded, so the loader resolves
    # both before the entry point. Asserted here so a future build that made either lazy breaks the claim
    # loudly instead of leaving the blocker silently unarmed on that transport.
    static_chain = []
    for module, dll in (("echovr.exe", "bugsplat64.dll"), ("echovr.exe", "wininet.dll"),
                        ("BugSplat64.dll", "wininet.dll")):
        path = os.path.join(BIN, module)
        ok = False
        if os.path.exists(path):
            try:
                imp = imports(path)
            except Exception:
                imp = {}
            ok = (dll, "static") in imp and len(imp[(dll, "static")]) > 0
            lazy = (dll, "delay") in imp
            if lazy:
                print("  FAIL %s delay-loads %s; the WININET hooks would not be armed at DllMain time"
                      % (module, dll))
        if not ok:
            print("  FAIL %s does not statically import %s" % (module, dll))
        static_chain.append(ok)
    if all(static_chain):
        print("  wininet is mapped before the entry point: echovr.exe -> BugSplat64.dll -> wininet, and "
              "echovr.exe -> wininet directly. Both static.")

    echovr = [r for r in rows if r[0].lower().endswith("echovr.exe") and r[1] == "ws2_32.dll"]
    print("  echovr.exe imports %d ws2_32 entries (%d by ordinal)"
          % (len(echovr), sum(1 for r in echovr if r[3] == "ordinal")))
    seen = set()
    for fn, dll, name in uncovered:
        if (dll, name) in seen:
            continue
        seen.add((dll, name))
        print("  NOT-COVERED  %-14s %-28s first seen in %s" % (dll, name, fn))
    print("  %d row(s), %d covered, %d distinct not-covered" % (len(rows), len(covered), len(seen)))
    ok = bool(rows) and all(static_chain)
    print("RESULT networksurface status=%s rows=%d covered=%d uncovered=%d wininet_static=%s"
          % ("PASS" if ok else "FAIL", len(rows), len(covered), len(seen),
             "yes" if all(static_chain) else "NO"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
