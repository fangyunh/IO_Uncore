#!/usr/bin/env python3
"""Spec §7.4: app cores needed to sustain a target aggregate IOPS.

Linear interpolation on the running-max envelope of (cores, IOPS) with (0, 0)
prepended; None above the largest measured IOPS (never extrapolate).
Targets: 0.5 MIOPS steps up to min(max_base, max_iau).

Usage: cores_per_miops.py <cells.csv> --qd 128 --qpairs 1 [--variant lean]
                          [--out table.csv]
Band: cores from the q3 curve (optimistic) and the q1 curve (pessimistic).
Cells of every placement for the chosen (variant, qd, qpairs, compute=0) form one curve; the 8-core point is cross-socket.
"""
import argparse
import csv
import sys


def cores_for(points, target):
    env, best = [(0.0, 0.0)], 0.0
    for cores, iops in sorted(points):
        best = max(best, iops)
        env.append((float(cores), best))
    if target > env[-1][1]:
        return None
    for (c0, v0), (c1, v1) in zip(env, env[1:]):
        if v0 < target <= v1:
            return c0 + (c1 - c0) * (target - v0) / (v1 - v0)
    return None


def table(base, iau, base_lo=None, base_hi=None, iau_lo=None, iau_hi=None):
    top = min(max(v for _, v in base), max(v for _, v in iau))
    rows, k = [], 1
    while k * 0.5e6 <= top + 1e-6:
        t = k * 0.5e6
        cb, ci = cores_for(base, t), cores_for(iau, t)
        row = {"target_miops": k * 0.5, "cores_base": cb, "cores_iau": ci,
               "saving_pct": (100 * (1 - ci / cb)) if (cb and ci) else None}
        if base_lo:
            row.update(cores_base_opt=cores_for(base_hi, t), cores_base_pess=cores_for(base_lo, t),
                       cores_iau_opt=cores_for(iau_hi, t), cores_iau_pess=cores_for(iau_lo, t))
        rows.append(row)
        k += 1
    return rows


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cells"); ap.add_argument("--qd", required=True); ap.add_argument("--qpairs", required=True)
    ap.add_argument("--variant", default="lean"); ap.add_argument("--out")
    a = ap.parse_args(argv)
    curves = {"0": {"med": [], "q1": [], "q3": []}, "4": {"med": [], "q1": [], "q3": []}}
    for r in csv.DictReader(open(a.cells)):
        if r["variant"] != a.variant or r["qd"] != a.qd or r["qpairs"] != a.qpairs or r["compute"] != "0":
            continue
        if r["mode"] not in curves:
            continue
        n = int(r["app_cores"])
        for k, col in (("med", "iops_med"), ("q1", "iops_q1"), ("q3", "iops_q3")):
            curves[r["mode"]][k].append((n, float(r[col])))
    b, i = curves["0"], curves["4"]
    if not b["med"] or not i["med"]:
        print("no matching cells", file=sys.stderr)
        return 2
    rows = table(b["med"], i["med"], b["q1"], b["q3"], i["q1"], i["q3"])
    fh = open(a.out, "w", newline="") if a.out else sys.stdout
    w = csv.DictWriter(fh, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    if a.out:
        fh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
