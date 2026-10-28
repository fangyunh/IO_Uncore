#!/usr/bin/env python3
"""Analysis for the IAU headroom investigation (spec 2026-10-13).

  analyze.py cells     <campaign_dir> [--out F]
  analyze.py compare   <campaign_dir> --base ARM --test ARM[,ARM...] [--ci] [--out F]
  analyze.py cpubound  <campaign_dir> [--spin-cpi C] [--out F]
  analyze.py published --map cpu_bound_map.csv --stage-cells F [F ...] [--out F]

A campaign dir holds points.csv (from gen_inv_points.py, with arm and cell columns) and
ledger.csv (from campaign.sh). Only status=ok rows count.
cells also reads each ok point's host log (<run_dir>/logs/run_<STEM>.log, check_l.py's STEM) for perf's idle_polls /
polls (check_l.host_idle_frac, A16): idle_med is its median over the arm-cell's repeats (rounded for display),
idle_n the repeats with poll counts, and bound = host when the unrounded median <= 0.05 (a host-cost ns/IO claim is
allowed), else latency (the ns/IO is an upper bound on host cost); unknown (idle_med empty) when idle_n == 0.
idle_polls / polls is NOT CPU idle. P4.12 (A16's source refinement): a worker-group arm-cell (SPDK_PERF_WORKER_GROUP=1)
whose every repeat with a host log carries perf's PERF_IDLE_TIME lines is labelled by the median of its time-based
idle_time_frac instead (idle_time_med, idle_src = time); a worker-group arm-cell without them (a perf from before P4.12)
keeps the poll-count label, idle_src = "poll-count (pre-P4.12)"; every other arm-cell idle_src = poll-count.
The compare --ci columns are a percentile bootstrap (10,000 resamples, seed 12345) 95% CI of the median
ns/IO difference, supplementary to the 2x-spread rule. With ~5 repeats per arm the median bootstrap is discrete and tends to
under-cover (coarse, optimistic); the 2x-spread rule remains the decision criterion. ns/IO = app_cores * 1e9 / IOPS.
cpubound expects arms named m<mode>_<level>. Device levels ch32 ch64 ch64t ch64tt are always required.
Spin levels, either set: sn20 sn40 (ch64t plus a TSC-timed spin, SPDK_INV_SPIN_NS=k). The TSC spin has a
resolution floor of about 20 ns, so the spec's k in {10,20} is scaled to {20,40}. The injected time per IO is
not k: it was calibrated in situ in the floor mode (pure CPU), giving x(20)=26.86 ns and x(40)=47.96 ns, which
already include the spin's fixed exit overhead (override with --spin-x "20=..,40=.."). slope is the
least-squares slope of median ns/IO vs x over (0, ns_ch64t), (x20, ns_sn20), (x40, ns_sn40), k=0 included;
rise20/rise40 are the per-level ratios (ns_snK - ns_ch64t)/x(K). No GHz needed; slope_kind=ns. Or s32 s64 (32/64 spin iterations; spin_cpi = cycles per iteration,
from T0.6; needs ghz in the ledger; slope_kind=cycles). The ns set wins when both are present.
"""
import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invstats as st  # noqa: E402
import check_l  # noqa: E402  (host_idle_frac, stem, HOST_BOUND_IDLE: one definition with check_l)

# predicted label only: the 2026-10-13 scratch study saw device limits at 82-88% per-SQ utilisation (spec §2 C1); 0.70 leaves margin
PREDICT_UTIL_LIMIT = 0.70
DEVICE_LEVELS = ("ch32", "ch64", "ch64t", "ch64tt")
NS_LEVELS = ("sn20", "sn40")              # TSC-timed spin levels k (ns requested) on top of ch64t
DEFAULT_SPIN_X = "20=26.86,40=47.96"      # in-situ floor-mode calibration: k -> actual injected ns per IO
CYC_LEVELS = ("s32", "s64")               # older cycle-count spin
ALL_LEVELS = DEVICE_LEVELS + NS_LEVELS + CYC_LEVELS


