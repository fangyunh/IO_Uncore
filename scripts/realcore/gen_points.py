#!/usr/bin/env python3
"""Generate a points CSV (stdout) for one real-core campaign.

  stage2                                   device capacity (mode 0, lean)
  stage3 [--dev-cpus 4-5]                  delay-bdev timing calibration (mode 0, instr)
  stage5 --dev-cpus D                      1 app core: modes x QD x placement x variant
  stage6 --dev-per-app K                   scaling: app cores {1,2,4,8} x qpairs {1,2} x QD x modes
                                           + "_plus1" rows (one extra device core, 2 qpairs per app core, QD128) for §7.3 check 2,
                                           only where the app socket's device pool has a spare CPU
  stage7 --dev-cpus D                      co-resident compute sweep
Common: --bdev null|delay|fastpath (default fastpath) --lat-us N (stages 5-7; set from the Stage 3 decision in the ledger)

CPU lists use '+' instead of ',' inside a field.
"""
import argparse
import csv
import sys

COLS = ["point_id", "tag", "variant", "mode", "app_cpus", "dev_cpus", "qd", "qpairs", "bdev",
        "lat_us", "placement", "compute", "run_id", "warmup", "time", "lat"]
RUNS = (1, 2, 3)
# Device-core pools: socket 0 physical 4-7 then their HT siblings 20-23; socket 1 likewise.
POOL = {0: [4, 5, 6, 7, 20, 21, 22, 23], 1: [12, 13, 14, 15, 28, 29, 30, 31]}
APP_SETS = {1: ("0", [(0, 1)]), 2: ("0-1", [(0, 2)]), 4: ("0-3", [(0, 4)]),
            8: ("0-3+8-11", [(0, 4), (1, 4)])}


def cpus(lst):
    return "+".join(str(c) for c in lst)


def dev_for(app_n, k, extra=0):
    """Device CPUs for app_n app cores at k device cores per app core, on the
    same socket(s) as the app cores. Returns None if a socket pool is too small."""
    _, per_socket = APP_SETS[app_n]
    out = []
    for i, (sock, n) in enumerate(per_socket):
        want = k * n + (extra if i == 0 else 0)
        if want > len(POOL[sock]):
            return None
        out += POOL[sock][:want]
    return cpus(out)


def row(rows, tag, variant, mode, app, dev, qd, qp, placement, compute=0, bdev="fastpath",
        lat_us=1, run_id=1, warmup=5, time=20, lat=1):
    rows.append(dict(point_id="p%04d" % len(rows), tag=tag, variant=variant, mode=mode,
                     app_cpus=app, dev_cpus=dev, qd=qd, qpairs=qp, bdev=bdev, lat_us=lat_us,
                     placement=placement, compute=compute, run_id=run_id, warmup=warmup, time=time, lat=lat))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("campaign", choices=["stage2", "stage3", "stage5", "stage6", "stage7"])
    ap.add_argument("--dev-cpus", default="4-5")
    ap.add_argument("--dev-per-app", type=int, default=2)
    ap.add_argument("--bdev", default="fastpath", choices=["null", "delay", "fastpath"])
    ap.add_argument("--lat-us", type=int, default=1)
    ap.add_argument("--tag-prefix", default=None,
                    help="tag prefix (default s2cap/s3cal/s5/s6/s7 per campaign); tags are <prefix>_<rest>")
    ap.add_argument("--modes", default="0,4", help="comma list of modes for stage5/6/7")
    ap.add_argument("--dev-cpus2", default="4-5", help="stage5 device CPUs for the 2-qpair cells")
    a = ap.parse_args(argv)
    if a.tag_prefix is None:
        a.tag_prefix = dict(stage2="s2cap", stage3="s3cal", stage5="s5", stage6="s6",
                            stage7="s7")[a.campaign]
    rows = []
    bd = dict(bdev=a.bdev, lat_us=a.lat_us)
    modes = [int(m) for m in a.modes.split(",")]
    for m in modes:
        if m not in (0, 4):
            raise SystemExit("--modes entries must be 0 or 4")
    pre = a.tag_prefix
    if a.campaign == "stage2":
        for dev, qp in (("4", 1), ("4-5", 1), ("4-7", 1), ("4-5", 2), ("4-7", 4)):
            for r in RUNS:
                row(rows, pre + "_d%s_qp%d" % (dev.replace("-", "to"), qp), "lean", 0, "0", dev, 128, qp,
                    "same-socket", run_id=r, **bd)
    elif a.campaign == "stage3":
        for qd in (16, 128):
            for r in RUNS:
                row(rows, pre + "_m0", "instr", 0, "0", a.dev_cpus, qd, 1, "same-socket",
                    bdev="delay", run_id=r)
    elif a.campaign == "stage5":
        for variant in ("lean", "instr"):
            for mode in modes:
                for placement, dev in (("same-socket", a.dev_cpus), ("sibling", "16")):
                    for qd in (16, 32, 64, 128, 256):
                        for r in RUNS:
                            row(rows, pre + "_%s_m%d_%s" % (variant, mode, placement), variant, mode, "0",
                                dev, qd, 1, placement, run_id=r, **bd)
        for mode in modes:
            for r in RUNS:
                row(rows, pre + "_lean_m%d_qp2" % mode, "lean", mode, "0", a.dev_cpus2, 128, 2,
                    "same-socket", run_id=r, **bd)
            for r in RUNS:
                row(rows, pre + "_lean_m%d_nolat" % mode, "lean", mode, "0", a.dev_cpus, 128, 1,
                    "same-socket", run_id=r, lat=0, **bd)
    elif a.campaign == "stage6":
        for app_n in (1, 2, 4, 8):
            app = APP_SETS[app_n][0]
            placement = "cross-socket" if app_n == 8 else "same-socket"
            dev = dev_for(app_n, a.dev_per_app)
            if dev is None:
                raise SystemExit("device pool exhausted for %d app cores at %d per app core"
                                 % (app_n, a.dev_per_app))
            for qp in (1, 2):
                for qd in (32, 128, 256):
                    for mode in modes:
                        for r in RUNS:
                            row(rows, pre + "_m%d_a%d_qp%d" % (mode, app_n, qp), "lean", mode, app,
                                dev, qd, qp, placement, run_id=r, **bd)
            dev_plus = dev_for(app_n, a.dev_per_app, extra=1)
            if dev_plus is None:
                print("gen_points: plus1 skipped for %d app cores (device pool full); "
                      "the busy < 0.70 criterion alone applies there" % app_n, file=sys.stderr)
                continue
            for mode in modes:
                for r in RUNS:
                    row(rows, pre + "_m%d_a%d_qp2_plus1" % (mode, app_n), "lean", mode, app,
                        dev_plus, 128, 2, placement, run_id=r, **bd)
    elif a.campaign == "stage7":
        for compute in (0, 1, 2, 4, 8, 16, 32):
            for mode in modes:
                for r in RUNS:
                    row(rows, pre + "_m%d_c%d" % (mode, compute), "lean", mode, "0", a.dev_cpus, 128, 1,
                        "same-socket", compute=compute, run_id=r, **bd)
    w = csv.DictWriter(sys.stdout, fieldnames=COLS, lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
