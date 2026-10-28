#!/usr/bin/env python3
"""cycles/IO, effective GHz and IPC from a `perf stat -x ';'` window file (spec §4 K).

Usage: perfstat_window.py FILE --iops X --window-s S
Prints one line: cycles_per_io=<v> ghz=<v> ipc=<v>  (NA where a counter is missing).
cycles are summed over every thread of the process, so cycles_per_io is core-cycles per IO.
"""
import argparse
import sys


def parse(path):
    vals = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            f = line.split(";")
            if len(f) < 3:
                continue
            ev = f[2].split(":")[0]
            try:
                vals[ev] = float(f[0])
            except ValueError:
                vals[ev] = None
    return vals


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--iops", type=float, required=True)
    ap.add_argument("--window-s", type=float, required=True)
    a = ap.parse_args(argv)
    v = parse(a.file)
    cyc, ins, tc = v.get("cycles"), v.get("instructions"), v.get("task-clock")
    if not cyc or not tc or a.iops <= 0:
        print("cycles_per_io=NA ghz=NA ipc=NA")
        return 0
    ipc = "%.3f" % (ins / cyc) if ins else "NA"
    print("cycles_per_io=%.2f ghz=%.4f ipc=%s" % (cyc / (a.iops * a.window_s), cyc / (tc * 1e6), ipc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
