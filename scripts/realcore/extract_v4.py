#!/usr/bin/env python3
"""Extract the IAU-v3-campaign per-run summary row for the CACHE_GEM5_FINDINGS
§11 table.

Given a run tag (a dir under results/phase1_runs/), pull:
  - IOPS, Cycles_Per_IO, Completions, MMIO_Writes_Per_IO   (phase1_results.csv)
  - qid=1 median per-stage host CPU times                  (cycle_breakdown_*.csv)
  - dcache / L2 / icache overall miss rate                 (stats.txt, if preserved)

The phase1_results.csv aggregate stage columns are polluted by the slow qid=0
admin commands, so the per-stage medians here are recomputed over qid=1 (the
steady I/O path) only -- the honest number for the table.

Usage:
  python3 scripts/bigann/extract_v3camp.py <tag> [<tag> ...]
  python3 scripts/bigann/extract_v3camp.py --qid 1 v3camp_o3_mode0 v3camp_o3_iauv3
  python3 scripts/realcore/extract_v4.py --root results/realcore --subdir core0_qp1 <tag>
Options:
  --qid N        qid to take stage medians over (default 1)
  --root DIR     runs root (default results/phase1_runs)
  --subdir NAME  per-tag subdirectory (default core0_qp1)
"""
import csv
import glob
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNS = os.path.join(ROOT, "results", "phase1_runs")
SUBDIR = "core0_qp1"

# stage columns in cycle_breakdown_*.csv worth a qid-filtered median
STAGE_COLS = [
    "submit_ns", "completion_ns", "submit_preamble_ns", "tracker_alloc_ns",
    "addr_xlate_ns", "cmd_construct_ns", "fence_ns", "doorbell_ns",
    "cqe_detect_ns", "tracker_lookup_ns", "state_dealloc_ns",
    "lib_preamble_ns", "app_callback_ns",
]
# headline columns straight out of phase1_results.csv
RESULT_COLS = ["IOPS", "Cycles_Per_IO", "Completions", "MMIO_Writes_Per_IO"]
# cache controls out of stats.txt (proves a gain is not a cache artifact)
CACHE_STATS = {
    "dcache_miss": "system.cpu.dcache.overall_miss_rate::total",
    "l2_miss":     "system.l2.overall_miss_rate::total",
    "icache_miss": "system.cpu.icache.overall_miss_rate::total",
}


def _median(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def stage_medians(tag, qid):
    """Median of each STAGE_COL over rows with the given qid, across all
    cycle_breakdown_*.csv under the tag (QD sweeps produce one per s/q/r)."""
    pat = os.path.join(RUNS, tag, SUBDIR, "logs", "cycle_breakdown_*.csv")
    files = sorted(glob.glob(pat))
    cols = {c: [] for c in STAGE_COLS}
    n_rows = 0
    for fp in files:
        with open(fp) as fh:
            for row in csv.DictReader(fh):
                if str(row.get("qid", "")).strip() != str(qid):
                    continue
                n_rows += 1
                for c in STAGE_COLS:
                    if c in row and row[c] not in ("", None):
                        try:
                            cols[c].append(float(row[c]))
                        except ValueError:
                            pass
    return {c: _median(v) for c, v in cols.items()}, n_rows, len(files)


def result_row(tag):
    fp = os.path.join(RUNS, tag, SUBDIR, "phase1_results.csv")
    if not os.path.exists(fp):
        return None
    with open(fp) as fh:
        rows = list(csv.DictReader(fh))
    return rows[-1] if rows else None


def cache_stats(tag):
    # stats.txt is preserved at the tag root (cp -p after the run)
    for cand in (os.path.join(RUNS, tag, "stats.txt"),
                 os.path.join(RUNS, tag, SUBDIR, "stats.txt")):
        if os.path.exists(cand):
            wanted = {v: k for k, v in CACHE_STATS.items()}
            out = {}
            with open(cand) as fh:
                for line in fh:
                    parts = line.split()
                    if len(parts) >= 2 and parts[0] in wanted:
                        try:
                            out[wanted[parts[0]]] = float(parts[1])
                        except ValueError:
                            pass
            return out or None
    return None


def fmt(v, nd=1):
    if v is None:
        return "  n/a"
    return f"{v:.{nd}f}"


def report(tag, qid):
    print(f"\n===== {tag} (qid={qid} medians) =====")
    rr = result_row(tag)
    if rr is None:
        print("  !! no phase1_results.csv -- run missing or empty")
        return
    print(f"  IOPS={rr.get('IOPS','?')}  Cycles/IO={rr.get('Cycles_Per_IO','?')}"
          f"  Completions={rr.get('Completions','?')}"
          f"  MMIO_Writes/IO={rr.get('MMIO_Writes_Per_IO','?')}"
          f"  Polls={rr.get('Polls','?')}")
    sm, n_rows, n_files = stage_medians(tag, qid)
    print(f"  [stage medians over {n_rows} qid={qid} rows in {n_files} file(s)]")
    order = ["submit_ns", "tracker_alloc_ns", "addr_xlate_ns", "cmd_construct_ns",
             "fence_ns", "doorbell_ns", "completion_ns", "cqe_detect_ns",
             "tracker_lookup_ns", "state_dealloc_ns", "lib_preamble_ns",
             "app_callback_ns"]
    for c in order:
        print(f"    {c:20s} = {fmt(sm.get(c))}")
    cs = cache_stats(tag)
    if cs:
        print("  [cache controls] "
              + "  ".join(f"{k}={fmt(v,4)}" for k, v in cs.items()))
    else:
        print("  [cache controls] stats.txt not preserved at tag root")


def main():
    global RUNS, SUBDIR
    args = sys.argv[1:]
    qid = 1
    tags = []
    i = 0
    while i < len(args):
        if args[i] == "--qid":
            qid = int(args[i + 1]); i += 2
        elif args[i] == "--root":
            RUNS = os.path.abspath(args[i + 1]); i += 2
        elif args[i] == "--subdir":
            SUBDIR = args[i + 1]; i += 2
        else:
            tags.append(args[i]); i += 1
    if not tags:
        print(__doc__); sys.exit(1)
    for t in tags:
        report(t, qid)


if __name__ == "__main__":
    main()
