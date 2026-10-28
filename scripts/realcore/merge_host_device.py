#!/usr/bin/env python3
"""Join host per-IO probes (cycle_breakdown_*.csv) with device timestamps
(device_q<qid>.csv) into one per-IO timeline. Both sides read the same TSC.

Usage: merge_host_device.py --host H.csv --device D.csv [--device D2.csv ...]
                            --summary S.json [--merged M.csv]
Summary: host_rows, matched, match_rate, ticks_per_ns, negative_fraction,
median_ns per interval and of the total (t_end - t_start).
"""
import argparse
import bisect
import csv
import json
import statistics
import sys

INTERVALS = ["host_submit", "doorbell_to_device", "device_fetch", "device_media",
             "device_post", "post_to_host", "host_completion"]


def load_device(paths):
    idx = {}
    for p in paths:
        with open(p, newline="") as fh:
            for r in csv.DictReader(fh):
                key = (int(r["qid"]), int(r["cid"]))
                idx.setdefault(key, []).append(tuple(int(r[k]) for k in
                    ("ts_db_seen", "ts_fetched", "ts_exec", "ts_done", "ts_posted")))
    for v in idx.values():
        v.sort()
    return idx


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--device", required=True, action="append")
    ap.add_argument("--summary", required=True)
    ap.add_argument("--merged")
    a = ap.parse_args(argv)
    dev = load_device(a.device)
    with open(a.host, newline="") as fh:
        host = list(csv.DictReader(fh))
    ratios = []
    for r in host:
        cns = float(r.get("completion_ns") or 0)
        t_end = int(r["t_end"]); t_cpl = int(r["t_completion"] or 0) or int(r["t_doorbell"] or 0) \
            or int(r["t_sqe_copy"] or 0) or int(r["t_start"])
        if cns > 0 and t_end > t_cpl:
            ratios.append((t_end - t_cpl) / cns)
    if not ratios:
        print("merge: cannot infer tick rate (no completion_ns > 0)", file=sys.stderr)
        return 2
    tpn = statistics.median(ratios)
    merged, neg = [], 0
    for r in host:
        t_start = int(r["t_start"]); t_end = int(r["t_end"])
        t_sqe = int(r["t_sqe_copy"] or 0) or t_start
        t_db = int(r["t_doorbell"] or 0) or t_sqe
        t_cpl = int(r["t_completion"] or 0) or t_db
        cand = dev.get((int(r["qid"]), int(r["cid"])), [])
        i = bisect.bisect_left(cand, (t_start,))
        if i >= len(cand) or cand[i][0] > t_end:
            continue
        db, fe, ex, do, po = cand[i]
        iv = {"host_submit": t_db - t_start, "doorbell_to_device": db - t_db,
              "device_fetch": ex - db, "device_media": do - ex, "device_post": po - do,
              "post_to_host": t_cpl - po, "host_completion": t_end - t_cpl}
        if any(v < 0 for v in iv.values()):
            neg += 1
        row = {k: v / tpn for k, v in iv.items()}
        row["total"] = (t_end - t_start) / tpn
        row["qid"], row["cid"] = r["qid"], r["cid"]
        merged.append(row)
    med = {k: (statistics.median([m[k] for m in merged]) if merged else None)
           for k in INTERVALS + ["total"]}
    summary = {"host_rows": len(host), "matched": len(merged),
               "match_rate": (len(merged) / len(host)) if host else 0.0,
               "ticks_per_ns": round(tpn, 6),
               "negative_fraction": (neg / len(merged)) if merged else 0.0, "median_ns": med}
    with open(a.summary, "w") as fh:
        json.dump(summary, fh, indent=1, sort_keys=True)
    if a.merged:
        with open(a.merged, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=["qid", "cid"] + INTERVALS + ["total"])
            w.writeheader(); w.writerows(merged)
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
