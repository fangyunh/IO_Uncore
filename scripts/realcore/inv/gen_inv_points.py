#!/usr/bin/env python3
"""Points CSV (stdout) for one investigation campaign: arms x cells x repeats, interleaved.

Usage: gen_inv_points.py --arms ARMS.csv --cells CELLS.csv --repeats N --prefix P [--warmup 5] [--time 20]
ARMS.csv : arm_id,variant,tgt_variant,mode(0|4|5|6),env,trace
CELLS.csv: cell_id,app_cpus,dev_cpus,qd,qpairs,controllers,compute,lat,placement,env[,workload,rwmix,io_size,engine_cpus]
          (optional workload,rwmix,io_size default randread,100,4096; non-default needs arm trace=none)
          engine_cpus (IAU-next P3): CPU list of the IAU-engine threads, used by mode 5 and mode 6 arms (required
          there), ignored by mode 0/4, disjoint from app_cpus and dev_cpus.
Mode 6 (IAU-next P4.6) = target mode 5 + host SPDK_IAU_TILE=1. env entries: the ENV_PREFIXES, plus the v6 host knobs by
exact name (ENV_NAMES); SPDK_IAU_TILE and SPDK_IAU_ENGINE are refused (the mode sets them). After the cell/arm merge a
point is refused when a TILE_ONLY knob meets a mode other than 6 (a mistyped I1v6n48 mode would run I1) or SPDK_IAU_S2
a mode other than 0. The host library takes SPDK_IAU_GROUP / _FAST / _S2 since P4.8 / P4.9 / P4.11.
A non-zero SPDK_IAU_DB_BATCH (P4.8e, the group pass's doorbell chunk) is refused without SPDK_IAU_GROUP=1 (the host
library would abort the point); 0 is allowed anywhere on mode 6 (a no-op, decision m3).
Order: for each cell, for each repeat r, every arm once, rotated by (r-1)*step with
step = max(1, n_arms // repeats), so the first slot is spread over the arm list (2 arms: A B, B A, A B)
and no arm always runs first (spec §4 K). env = cell env updated by arm env (arm wins). CPU lists use '+' for ','.
"""
import argparse
import csv
import re
import sys

COLS = ["point_id", "tag", "variant", "mode", "app_cpus", "dev_cpus", "qd", "qpairs", "bdev", "lat_us",
        "placement", "compute", "run_id", "warmup", "time", "lat", "env", "controllers", "tgt_variant",
        "trace", "arm", "cell", "workload", "rwmix", "io_size", "engine_cpus"]
WORKLOADS = ("randread", "randwrite", "randrw")
ENV_PREFIXES = ("IAU_DEV_", "IAU_CQ_", "IAU_INV_", "SPDK_INV_", "SPDK_PERF_", "SPDK_MD_", "IAU_ENG_FAULT_")
TILE_ONLY = ("SPDK_IAU_GROUP", "SPDK_IAU_FAST", "SPDK_IAU_RINGS", "SPDK_IAU_RING_ENTRIES", "SPDK_IAU_DESC_STRIDE",
           "SPDK_IAU_RCONS", "SPDK_IAU_DB_BATCH")   # mode 6 only (mirrors check_l TILE_ONLY_KNOBS, lib.sh rc_export_inv_env)
S2_KNOB, S2_MODE = "SPDK_IAU_S2", "0"   # the S2 ablation (P4.11): mode 0 only
DB_BATCH_KNOB = "SPDK_IAU_DB_BATCH"     # P4.8e: non-zero only with SPDK_IAU_GROUP=1
DB_COST_KNOBS = ("SPDK_DB_COST_NS", "SPDK_DB_COST_IAU_NS")   # P11 (review M2): doorbell-cost spin, any mode
S2_WB_KNOB = "SPDK_IAU_S2_WB"             # P11 (review M3): S2 write-back SQE stores, mode 0 with SPDK_IAU_S2=1
ENV_NAMES = TILE_ONLY + (S2_KNOB,)        # IAU tile host knobs (P4.6), exact names
REMOVED_KNOBS = ("SPDK_PERF_FAULT_RAW_INVAL_EVERY",)   # P4.12b: removed from perf (never reached the payload builder)
SPILL_KNOB = "IAU_TILE_CTX_SPILL"         # P4.10b (A19): the target ctx-spill emulation; 0|1, 1 on mode 6 only (mirrors check_l)
MODE_SET = ("SPDK_IAU_TILE", "SPDK_IAU_ENGINE")   # set by the mode (lib.sh rc_host_mode_env), never by an env column
MODES = ("0", "4", "5", "6")
ENGINE_MODES = {"5": "mode 5 (IAU engine)", "6": "mode 6 (IAU tile host on the IAU engine)"}
ID = re.compile(r"^[A-Za-z0-9_-]+$")
CPUS = re.compile(r"^[0-9]+(-[0-9]+)?(\+[0-9]+(-[0-9]+)?)*$")
ARM_COLS = ("arm_id", "variant", "tgt_variant", "mode", "env", "trace")
CELL_COLS = ("cell_id", "app_cpus", "dev_cpus", "qd", "qpairs", "controllers", "compute", "lat",
             "placement", "env")


