#!/usr/bin/env python3
"""Build one phase1_results.csv row (the same 38 columns that the gem5 guest
script scripts/bigann/phase1_trace_replay.sh on branch gem5-sim writes) from a host-native
spdk_nvme_perf run.

Multi-core aggregation (the guest script assumed one core): IOPS comes from the
"Total" line; p50 = mean of the per-core p50 lines; p99 and p99.9 = max of the
per-core lines; pcie_stats counters and completion_calls are summed over all
lines; the completions-per-poll histogram is the first one printed.
Cycle-breakdown columns are means over all rows, like the guest script.

Prints one line: iops=<v> p50_us=<v> p99_us=<v> mmio_per_io=<v>
Exit 1 if the perf log has no "Total" line.
"""
import argparse
import csv
import os
import re
import sys

HEADER = [
    "QD", "Qpairs", "IO_Size", "Run_ID", "IOPS", "Cycles", "Instructions", "LLC_Misses",
    "Cycles_Per_IO", "Instr_Per_IO", "LLC_Misses_Per_IO", "p50_Latency", "p99_Latency",
    "p99.9_Latency", "Polls", "Completions", "Scans_Per_Completion", "Completions_Per_Call",
    "MMIO_Writes_Per_IO", "Completions_Per_Poll_Hist", "Submit_Logic_ns", "Completion_Logic_ns",
    "Submit_Preamble_ns", "Tracker_Alloc_ns", "Addr_Xlate_ns", "Cmd_Construct_ns", "Fence_ns",
    "Doorbell_ns", "CQE_Detect_ns", "Tracker_Lookup_ns", "State_Dealloc_ns",
    "State_Dealloc_Library_ns", "State_Dealloc_Callback_ns", "State_Dealloc_Total_ns",
    "Trace_Source", "Trace_Entries", "Core_Count", "Core_Mask",
]
CYCLE_COLS = [
    "submit_ns", "completion_ns", "submit_preamble_ns", "tracker_alloc_ns", "addr_xlate_ns",
    "cmd_construct_ns", "fence_ns", "doorbell_ns", "cqe_detect_ns", "tracker_lookup_ns",
    "state_dealloc_ns", "state_dealloc_library_ns", "state_dealloc_callback_ns",
    "state_dealloc_total_ns",
]
NUM = re.compile(r"^-?[0-9][0-9.]*$")


def fmt(x):
    s = "%.6f" % x
    return s.rstrip("0").rstrip(".") if "." in s else s


def parse_iops(text):
    for line in text.splitlines():
        if line.startswith("Total"):
            for tok in line.split():
                if NUM.match(tok):
                    return float(tok)
    return None


def parse_pct(text, label):
    vals = []
    for line in text.splitlines():
        if label in line:
            parts = line.split()
            if len(parts) >= 3:
                tok = parts[2]
                if tok.endswith("us"):
                    tok = tok[:-2]
                try:
                    vals.append(float(tok))
                except ValueError:
                    pass
    return vals


def parse_transport(text):
    tot = {"polls": 0, "completions": 0, "sq_mmio": 0, "cq_mmio": 0}
    calls, seen_calls, hist = 0, False, ""
    for line in text.splitlines():
        if "pcie_stats:" in line:
            for k in tot:
                m = re.search(r"(?<![a-z_])%s=(\d+)" % k, line)
                if m:
                    tot[k] += int(m.group(1))
        elif "completion_calls:" in line:
            m = re.search(r"completion_calls:\s*([\d,]+)", line)
            if m:
                calls += int(m.group(1).replace(",", ""))
                seen_calls = True
        elif "completions_per_poll_hist:" in line and not hist:
            hist = line.split(":", 1)[1].strip()
    return tot, (calls if seen_calls else tot["polls"]), hist


def parse_perf_stat(path):
    out = {"cycles": 0.0, "instructions": 0.0, "LLC-load-misses": 0.0}
    if not path or not os.path.exists(path):
        return out
    with open(path) as fh:
        for line in fh:
            f = line.strip().split(";")
            if len(f) < 3:
                continue
            ev = f[2]
            for k in out:
                if ev == k or ev.startswith(k + ":") or ev.endswith("/" + k + "/"):
                    try:
                        out[k] = float(f[0])
                    except ValueError:
                        out[k] = 0.0
    return out


def cycle_means(path):
    if not path or not os.path.exists(path):
        return [0.0] * len(CYCLE_COLS)
    sums, n = [0.0] * len(CYCLE_COLS), 0
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            try:
                vals = [float(row.get(c) or 0) for c in CYCLE_COLS]
            except ValueError:
                continue
            sums = [a + b for a, b in zip(sums, vals)]
            n += 1
    return [s / n for s in sums] if n else [0.0] * len(CYCLE_COLS)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--perf-log", required=True)
    ap.add_argument("--cycle-csv")
    ap.add_argument("--perf-stat")
    for k in ("--qd", "--qpairs", "--io-size", "--run-id", "--core-count"):
        ap.add_argument(k, required=True, type=int)
    ap.add_argument("--duration", required=True, type=float)
    ap.add_argument("--trace", required=True)
    ap.add_argument("--core-mask", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    with open(a.perf_log, errors="replace") as fh:
        text = fh.read()
    iops = parse_iops(text)
    if iops is None:
        print("make_row: no 'Total' line in %s" % a.perf_log, file=sys.stderr)
        return 1
    p50s, p99s, p999s = (parse_pct(text, l) for l in ("50.00000%", "99.00000%", "99.90000%"))
    p50 = sum(p50s) / len(p50s) if p50s else 0.0
    p99 = max(p99s) if p99s else 0.0
    p999 = max(p999s) if p999s else 0.0
    tr, calls, hist = parse_transport(text)
    ps = parse_perf_stat(a.perf_stat)
    total_ios = iops * a.duration
    per = (lambda v: v / total_ios) if total_ios > 0 else (lambda v: 0.0)
    comps = tr["completions"]
    # both counters span warmup + run (review V1-01)
    mmio = fmt((tr["sq_mmio"] + tr["cq_mmio"]) / comps if comps else 0.0)
    trace_entries = os.path.getsize(a.trace) // 16 if os.path.exists(a.trace) else 0

    row = [a.qd, a.qpairs, a.io_size, a.run_id, fmt(iops),
           fmt(ps["cycles"]), fmt(ps["instructions"]), fmt(ps["LLC-load-misses"]),
           fmt(per(ps["cycles"])), fmt(per(ps["instructions"])), fmt(per(ps["LLC-load-misses"])),
           fmt(p50), fmt(p99), fmt(p999), tr["polls"], comps,
           fmt(calls / comps if comps else 0.0), fmt(comps / calls if calls else 0.0),
           mmio, hist]
    row += [fmt(v) for v in cycle_means(a.cycle_csv)]
    row += [os.path.basename(a.trace), trace_entries, a.core_count, a.core_mask]

    new = not os.path.exists(a.out) or os.path.getsize(a.out) == 0
    with open(a.out, "a", newline="") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(HEADER)
        w.writerow(row)
    print("iops=%s p50_us=%s p99_us=%s mmio_per_io=%s"
          % (fmt(iops), fmt(p50), fmt(p99), mmio))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
