#!/usr/bin/env python3
"""Record which device CPU serves each app qpair of a run.

  qpair_placement.py <run_dir> [--stem S]

Joins, per stem, logs/run_<stem>.log (QPAIR_MAP lcore=.. qid=..),
logs/qpairs_<stem>.json (nvmf_subsystem_get_qpairs: qid -> poll-group thread; qid 0 skipped) and
logs/devstats_a_<stem>.json (thread name -> cpumask hex -> CPU index).
When logs/reactors_<stem>.json (framework_get_reactors) exists it is authoritative for thread -> CPU,
because poll-group threads carry the whole target cpumask, not a single CPU; the single-bit devstats
cpumask is the fallback.
Socket of a CPU: 0-7 and 16-23 -> 0, 8-15 and 24-31 -> 1.
Prints one line per stem:
  PLACEMENT stem=<s> pairs=<n> cross_socket=<k> max_qpairs_per_thread=<m> map=<lcore:qid@cpu,...>
Exit 0 ok, 2 if a qid is missing from either side.
"""
import argparse
import glob
import json
import os
import re
import sys


def socket_of(cpu):
    return 0 if (cpu % 16) < 8 else 1


def mask_cpus(mask):
    v = int(mask, 16)
    return [i for i in range(v.bit_length()) if (v >> i) & 1]


def one(logs, stem):
    pairs = []
    with open(os.path.join(logs, "run_%s.log" % stem)) as fh:
        for line in fh:
            m = re.match(r"QPAIR_MAP lcore=(\d+) qid=(\d+)", line)
            if m:
                pairs.append((int(m.group(1)), int(m.group(2))))
    qdata = json.load(open(os.path.join(logs, "qpairs_%s.json" % stem)))
    if isinstance(qdata, dict):
        qdata = qdata.get("qpairs", [])
    qthread = {q["qid"]: q["thread"] for q in qdata if q.get("qid") != 0}
    dev = json.load(open(os.path.join(logs, "devstats_a_%s.json" % stem)))
    tcpu = {}
    rpath = os.path.join(logs, "reactors_%s.json" % stem)
    if os.path.exists(rpath):
        try:
            for r in json.load(open(rpath)).get("reactors", []):
                for t in r.get("lw_threads", []):
                    tcpu[t["name"]] = r["lcore"]
        except (ValueError, KeyError):
            tcpu = {}
    for t in dev.get("threads", []):
        cpus = mask_cpus(t["cpumask"])
        if len(cpus) == 1 and t["name"] not in tcpu:
            tcpu[t["name"]] = cpus[0]
    parts, cross, per_thread = [], 0, {}
    for lcore, qid in pairs:
        th = qthread.get(qid)
        if th is None or th not in tcpu:
            print("qpair_placement: stem=%s qid=%d lcore=%d missing from qpairs/devstats" % (stem, qid, lcore),
                  file=sys.stderr)
            return None
        cpu = tcpu[th]
        if socket_of(cpu) != socket_of(lcore):
            cross += 1
        per_thread[th] = per_thread.get(th, 0) + 1
        parts.append("%d:%d@%d" % (lcore, qid, cpu))
    return "PLACEMENT stem=%s pairs=%d cross_socket=%d max_qpairs_per_thread=%d map=%s" % (
        stem, len(pairs), cross, max(per_thread.values()) if per_thread else 0, ",".join(parts))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--stem")
    a = ap.parse_args(argv)
    logs = os.path.join(a.run_dir, "logs")
    if a.stem:
        stems = [a.stem]
    else:
        stems = sorted(re.match(r"qpairs_(.*)\.json$", os.path.basename(p)).group(1)
                       for p in glob.glob(os.path.join(logs, "qpairs_*.json")))
    rc = 0
    for s in stems:
        try:
            line = one(logs, s)
        except (OSError, ValueError, KeyError) as e:
            print("qpair_placement: stem=%s: %s" % (s, e), file=sys.stderr)
            line = None
        if line is None:
            rc = 2
        else:
            print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
