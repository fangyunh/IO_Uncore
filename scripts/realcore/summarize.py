#!/usr/bin/env python3
"""Per-cell summary of a real-core campaign directory (points.csv + ledger.csv).

Usage: summarize.py <campaign_dir> [--out cells.csv] [--db-ns 150]
Cell key: variant, mode, app_cpus, dev_cpus, qd, qpairs, placement, compute, bdev, lat
(lat = perf -L latency tracking, 1 on / 0 off; a missing or empty lat counts as 1).
"""
import argparse
import csv
import os
import statistics
import sys

KEY = ["variant", "mode", "app_cpus", "dev_cpus", "qd", "qpairs", "placement", "compute", "bdev", "lat"]
OUT = KEY + ["app_cores", "n", "iops_med", "iops_q1", "iops_q3", "busy_max", "mmio_med",
             "p50_med", "p99_med", "ns_per_io", "ns_per_io_corr", "iops_corr"]


def cpu_count(s):
    n = 0
    for part in s.replace("+", ",").split(","):
        if "-" in part:
            a, b = part.split("-"); n += int(b) - int(a) + 1
        elif part:
            n += 1
    return n


def quartiles(v):
    v = sorted(v)
    if len(v) == 1:
        return v[0], v[0], v[0]
    q = statistics.quantiles(v, n=4, method="inclusive")
    return q[0], statistics.median(v), q[2]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("campaign_dir"); ap.add_argument("--out"); ap.add_argument("--db-ns", type=float, default=150.0)
    a = ap.parse_args(argv)
    pts = {r["point_id"]: r for r in csv.DictReader(open(os.path.join(a.campaign_dir, "points.csv")))}
    cells, failed = {}, 0
    for r in csv.DictReader(open(os.path.join(a.campaign_dir, "ledger.csv"))):
        if r["status"] != "ok":
            failed += 1
            continue
        p = pts[r["point_id"]]
        cells.setdefault(tuple((p.get(k) or "1") if k == "lat" else p[k] for k in KEY), []).append(r)
    out_rows = []
    for key, rs in sorted(cells.items()):
        iops = [float(x["iops"]) for x in rs]
        q1, med, q3 = quartiles(iops)
        mmio = statistics.median(float(x["mmio_per_io"] or 0) for x in rs)
        cores = cpu_count(key[KEY.index("app_cpus")])
        ns = cores * 1e9 / med
        ns_corr = ns + mmio * a.db_ns
        row = dict(zip(KEY, key))
        row.update(app_cores=cores, n=len(rs), iops_med=med, iops_q1=q1, iops_q3=q3,
                   busy_max=max(float(x["busy_max"]) for x in rs), mmio_med=mmio,
                   p50_med=statistics.median(float(x["p50_us"]) for x in rs),
                   p99_med=statistics.median(float(x["p99_us"]) for x in rs),
                   ns_per_io=round(ns, 3), ns_per_io_corr=round(ns_corr, 3),
                   iops_corr=round(cores * 1e9 / ns_corr, 1))
        out_rows.append(row)
    out = a.out or os.path.join(a.campaign_dir, "cells.csv")
    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=OUT); w.writeheader(); w.writerows(out_rows)
    print("cells=%d failed_points=%d -> %s" % (len(out_rows), failed, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
