#!/usr/bin/env bash
# p3_negative_smoke.sh — IAU-next P3.12 Step 6b: negative, lifecycle and linked-layout checks on one
# snapshot (SNAP). Every case runs alone on the measurement CPUs (app 0, SSD threads 4,5, engines 6,7),
# through start_target.sh; targets are stopped only with stop_target.sh. The perf processes are launched
# here, and only their own PIDs are ever signalled (case 6 kills the perf it started, never by name).
#   1 mode 5 + IAU_CQ_BATCH_N=8           -> add_listener fails with the validator's message, no target left
#   2 mode 5, engine CPU in the PG mask   -> add_listener fails (poll group on an engine core), no target left
#   3 mode 4 target + SPDK_IAU_ENGINE=1   -> Create I/O SQ INVALID_FIELD, perf fails, target names the reason
#   4 mode 5 target + host w/o the flag   -> as 3 (a: Mode D host, b: vanilla host)
#   5 mode 4 + IAU_DEV_FASTPATH_C0=0      -> a: start_target refuses it with --bdev fastpath (exit 2, nothing
#                                            launched); b: with --bdev null add_listener fails (validator)
#   6 mode 5 at A, host SIGKILLed mid-run -> STATS for both queues, target survives and serves a new host,
#                                            stop_target completes, no DETACH/BIND timeout or abort lines
#   7 mode 5, two perf runs on one target -> the same qids are deleted and re-created: BIND + STATS per run,
#                                            each run exactly-once (host completions == engine records)
#   8 linked-binary layout                -> nm address mod 64 of the hot symbols in SNAP vs REF binaries
#                                            (phase changes are recorded, not failed: G2 arbitrates)
# Output: one "NEG_SMOKE case=<n> name=<name> PASS|FAIL <decisive line>" per case, then
#   NEG_SMOKE_DONE tag=<tag> pass=<p> fail=<f>; logs under results/realcore/<tag>/case<n>/.
# Usage: p3_negative_smoke.sh --variant nx-p3a [--ref nx-p2] [--cases 1,2,...,8] [--tag TAG]
# Exit: 0 all PASS | 1 any FAIL | 2 usage | 75 a measurement campaign runs or the host stays busy.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"
VARIANT=""; REF=nx-p2; CASES="1,2,3,4,5,6,7,8"; TAG="nx_p3_neg_$(date +%Y%m%d_%H%M%S)"
APP=0; DEV=4,5; ENG=6,7
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) VARIANT="$2"; shift 2 ;;
    --ref) REF="$2"; shift 2 ;;
    --cases) CASES="$2"; shift 2 ;;
    --tag) TAG="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[ -n "$VARIANT" ] || { echo "--variant required" >&2; exit 2; }
