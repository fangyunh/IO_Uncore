#!/usr/bin/env python3
"""IAU-next P6.0: SSD device classes for the target's fast-path model, calibrated as SwarmIO.

SwarmIO (Kim, Yeo, Rhu, arXiv:2604.06668; github.com/VIA-Research/SwarmIO at 25594a1a, GPL-2.0, nothing copied)
runs NVMeVirt's simple timing model with, from its scripts/load.sh,
    sched_delay = trunc(1000 / target_miops * insts) ns,   min_delay = target_latency - sched_delay,
and, from src/simple_timing_model.c, for a request of one I/O unit
    u = slba % insts;  busy[u] = max(now, busy[u]) + sched_delay;  due = busy[u] + min_delay.
The target's fp_schedule() (spdk/lib/nvmf/vfio_user_fastpath.c) gives the same due with IAU_DEV_CHANNELS = insts,
IAU_DEV_TMAX = cap / insts (one instance's rate, svc = 1 / TMAX = sched_delay), IAU_DEV_LMIN_NS = target_latency and
IAU_DEV_MODEL=ctrlr (every SQ of a drive shares its instances), for 4 KiB requests on the 4 KiB-block namespace,
when sched_delay <= target_latency. tests/realcore/test_vfio_user_fastpath.c case (m) checks it due for due.

Subcommands:
  env CLASS                                         the cell env string
  swarmio CLASS                                     SwarmIO's sched_delay and min_delay (ns)
  qd CLASS --fo N --rate R                          per-drive queue depth, P6.0 in-flight rule
  lint CELLS_CSV MAP_CSV [--arms ARMS_CSV] [--rate R]   cells carry their class exactly; arms set no class key
  calcheck CELLS_SUMMARY_CSV MAP_CSV                D4 verdicts from `analyze.py cells` output
"""
import argparse
import csv
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CSV = os.path.join(HERE, "devclasses.csv")
MAX_CHANNELS = 64               # FP_MAX_CHANNELS == DEVCFG_MAX_CHANNELS
SLACK = 1.4                     # in-flight rule: 1.4 x rate x Lmin
QD_STEP = 16
QD_MAX = 896                    # num_trackers at 1024 queue entries (S0/S1); v6 credits stay below 1024
CLASS_KEYS = ("IAU_DEV_MODEL", "IAU_DEV_CHANNELS", "IAU_DEV_TMAX", "IAU_DEV_LMIN_NS")
OVERRIDE = re.compile(r"^IAU_DEV_(MODEL|CHANNELS|TMAX|LMIN_NS)_C\d+$")
CAP_LO, CAP_HI = 0.97, 1.01     # D4: drive-bound IOPS / effective cap
LAT_LO_US, LAT_HI_US = 0.0, 5.0 # D4: QD1 p50 minus Lmin


class DevClass:
    def __init__(self, row):
        self.name = row["class"]
        self.cap_iops = int(row["cap_iops"])
        self.lmin_ns = int(row["lmin_ns"])
        self.insts = int(row["insts"])
        self.status = row["status"]
        self.source = row["source"]
        if not 1 <= self.insts <= MAX_CHANNELS:
            raise ValueError("%s: insts %d outside 1..%d" % (self.name, self.insts, MAX_CHANNELS))
        self.tmax = int(round(self.cap_iops / self.insts))
        self.cap_eff = self.tmax * self.insts
        if abs(self.cap_eff - self.cap_iops) > 1e-4 * self.cap_iops:
            raise ValueError("%s: cap %d not reachable with %d instances" % (self.name, self.cap_iops, self.insts))
        self.sched_ns = (1000 * self.insts * 10 ** 6) // self.cap_iops
        self.min_ns = self.lmin_ns - self.sched_ns
        if self.min_ns < 0:
            raise ValueError("%s: sched_delay %d ns > target_latency %d ns" % (self.name, self.sched_ns, self.lmin_ns))

    def env(self):
        return "IAU_DEV_MODEL=ctrlr;IAU_DEV_CHANNELS=%d;IAU_DEV_TMAX=%d;IAU_DEV_LMIN_NS=%d" % (
            self.insts, self.tmax, self.lmin_ns)

    def qd(self, fo, rate):
        lsec = self.lmin_ns * 1e-9
        if fo * self.cap_iops < rate:   # the drives bind first: saturate each one (knee cells)
            need = SLACK * self.cap_iops * lsec
        else:                           # the core binds: cover its rate across the drives
            need = SLACK * rate * lsec / fo
        q = int(math.ceil(round(need, 6) / QD_STEP)) * QD_STEP
        return max(QD_STEP, min(QD_MAX, q))


