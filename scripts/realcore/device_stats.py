#!/usr/bin/env python3
"""Device poll-group busy fractions between two `rpc.py thread_get_stats` snapshots.

busy_fraction = d(busy) / (d(busy) + d(idle)) per SPDK thread (ticks from the
same thread). Poll-group threads are those whose name contains "poll_group".
A poll group counts as active when its busy fraction is >= 0.05.

IAU-engine threads (names containing "iau_engine", IAU-next P3 mode 5) are reported under "engines";
MAX_BUSY is the max over poll groups and engines (the busy <= 0.70 rule covers both).

Usage: device_stats.py <snap_a.json> <snap_b.json> [--out device_busy.json]
Prints one line per poll group and a final MAX_BUSY=<x>. Exit 2 if no poll group.
"""
import argparse
import json
import sys


def fractions(a, b):
    prev = {t["id"]: t for t in a["threads"]}
    out = {}
    for t in b["threads"]:
        p = prev.get(t["id"])
        if p is None:
            continue
        db, di = t["busy"] - p["busy"], t["idle"] - p["idle"]
        tot = db + di
        out[t["name"]] = (db / tot) if tot > 0 else 0.0
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("snap_a"); ap.add_argument("snap_b"); ap.add_argument("--out")
    a = ap.parse_args(argv)
    with open(a.snap_a) as fa, open(a.snap_b) as fb:
        fr = fractions(json.load(fa), json.load(fb))
    pg = {k: v for k, v in fr.items() if "poll_group" in k}
    eng = {k: v for k, v in fr.items() if "iau_engine" in k}
    if not pg:
        print("device_stats: no poll_group threads in snapshots", file=sys.stderr)
        return 2
    res = {"poll_groups": pg, "engines": eng, "all_threads": fr,
           "max_busy_ssd": max(pg.values()), "max_busy_engine": max(eng.values()) if eng else 0.0,
           "max_busy": max(list(pg.values()) + list(eng.values())),
           "active_poll_groups": sum(1 for v in pg.values() if v >= 0.05)}
    if a.out:
        with open(a.out, "w") as fh:
            json.dump(res, fh, indent=1, sort_keys=True)
    for k in sorted(pg):
        print("%s %.4f" % (k, pg[k]))
    for k in sorted(eng):
        print("%s %.4f" % (k, eng[k]))
    print("MAX_BUSY=%.4f" % res["max_busy"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
