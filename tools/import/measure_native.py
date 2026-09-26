#!/usr/bin/env python3
"""Native pose facts and ground-truth event tables for the legacy-format converter.

DESIGN-FORMATS 5.3 (H0-H5), 9.2 (T1) and 11.1 (facts guard). Everything native is read through
`echodemo rows`, so the numbers here are the numbers the shipping decoders produce.

  measure_native.py facts --echodemo EXE --capture A.echodemo [--capture B.echodemo ...]
                          --api X.echoreplay [--api Y.echoreplay ...] --out native_facts.json
  measure_native.py truth --echodemo EXE --out-dir DIR FILE.echodemo [...]

`facts` prints every measurement with its method, n and spread, and writes the fact rows that
src/import/import_facts.h is pinned to. `truth` writes one JSON table per capture: stun rows, goal
rows, stat increments, per-player bit spans, holds, triggers and the disc owner, keyed by user slot.
Neither output carries a display name or an account id.
"""

import argparse
import csv
import io
import json
import math
import os
import subprocess
import sys
import zipfile

import numpy as np

# ---------------------------------------------------------------------------------------------
# Reading `echodemo rows`

STRING_COLUMNS = {"src", "tap", "payload", "name"}
HEX_COLUMNS = {"bits28", "owner", "idValue", "bits11", "victim", "puncher", "scorer", "assist", "entrant",
               "hash", "raw", "present", "phaseSymbol", "frisbeeOwnerId"}
BIT = {
    "punchL": 0x2, "punchR": 0x4, "attachA": 0x20, "attachB": 0x40, "movingAttached": 0x80,
    "boost": 0x100, "brake": 0x200, "thrustA": 0x400, "thrustB": 0x800,
}


def rows(exe, path, what):
    """One `echodemo rows` table as {column: np.array}. Refuses on any decode fault it reports."""
    proc = subprocess.run([exe, "rows", path, "--what", what], capture_output=True, text=True)
    census = proc.stderr.strip()
    if proc.returncode != 0:
        raise RuntimeError(f"echodemo rows {what} {path}: rc={proc.returncode} {census}")
    reader = csv.reader(io.StringIO(proc.stdout))
    header = next(reader)
    body = [r for r in reader if not (what == "body" and r[1] != "wire")]
    table = {}
    for i, name in enumerate(header):
        col = [r[i] for r in body]
        if name in STRING_COLUMNS:
            table[name] = np.array(col, dtype=object)
        elif name in HEX_COLUMNS:
            table[name] = np.array([np.int64(np.uint64(int(v, 16))) if v else -1 for v in col], dtype=np.int64)
        else:
            table[name] = np.array([float(v) if v else math.nan for v in col], dtype=np.float64)
    table["_n"] = len(body)
    table["_census"] = census
    return table


def take(t, mask):
    return {k: (v[mask] if isinstance(v, np.ndarray) else v) for k, v in t.items()}


def vec(t, *names):
    return np.stack([t[n] for n in names], axis=-1)