def load(path, cols, kind):
    with open(path, newline="") as f:
        rd = csv.DictReader(f)
        missing = [c for c in cols if c not in (rd.fieldnames or [])]
        if missing:
            raise SystemExit("%s file %s is missing column(s): %s" % (kind, path, ", ".join(missing)))
        return list(rd)


def parse_env(s):
    out = {}
    for kv in filter(None, (s or "").split(";")):
        k, sep, v = kv.partition("=")
        if sep and k in MODE_SET:
            raise SystemExit("env entry %r: %s is set by the mode (6 = SPDK_IAU_TILE=1, 5 = SPDK_IAU_ENGINE=1)" % (kv, k))
        if k in REMOVED_KNOBS:
            raise SystemExit("env entry %r: %s was removed from perf (P4.12b: the ns_cmd SGL split never reaches the "
                             "payload builder)" % (kv, k))
        if not sep or not (k.startswith(ENV_PREFIXES) or k in ENV_NAMES or k == SPILL_KNOB
                                   or k in DB_COST_KNOBS or k == S2_WB_KNOB):
            raise SystemExit("bad env entry %r (allowed prefixes %s; names %s)"
                             % (kv, ", ".join(ENV_PREFIXES), ", ".join(ENV_NAMES)))
        out[k] = v
    return out