def read_csv(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def load_classes(path=DEFAULT_CSV):
    out = {}
    for r in read_csv(path):
        c = DevClass(r)
        if c.name in out:
            raise ValueError("duplicate class %s" % c.name)
        out[c.name] = c
    return out


def parse_env(s):
    d = {}
    for kv in (s or "").split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            d[k.strip()] = v.strip()
    return d


def lint(cells_path, map_path, classes, arms_path=None, rate=None):
    """Errors (strings): a mapped cell whose class keys differ from its class, carry a per-controller override,
    use a projection class, or (with rate) break the in-flight rule; an arm that sets a class key."""
    cells = {r["cell_id"]: r for r in read_csv(cells_path)}
    errs = []
    for m in read_csv(map_path):
        cell, name = m["cell"], m["class"]
        c = classes.get(name)
        if c is None:
            errs.append("%s: unknown class %s" % (cell, name))
            continue
        if c.status == "projection":
            errs.append("%s: class %s is a projection, not emulated" % (cell, name))
            continue
        if cell not in cells:
            errs.append("%s: not in %s" % (cell, cells_path))
            continue
        env = parse_env(cells[cell]["env"])
        want = parse_env(c.env())
        for k in CLASS_KEYS:
            if env.get(k) != want[k]:
                errs.append("%s: %s=%s, class %s needs %s" % (cell, k, env.get(k), name, want[k]))
        for k in sorted(env):
            if OVERRIDE.match(k):
                errs.append("%s: per-controller override %s breaks class %s" % (cell, k, name))
        if rate and m.get("kind", "run") == "run":
            want_qd = c.qd(int(cells[cell]["controllers"]), rate)
            if int(cells[cell]["qd"]) != want_qd:
                errs.append("%s: qd %s, the P6.0 rule gives %d" % (cell, cells[cell]["qd"], want_qd))
    if arms_path:
        for a in read_csv(arms_path):
            for k in sorted(parse_env(a.get("env"))):
                if k in CLASS_KEYS or OVERRIDE.match(k):
                    errs.append("arm %s sets %s (it would change the drive class)" % (a["arm_id"], k))
    return errs


def calcheck(summary_path, map_path, classes):
    """D4 verdicts: (cell, class, kind, PASS/FAIL, detail) per mapped cap or lat cell."""
    by_cell = {}
    for r in read_csv(summary_path):
        by_cell.setdefault(r["cell"], []).append(r)
    out = []
    for m in read_csv(map_path):
        cell, kind, c = m["cell"], m["kind"], classes[m["class"]]
        rs = by_cell.get(cell, [])
        if kind == "run":
            continue
        if not rs:
            out.append((cell, c.name, kind, "FAIL", "no rows in %s" % summary_path))
            continue
        if kind == "cap":
            best = max(rs, key=lambda r: float(r["iops_med"]))
            ratio = float(best["iops_med"]) / c.cap_eff
            ok = CAP_LO <= ratio <= CAP_HI
            detail = "arm=%s iops=%.0f cap=%d ratio=%.4f busy_max=%s" % (
                best["arm"], float(best["iops_med"]), c.cap_eff, ratio, best["busy_max"])
        elif kind == "lat":
            s1 = [r for r in rs if r["arm"] == "S1"]
            r = s1[0] if s1 else min(rs, key=lambda x: float(x["p50_med"]))
            over = float(r["p50_med"]) - c.lmin_ns / 1000.0
            ok = LAT_LO_US <= over <= LAT_HI_US
            detail = "arm=%s p50_us=%s lmin_us=%.1f over_us=%.2f" % (r["arm"], r["p50_med"], c.lmin_ns / 1000.0, over)
        else:
            ok, detail = False, "unknown kind %s" % kind
        out.append((cell, c.name, kind, "PASS" if ok else "FAIL", detail))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--classes", default=DEFAULT_CSV)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("env"); p.add_argument("cls")
    p = sp.add_parser("swarmio"); p.add_argument("cls")
    p = sp.add_parser("qd"); p.add_argument("cls"); p.add_argument("--fo", type=int, required=True)
    p.add_argument("--rate", type=float, required=True)
    p = sp.add_parser("lint"); p.add_argument("cells"); p.add_argument("map"); p.add_argument("--arms")
    p.add_argument("--rate", type=float)
    p = sp.add_parser("calcheck"); p.add_argument("summary"); p.add_argument("map")
    a = ap.parse_args(argv)
    classes = load_classes(a.classes)
    if a.cmd == "env":
        print(classes[a.cls].env())
    elif a.cmd == "swarmio":
        c = classes[a.cls]
        print("class=%s sched_delay_ns=%d min_delay_ns=%d cap_eff=%d" % (c.name, c.sched_ns, c.min_ns, c.cap_eff))
    elif a.cmd == "qd":
        print(classes[a.cls].qd(a.fo, a.rate))
    elif a.cmd == "lint":
        errs = lint(a.cells, a.map, classes, a.arms, a.rate)
        for e in errs:
            print("LINT " + e)
        print("DEVCLASS_LINT errors=%d" % len(errs))
        return 1 if errs else 0
    elif a.cmd == "calcheck":
        res = calcheck(a.summary, a.map, classes)
        for r in res:
            print("DEVCAL %s class=%s kind=%s %s %s" % r)
        nfail = sum(r[3] == "FAIL" for r in res)
        print("DEVCAL pass=%d fail=%d" % (len(res) - nfail, nfail))
        return 1 if nfail else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
