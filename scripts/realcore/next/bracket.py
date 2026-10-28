#!/usr/bin/env python3
"""Cross-campaign bracket check (IAU-next P3 gate): is an arm's median ns/IO in a new campaign within noise of
an arm of a reference campaign?

  bracket.py --base DIR --test DIR --pair BASE_ARM=TEST_ARM [--pair ...] [--cells A,B] [--out F]

Reads <dir>/cells.csv (analyze.py cells). For each pair and each cell (default: every cell of the BASE arm in
the base campaign, so a cell the test campaign lacks is reported missing; --cells restricts the list): the two
arms' per-repeat values are summarised as [ns_min, ns_med, ns_max] and compared with invstats.effect (floor 1 ns,
2x the larger spread); within iff the difference is not real. One BRACKET line per cell (with n per arm), then
BRACKET_SUMMARY within=N outside=M; exit 1 if any cell is outside or missing. Non-numeric values exit 2.
"""
import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "inv"))
from invstats import effect  # noqa: E402


def load(d):
    with open(os.path.join(d, "cells.csv")) as fh:
        return {(r["arm"], r["cell"]): r for r in csv.DictReader(fh)}


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--pair", action="append", required=True)
    ap.add_argument("--cells", default="")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    base, test = load(a.base), load(a.test)
    want = [c for c in a.cells.split(",") if c]
    rows, bad = [], 0
    for pr in a.pair:
        b_arm, sep, t_arm = pr.partition("=")
        if not sep or not b_arm or not t_arm:
            raise SystemExit("--pair must be BASE_ARM=TEST_ARM: %r" % pr)
        cells = want or sorted(c for (x, c) in base if x == b_arm)
        if not cells:
            print("BRACKET pair=%s missing in one campaign" % pr)
            bad += 1
            continue
        for c in cells:
            rb, rt = base.get((b_arm, c)), test.get((t_arm, c))
            if rb is None or rt is None:
                print("BRACKET pair=%s cell=%s missing" % (pr, c))
                bad += 1
                continue
            try:
                bv = [float(rb[k]) for k in ("ns_min", "ns_med", "ns_max")]
                tv = [float(rt[k]) for k in ("ns_min", "ns_med", "ns_max")]
            except (ValueError, KeyError, TypeError) as e:
                print("bracket.py: non-numeric or missing ns column for pair=%s cell=%s: %s" % (pr, c, e), file=sys.stderr)
                return 2
            e = effect(bv, tv)
            d, s, ok = e["delta"], e["spread"], not e["real"]
            bad += 0 if ok else 1
            rows.append(dict(base_arm=b_arm, test_arm=t_arm, cell=c, base_ns=rb["ns_med"], test_ns=rt["ns_med"],
                             delta_ns=round(d, 4), spread_ns=round(s, 4), within="yes" if ok else "no"))
            print("BRACKET base=%s test=%s cell=%s base_ns=%s test_ns=%s delta=%.4f spread=%.4f n_base=%s n_test=%s within=%s"
                  % (b_arm, t_arm, c, rb["ns_med"], rt["ns_med"], d, s, rb.get("n", "?"), rt.get("n", "?"),
                     "yes" if ok else "no"))
    if a.out:
        with open(a.out, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=["base_arm", "test_arm", "cell", "base_ns", "test_ns", "delta_ns",
                                               "spread_ns", "within"], lineterminator="\n")
            w.writeheader()
            w.writerows(rows)
    print("BRACKET_SUMMARY within=%d outside=%d" % (sum(r["within"] == "yes" for r in rows), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