def cpu_count(s):
    n = 0
    for part in s.replace("+", ",").split(","):
        if "-" in part:
            a, b = part.split("-")
            if int(b) < int(a):
                raise ValueError("reversed cpu range %r in %r" % (part, s))
            n += int(b) - int(a) + 1
        elif part:
            n += 1
    if n == 0:
        raise ValueError("empty cpu list %r" % s)
    return n


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def host_lines(p, led):
    """The point's host log lines (check_l's STEM), None without a log."""
    rd = led.get("run_dir") or ""
    if not os.path.isabs(rd) or not all(p.get(k) for k in ("qd", "run_id")):
        return None
    log = os.path.join(rd, "logs", "run_%s.log" % check_l.stem(p["qd"], p["run_id"], p.get("lat") or "1"))
    return check_l.read_lines(log) if os.path.isfile(log) else None


def idle_frac(p, led):
    """perf's idle_polls / polls of the point's host log (A16), None without a log or poll counts."""
    hl = host_lines(p, led)
    return check_l.host_idle_frac(hl) if hl is not None else None


def idle_time(p, led):
    """P4.12: perf's time-based idle_time_frac of the point's host log (PERF_IDLE_TIME), None without one."""
    hl = host_lines(p, led)
    return check_l.perf_idle_time_frac(hl) if hl is not None else None


def load(cdir):
    with open(os.path.join(cdir, "points.csv")) as fh:
        pts = {r["point_id"]: r for r in csv.DictReader(fh)}
    ok = {}
    with open(os.path.join(cdir, "ledger.csv")) as fh:
        for r in csv.DictReader(fh):
            if r["status"] == "ok":
                ok[r["point_id"]] = r      # last ok row wins
    rows = []
    for r in ok.values():
        p = pts[r["point_id"]]
        iops = float(r["iops"])
        rows.append(dict(arm=p["arm"], cell=p["cell"], mode=p["mode"], iops=iops,
                         ns=st.ns_per_io(iops, cpu_count(p["app_cpus"])),
                         cyc=fnum(r.get("cycles_per_io")), ghz=fnum(r.get("ghz")),
                         mmio=fnum(r.get("mmio_per_io")), p50=fnum(r.get("p50_us")),
                         p99=fnum(r.get("p99_us")), busy=fnum(r.get("busy_max")), idle=idle_frac(p, r),
                         idle_time=idle_time(p, r),
                         wg=check_l.parse_env(p.get("env")).get("SPDK_PERF_WORKER_GROUP") == "1"))
    return rows


def group(rows):
    g = {}
    for r in rows:
        g.setdefault((r["arm"], r["cell"]), []).append(r)
    return g


def vals(rs, key):
    return [r[key] for r in rs if r[key] is not None]


def medv(rs, key):
    v = vals(rs, key)
    return round(st.med(v), 4) if v else ""