def cpu_set(s):
    out = set()
    for part in s.split("+"):
        a, _, b = part.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--arms", required=True)
    ap.add_argument("--cells", required=True)
    ap.add_argument("--repeats", type=int, required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--warmup", type=int, default=5)
    ap.add_argument("--time", type=int, default=20)
    a = ap.parse_args(argv)
    if a.repeats < 1:
        raise SystemExit("--repeats must be >= 1")
    if not ID.match(a.prefix):
        raise SystemExit("--prefix must match [A-Za-z0-9_-]+: %r" % a.prefix)
    if a.repeats < 5:
        print("note: <5 repeats: classification only (spec §4 K needs >=5 for a claim)", file=sys.stderr)
    arms = load(a.arms, ARM_COLS, "arms")
    cells = load(a.cells, CELL_COLS, "cells")
    for kind, items, key in (("arm", arms, "arm_id"), ("cell", cells, "cell_id")):
        ids = [x[key] for x in items]
        if len(ids) != len(set(ids)) or not all(ID.match(i) for i in ids):
            raise SystemExit("%s ids must be unique and match [A-Za-z0-9_-]+: %s" % (kind, ids))
    for arm in arms:
        if arm["mode"] not in MODES:
            raise SystemExit("arm %s: mode must be 0, 4, 5 or 6" % arm["arm_id"])
    step = max(1, len(arms) // a.repeats)
    rows = []
    for cell in cells:
        for r in range(1, a.repeats + 1):
            k = ((r - 1) * step) % len(arms)
            for arm in arms[k:] + arms[:k]:
                env = parse_env(cell.get("env", ""))
                env.update(parse_env(arm.get("env", "")))
                rows.append(dict(point_id="p%05d" % len(rows),
                                 tag="%s_%s_%s" % (a.prefix, arm["arm_id"], cell["cell_id"]),
                                 variant=arm["variant"], mode=arm["mode"], app_cpus=cell["app_cpus"],
                                 dev_cpus=cell["dev_cpus"], qd=cell["qd"], qpairs=cell["qpairs"],
                                 bdev="fastpath", lat_us=1, placement=cell["placement"],
                                 compute=cell["compute"], run_id=r, warmup=a.warmup, time=a.time,
                                 lat=cell["lat"], env=";".join("%s=%s" % (x, env[x]) for x in sorted(env)),
                                 controllers=cell["controllers"],
                                 tgt_variant=arm["tgt_variant"] or arm["variant"],
                                 trace=arm.get("trace", ""), arm=arm["arm_id"], cell=cell["cell_id"],
                                 workload=cell.get("workload") or "randread",
                                 rwmix=cell.get("rwmix") or "100",
                                 io_size=cell.get("io_size") or "4096",
                                 engine_cpus=cell.get("engine_cpus") or ""))
    for row in rows:
        for fld, val in row.items():
            if any(ch in str(val) for ch in ',"\n\r'):
                raise SystemExit("arm %s cell %s: field %s value %r contains ',', '\"' or newline"
                                 % (row["arm"], row["cell"], fld, val))
        if row["workload"] not in WORKLOADS:
            raise SystemExit("cell %s: workload must be one of %s" % (row["cell"], ", ".join(WORKLOADS)))
        if not row["rwmix"].isdigit() or not 0 <= int(row["rwmix"]) <= 100:
            raise SystemExit("cell %s: rwmix must be an integer 0-100" % row["cell"])
        if not row["io_size"].isdigit() or int(row["io_size"]) % 512 or not 512 <= int(row["io_size"]) <= 1048576:
            raise SystemExit("cell %s: io_size must be a multiple of 512 in [512, 1048576]" % row["cell"])
        if row["rwmix"] != "100" and row["workload"] != "randrw":
            raise SystemExit("cell %s: rwmix must be 100 unless workload is randrw" % row["cell"])
        if row["trace"] != "none" and (row["workload"] != "randread" or row["io_size"] != "4096"):
            raise SystemExit("arm %s cell %s: the trace replay is 4 KiB reads; set trace=none for other workloads"
                             % (row["arm"], row["cell"]))
        for fld in ("app_cpus", "dev_cpus"):
            if not CPUS.match(row[fld]):
                raise SystemExit("arm %s cell %s: bad %s %r (CPU lists use '+' instead of ',', e.g. 4+5 or 4-7)"
                                 % (row["arm"], row["cell"], fld, row[fld]))
        if row["engine_cpus"] and not CPUS.match(row["engine_cpus"]):
            raise SystemExit("arm %s cell %s: bad engine_cpus %r (CPU lists use '+' instead of ',')"
                             % (row["arm"], row["cell"], row["engine_cpus"]))
        env = parse_env(row["env"])
        for k in TILE_ONLY:
            if k in env and row["mode"] != "6":
                raise SystemExit("arm %s cell %s: v6 knob %s on a mode %s point (mode 6 only)"
                                 % (row["arm"], row["cell"], k, row["mode"]))
        if SPILL_KNOB in env and env[SPILL_KNOB] not in ("", "0", "1"):
            raise SystemExit("arm %s cell %s: %s=%s (0 or 1)" % (row["arm"], row["cell"], SPILL_KNOB, env[SPILL_KNOB]))
        if env.get(SPILL_KNOB) == "1" and row["mode"] != "6":
            raise SystemExit("arm %s cell %s: %s=1 on a mode %s point (the A19 ctx spill needs v6 queues: mode 6 only)"
                             % (row["arm"], row["cell"], SPILL_KNOB, row["mode"]))
        db = env.get(DB_BATCH_KNOB)
        if db is not None and not (db.isdigit() and int(db) == 0) and env.get("SPDK_IAU_GROUP") != "1":
            raise SystemExit("arm %s cell %s: %s=%s needs SPDK_IAU_GROUP=1 (the group pass's doorbell chunk; only 0 "
                             "is allowed without the group)" % (row["arm"], row["cell"], DB_BATCH_KNOB, db))
        if S2_KNOB in env and row["mode"] != S2_MODE:
            raise SystemExit("arm %s cell %s: %s on a mode %s point (S2 is mode 0 only)"
                             % (row["arm"], row["cell"], S2_KNOB, row["mode"]))
        if S2_WB_KNOB in env and env.get(S2_KNOB) != "1":
            raise SystemExit("arm %s cell %s: %s needs %s=1 (mode 0)" % (row["arm"], row["cell"], S2_WB_KNOB, S2_KNOB))
        if row["mode"] in ENGINE_MODES and not row["engine_cpus"]:
            raise SystemExit("arm %s cell %s: %s needs an engine_cpus cell column"
                             % (row["arm"], row["cell"], ENGINE_MODES[row["mode"]]))
        if row["engine_cpus"] and cpu_set(row["engine_cpus"]) & (cpu_set(row["app_cpus"]) | cpu_set(row["dev_cpus"])):
            raise SystemExit("arm %s cell %s: engine_cpus %s overlap app_cpus/dev_cpus"
                             % (row["arm"], row["cell"], row["engine_cpus"]))
    w = csv.DictWriter(sys.stdout, fieldnames=COLS, lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
