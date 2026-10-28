#!/usr/bin/env python3
"""Spec §7.3 non-binding checks.

  point --busy B [--limit 0.70]          PASS iff B < limit (max device poll-group busy fraction)
  thread --iops X --active-pg N [--capacity 13690000] [--margin 1.3]
                                          PASS iff X / max(N,1) <= capacity / margin (capacity test, D3)
  plus1 --iops-n X --iops-n1 Y [--tol 0.02]
                                          PASS iff |Y/X - 1| < tol (one extra device core
                                          changes app IOPS by less than tol)
Prints NONBINDING_POINT=, NONBINDING_THREAD= or NONBINDING_PLUS1=PASS|FAIL; exit 0 on PASS, 1 on FAIL.
"""
import argparse
import sys


def point_ok(busy, limit=0.70):
    return busy < limit


def thread_ok(iops, active_pg, capacity=13690000.0, margin=1.3):
    per = iops / max(active_pg, 1)
    return per <= capacity / margin, per, capacity / margin


def plus1_ok(iops_n, iops_n1, tol=0.02):
    return iops_n > 0 and abs(iops_n1 / iops_n - 1.0) < tol


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("point"); p.add_argument("--busy", type=float, required=True); p.add_argument("--limit", type=float, default=0.70)
    t = sub.add_parser("thread"); t.add_argument("--iops", type=float, required=True); t.add_argument("--active-pg", type=int, required=True)
    t.add_argument("--capacity", type=float, default=13690000.0); t.add_argument("--margin", type=float, default=1.3)
    q = sub.add_parser("plus1"); q.add_argument("--iops-n", type=float, required=True); q.add_argument("--iops-n1", type=float, required=True); q.add_argument("--tol", type=float, default=0.02)
    a = ap.parse_args(argv)
    if a.cmd == "point":
        ok = point_ok(a.busy, a.limit)
        print("NONBINDING_POINT=%s busy=%.4f limit=%.2f" % ("PASS" if ok else "FAIL", a.busy, a.limit))
    elif a.cmd == "thread":
        ok, per, lim = thread_ok(a.iops, a.active_pg, a.capacity, a.margin)
        print("NONBINDING_THREAD=%s per_thread=%.0f limit=%.0f" % ("PASS" if ok else "FAIL", per, lim))
    else:
        ok = plus1_ok(a.iops_n, a.iops_n1, a.tol)
        print("NONBINDING_PLUS1=%s ratio=%.4f tol=%.3f" % ("PASS" if ok else "FAIL", a.iops_n1 / a.iops_n, a.tol))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