def write(rows, cols, out):
    fh = open(out, "w", newline="") if out else sys.stdout
    w = csv.DictWriter(fh, fieldnames=cols, lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    if out:
        fh.close()
        print("wrote %d rows -> %s" % (len(rows), out))


def cmd_cells(a):
    out = []
    for (arm, cell), rs in sorted(group(load(a.campaign_dir)).items()):
        ns = vals(rs, "ns")
        idle = vals(rs, "idle")
        tidle = vals(rs, "idle_time")
        wg = any(r["wg"] for r in rs)
        src = "poll-count" if not wg else "time" if tidle and len(tidle) >= len(idle) else "poll-count (pre-P4.12)"
        src_v = tidle if src == "time" else idle
        bound = "unknown" if not src_v else "host" if st.med(src_v) <= check_l.HOST_BOUND_IDLE else "latency"
        out.append(dict(arm=arm, cell=cell, n=len(rs), ns_med=round(st.med(ns), 4), ns_min=round(min(ns), 4),
                        ns_max=round(max(ns), 4), iops_med=medv(rs, "iops"), cyc_med=medv(rs, "cyc"),
                        ghz_med=medv(rs, "ghz"), mmio_med=medv(rs, "mmio"), p50_med=medv(rs, "p50"),
                        p99_med=medv(rs, "p99"),
                        busy_max=max(vals(rs, "busy")) if vals(rs, "busy") else "", idle_med=medv(rs, "idle"),
                        idle_n=len(idle), bound=bound, idle_time_med=medv(rs, "idle_time"), idle_src=src))
    write(out, ["arm", "cell", "n", "ns_med", "ns_min", "ns_max", "iops_med", "cyc_med", "ghz_med",
                "mmio_med", "p50_med", "p99_med", "busy_max", "idle_med", "idle_n", "bound", "idle_time_med",
                "idle_src"], a.out)


def cmd_compare(a):
    g = group(load(a.campaign_dir))
    cells = sorted({c for (_, c) in g})
    out = []
    for test in a.test.split(","):
        for cell in cells:
            b, t = g.get((a.base, cell)), g.get((test, cell))
            if not b or not t:
                continue
            e = st.effect(vals(b, "ns"), vals(t, "ns"))
            bc, tc = vals(b, "cyc"), vals(t, "cyc")
            row = dict(base=a.base, test=test, cell=cell, n_base=len(b), n_test=len(t),
                            base_ns=round(st.med(vals(b, "ns")), 4), test_ns=round(st.med(vals(t, "ns")), 4),
                            delta_ns=round(e["delta"], 4), spread_ns=round(e["spread"], 4), real=e["real"],
                            gain_pct=round((st.med(vals(b, "ns")) / st.med(vals(t, "ns")) - 1) * 100, 4),
                            delta_cyc=round(st.med(tc) - st.med(bc), 2) if bc and tc else "")
            if a.ci:
                lo, hi = st.bootstrap_ci(vals(b, "ns"), vals(t, "ns"))
                row.update(ci_lo_ns=round(lo, 4), ci_hi_ns=round(hi, 4))
            out.append(row)
    write(out, ["base", "test", "cell", "n_base", "n_test", "base_ns", "test_ns", "delta_ns", "spread_ns",
                "real", "gain_pct", "delta_cyc"] + (["ci_lo_ns", "ci_hi_ns"] if a.ci else []), a.out)


def parse_spin_x(s):
    d = {}
    for part in s.split(","):
        k, x = part.split("=")
        d["sn%d" % int(k)] = float(x)
    for L in NS_LEVELS:
        if L not in d:
            raise SystemExit("--spin-x lacks level %s" % L)
    return d


def cmd_cpubound(a):
    spin_x = parse_spin_x(a.spin_x)
    g = group(load(a.campaign_dir))
    out = []
    for cell in sorted({c for (_, c) in g}):
        for mode in ("0", "4"):
            lv = {L: g.get(("m%s_%s" % (mode, L), cell)) for L in ALL_LEVELS}
            if not any(lv.values()):
                continue
            dev_missing = [L for L in DEVICE_LEVELS if not lv[L]]
            ns_ok = all(lv[L] for L in NS_LEVELS)
            cyc_ok = all(lv[L] for L in CYC_LEVELS)
            if dev_missing or not (ns_ok or cyc_ok):
                miss = dev_missing + ([] if ns_ok or cyc_ok else
                                      ["spin(%s or %s)" % ("+".join(NS_LEVELS), "+".join(CYC_LEVELS))])
                print("warning: skipping cell %s mode %s, missing levels: %s" % (cell, mode, ",".join(miss)),
                      file=sys.stderr)
                continue
            ns = {L: vals(lv[L], "ns") for L in ALL_LEVELS if lv[L]}
            io = {L: vals(lv[L], "iops") for L in DEVICE_LEVELS}
            ghz_med = ""
            if ns_ok:
                kind = "ns"
                xs = [0.0] + [spin_x[L] for L in NS_LEVELS]
                sl = st.slope_fit(xs, [st.med(ns["ch64t"])] + [st.med(ns[L]) for L in NS_LEVELS])
            else:
                kind = "cycles"
                ghz = vals(lv["s32"], "ghz") + vals(lv["s64"], "ghz")
                if not ghz:
                    print("warning: skipping cell %s mode %s, ghz missing (run the campaign with RC_CAMPAIGN_PERF_STAT=2)"
                          % (cell, mode), file=sys.stderr)
                    continue
                ghz_med = st.med(ghz)
                sl = st.slope(ns["s32"], ns["s64"], 32.0 * a.spin_cpi, ghz_med)
            h32, h64, h64t = (st.headroom(io["ch32"], io["ch64"]), st.headroom(io["ch64"], io["ch64t"]),
                              st.headroom(io["ch64t"], io["ch64tt"]))
            # a real IOPS gain = a real ns/IO decrease between the two device levels (spec §4 K)
            r32, r64, r64t = (st.effect(ns["ch32"], ns["ch64"])["real"], st.effect(ns["ch64"], ns["ch64t"])["real"],
                              st.effect(ns["ch64t"], ns["ch64tt"])["real"])
            v64, v64t = st.verdict(h64, sl, headroom_real=r64), st.verdict(h64t, sl, headroom_real=r64t)

            def nsm(L):
                return round(st.med(ns[L]), 3) if L in ns else ""
            out.append(dict(cell=cell, mode=mode, ns_ch32=nsm("ch32"), ns_ch64=nsm("ch64"), ns_ch64t=nsm("ch64t"),
                            ns_ch64tt=nsm("ch64tt"), ns_s32=nsm("s32"), ns_s64=nsm("s64"),
                            ns_sn20=nsm("sn20"), ns_sn40=nsm("sn40"),
                            rise20=round((st.med(ns["sn20"]) - st.med(ns["ch64t"])) / spin_x["sn20"], 4) if ns_ok else "",
                            rise40=round((st.med(ns["sn40"]) - st.med(ns["ch64t"])) / spin_x["sn40"], 4) if ns_ok else "",
                            headroom_32to64=round(h32, 5), headroom_64to64t=round(h64, 5),
                            headroom_64tto64tt=round(h64t, 5),
                            real_32to64=r32, real_64to64t=r64, real_64tto64tt=r64t, slope=round(sl, 4), slope_kind=kind,
                            ghz=round(ghz_med, 4) if ghz_med != "" else "",
                            # ch32 is device-limited if raising 32->64 channels helps, else it inherits the ch64 verdict
                            verdict_ch32="device-limited" if (h32 >= st.HEADROOM_TOL and r32) else v64,
                            verdict_ch64=v64, verdict_ch64t=v64t))
    write(out, ["cell", "mode", "ns_ch32", "ns_ch64", "ns_ch64t", "ns_ch64tt", "ns_s32", "ns_s64",
                "ns_sn20", "ns_sn40", "rise20", "rise40", "headroom_32to64", "headroom_64to64t",
                "headroom_64tto64tt", "real_32to64", "real_64to64t", "real_64tto64tt", "slope", "slope_kind", "ghz", "verdict_ch32", "verdict_ch64",
                "verdict_ch64t"], a.out)


def cmd_published(a):
    with open(a.map) as fh:
        m = {(r["mode"], r["cell"]): r for r in csv.DictReader(fh)}
    out = []
    for f in a.stage_cells:
        with open(f) as fh:
            stage_rows = list(csv.DictReader(fh))
        for r in stage_rows:
            cores, qp, qd = int(r["app_cores"]), int(r["qpairs"]), int(r["qd"])
            u = st.per_sq_util(float(r["iops_med"]), cores, qp)
            key = (r["mode"], "q%dp%d" % (qd, qp))
            if key in m:
                label, basis = m[key]["verdict_ch32"], ("measured" if cores == 1 else "measured@1core")
            else:
                label, basis = ("device-limited" if u >= PREDICT_UTIL_LIMIT else "cpu-bound"), "predicted"
            out.append(dict(source=os.path.relpath(f), variant=r.get("variant", ""), mode=r["mode"],
                            placement=r.get("placement", ""), compute=r.get("compute", ""), app_cores=cores,
                            qd=qd, qpairs=qp, iops_med=r["iops_med"], per_sq_util=round(u, 3),
                            label=label, basis=basis))
    write(out, ["source", "variant", "mode", "placement", "compute", "app_cores", "qd", "qpairs", "iops_med",
                "per_sq_util", "label", "basis"], a.out)


def main(argv):
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("cells"); p.add_argument("campaign_dir"); p.add_argument("--out")
    p = sp.add_parser("compare"); p.add_argument("campaign_dir"); p.add_argument("--base", required=True)
    p.add_argument("--test", required=True); p.add_argument("--out")
    p.add_argument("--ci", action="store_true")
    p = sp.add_parser("cpubound"); p.add_argument("campaign_dir"); p.add_argument("--spin-cpi", type=float, default=1.0)
    p.add_argument("--spin-x", default=DEFAULT_SPIN_X)
    p.add_argument("--out")
    p = sp.add_parser("published"); p.add_argument("--map", required=True)
    p.add_argument("--stage-cells", nargs="+", required=True); p.add_argument("--out")
    a = ap.parse_args(argv)
    dict(cells=cmd_cells, compare=cmd_compare, cpubound=cmd_cpubound, published=cmd_published)[a.cmd](a)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