[[ "$TAG" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "bad --tag $TAG" >&2; exit 2; }
rc_select_variant "$VARIANT"
[ -x "$RC_BIN/nvmf_tgt" ] && [ -x "$RC_BIN/spdk_nvme_perf" ] || { echo "variant $VARIANT is not built" >&2; exit 2; }
RC_RPC_PY="$RC_SPDK/scripts/rpc.py"
OUT="$RC_RESULTS/$TAG"; mkdir -p "$OUT"
cp -f "$RC_BUILD/fingerprint.txt" "$OUT/fingerprint.txt"
NPASS=0; NFAIL=0

verdict() {   # $1 case, $2 name, $3 PASS|FAIL, $4 decisive line
  local l="NEG_SMOKE case=$1 name=$2 $3 $4"
  echo "$l"; echo "$l" >> "$OUT/verdicts.txt"
  if [ "$3" = PASS ]; then NPASS=$((NPASS + 1)); else NFAIL=$((NFAIL + 1)); fi
}
# One measurement at a time: refuse while a campaign runs; wait (bounded) for quiet measurement CPUs.
preflight() {
  local i
  if pgrep -f 'scripts/realcore/[c]ampaign\.sh' >/dev/null; then
    sleep 5; pgrep -f 'scripts/realcore/[c]ampaign\.sh' >/dev/null && { echo "WAIT: a campaign is running" >&2; exit 75; }
  fi
  for ((i = 0; i < 10; i++)); do
    rc_host_check "$APP,$DEV,$ENG" >/dev/null && return 0
    sleep 30
  done
  echo "HOST_BUSY on $APP,$DEV,$ENG" >&2; exit 75
}
# The pid of the nvmf_tgt that wrote log $1 (DPDK's --file-prefix=spdk_pid<pid>), or empty.
tgt_pid_of_log() { sed -n 's/.*--file-prefix=spdk_pid\([0-9]*\).*/\1/p' "$1" 2>/dev/null | head -1; }
# True when no target of ours is left: no pidfile and the logged pid is not a live nvmf_tgt.
tgt_gone() {   # $1 = pid (may be empty)
  [ ! -f "$RC_TGT_PIDFILE" ] || return 1
  [ -z "$1" ] && return 0
  kill -0 "$1" 2>/dev/null || return 0
  [[ "$(tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null)" != *nvmf_tgt* ]]
}
tgt_alive() { local p; p="$(cat "$RC_TGT_PIDFILE" 2>/dev/null)"; [ -n "$p" ] && kill -0 "$p" 2>/dev/null && rc_rpc spdk_get_version >/dev/null 2>&1; }
start_tgt() {  # $1 = case dir, then start_target.sh args; env from the caller. Returns start_target's rc.
  local d="$1" rc; shift
  RC_VARIANT="$VARIANT" RC_PG_BY_QID="${RC_PG_BY_QID:-1}" "$RC_LIB_DIR/start_target.sh" "$@" > "$d/target_start.log" 2>&1
  rc=$?
  cp -f "$RC_TGT_LOG" "$d/nvmf_tgt.log" 2>/dev/null || true
  return "$rc"
}
stop_tgt() {   # $1 = case dir; stop_target.sh only
  RC_VARIANT="$VARIANT" "$RC_LIB_DIR/stop_target.sh" > "$1/target_stop.log" 2>&1; local rc=$?
  cp -f "$RC_TGT_LOG" "$1/nvmf_tgt.log" 2>/dev/null || true
  return "$rc"
}
# Launch perf in the background against controller 0, 2 qpairs x QD128 random 4 KiB reads; host mode $2:
# 0 vanilla | 4 Mode D | 5 Mode D + SPDK_IAU_ENGINE=1 (I1e host). Sets PERF_PID (perf itself: env execs it).
# $1 log, $3 time s, $4 warmup s.
perf_launch() {
  local log="$1" hm="$2" t="$3" w="$4" hostenv=()
  case "$hm" in
    0) ;;
    4) hostenv=(SPDK_UNCORE_MODE_C=1 SPDK_UNCORE_MODE_D=1) ;;
    5) hostenv=(SPDK_UNCORE_MODE_C=1 SPDK_UNCORE_MODE_D=1 SPDK_IAU_ENGINE=1) ;;
  esac
  env -u SPDK_UNCORE_MODE_B -u SPDK_UNCORE_MODE_C -u SPDK_UNCORE_MODE_D -u SPDK_UNCORE_MODE_E \
      -u SPDK_IAU_ENGINE -u SPDK_PERF_WORKER_GROUP "${hostenv[@]}" SPDK_COMPUTE_PER_IO=0 PERF_QPAIR_ORDERED=1 \
      SPDK_IO_CYCLE_ENABLE=0 \
    "$RC_BIN/spdk_nvme_perf" -r "trtype:VFIOUSER traddr:$(rc_vfu_dir 0) subnqn:$(rc_nqn 0)" -w randread -o 4096 \
      -q 128 -t "$t" --warmup-time "$w" -c "$(rc_cpuset_to_mask "$APP")" -P 2 --transport-stats -Q 1 \
      $(rc_mem_args "$RC_PERF_MEM_MB") > "$log" 2>&1 < /dev/null &
  PERF_PID=$!
}
# Bounded wait on our own perf PID $1 (limit $2 s); on timeout SIGTERM then SIGKILL that PID only. rc of perf.
perf_wait() {
  local pid="$1" lim="$2" i
  for ((i = 0; i < lim; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
  if kill -0 "$pid" 2>/dev/null; then
    echo "PERF_WATCHDOG pid=$pid after=${lim}s" >&2
    kill -TERM "$pid" 2>/dev/null; sleep 5; kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null; return 124
  fi
  wait "$pid"
}
wait_lines() {  # $1 file, $2 pattern, $3 count, $4 limit s: wait until >= count lines match
  local i
  for ((i = 0; i < $4 * 10; i++)); do
    [ "$(grep -c -- "$2" "$1" 2>/dev/null)" -ge "$3" ] && return 0
    sleep 0.1
  done
  return 1
}
first() { grep -m1 -hF -- "$1" "$2" 2>/dev/null | sed 's/.*\*\(NOTICE\|ERROR\)\*: //; s#^/[^ ]*/vfu[0-9]*: ##' | cut -c1-240; }
has_case() { [[ ",$CASES," == *",$1,"* ]]; }
# Exactly-once over IAU_ENGINE_STATS lines on stdin: one line per (ctrlr, qid), equal counts, zero state.
stats_ok() {
  python3 -I -c '
import re, sys
want = int(sys.argv[1]); seen = set(); bad = []; rec = 0
for line in sys.stdin:
    m = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", line))
    if (m["ctrlr"], m["qid"]) in seen: bad.append("dup")
    seen.add((m["ctrlr"], m["qid"]))
    if not (m["desc_fetched"] == m["sqes_written"] == m["cqes_consumed"] == m["records_emitted"]): bad.append("counts")
    if m["inflight"] or m["bad_cid"] or m["bad_db"] or m["host_ssd_db"] or m["reissues"] or m["err_records"] or m["bad_sqhd"]:
        bad.append("state")
    rec += m["records_emitted"]
print("lines=%d records=%d bad=%s" % (len(seen), rec, ",".join(bad) or "none"))
sys.exit(0 if len(seen) == want and not bad else 1)' "$1"
}
host_completions() { sed -n 's/.*pcie_stats: .*completions=\([0-9]*\).*/\1/p' "$1" | awk '{s+=$1} END {print s+0}'; }
records_of() { sed -n 's/.*IAU_ENGINE_STATS.*records_emitted=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}'; }
ABORT_RE='IAU_ENGINE_DETACH_TIMEOUT|IAU_ENGINE_BIND_TIMEOUT|still bound at transport destroy|aborting rather|Segmentation|core dumped|Assertion'

# --- 1, 2, 5b: the listener must fail with the validator's message and leave no target --------------
listener_case() {   # $1 case, $2 name, $3 expected target-log message, then: env assignments -- start args
  local n="$1" name="$2" msg="$3" d="$OUT/case$1" rc pid envs=()
  shift 3
  while [ "$1" != -- ]; do envs+=("$1"); shift; done; shift
  mkdir -p "$d"; preflight
  env "${envs[@]}" bash -c 'RC_VARIANT="$0" RC_PG_BY_QID=1 "$1/start_target.sh" "${@:2}"' "$VARIANT" "$RC_LIB_DIR" "$@" \
    > "$d/target_start.log" 2>&1
  rc=$?
  cp -f "$RC_TGT_LOG" "$d/nvmf_tgt.log" 2>/dev/null || true
  pid="$(tgt_pid_of_log "$d/nvmf_tgt.log")"
  if [ "$rc" -ne 0 ] && grep -q 'nvmf_subsystem_add_listener .* failed' "$d/target_start.log" \
     && grep -qF -- "$msg" "$d/nvmf_tgt.log" && tgt_gone "$pid"; then
    verdict "$n" "$name" PASS "start_rc=$rc tgt_pid=$pid gone=1 | $(first "$msg" "$d/nvmf_tgt.log")"
  else
    tgt_gone "$pid" || stop_tgt "$d"
    verdict "$n" "$name" FAIL "start_rc=$rc tgt_pid=$pid | $(tail -1 "$d/target_start.log")"
  fi
}

# --- 3, 4: a host/target registration mismatch fails Create I/O SQ with INVALID_FIELD ----------------
mismatch_case() {   # $1 case, $2 name, $3 target mode, $4 host mode, $5 expected target-log message
  local n="$1" name="$2" tm="$3" hm="$4" msg="$5" d="$OUT/case$1" args prc ok=1 why=""
  mkdir -p "$d"; preflight
  args=(--dev-cpus "$DEV" --mode "$tm"); [ "$tm" = 5 ] && args+=(--engine-cpus "$ENG")
  if ! IAU_DEV_CHANNELS=64 start_tgt "$d" "${args[@]}"; then
    verdict "$n" "$name" FAIL "target did not start: $(tail -1 "$d/target_start.log")"; return
  fi
  perf_launch "$d/run.log" "$hm" 5 1; echo "$PERF_PID" > "$d/perf.pid"
  perf_wait "$PERF_PID" 60; prc=$?
  cp -f "$RC_TGT_LOG" "$d/nvmf_tgt.log"
  [ "$prc" -ne 0 ] || { ok=0; why+=" perf_rc=0"; }
  grep -q '^Total' "$d/run.log" && { ok=0; why+=" perf_measured"; }
  # perf prints at ERROR level only, so the NOTICE "INVALID FIELD (00/02)" completion print is suppressed;
  # the host's error completion is "nvme_create_io_sq failed" (one per qpair), and the target's reason line
  # is printed only on sq_eng_connect()'s paths that return SPDK_NVME_SC_INVALID_FIELD.
  [ "$(grep -c 'nvme_create_io_sq failed' "$d/run.log")" -eq 2 ] || { ok=0; why+=" host_create_sq_errors!=2"; }
  grep -q 'ERROR: init_ns_worker_ctx() failed' "$d/run.log" || { ok=0; why+=" no_init_ns_worker_ctx_failure"; }
  [ "$(grep -cF -- "$msg" "$d/nvmf_tgt.log")" -eq 2 ] || { ok=0; why+=" target_reason!=2"; }
  grep -q 'IAU_ENGINE_BIND ' "$d/nvmf_tgt.log" && { ok=0; why+=" bound"; }
  tgt_alive || { ok=0; why+=" target_died"; }
  stop_tgt "$d" || { ok=0; why+=" stop_failed"; }
  grep -qE "$ABORT_RE" "$d/nvmf_tgt.log" && { ok=0; why+=" abort_line"; }
  # Connect-failure time (first to last host log timestamp) and the stock-SPDK retry flood: when another
  # qpair's admin poll completes this qpair's Delete CQ, the early pcie_state FAILED return (upstream too)
  # leaves it CONNECTING until perf's 10 s connect timeout.
  dur=$(grep -o '^\[[0-9-]* [0-9:.]*\]' "$d/run.log" | sed -n '1p;$p' | tr -d '[]' | \
        python3 -I -c 'import sys, datetime as d; t = [d.datetime.fromisoformat(l.strip()) for l in sys.stdin]; print("%.3f" % (t[-1] - t[0]).total_seconds() if t else "NA")')
  flood=$(grep -c 'CQ transport error -6' "$d/run.log")
  if [ "$(stat -c %s "$d/run.log")" -gt 8000000 ]; then gzip -f "$d/run.log"; fi
  if [ "$ok" = 1 ]; then
    verdict "$n" "$name" PASS "perf_rc=$prc connect_fail_s=$dur transport_err_lines=$flood host: nvme_create_io_sq failed x2 | target: $(first "$msg" "$d/nvmf_tgt.log")"
  else
    verdict "$n" "$name" FAIL "perf_rc=$prc connect_fail_s=$dur${why}"
  fi
}

if has_case 1; then
  listener_case 1 mode5_batch_n8 'IAU mode 5 publishes every record at once and needs IAU_CQ_BATCH_N=1 (got 8)' \
    IAU_CQ_BATCH_N=8 IAU_DEV_CHANNELS=64 -- --dev-cpus "$DEV" --mode 5 --engine-cpus "$ENG"
fi
if has_case 2; then
  # Engine CPUs 5,6 overlap the poll-group mask (dev 4,5): run_point.sh refuses the overlap, so start_target directly.
  listener_case 2 mode5_pg_on_engine_core 'IAU mode 5: an nvmf poll group runs on engine core 5' \
    IAU_DEV_CHANNELS=64 -- --dev-cpus "$DEV" --mode 5 --engine-cpus 5,6
fi
if has_case 3; then
  mismatch_case 3 mode4_tgt_engine_host 4 5 'SPDK_IAU_ENGINE=1 needs IAU_UNCORE_MODE=5); failing its Create I/O SQ'
fi
if has_case 4; then
  mismatch_case 4a mode5_tgt_modeD_host 5 4 'IAU mode 5, but the host did not register sqid:'
  mismatch_case 4b mode5_tgt_vanilla_host 5 0 'IAU mode 5, but the host did not register sqid:'
fi
if has_case 5; then
  d="$OUT/case5a"; mkdir -p "$d"; preflight
  before="$(stat -c %Y "$RC_TGT_LOG" 2>/dev/null)"
  env IAU_DEV_FASTPATH_C0=0 bash -c 'RC_VARIANT="$0" "$1/start_target.sh" --dev-cpus "$2" --mode 4 --bdev fastpath' \
    "$VARIANT" "$RC_LIB_DIR" "$DEV" > "$d/target_start.log" 2>&1
  rc=$?
  if [ "$rc" -eq 2 ] && grep -q 'IAU_DEV_FASTPATH_C0=0 contradicts --bdev fastpath' "$d/target_start.log" \
     && [ ! -f "$RC_TGT_PIDFILE" ] && [ "$(stat -c %Y "$RC_TGT_LOG" 2>/dev/null)" = "$before" ]; then
    verdict 5a mode4_fastpath_c0_off_bdev_fastpath PASS "start_rc=2 nothing launched | $(head -1 "$d/target_start.log")"
  else
    tgt_gone "" || stop_tgt "$d"
    verdict 5a mode4_fastpath_c0_off_bdev_fastpath FAIL "start_rc=$rc | $(tail -1 "$d/target_start.log")"
  fi
  listener_case 5b mode4_fastpath_c0_off_bdev_null \
    'IAU Mode D (IAU_UNCORE_MODE=4) requires the fast path, but controller 0 has IAU_DEV_FASTPATH=0' \
    IAU_DEV_FASTPATH_C0=0 IAU_DEV_CHANNELS=64 -- --dev-cpus "$DEV" --mode 4 --bdev null
fi

if has_case 6; then
  d="$OUT/case6"; mkdir -p "$d"; preflight; ok=1; why=""
  if ! IAU_DEV_CHANNELS=64 start_tgt "$d" --dev-cpus "$DEV" --mode 5 --engine-cpus "$ENG"; then
    verdict 6 lifecycle_host_killed FAIL "target did not start: $(tail -1 "$d/target_start.log")"
  else
    TPID="$(cat "$RC_TGT_PIDFILE")"
    perf_launch "$d/run1.log" 5 30 2; echo "$PERF_PID" > "$d/perf1.pid"
    sleep 8
    CMD="$(tr '\0' ' ' < "/proc/$PERF_PID/cmdline" 2>/dev/null)"
    wait_lines "$RC_TGT_LOG" 'IAU_ENGINE_BIND ' 2 1 || { ok=0; why+=" no_2_binds"; }
    if [[ "$CMD" == *spdk_nvme_perf* ]] && kill -0 "$PERF_PID" 2>/dev/null; then
      echo "KILL pid=$PERF_PID cmd=$CMD" > "$d/kill.txt"
      kill -KILL "$PERF_PID"; wait "$PERF_PID" 2>/dev/null; echo "perf_rc=$?" >> "$d/kill.txt"
    else
      ok=0; why+=" perf_not_running_at_kill"
    fi
    wait_lines "$RC_TGT_LOG" 'IAU_ENGINE_STATS' 2 15 || { ok=0; why+=" no_2_stats_after_kill"; }
    sleep 2
    cp -f "$RC_TGT_LOG" "$d/nvmf_tgt_after_kill.log"
    kill -0 "$TPID" 2>/dev/null && tgt_alive || { ok=0; why+=" target_died"; }
    # Each queue's engine STATS precede its SSD-side SQ teardown line (OPC_STATS, printed after unmap_q):
    # the unbind is the first step of the teardown, and the SSD side retired exactly the engine's records.
    ORD=$(python3 -I -c '
import re, sys
st, op = {}, {}
for i, line in enumerate(open(sys.argv[1])):
    m = re.search(r"IAU_ENGINE_STATS ctrlr=0 qid=(\d+) .*records_emitted=(\d+)", line)
    if m: st.setdefault(int(m.group(1)), (i, int(m.group(2))))
    m = re.search(r"OPC_STATS qid=(\d+) .*fp_done=(\d+)", line)
    if m: op.setdefault(int(m.group(1)), (i, int(m.group(2))))
ok = sorted(st) == [1, 2] == sorted(op) and all(st[q][0] < op[q][0] and st[q][1] == op[q][1] for q in st)
print("stats_before_sq_teardown=%d %s" % (ok, " ".join("qid%d:stats@%d<opc@%d,rec=%d,fp_done=%d" % (q, st[q][0] + 1,
      op.get(q, (-2, 0))[0] + 1, st[q][1], op.get(q, (0, -1))[1]) for q in sorted(st))))
sys.exit(0 if ok else 1)' "$d/nvmf_tgt_after_kill.log") || { ok=0; why+=" order($ORD)"; }
    N1=$(wc -l < "$d/nvmf_tgt_after_kill.log")
    # The target also serves a new host after the kill: a fresh bind and an exactly-once run.
    perf_launch "$d/run2.log" 5 4 1; echo "$PERF_PID" > "$d/perf2.pid"
    perf_wait "$PERF_PID" 60; p2=$?
    [ "$p2" -eq 0 ] && grep -q '^Total' "$d/run2.log" || { ok=0; why+=" rerun_perf_rc=$p2"; }
    wait_lines "$RC_TGT_LOG" 'IAU_ENGINE_STATS' 4 15 || { ok=0; why+=" no_rerun_stats"; }
    cp -f "$RC_TGT_LOG" "$d/nvmf_tgt_before_stop.log"
    tail -n +"$((N1 + 1))" "$d/nvmf_tgt_before_stop.log" > "$d/nvmf_tgt_rerun.log"
    [ "$(grep -c 'IAU_ENGINE_BIND ' "$d/nvmf_tgt_rerun.log")" -eq 2 ] || { ok=0; why+=" rerun_binds"; }
    S2=$(grep 'IAU_ENGINE_STATS' "$d/nvmf_tgt_rerun.log" | stats_ok 2) || { ok=0; why+=" rerun_stats($S2)"; }
    H2=$(host_completions "$d/run2.log"); R2=$(grep 'IAU_ENGINE_STATS' "$d/nvmf_tgt_rerun.log" | records_of)
    [ "$H2" -gt 0 ] && [ "$H2" -eq "$R2" ] || { ok=0; why+=" rerun_host=$H2!=records=$R2"; }
    stop_tgt "$d" || { ok=0; why+=" stop_failed"; }
    grep -q TARGET_STOPPED "$d/target_stop.log" || { ok=0; why+=" no_TARGET_STOPPED"; }
    grep -qE "$ABORT_RE" "$d/nvmf_tgt.log" && { ok=0; why+=" abort_line($(grep -m1 -E "$ABORT_RE" "$d/nvmf_tgt.log" | cut -c1-120))"; }
    K=$(grep 'IAU_ENGINE_STATS' "$d/nvmf_tgt_after_kill.log" | sed 's/.*IAU_ENGINE_STATS/STATS/' | \
        sed -E 's/ (ssd_db_writes|cq_db_writes|bad_db|host_ssd_db|reissues|err_records|bad_sqhd)=[0-9]+//g' | paste -sd'|')
    if [ "$ok" = 1 ]; then
      verdict 6 lifecycle_host_killed PASS "killed perf pid=$(cat "$d/perf1.pid") rc=137, target alive, $ORD, rerun $S2 host=$H2, stopped, no timeout/abort | killed-run $K"
    else
      verdict 6 lifecycle_host_killed FAIL "${why} | killed-run $K"
    fi
  fi
fi

if has_case 7; then
  d="$OUT/case7"; mkdir -p "$d"; preflight; ok=1; why=""
  if ! IAU_DEV_CHANNELS=64 start_tgt "$d" --dev-cpus "$DEV" --mode 5 --engine-cpus "$ENG"; then
    verdict 7 lifecycle_qid_recreate FAIL "target did not start: $(tail -1 "$d/target_start.log")"
  else
    prev=0; summary=""
    for r in 1 2; do
      perf_launch "$d/run$r.log" 5 5 1; echo "$PERF_PID" > "$d/perf$r.pid"
      perf_wait "$PERF_PID" 60; prc=$?
      [ "$prc" -eq 0 ] && grep -q '^Total' "$d/run$r.log" || { ok=0; why+=" run${r}_perf_rc=$prc"; }
      wait_lines "$RC_TGT_LOG" 'IAU_ENGINE_STATS' $((2 * r)) 15 || { ok=0; why+=" run${r}_no_stats"; }
      sleep 1
      cp -f "$RC_TGT_LOG" "$d/nvmf_tgt_after_run$r.log"
      tail -n +"$((prev + 1))" "$d/nvmf_tgt_after_run$r.log" > "$d/nvmf_tgt_run$r.log"
      prev=$(wc -l < "$d/nvmf_tgt_after_run$r.log")
      B=$(grep 'IAU_ENGINE_BIND ' "$d/nvmf_tgt_run$r.log" | sed -n 's/.*\(qid=[0-9]*\).*/\1/p' | sort | paste -sd,)
      [ "$B" = "qid=1,qid=2" ] || { ok=0; why+=" run${r}_binds=$B"; }
      S=$(grep 'IAU_ENGINE_STATS' "$d/nvmf_tgt_run$r.log" | stats_ok 2) || { ok=0; why+=" run${r}_stats($S)"; }
      H=$(host_completions "$d/run$r.log"); R=$(grep 'IAU_ENGINE_STATS' "$d/nvmf_tgt_run$r.log" | records_of)
      [ "$H" -gt 0 ] && [ "$H" -eq "$R" ] || { ok=0; why+=" run${r}_host=$H!=records=$R"; }
      # BIND of this run follows the previous run's STATS (a fresh bind of a re-created qid).
      summary+=" run$r: binds=$B stats $S host=$H"
    done
    tgt_alive || { ok=0; why+=" target_died"; }
    stop_tgt "$d" || { ok=0; why+=" stop_failed"; }
    grep -qE "$ABORT_RE" "$d/nvmf_tgt.log" && { ok=0; why+=" abort_line"; }
    if [ "$ok" = 1 ]; then verdict 7 lifecycle_qid_recreate PASS "one target pid,$summary"
    else verdict 7 lifecycle_qid_recreate FAIL "${why} |$summary"; fi
  fi
fi

if has_case 8; then
  d="$OUT/case8"; mkdir -p "$d"
  RB="$RC_ROOT/.build/spdk-host-$REF/src/spdk/build/bin"
  HOST_SYMS=(nvme_pcie_md_poll nvme_pcie_md_submit nvme_pcie_qpair_process_completions
             nvme_pcie_qpair_process_completions_stock nvme_pcie_qpair_submit_request
             nvme_pcie_qpair_submit_request_stock nvme_pcie_qpair_submit_tracker
             nvme_pcie_qpair_complete_tracker nvme_qpair_submit_request spdk_nvme_qpair_process_completions)
  TGT_SYMS=(nvmf_vfio_user_sq_poll nvmf_vfio_user_poll_group_poll post_completion handle_cmd_req iau_stage_cpl)
  python3 -I - "$RB" "$RC_BIN" "$REF" "$VARIANT" "${HOST_SYMS[*]}" "${TGT_SYMS[*]}" > "$d/layout.txt" 2>&1 <<'PY'
import subprocess, sys
ref_bin, snap_bin, ref, snap, host, tgt = sys.argv[1:7]
def syms(path):   # name -> address; a name defined at two addresses (two static copies) maps to None
    out = {}
    for line in subprocess.run(["nm", path], capture_output=True, text=True, check=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in ("t", "T"):
            a = int(f[0], 16)
            out[f[2]] = a if out.get(f[2], a) == a else None
    return out
def resolve(table, base):   # the function incl. .constprop/.isra/.part suffixes, plus its .cold part
    names = sorted(n for n in table if n == base or (n.startswith(base + ".") and not n.endswith(".cold")))
    hot = names[0] if names else None
    cold = [n for n in table if n.startswith(base + ".") and n.endswith(".cold")]
    return hot, (sorted(cold)[0] if cold else None)
missing = changed = 0
for binname, bases in (("spdk_nvme_perf", host.split()), ("nvmf_tgt", tgt.split())):
    a, b = syms("%s/%s" % (ref_bin, binname)), syms("%s/%s" % (snap_bin, binname))
    for base in bases:
        ha, ca = resolve(a, base); hb, cb = resolve(b, base)
        for kind, na, nb in (("hot", ha, hb), ("cold", ca, cb)):
            if kind == "cold" and na is None and nb is None:
                continue
            if na is None or nb is None or a[na] is None or b[nb] is None:
                missing += 1
                print("LAYOUT bin=%s sym=%s part=%s MISSING_OR_AMBIGUOUS %s=%s %s=%s" % (binname, base, kind, ref, na, snap, nb))
                continue
            pa, pb = a[na] % 64, b[nb] % 64
            same = pa == pb
            changed += not same
            print("LAYOUT bin=%s sym=%s %s=0x%x/%d %s=0x%x/%d phase=%s" % (binname, nb, ref, a[na], pa, snap, b[nb], pb,
                  "same" if same else "CHANGED"))
print("LAYOUT_SUMMARY missing=%d phase_changes=%d" % (missing, changed))
sys.exit(1 if missing else 0)
PY
  lrc=$?
  cat "$d/layout.txt"
  SUMM=$(grep '^LAYOUT_SUMMARY' "$d/layout.txt")
  if [ "$lrc" -eq 0 ]; then verdict 8 linked_layout PASS "$REF vs $VARIANT: ${SUMM#LAYOUT_SUMMARY } (phase changes are recorded; G2 arbitrates)"
  else verdict 8 linked_layout FAIL "${SUMM:-layout script failed}"; fi
fi

echo "NEG_SMOKE_DONE tag=$TAG pass=$NPASS fail=$NFAIL"
[ "$NFAIL" -eq 0 ]
