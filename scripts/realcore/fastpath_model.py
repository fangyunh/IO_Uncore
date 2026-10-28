#!/usr/bin/env python3
"""SimpleSSD fast-path device latency (gem5 configs/ssd/fast_ssd_cache.cfg on branch gem5-sim):

  target_time         = max(now, channel_next_free[ch]) + FastPathLmin
  channel_next_free[ch] = max(now, channel_next_free[ch]) + 1e12 / FastPathTmaxPerChannel

Evaluated offline for n I/Os arriving evenly at `iops` (deterministic arrivals).
policy 0 = round-robin channel counter, 1 = lba % channels (lbas in 4 KiB units).
Usage: fastpath_model.py --iops X [--n 200000] [--channels 32] [--tmax 250000]
                         [--lmin-us 1.0] [--policy 0|1] [--trace PATH]
"""
import argparse
import json
import struct
import sys


def simulate(iops, n, channels=32, tmax=250000.0, lmin_us=1.0, policy=0, lbas=None):
    service_us = 1e6 / tmax
    next_free = [0.0] * channels
    lat = []
    for i in range(n):
        now = i * 1e6 / iops
        ch = (i % channels) if policy == 0 else (lbas[i % len(lbas)] % channels)
        start = max(now, next_free[ch])
        next_free[ch] = start + service_us
        lat.append(start + lmin_us - now)
    lat.sort()
    return {"n": n, "mean_us": round(sum(lat) / n, 9), "p50_us": round(lat[n // 2], 9),
            "p99_us": round(lat[int(0.99 * (n - 1))], 9), "max_us": round(lat[-1], 9)}


def trace_lbas(path, limit):
    out = []
    with open(path, "rb") as fh:
        while len(out) < limit:
            b = fh.read(16)
            if len(b) < 16:
                break
            off, _sz, _op = struct.unpack("<QIB", b[:13])
            out.append(off // 4096)
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--iops", type=float, required=True)
    ap.add_argument("--n", type=int, default=200000)
    ap.add_argument("--channels", type=int, default=32)
    ap.add_argument("--tmax", type=float, default=250000.0)
    ap.add_argument("--lmin-us", type=float, default=1.0)
    ap.add_argument("--policy", type=int, default=0, choices=(0, 1))
    ap.add_argument("--trace")
    a = ap.parse_args(argv)
    lbas = trace_lbas(a.trace, a.n) if (a.policy == 1 and a.trace) else None
    if a.policy == 1 and not lbas:
        print("--policy 1 needs --trace", file=sys.stderr)
        return 2
    print(json.dumps(simulate(a.iops, a.n, a.channels, a.tmax, a.lmin_us, a.policy, lbas), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