def quat_to_mat(q):
    """xyzw quaternions (N,4) -> rotation matrices (N,3,3); column k is R*e_k."""
    q = q / np.linalg.norm(q, axis=1, keepdims=True)
    x, y, z, w = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    m = np.empty((len(q), 3, 3))
    m[:, 0, 0] = 1 - 2 * (y * y + z * z)
    m[:, 0, 1] = 2 * (x * y - z * w)
    m[:, 0, 2] = 2 * (x * z + y * w)
    m[:, 1, 0] = 2 * (x * y + z * w)
    m[:, 1, 1] = 1 - 2 * (x * x + z * z)
    m[:, 1, 2] = 2 * (y * z - x * w)
    m[:, 2, 0] = 2 * (x * z - y * w)
    m[:, 2, 1] = 2 * (y * z + x * w)
    m[:, 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def unit(v):
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.where(n == 0, 1, n)


def pct(a, ps=(1, 50, 90, 99)):
    a = np.asarray(a, dtype=float)
    a = a[np.isfinite(a)]
    if a.size == 0:
        return {"n": 0}
    out = {"n": int(a.size)}
    for p in ps:
        out[f"p{p}"] = round(float(np.percentile(a, p)), 5)
    return out


def empty_table(exe, path, what, reason, refused):
    refused[what] = reason
    return {"_n": 0, "_census": reason, "tUs": np.zeros(0, dtype=np.int64)}


def load_native(exe, path):
    """Players and bodies are required; a side table that does not decode is recorded as refused and
    left empty, so one old stream layout does not throw away a capture's poses."""
    refused = {}
    players = rows(exe, path, "player")
    bodies = rows(exe, path, "body")
    side = {}
    for what in ("event", "netvar", "gamestate", "roster"):
        try:
            side[what] = rows(exe, path, what)
        except RuntimeError as err:
            side[what] = None
            refused[what] = str(err)[:300]
    cap = {
        "path": path,
        "all_players": players,
        "players": take(players, players["src"] == "wire"),
        "bodies": take(bodies, bodies["idTag"] == 0),
        "disc": take(bodies, bodies["idTag"] == 1),
        "events": side["event"], "netvars": side["netvar"], "gamestate": side["gamestate"],
        "roster": side["roster"], "refused": refused,
    }
    join_player_body(cap)
    return cap


def join_player_body(cap):
    """Pairs each player row with the SimState row of the same sender and source stamp: one packet
    carries both, so the stamp is an exact key."""
    p, b = cap["players"], cap["bodies"]
    bslot = b["idValue"] & 0xFFFF
    bkey = (bslot.astype(np.int64) << 40) | b["stampUs"].astype(np.int64)
    pkey = (p["slot"].astype(np.int64) << 40) | p["stampUs"].astype(np.int64)
    order = np.argsort(bkey, kind="stable")
    sk = bkey[order]
    at = np.clip(np.searchsorted(sk, pkey), 0, len(sk) - 1)
    hit = sk[at] == pkey
    idx = order[at]
    joined = take(p, hit)
    for name in ("comX", "comY", "comZ", "oriX", "oriY", "oriZ", "oriW", "velX", "velY", "velZ",
                 "angX", "angY", "angZ", "lastHeldX", "lastHeldY", "lastHeldZ", "bits11"):
        joined["b_" + name] = b[name][idx[hit]]
    order = np.lexsort((joined["tUs"], joined["slot"]))
    cap["joined"] = take(joined, order)
    cap["join_rate"] = float(hit.mean()) if len(hit) else 0.0


def team_of_slot(cap):
    r = cap["roster"]
    if r is None:
        return {}
    return {int(s): int(t) for s, t in zip(r["userSlot"], r["team"])}


# ---------------------------------------------------------------------------------------------
# Reading Spark .echoreplay (API) files, for the distribution parities


def api_lines(path):
    with open(path, "rb") as f:
        magic = f.read(2)
    if magic == b"PK":
        with zipfile.ZipFile(path) as z:
            with z.open(z.infolist()[0]) as raw:
                for line in io.TextIOWrapper(raw, encoding="utf-8", errors="replace"):
                    yield line
    else:
        with open(path, encoding="utf-8", errors="replace") as f:
            yield from f


def basis(d, pos_key):
    return (d[pos_key], np.array([d["left"], d["up"], d["forward"]], dtype=float).T)


def load_api(path, stride):
    rec = {k: [] for k in ("team", "head", "Rhead", "Rbody", "lh", "Rlh", "rh", "Rrh", "vel",
                           "holdL", "holdR", "stunned", "t", "key")}
    disc = {k: [] for k in ("pos", "R", "vel", "held", "t")}
    for i, line in enumerate(api_lines(path)):
        if i % stride:
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 2:
            continue
        try:
            s = json.loads(parts[1])
        except ValueError:
            continue
        if s.get("game_status") not in ("playing", "round_start", "score", "pre_match", "round_over"):
            continue
        held = False
        for ti, team in enumerate(s.get("teams", [])[:2]):
            for p in team.get("players") or []:
                try:
                    hp, Rh = basis(p["head"], "position")
                    _, Rb = basis(p["body"], "position")
                    lp, Rl = basis(p["lhand"], "pos")
                    rp, Rr = basis(p["rhand"], "pos")
                except KeyError:
                    continue
                rec["team"].append(ti)
                rec["head"].append(hp)
                rec["Rhead"].append(Rh)
                rec["Rbody"].append(Rb)
                rec["lh"].append(lp)
                rec["Rlh"].append(Rl)
                rec["rh"].append(rp)
                rec["Rrh"].append(Rr)
                rec["vel"].append(p.get("velocity", [0, 0, 0]))
                rec["holdL"].append(p.get("holding_left", "none"))
                rec["holdR"].append(p.get("holding_right", "none"))
                rec["stunned"].append(bool(p.get("stunned")))
                rec["t"].append(i)
                rec["key"].append(p.get("userid", 0))
                held = held or "disc" in (p.get("holding_left"), p.get("holding_right"))
        d = s.get("disc")
        if d:
            disc["pos"].append(d["position"])
            disc["R"].append(np.array([d["left"], d["up"], d["forward"]], dtype=float).T)
            disc["vel"].append(d["velocity"])
            disc["held"].append(held)
            disc["t"].append(i)
    out = {k: np.array(v) for k, v in rec.items()}
    out["disc"] = {k: np.array(v) for k, v in disc.items()}
    return out


def merge_api(parts):
    out = {k: np.concatenate([p[k] for p in parts]) for k in parts[0] if k != "disc"}
    out["disc"] = {k: np.concatenate([p["disc"][k] for p in parts]) for k in parts[0]["disc"]}
    return out


# ---------------------------------------------------------------------------------------------
# Span and edge helpers


def spans(t, on, slot=None):
    """[start, end] runs where `on` holds, over rows sorted by time within one player. The end is the
    last row the condition held on."""
    out = []
    start = None
    for i in range(len(on)):
        if on[i] and start is None:
            start = i
        if start is not None and (not on[i] or i == len(on) - 1):
            end = i if on[i] else i - 1
            out.append((int(t[start]), int(t[end])))
            start = None
    return out


def per_slot(j):
    for s in np.unique(j["slot"]):
        m = j["slot"] == s
        yield int(s), take(j, m)


def angle_deg(cosv):
    return np.degrees(np.arccos(np.clip(cosv, -1, 1)))


def yaw_of(R):
    f = R[:, :, 2]
    return np.arctan2(f[:, 0], f[:, 2])


def wrap_deg(a):
    return (np.degrees(a) + 180) % 360 - 180


# ---------------------------------------------------------------------------------------------
# The measurements


def measure_capture(cap):
    j = cap["joined"]
    R_hmd = quat_to_mat(vec(j, "hmdX", "hmdY", "hmdZ", "hmdW"))
    R_ori = quat_to_mat(vec(j, "b_oriX", "b_oriY", "b_oriZ", "b_oriW"))
    R_l = quat_to_mat(vec(j, "lqX", "lqY", "lqZ", "lqW"))
    R_r = quat_to_mat(vec(j, "rqX", "rqY", "rqZ", "rqW"))
    com = vec(j, "b_comX", "b_comY", "b_comZ")
    vel = vec(j, "b_velX", "b_velY", "b_velZ")
    refv = vec(j, "velX", "velY", "velZ")
    loff = vec(j, "lOffX", "lOffY", "lOffZ")
    roff = vec(j, "rOffX", "rOffY", "rOffZ")
    bits = j["bits28"].astype(np.int64)
    speed = np.linalg.norm(vel, axis=1)
    m = {"file": os.path.basename(cap["path"]), "player_rows": int(len(cap["players"]["tUs"])),
         "joined_rows": int(len(j["tUs"])), "join_rate": round(cap["join_rate"], 5)}

    # Sample cadence per sender and the finite-difference unit check: d(com)/dt against vel.
    same = np.r_[False, j["slot"][1:] == j["slot"][:-1]]
    dt = np.r_[0, np.diff(j["tUs"])] / 1e6
    ok = same & (dt > 0.005) & (dt < 0.05)
    dcom = np.r_[np.zeros((1, 3)), np.diff(com, axis=0)]
    fd = dcom[ok] / dt[ok, None]
    vmid = 0.5 * (vel[ok] + np.r_[np.zeros((1, 3)), vel[:-1]][ok])
    moving = np.linalg.norm(vmid, axis=1) > 1.0
    slope = float(np.sum(fd[moving] * vmid[moving]) / np.sum(vmid[moving] * vmid[moving]))
    m["units"] = {
        "row_dt_ms": pct(dt[ok] * 1e3, (10, 50, 90)),
        "fd_velocity_slope": round(slope, 4),
        "fd_velocity_n": int(moving.sum()),
        "com_x": pct(com[:, 0], (0.1, 50, 99.9)), "com_y": pct(com[:, 1], (0.1, 50, 99.9)),
        "com_z": pct(com[:, 2], (0.1, 50, 99.9)),
        "speed": pct(speed, (50, 99, 99.9)),
        "refVel_equals_body_vel": round(float(np.mean(np.all(np.abs(refv - vel) < 0.07, axis=1))), 4),
        "anchor_counts": {str(int(a)): int(c) for a, c in zip(*np.unique(j["anchor"], return_counts=True))},
        "refVel_len": pct(np.linalg.norm(refv, axis=1), (50, 99)),
        "refVel_minus_vel_len": pct(np.linalg.norm(refv - vel, axis=1), (50, 90)),
    }

    # Chirality (5.4) for each candidate orientation, so H0/H2/H3 read off one table.
    fast = speed > 2
    for name, R in (("hmdOri", R_hmd), ("bodyOri", R_ori)):
        m[f"chirality_{name}"] = {
            "up_median": round(float(np.median(R[:, 1, 1])), 4),
            "forward_dot_vhat_mean": round(float(np.mean(np.einsum("ij,ij->i", R[fast, :, 2], unit(vel[fast])))), 4),
            "left_hand_on_Rx_frac": round(float(np.mean(
                np.einsum("ij,ij->i", loff, R[:, :, 0]) > np.einsum("ij,ij->i", roff, R[:, :, 0]))), 4),
            "tilt_deg": pct(angle_deg(R[:, 1, 1]), (50, 90, 99, 100)),
            "tilt_over_1deg_frac": round(float(np.mean(angle_deg(R[:, 1, 1]) > 1.0)), 5),
        }
    m["yaw_hmd_minus_bodyOri_deg"] = pct(np.abs(wrap_deg(yaw_of(R_hmd) - yaw_of(R_ori))), (50, 90, 99))
    fwd_ori = R_ori[:, :, 2]
    m["bodyOri_pitch_deg"] = pct(np.degrees(np.arcsin(np.clip(fwd_ori[:, 1], -1, 1))), (1, 50, 99))

    # Hands (H1, H4): offsets and the hand bases relative to each candidate reference.
    m["hand_offset"] = {
        "left_len": pct(np.linalg.norm(loff, axis=1), (50, 90, 99)),
        "right_len": pct(np.linalg.norm(roff, axis=1), (50, 90, 99)),
        "left_y": pct(loff[:, 1], (10, 50, 90)), "right_y": pct(roff[:, 1], (10, 50, 90)),
        "left_in_hmd_frame": [round(float(x), 3) for x in np.median(np.einsum("nji,nj->ni", R_hmd, loff), axis=0)],
        "left_in_bodyOri_frame": [round(float(x), 3) for x in np.median(np.einsum("nji,nj->ni", R_ori, loff), axis=0)],
    }
    for ref_name, Rref in (("hmd", R_hmd), ("bodyOri", R_ori)):
        for hand, Rh in (("left", R_l), ("right", R_r)):
            rel = np.einsum("nji,njk->nik", Rref, Rh)
            m[f"hand_rel_{ref_name}_{hand}_mean"] = np.round(rel.mean(axis=0), 3).tolist()

    # Floor contact: the lowest com the players reach.
    m["com_y_low"] = pct(com[:, 1], (0.1, 1, 5))

    # The disc: owner semantics (H5), grip offset (H1), frame (disc).
    d = cap["disc"]
    dcom = vec(d, "comX", "comY", "comZ")
    dvel = vec(d, "velX", "velY", "velZ")
    dR = quat_to_mat(vec(d, "oriX", "oriY", "oriZ", "oriW"))
    owner = d["owner"] & 0xFFFFFFFF
    owned = owner != 0xFFFFFFFF
    m["disc"] = {"rows": int(len(dcom)), "idValues": sorted({int(v) for v in d["idValue"]}),
                 "owned_frac": round(float(owned.mean()), 4),
                 "owner_values": sorted({int(v) for v in owner[owned]})}
    near_world = np.full(len(dcom), np.nan)
    near_local = np.full(len(dcom), np.nan)
    near_com = np.full(len(dcom), np.nan)
    slot_rows = {s: np.where(j["slot"] == s)[0] for s in np.unique(j["slot"])}
    for k in np.where(owned)[0]:
        idx = slot_rows.get(int(owner[k] & 0xFFFF))
        if idx is None or len(idx) == 0:
            continue
        pos = idx[min(np.searchsorted(j["tUs"][idx], d["tUs"][k]), len(idx) - 1)]
        if abs(int(j["tUs"][pos]) - int(d["tUs"][k])) > 30000:
            continue
        near_world[k] = min(np.linalg.norm(dcom[k] - com[pos] - off) for off in (loff[pos], roff[pos]))
        near_local[k] = min(np.linalg.norm(dcom[k] - com[pos] - R_hmd[pos] @ off) for off in (loff[pos], roff[pos]))
        near_com[k] = np.linalg.norm(dcom[k] - com[pos])
    have = np.isfinite(near_world)
    close = have & (near_world < 1.0)
    far = have & (near_world > 1.0)
    dspeed = np.linalg.norm(dvel, axis=1)
    m["disc"]["owned_joined"] = int(have.sum())
    m["disc"]["owner_hand_dist_world_axes"] = pct(near_world[have], (10, 50, 90))
    m["disc"]["owner_hand_dist_world_axes_when_under_1m"] = pct(near_world[close], (10, 50, 90))
    m["disc"]["owner_hand_dist_hmd_axes_when_under_1m"] = pct(near_local[close], (10, 50, 90))
    m["disc"]["owner_com_dist_when_under_1m"] = pct(near_com[close], (10, 50, 90))
    m["disc"]["owned_within_0p3m_frac"] = round(float(np.mean(near_world[have] < 0.3)), 4)
    m["disc"]["owned_beyond_1m_frac"] = round(float(np.mean(far[have])), 4)
    m["disc"]["speed_owned_beyond_1m"] = pct(dspeed[far], (10, 50, 90))
    m["disc"]["speed_owned_within_1m"] = pct(dspeed[close], (10, 50, 90))
    grip_rel = []
    for k in np.where(have & (near_world < 0.3))[0]:
        idx = slot_rows[int(owner[k] & 0xFFFF)]
        pos = idx[min(np.searchsorted(j["tUs"][idx], d["tUs"][k]), len(idx) - 1)]
        dl = np.linalg.norm(dcom[k] - com[pos] - loff[pos])
        dr = np.linalg.norm(dcom[k] - com[pos] - roff[pos])
        Rh = R_l[pos] if dl <= dr else R_r[pos]
        grip_rel.append(np.abs(Rh.T @ dR[k]))
    if grip_rel:
        m["disc"]["abs_hand_rel_disc_mean_in_grip"] = np.round(np.mean(grip_rel, axis=0), 3).tolist()
    free = (dspeed > 3) & (~owned | far)
    vh = unit(dvel[free])
    m["disc"]["free_flight_rows"] = int(free.sum())
    m["disc"]["abs_Ry_dot_vhat"] = pct(np.abs(np.einsum("ij,ij->i", dR[free, :, 1], vh)), (50, 90))
    m["disc"]["abs_Rz_dot_vhat"] = pct(np.abs(np.einsum("ij,ij->i", dR[free, :, 2], vh)), (50, 90))
    m["disc"]["abs_Rx_dot_vhat"] = pct(np.abs(np.einsum("ij,ij->i", dR[free, :, 0], vh)), (50, 90))
    m["disc"]["tilt_deg_free"] = pct(angle_deg(np.abs(dR[free, 1, 1])), (50, 90))

    # Bits: idle word, per-bit frequency, attach L/R by hand stationarity, span lengths.
    m["bits_and_all"] = f"0x{int(np.bitwise_and.reduce(bits)):07X}"
    m["bits_or_all"] = f"0x{int(np.bitwise_or.reduce(bits)):07X}"
    m["bit_frequency"] = {f"0x{1 << b:07X}": round(float(np.mean((bits >> b) & 1)), 5) for b in range(28)
                          if np.any((bits >> b) & 1)}
    hand_speed = {}
    for side, off in (("left", loff), ("right", roff)):
        hw = com + off
        dh = np.r_[np.zeros((1, 3)), np.diff(hw, axis=0)]
        sp = np.full(len(hw), np.nan)
        sp[ok] = np.linalg.norm(dh[ok], axis=1) / dt[ok]
        hand_speed[side] = sp
    attach = {}
    for bit in ("attachA", "attachB"):
        other = "attachB" if bit == "attachA" else "attachA"
        only = ((bits & BIT[bit]) != 0) & ((bits & BIT[other]) == 0) & ok
        attach[bit] = {"rows": int(only.sum()),
                       "left_hand_speed": pct(hand_speed["left"][only], (50,)),
                       "right_hand_speed": pct(hand_speed["right"][only], (50,)),
                       "com_speed": pct(speed[only], (50,))}
    m["attach_stationarity"] = attach
    span_len = {}
    for name in ("punchL", "punchR", "boost", "brake", "thrustA", "thrustB", "attachA", "attachB", "movingAttached"):
        lens = []
        for s, one in per_slot(j):
            on = (one["bits28"].astype(np.int64) & BIT[name]) != 0
            lens += [(b - a) / 1e6 for a, b in spans(one["tUs"], on)]
        span_len[name] = {"spans": len(lens), **pct(lens, (10, 50, 90))}
    m["span_seconds"] = span_len

    # Thruster axis: acceleration in each hand's frame while exactly one thruster bit is set and
    # nothing else pushes (no attach, boost, brake).
    dv = np.r_[np.zeros((1, 3)), np.diff(vel, axis=0)]
    acc = np.zeros_like(vel)
    acc[ok] = dv[ok] / dt[ok, None]
    quiet = ((bits & (BIT["attachA"] | BIT["attachB"] | BIT["boost"] | BIT["brake"])) == 0) & ok
    thr = {}
    for bit, other in (("thrustA", "thrustB"), ("thrustB", "thrustA")):
        sel = quiet & ((bits & BIT[bit]) != 0) & ((bits & BIT[other]) == 0)
        a = acc[sel]
        strong = np.linalg.norm(a, axis=1) > 0.5
        entry = {"rows": int(sel.sum()), "accel": pct(np.linalg.norm(a, axis=1), (50, 90))}
        for hand, Rh in (("left", R_l), ("right", R_r)):
            local = unit(np.einsum("nji,nj->ni", Rh[sel][strong], a[strong]))
            mean = local.mean(axis=0) if len(local) else np.zeros(3)
            entry[f"{hand}_hand_mean_dir"] = np.round(mean, 3).tolist()
            entry[f"{hand}_hand_resultant"] = round(float(np.linalg.norm(mean)), 3)
        thr[bit] = entry
    idle_acc = quiet & ((bits & (BIT["thrustA"] | BIT["thrustB"])) == 0)
    thr["no_thruster_accel"] = pct(np.linalg.norm(acc[idle_acc], axis=1), (50, 90))
    m["thrusters"] = thr

    attached = (bits & (BIT["attachA"] | BIT["attachB"])) != 0
    ref_moving = np.linalg.norm(refv, axis=1) > 0.05
    m["refVel_nonzero_frac"] = {"all": round(float(ref_moving.mean()), 4),
                                "attached": round(float(ref_moving[attached].mean()), 4),
                                "not_attached": round(float(ref_moving[~attached].mean()), 4),
                                "refVel_vs_vel_when_attached": pct(np.linalg.norm(refv - vel, axis=1)[attached], (50, 90))}
    m["anchor_by_attach"] = {f"{name}": {str(int(a)): int(np.sum((j["anchor"] == a) & sel)) for a in np.unique(j["anchor"])}
                             for name, sel in (("attachA_only", ((bits & BIT["attachA"]) != 0) & ((bits & BIT["attachB"]) == 0)),
                                               ("attachB_only", ((bits & BIT["attachB"]) != 0) & ((bits & BIT["attachA"]) == 0)),
                                               ("both", ((bits & BIT["attachA"]) != 0) & ((bits & BIT["attachB"]) != 0)),
                                               ("neither", ~attached))}
    tilted = angle_deg(R_hmd[:, 1, 1]) > 1.0
    m["hmd_tilted_frac"] = {"attached": round(float(tilted[attached].mean()), 4),
                            "not_attached": round(float(tilted[~attached].mean()), 4)}
    m["bend_rest"] = {"bendBack": pct(j["bendBack"], (1, 50, 99)), "bendFwd": pct(j["bendFwd"], (1, 50, 99)),
                      "restPoseHeight": pct(j["restPoseHeight"], (1, 50, 99))}
    m["trigger_values"] = {f"{v:.1f}": int(c) for v, c in zip(*np.unique(np.round(j["trigL"], 1), return_counts=True))}
    held = (j["holdLSlot"] != 65535) | (j["holdRSlot"] != 65535)
    m["hold_rows"] = int(held.sum())

    e = cap["events"]
    stun = e["tap"] == "SR15StunPlayerMsg"
    m["stun"] = {"rows": int(stun.sum()), "duration": pct(e["duration"][stun], (0, 50, 100)),
                 "blocked": int(np.sum(e["blocked"][stun] == 1))}

    # Team sides: which goal each team stands in front of while still.
    side = {}
    teams = team_of_slot(cap)
    tslot = np.array([teams.get(int(s), -1) for s in j["slot"]])
    still = speed < 0.5
    for t in (0, 1):
        sel = still & (tslot == t)
        side[str(t)] = {"rows_z_below_-60": int(np.sum(sel & (com[:, 2] < -60))),
                        "rows_z_above_60": int(np.sum(sel & (com[:, 2] > 60)))}
    m["team_sides_still"] = side
    return m


def measure_api(api):
    Rh, Rb, Rl, Rr = api["Rhead"], api["Rbody"], api["Rlh"], api["Rrh"]
    head, lh, rh, vel = api["head"], api["lh"], api["rh"], api["vel"].astype(float)
    speed = np.linalg.norm(vel, axis=1)
    fast = speed > 2
    m = {"player_samples": int(len(head))}
    for name, R in (("head", Rh), ("body", Rb)):
        m[f"chirality_{name}"] = {
            "det_median": round(float(np.median(np.linalg.det(R))), 4),
            "up_median": round(float(np.median(R[:, 1, 1])), 4),
            "forward_dot_vhat_mean": round(float(np.mean(np.einsum("ij,ij->i", R[fast, :, 2], unit(vel[fast])))), 4),
            "left_hand_on_left_frac": round(float(np.mean(
                np.einsum("ij,ij->i", lh - head, R[:, :, 0]) > np.einsum("ij,ij->i", rh - head, R[:, :, 0]))), 4),
            "tilt_deg": pct(angle_deg(R[:, 1, 1]), (50, 90, 99, 100)),
            "tilt_over_1deg_frac": round(float(np.mean(angle_deg(R[:, 1, 1]) > 1.0)), 5),
        }
    m["yaw_body_minus_head_deg"] = pct(np.abs(wrap_deg(yaw_of(Rb) - yaw_of(Rh))), (50, 90, 99))
    m["head_pitch_deg"] = pct(np.degrees(np.arcsin(np.clip(Rh[:, 1, 2], -1, 1))), (1, 50, 99))
    lo, ro = lh - head, rh - head
    m["hand_offset"] = {
        "left_len": pct(np.linalg.norm(lo, axis=1), (50, 90, 99)),
        "right_len": pct(np.linalg.norm(ro, axis=1), (50, 90, 99)),
        "left_y": pct(lo[:, 1], (10, 50, 90)), "right_y": pct(ro[:, 1], (10, 50, 90)),
        "left_in_body_frame": [round(float(x), 3) for x in np.median(np.einsum("nji,nj->ni", Rb, lo), axis=0)],
        "left_in_head_frame": [round(float(x), 3) for x in np.median(np.einsum("nji,nj->ni", Rh, lo), axis=0)],
    }
    for ref_name, Rref in (("body", Rb), ("head", Rh)):
        for hand, Rhand in (("left", Rl), ("right", Rr)):
            rel = np.einsum("nji,njk->nik", Rref, Rhand)
            m[f"hand_rel_{ref_name}_{hand}_mean"] = np.round(rel.mean(axis=0), 3).tolist()
    m["head_y_low"] = pct(head[:, 1], (0.1, 1, 5))
    m["speed"] = pct(speed, (50, 99, 99.9))
    m["head_z"] = pct(head[:, 2], (0.1, 50, 99.9))

    # Grip: the holding hand to the disc, per API frame (line index), under holding_*=="disc".
    d = api["disc"]
    m["disc"] = {}
    disc_at = {int(t): (pos, R) for t, pos, R in zip(d["t"], d["pos"], d["R"])}
    grip, grip_rel = [], []
    for i, t in enumerate(api["t"]):
        at = disc_at.get(int(t))
        if at is None:
            continue
        for held, hand, Rhand in ((api["holdL"][i], lh[i], Rl[i]), (api["holdR"][i], rh[i], Rr[i])):
            if held == "disc":
                dist = np.linalg.norm(at[0] - hand)
                grip.append(dist)
                if dist < 0.3:
                    grip_rel.append(np.abs(Rhand.T @ at[1]))
    grip = np.array(grip)
    m["disc"]["holder_hand_dist"] = pct(grip, (10, 50, 90, 99))
    m["disc"]["holding_disc_beyond_1m_frac"] = round(float(np.mean(grip > 1.0)), 4) if len(grip) else None
    m["disc"]["holding_disc_within_0p3m_frac"] = round(float(np.mean(grip < 0.3)), 4) if len(grip) else None
    if grip_rel:
        m["disc"]["abs_hand_rel_disc_mean_in_grip"] = np.round(np.mean(grip_rel, axis=0), 3).tolist()
    free = (~d["held"]) & (np.linalg.norm(d["vel"], axis=1) > 3)
    vh = unit(d["vel"][free])
    R = d["R"][free]
    m["disc"]["free_flight_rows"] = int(free.sum())
    m["disc"]["abs_up_dot_vhat"] = pct(np.abs(np.einsum("ij,ij->i", R[:, :, 1], vh)), (50, 90))
    m["disc"]["abs_forward_dot_vhat"] = pct(np.abs(np.einsum("ij,ij->i", R[:, :, 2], vh)), (50, 90))
    m["disc"]["abs_left_dot_vhat"] = pct(np.abs(np.einsum("ij,ij->i", R[:, :, 0], vh)), (50, 90))
    m["disc"]["tilt_deg_free"] = pct(angle_deg(np.abs(R[:, 1, 1])), (50, 90))
    team = api["team"]
    still = speed < 0.5
    m["team_sides_still"] = {str(t): {"rows_z_below_-60": int(np.sum(still & (team == t) & (head[:, 2] < -60))),
                                      "rows_z_above_60": int(np.sum(still & (team == t) & (head[:, 2] > 60)))}
                             for t in (0, 1)}
    return m


# ---------------------------------------------------------------------------------------------
# Ground truth tables (T1: what the engine did, keyed by user slot)


def slot_of(v):
    return None if v in (-1, 0xFFFFFFFF) else int(v) & 0xFFFF


def player_truth(one):
    b = one["bits28"].astype(np.int64)
    t = one["tUs"]
    rec = {"rows": int(len(t)), "first_tUs": int(t[0]), "last_tUs": int(t[-1]), "bits": {}}
    for name, mask in BIT.items():
        rec["bits"][name] = spans(t, (b & mask) != 0)
    for side in ("L", "R"):
        slot = one[f"hold{side}Slot"]
        gen = one[f"hold{side}Gen"]
        holds = []
        start = 0
        for k in range(1, len(t) + 1):
            if k == len(t) or slot[k] != slot[start] or gen[k] != gen[start]:
                if slot[start] != 65535:
                    holds.append({"start": int(t[start]), "end": int(t[k - 1]), "target": int(slot[start]),
                                  "generation": int(gen[start])})
                start = k
        rec[f"hold{side}"] = holds
        rec[f"trig{side}_spans"] = spans(t, one[f"trig{side}"] >= 0.5)
    return rec


def truth(cap):
    r = cap["roster"]
    out = {"file": os.path.basename(cap["path"]), "time_axis": "capture microseconds (echodemo rows tUs)",
           "players_key": "<src>:<user slot>; wire rows are remote players, sampled rows the recorder's own pawns",
           "refused_tables": cap["refused"],
           "roster": [] if r is None else [{"slot": int(s), "generation": int(g), "team": int(t)}
                                           for s, g, t in zip(r["userSlot"], r["generation"], r["team"])],
           "stuns": [], "goals": [], "stat_updates": [], "players": {}, "disc_owner_spans": [],
           "score_netvars": [], "game_state": []}
    e = cap["events"] if cap["events"] is not None else {"tap": np.zeros(0, dtype=object)}
    for i in np.where(e["tap"] == "SR15StunPlayerMsg")[0]:
        out["stuns"].append({"tUs": int(e["tUs"][i]), "victim": slot_of(e["victim"][i]),
                             "puncher": slot_of(e["puncher"][i]), "duration": float(e["duration"][i]),
                             "blocked": int(e["blocked"][i])})
    for i in np.where(e["tap"] == "SR15NetReliablePostScoreUpdate")[0]:
        out["goals"].append({"tUs": int(e["tUs"][i]), "team": int(e["team"][i]), "points": int(e["points"][i]),
                             "goalType": int(e["goalType"][i]), "scorer": slot_of(e["scorer"][i]),
                             "assist": slot_of(e["assist"][i]), "speed": float(e["speed"][i]),
                             "distance": float(e["distance"][i]), "possession": float(e["possession"][i])})
    for i in np.where(e["tap"] == "SR15NetReliableStatUpdate")[0]:
        try:
            doc = json.loads(e["payload"][i])
        except ValueError:
            continue
        deltas = {k: v.get("val") for k, v in doc.items() if isinstance(v, dict) and v.get("op") == "add"}
        if deltas:
            out["stat_updates"].append({"tUs": int(e["tUs"][i]), "entrant": int(e["entrant"][i]), "add": deltas})
    allp = cap["all_players"]
    for src in ("wire", "sampled"):
        for s, one in per_slot(take(allp, allp["src"] == src)):
            out["players"][f"{src}:{s}"] = player_truth(take(one, np.argsort(one["tUs"], kind="stable")))
    d = cap["disc"]
    if len(d["tUs"]):
        order = np.argsort(d["tUs"], kind="stable")
        owner = (d["owner"][order] & 0xFFFFFFFF)
        t = d["tUs"][order]
        start = 0
        for k in range(1, len(t) + 1):
            if k == len(t) or owner[k] != owner[start]:
                if owner[start] != 0xFFFFFFFF:
                    out["disc_owner_spans"].append({"start": int(t[start]), "end": int(t[k - 1]),
                                                    "slot": int(owner[start]) & 0xFFFF})
                start = k
    n = cap["netvars"] or {"tUs": []}
    for i in range(len(n["tUs"])):
        if n["name"][i] in ("blue_score", "red_score", "blue_round_score", "red_round_score", "game_state",
                            "match_is_over", "sudden_death_active"):
            out["score_netvars"].append({"tUs": int(n["tUs"][i]), "name": n["name"][i], "value": int(n["value"][i]),
                                         "raw": f"0x{int(n['raw'][i]) & 0xFFFFFFFFFFFFFFFF:016X}"})
    g = cap["gamestate"] or {"tUs": []}
    last = None
    for i in range(len(g["tUs"])):
        row = (int(g["phase"][i]), int(g["bluePoints"][i]), int(g["redPoints"][i]), int(g["roundCounter"][i]))
        if row != last:
            out["game_state"].append({"tUs": int(g["tUs"][i]), "phase": row[0], "bluePoints": row[1],
                                      "redPoints": row[2], "roundCounter": row[3],
                                      "gameTimer": float(g["gameTimer"][i])})
            last = row
    return out


# ---------------------------------------------------------------------------------------------


def cmd_facts(args):
    report = {"captures": [], "api": None}
    caps = []
    for path in args.capture:
        cap = load_native(args.echodemo, path)
        caps.append(cap)
        report["captures"].append(measure_capture(cap))
        print(f"# {path}: {cap['players']['_census']}", file=sys.stderr)
    if args.api:
        report["api"] = measure_api(merge_api([load_api(p, args.api_stride) for p in args.api]))
        report["api"]["files"] = [os.path.basename(p) for p in args.api]
    json.dump(report, sys.stdout, indent=1)
    print()
    if args.out:
        with open(args.out, "w", newline="\n") as f:
            json.dump(report, f, indent=1)
            f.write("\n")


def cmd_truth(args):
    os.makedirs(args.out_dir, exist_ok=True)
    index = []
    for path in args.files:
        name = os.path.basename(path)
        try:
            cap = load_native(args.echodemo, path)
            table = truth(cap)
            refused = cap["refused"]
        except (RuntimeError, ValueError, IndexError) as err:
            index.append({"file": name, "status": "refused", "reason": str(err)[:300]})
            continue
        out = os.path.join(args.out_dir, os.path.splitext(name)[0] + ".truth.json")
        with open(out, "w", newline="\n") as f:
            json.dump(table, f, separators=(",", ":"))
        index.append({"file": name, "status": "ok" if not refused else "partial", "refused_tables": sorted(refused),
                      "players": len(table["players"]),
                      "stuns": len(table["stuns"]), "goals": len(table["goals"]),
                      "stat_updates": len(table["stat_updates"]), "disc_owner_spans": len(table["disc_owner_spans"])})
    with open(os.path.join(args.out_dir, "index.json"), "w", newline="\n") as f:
        json.dump(index, f, indent=1)
        f.write("\n")
    count = {k: sum(1 for r in index if r["status"] == k) for k in ("ok", "partial", "refused")}
    print(f"TRUTH files={len(index)} ok={count['ok']} partial={count['partial']} refused={count['refused']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("facts")
    f.add_argument("--echodemo", required=True)
    f.add_argument("--capture", action="append", required=True)
    f.add_argument("--api", action="append", default=[])
    f.add_argument("--api-stride", type=int, default=3)
    f.add_argument("--out")
    t = sub.add_parser("truth")
    t.add_argument("--echodemo", required=True)
    t.add_argument("--out-dir", required=True)
    t.add_argument("files", nargs="+")
    args = ap.parse_args()
    (cmd_facts if args.cmd == "facts" else cmd_truth)(args)


if __name__ == "__main__":
    main()
