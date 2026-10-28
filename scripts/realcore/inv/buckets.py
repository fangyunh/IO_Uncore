#!/usr/bin/env python3
"""Per-IO bucket attribution (spec §5.3) from `perf report --stdio --sort sym` output, plus per-IO
PMU events from `perf stat -x ';'` files.

Usage: buckets.py SYMS.txt --ns-per-io X [--stat F [F ...] --ios-in-window N] [--out F]
Output rows: kind=bucket (name, share_pct, ns_per_io) and kind=event (name, count, per_io).
The first matching bucket wins (the specific inv_* lever symbols sit in the transport buckets, which precede the generic app ^inv_ rule; perf-side hooks inv_spin/inv_floor/inv_trace_fast and nvme_thin_* stay app); [k] samples -> kernel; unmatched -> other.
Bucket ns are share x --ns-per-io, so they sum to ns/IO by construction (the 5% check is on the share
sum). --ns-per-io should come from a clean campaign run, not from the profiled run (perf perturbs it).
GCC clone suffixes (.part.N, .isra.N, .constprop.N, .cold, .lto_priv.N) are stripped before matching.
"""
import argparse
import csv
import re
import sys

BUCKETS = [
    ("poll", [r"poll_group_process_completions$", r"^spdk_nvme_qpair_process_completions$",
              r"^nvme_transport_qpair_process_completions$", r"^nvme_check_io$",
              r"^_nvme_qpair_complete_abort_queued_reqs$", r"^nvme_qpair_resubmit_requests$"]),
    ("transport_completion", [r"^nvme_pcie_qpair_process_completions$", r"^nvme_pcie_qpair_complete_tracker$",
                              r"^nvme_pcie_qpair_check_timeout$",
                              r"^inv_process_completions$", r"^inv_completions_trampoline$", r"^inv_records",
                              r"^inv_cqes", r"^inv_rec_pop$", r"^inv_thin_push$", r"^inv_trfree_c"]),
    ("transport_submit", [r"^nvme_pcie_qpair_submit_", r"^nvme_pcie_qpair_build_", r"^nvme_pcie_prp_list_append$",
                          r"^nvme_pcie_qpair_ring_sq_doorbell$", r"^spdk_nvme_inv_thin_read$",
                          r"^inv_trfree_submit$", r"^inv_rec16_thin_first$", r"^inv_thin_write_sqe$",
                          r"^inv_submit_lever$", r"^inv_desc_submit$", r"^inv_write_desc$",
                          r"^nvme_pcie_(copy_command|vtophys|qpair_insert_pending)", r"^spdk_vtophys$",
                          r"^spdk_mem_map_translate$"]),
    ("generic", [r"^spdk_nvme_ns_cmd_", r"^_nvme_ns_cmd_", r"^nvme_ns_cmd_", r"^nvme_allocate_request",
                 r"^_nvme_qpair_submit_request$", r"^nvme_qpair_submit_request$",
                 r"^nvme_transport_qpair_submit_request$", r"^nvme_free_request$", r"^nvme_complete_request$", r"^nvme_qpair_check_enabled$"]),
    ("app", [r"^submit_single_io$", r"^task_complete$", r"^io_complete$", r"^work_fn$", r"^spdk_get_ticks$",
             r"^ann_compute_kernel$", r"^nvme_submit_io$", r"^inv_", r"^nvme_thin_"]),
]
LINE = re.compile(r"^\s*([0-9.]+)%\s+\[(.)\]\s+(\S+)")


CLONE = re.compile(r"\.(part|isra|constprop|cold|lto_priv)(\.\d+)*$")


def strip_clone(sym):
    while True:
        n = CLONE.sub("", sym)
        if n == sym:
            return sym
        sym = n


def bucket_of(kind, sym):
    sym = strip_clone(sym)
    if kind == "k":
        return "kernel"
    for name, pats in BUCKETS:
        if any(re.search(p, sym) for p in pats):
            return name
    return "other"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("syms")
    ap.add_argument("--ns-per-io", type=float, required=True)
    ap.add_argument("--stat", nargs="*", default=[])
    ap.add_argument("--ios-in-window", type=float)
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    share = {}
    other = {}
    with open(a.syms) as fh:
        for line in fh:
            m = LINE.match(line)
            if m:
                b = bucket_of(m.group(2), m.group(3))
                share[b] = share.get(b, 0.0) + float(m.group(1))
                if b == "other":
                    other[m.group(3)] = other.get(m.group(3), 0.0) + float(m.group(1))
    rows = [dict(kind="bucket", name=b, share_pct=round(share.get(b, 0.0), 3),
                 ns_per_io=round(share.get(b, 0.0) / 100.0 * a.ns_per_io, 3), count="", per_io="")
            for b in [n for n, _ in BUCKETS] + ["kernel", "other"]]
    for f in a.stat:
        if not a.ios_in_window:
            raise SystemExit("--stat needs --ios-in-window")
        with open(f) as fh:
            for line in fh:
                p = line.strip().split(";")
                if line.startswith("#") or len(p) < 3:
                    continue
                try:
                    v = float(p[0])
                except ValueError:
                    print("WARNING: event %s in %s: %s" % (p[2], f, p[0]), file=sys.stderr)
                    continue
                rows.append(dict(kind="event", name=p[2], share_pct="", ns_per_io="", count=v,
                                 per_io=round(v / a.ios_in_window, 4)))
    total = sum(share.values())
    if abs(total - 100.0) > 5.0:
        print("WARNING: shares sum to %.1f%%" % total, file=sys.stderr)
    if share.get("other", 0.0) > 5.0:
        top = sorted(other.items(), key=lambda kv: -kv[1])[:5]
        print("WARNING: other=%.1f%%; top: %s" % (share["other"], ", ".join("%s %.2f%%" % kv for kv in top)),
              file=sys.stderr)
    fh = open(a.out, "w", newline="") if a.out else sys.stdout
    try:
        w = csv.DictWriter(fh, fieldnames=["kind", "name", "share_pct", "ns_per_io", "count", "per_io"],
                           lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    finally:
        if a.out:
            fh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
