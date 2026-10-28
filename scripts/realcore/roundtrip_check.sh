#!/usr/bin/env bash
# roundtrip_check.sh — Stage 4 proof: >= MIN clean Mode-D callback round-trips,
# fetched == emitted == host completions, no credits leaked, no errors; plus a
# Mode-0 regression point on the same build.
# Usage: roundtrip_check.sh [--variant instr] [--dev-cpus 4-5] [--min 131072] [--engine-cpus 6] [--tile] [--tile-group]
#                           [--dry-run]
#        roundtrip_check.sh --tile-check-logs HOST_LOG TGT_LOG --engine-cpus 6 [--min N]
#        roundtrip_check.sh --tile-group-check-logs LOG_DIR K --engine-cpus 6 [--min N]
#   --engine-cpus: also a mode 5 point (IAU-next P3, arm I1e): IAU_ENGINE_STATS exactly once,
#   host completions == engine records
#   Every point runs with the caller's SPDK_IAU_* and SPDK_UNCORE_* unset (run_point.sh scrubs them too).
#   --tile (IAU-next P4.6, needs --engine-cpus): also a mode 6 point (target mode 5 + host SPDK_IAU_TILE=1) on private
#   rings (no SPDK_IAU_GROUP / _FAST; P4.8 adds the group variant).
#   Its logs pass check_l's NX_TILE rules (three-way host records == engine records == SSD, lost=0, no refusal),
#   records >= MIN, no error text; prints "mode6 host_records=X engine_records=X".
#   --tile-group (IAU-next P4.8b, needs --engine-cpus): also two mode 6 GROUP-ring points, K=1 and K=2 (env
#   SPDK_IAU_GROUP=1;SPDK_IAU_RINGS=K;SPDK_PERF_WORKER_GROUP=1, tags <TAG>_m6g1 / _m6g2), each 2 controllers x 2
#   qpairs on one app core: one perf worker, one shared group of 4 routes, so routes share rings (K=1: all four on
#   ring 0; K=2: routes 0, 2 on ring 0 and 1, 3 on ring 1). Pass: check_l ok for the point (check_log + the perf
#   argv + the opcode mix, as a campaign's check_l would run it) and, per group and in total, host records ==
#   engine records == SSD posted + R_DETACHED (check_l.tile_group_records(); that equality is also one of check_l's
#   rules), records >= MIN, no error text; prints "mode6 group K=k host_records=X engine_records=Y ssd_posted=P
#   r_detached=D" and one such line per gid.
#   --dry-run: print the planned run_point.sh commands (DRY_RUN lines) and exit 0; nothing runs.
#   --tile-check-logs: only the mode 6 log check on existing logs (no point runs; fixture tests, re-checks).
#   --tile-group-check-logs LOG_DIR K: only the group check of a --tile-group point's logs dir (run_/nvmf_tgt_/cmd_
#   s4096_q128_r1 files) for K rings (no point runs; fixture tests, re-checks).
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
VARIANT=instr; DEV=4-5; MIN=131072; ENG=""; TILEMODE=0; TILEG=0; DRY=0; TILE_LOGS=(); TILEG_LOGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) VARIANT="$2"; shift 2 ;; --dev-cpus) DEV="$2"; shift 2 ;; --min) MIN="$2"; shift 2 ;;
    --engine-cpus) ENG="$2"; shift 2 ;;
    --tile) TILEMODE=1; shift ;; --tile-group) TILEG=1; shift ;; --dry-run) DRY=1; shift ;;
    --tile-check-logs) [ $# -ge 3 ] || { echo "--tile-check-logs needs HOST_LOG TGT_LOG" >&2; exit 2; }
                     TILE_LOGS=("$2" "$3"); shift 3 ;;
    --tile-group-check-logs) [ $# -ge 3 ] || { echo "--tile-group-check-logs needs LOG_DIR K" >&2; exit 2; }
                           [[ "$3" =~ ^[1-9][0-9]*$ ]] || { echo "--tile-group-check-logs K must be a ring count >= 1" >&2; exit 2; }
                           TILEG_LOGS=("$2" "$3"); shift 3 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[ "$TILEMODE" = 0 ] || [ -n "$ENG" ] || { echo "--tile needs --engine-cpus (the mode 6 point's IAU-engine threads)" >&2; exit 2; }
[ "$TILEG" = 0 ] || [ -n "$ENG" ] || { echo "--tile-group needs --engine-cpus (the mode 6 points' IAU-engine threads)" >&2; exit 2; }
TAG="s4roundtrip_$(date +%Y%m%d_%H%M%S)"
fail() { echo "ROUNDTRIP FAIL: $*"; exit 1; }
# >>> tile_logs_check
# The mode 6 point's logs (1 controller, 1 IO qpair): check_l NX_TILE (+ the P3 engine families that must stay silent
# and the engine timeouts), host records >= $4, no error text in the host log (IAU_TILE_STATS carries "aborted=", P4.12:
# IAU_S2_STATS "aborted=" / "aborts=").
tile_logs_check() {   # host_log tgt_log engine_cpus min
  python3 -I - "$RC_LIB_DIR/inv" "$@" <<'PY'
import os, re, sys
sys.path.insert(0, sys.argv[1])
import check_l
host, tgt, eng, mn = sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
if not (os.path.isfile(host) and os.path.isfile(tgt)):
    print("NX_TILE_FAIL log missing: %s %s" % (host, tgt)); sys.exit(1)
hl, tl = check_l.read_lines(host), check_l.read_lines(tgt)
bad = (check_l.check_tile(hl, 6, tl, tgt, 1, eng, 1, {}, {}) + check_l.check_engine(hl, 6, tl, tgt, 1, eng, 1, {})
       + check_l.check_engine_timeouts(tl))
hrec, erec = check_l.tile_records(hl, tl)
print("mode6 host_records=%d engine_records=%d" % (hrec, erec))
if hrec < mn:
    bad.append("host records %d < --min %d" % (hrec, mn))
if any(re.search(r"error|mismatch|abort", x, re.I) for x in hl if "IAU_TILE_STATS" not in x and "IAU_S2_STATS" not in x):
    bad.append("errors in the mode 6 host log")
for b in bad:
    print("NX_TILE_FAIL " + b)
sys.exit(1 if bad else 0)
PY
}
# <<< tile_logs_check
# >>> tile_group_logs_check
# A --tile-group point's logs dir (2 controllers x 2 qpairs on app core 0, one worker group, K rings): check_l ok as a
# campaign's check_l --expect-md would give it (check_log with the point's env + the perf argv + the opcode mix), the
# per-group and total three-way host == engine == SSD posted + R_DETACHED, host records >= $3, no error text in the
# host log (IAU_TILE_STATS carries "aborted=", P4.12: IAU_S2_STATS "aborted=" / "aborts=").
TILEG_ENV() { echo "SPDK_IAU_GROUP=1;SPDK_IAU_RINGS=$1;SPDK_PERF_WORKER_GROUP=1"; }
tile_group_logs_check() {   # log_dir engine_cpus min K
  python3 -I - "$RC_LIB_DIR/inv" "$1" "$2" "$3" "$4" "$(TILEG_ENV "$4")" <<'PY'
import os, re, sys
sys.path.insert(0, sys.argv[1])
import check_l
logd, eng, mn, k, env = sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5], check_l.parse_env(sys.argv[6])
st = check_l.stem("128", "1", "1")
host, tgt, cmd = (os.path.join(logd, "%s_%s.%s" % (n, st, x)) for n, x in (("run", "log"), ("nvmf_tgt", "log"),
                                                                           ("cmd", "txt")))
if not (os.path.isfile(host) and os.path.isfile(tgt)):
    print("NX_TILE_GROUP_FAIL log missing: %s %s" % (host, tgt)); sys.exit(1)
p = {"workload": "randread", "rwmix": "100", "io_size": "4096"}
reason, ex = check_l.check_log(host, env, 6, 2, tgt, "0", 2, True, eng, True, "fastpath")
bad = [x for x in (reason, check_l.check_cmd(cmd, p), check_l.check_opc(tgt, p, 6)[0]) if x]
hl, tl = check_l.read_lines(host), check_l.read_lines(tgt)
grec = check_l.tile_group_records(hl, tl, env)
tot = dict((z, sum(d[z] or 0 for d in grec.values())) for z in ("host", "engine", "ssd", "detached"))
print("mode6 group K=%s host_records=%d engine_records=%d ssd_posted=%d r_detached=%d"
      % (k, tot["host"], tot["engine"], tot["ssd"], tot["detached"]))
for gid in sorted(grec, key=int):
    d = grec[gid]
    print("mode6 group K=%s gid=%s host_records=%d engine_records=%d ssd_posted=%s r_detached=%d"
          % (k, gid, d["host"], d["engine"], "none" if d["ssd"] is None else d["ssd"], d["detached"]))
    if d["ssd"] is None or not d["host"] == d["engine"] == d["ssd"] + d["detached"]:
        bad.append("gid=%s three-way host %d engine %d ssd_posted %s + r_detached %d" % (gid, d["host"], d["engine"],
                                                                                         d["ssd"], d["detached"]))
if not grec or not tot["host"] == tot["engine"] == tot["ssd"] + tot["detached"]:
    bad.append("total three-way host %d engine %d ssd_posted %d + r_detached %d" % (tot["host"], tot["engine"], tot["ssd"],
                                                                                    tot["detached"]))
if tot["host"] < mn:
    bad.append("host records %d < --min %d" % (tot["host"], mn))
if any(re.search(r"error|mismatch|abort", x, re.I) for x in hl if "IAU_TILE_STATS" not in x and "IAU_S2_STATS" not in x):
    bad.append("errors in the mode 6 group host log")
print("mode6 group K=%s check_l %s" % (k, "ok" if not reason else "FAIL"))
for b in bad:
    print("NX_TILE_GROUP_FAIL " + b)
sys.exit(1 if bad else 0)
PY
}
# <<< tile_group_logs_check
if [ ${#TILE_LOGS[@]} -gt 0 ]; then
  [ -n "$ENG" ] || { echo "--tile-check-logs needs --engine-cpus" >&2; exit 2; }
  tile_logs_check "${TILE_LOGS[0]}" "${TILE_LOGS[1]}" "$ENG" "$MIN" || fail "mode 6 NX_TILE log check"
  echo "ROUNDTRIP_TILE_LOGS PASS"; exit 0
fi
if [ ${#TILEG_LOGS[@]} -gt 0 ]; then
  [ -n "$ENG" ] || { echo "--tile-group-check-logs needs --engine-cpus" >&2; exit 2; }
  tile_group_logs_check "${TILEG_LOGS[0]}" "$ENG" "$MIN" "${TILEG_LOGS[1]}" || fail "mode 6 group K=${TILEG_LOGS[1]} log check"
  echo "ROUNDTRIP_TILE_GROUP_LOGS PASS"; exit 0
fi
pt_args() {   # mode [K] -> PT: the run_point.sh arguments of that point (one definition for the run and --dry-run);
              # K: the --tile-group point with K rings (mode 6, 2 controllers x 2 qpairs, worker group)
  PT=(--tag "${TAG}_m$1${2:+g$2}" --variant "$VARIANT" --mode "$1" --app-cpus 0 --dev-cpus "$DEV")
  case "$1" in 5|6) PT+=(--engine-cpus "$ENG") ;; esac
  PT+=(--qd 128 --warmup 2 --time 10)
  [ -z "${2:-}" ] || PT+=(--qpairs 2 --controllers 2 --env "$(TILEG_ENV "$2")")
}
TILEG_KS="1 2"   # --tile-group: the ring counts
SCRUB=()   # every point: no caller SPDK_IAU_* / SPDK_UNCORE_* (the mode sets its own; mode 6: private rings)
while IFS= read -r _n; do case "$_n" in SPDK_IAU_*|SPDK_UNCORE_*) SCRUB+=(-u "$_n") ;; esac; done < <(compgen -e)
if [ "$DRY" = 1 ]; then
  for MODE in 4 0 ${ENG:+5} $([ "$TILEMODE" = 1 ] && echo 6); do
    pt_args "$MODE"
    echo "DRY_RUN env ${SCRUB[*]} run_point.sh ${PT[*]}"
  done
  if [ "$TILEG" = 1 ]; then
    for K in $TILEG_KS; do
      pt_args 6 "$K"
      echo "DRY_RUN env ${SCRUB[*]} run_point.sh ${PT[*]}"
    done
  fi
  exit 0
fi
for MODE in 4 0; do
  pt_args "$MODE"
  OUT=$(env "${SCRUB[@]}" "$RC_LIB_DIR/run_point.sh" "${PT[@]}" 2>&1) || { echo "$OUT"; fail "mode $MODE point failed"; }
  echo "$OUT" | grep '^POINT_RESULT'
done
LOGD="$RC_RESULTS/${TAG}_m4/core0_qp1/logs"
TGT=$(ls "$LOGD"/nvmf_tgt_*.log); RUN=$(ls "$LOGD"/run_*.log)
grep -qE 'IAU mode=4' "$TGT" || fail "target did not run Mode D"
grep -E 'IAU_STATS' "$TGT" | python3 -I -c '
import re, sys
min_n = int(sys.argv[1]); f = e = c = 0; lines = 0
for line in sys.stdin:
    m = dict(re.findall(r"(\w+)=(\d+)", line)); lines += 1
    f += int(m["fetched"]); e += int(m["emitted"]); c += int(m["credits_in_use"])
print("iau_stats lines=%d fetched=%d emitted=%d credits_in_use=%d" % (lines, f, e, c))
ok = lines > 0 and f == e and c == 0 and e >= min_n
sys.exit(0 if ok else 1)' "$MIN" || fail "IAU_STATS check (fetched==emitted>=$MIN, credits 0)"
HOSTC=$(sed -n 's/.*pcie_stats: .*completions=\([0-9]*\).*/\1/p' "$RUN" | awk '{s+=$1} END {print s+0}')
EMIT=$(grep -E 'IAU_STATS' "$TGT" | sed -n 's/.*emitted=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}')
echo "host_completions=$HOSTC device_emitted=$EMIT"
[ "$HOSTC" -gt 0 ] && [ "$HOSTC" -le "$EMIT" ] || fail "host completions ($HOSTC) inconsistent with emitted ($EMIT)"
grep -iE 'error|mismatch|abort' "$RUN" && fail "errors in host log"
if [ -n "$ENG" ]; then
  pt_args 5
  OUT=$(env "${SCRUB[@]}" "$RC_LIB_DIR/run_point.sh" "${PT[@]}" 2>&1) || { echo "$OUT"; fail "mode 5 point failed"; }
  echo "$OUT" | grep '^POINT_RESULT'
  LOGD5="$RC_RESULTS/${TAG}_m5/core0_qp1/logs"
  TGT5=$(ls "$LOGD5"/nvmf_tgt_*.log); RUN5=$(ls "$LOGD5"/run_*.log)
  grep -qE 'IAU mode=5' "$TGT5" || fail "target did not run mode 5"
  grep -q 'IAU_STATS qid=' "$TGT5" && fail "legacy Mode D engine attached in mode 5"
  grep -E 'IAU_ENGINE_STATS' "$TGT5" | python3 -I -c '
import re, sys
min_n = int(sys.argv[1]); lines = rec = 0; bad = []; seen = set()
for line in sys.stdin:
    m = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", line)); lines += 1
    if (m["ctrlr"], m["qid"]) in seen: bad.append("dup")
    seen.add((m["ctrlr"], m["qid"]))
    if not (m["desc_fetched"] == m["sqes_written"] == m["cqes_consumed"] == m["records_emitted"]): bad.append("counts")
    if m["inflight"] or m["bad_cid"] or m["bad_db"] or m["host_ssd_db"]: bad.append("state")
    if m.get("reissues", 0) or m.get("err_records", 0) or m.get("bad_sqhd", 0): bad.append("errors")
    rec += m["records_emitted"]
print("iau_engine_stats lines=%d records=%d bad=%s" % (lines, rec, ",".join(bad) or "none"))
sys.exit(0 if lines > 0 and not bad and rec >= min_n else 1)' "$MIN" || fail "IAU_ENGINE_STATS check (exactly once, >= $MIN records)"
  HOSTC5=$(sed -n 's/.*pcie_stats: .*completions=\([0-9]*\).*/\1/p' "$RUN5" | awk '{s+=$1} END {print s+0}')
  EMIT5=$(grep -E 'IAU_ENGINE_STATS' "$TGT5" | sed -n 's/.*records_emitted=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}')
  echo "mode5 host_completions=$HOSTC5 engine_records=$EMIT5"
  [ "$HOSTC5" -eq "$EMIT5" ] || fail "mode 5 host completions ($HOSTC5) != engine records ($EMIT5)"
  grep -iE 'error|mismatch|abort' "$RUN5" && fail "errors in mode 5 host log"
fi
if [ "$TILEMODE" = 1 ]; then
  pt_args 6
  OUT=$(env "${SCRUB[@]}" "$RC_LIB_DIR/run_point.sh" "${PT[@]}" 2>&1) || { echo "$OUT"; fail "mode 6 point failed"; }
  echo "$OUT" | grep '^POINT_RESULT'
  LOGD6="$RC_RESULTS/${TAG}_m6/core0_qp1/logs"
  TGT6=$(ls "$LOGD6"/nvmf_tgt_*.log); RUN6=$(ls "$LOGD6"/run_*.log)
  grep -qE 'IAU mode=5' "$TGT6" || fail "target did not run mode 5 for the mode 6 point"
  tile_logs_check "$RUN6" "$TGT6" "$ENG" "$MIN" || fail "mode 6 NX_TILE check (host records == engine records == SSD, lost=0)"
fi
if [ "$TILEG" = 1 ]; then
  for K in $TILEG_KS; do
    pt_args 6 "$K"
    OUT=$(env "${SCRUB[@]}" "$RC_LIB_DIR/run_point.sh" "${PT[@]}" 2>&1) || { echo "$OUT"; fail "mode 6 group K=$K point failed"; }
    echo "$OUT" | grep '^POINT_RESULT'
    LOGG="$RC_RESULTS/${TAG}_m6g$K/core0_qp2/logs"
    grep -qE 'IAU mode=5' "$LOGG"/nvmf_tgt_*.log || fail "target did not run mode 5 for the mode 6 group K=$K point"
    tile_group_logs_check "$LOGG" "$ENG" "$MIN" "$K" \
      || fail "mode 6 group K=$K check (check_l ok; per group host records == engine records == SSD posted + R_DETACHED)"
  done
fi
echo "ROUNDTRIP PASS tag=$TAG"
