#!/usr/bin/env bash
# run_point.sh — one real-core measurement point: fresh vfio-user target on the
# device CPUs, pinned spdk_nvme_perf BigANN replay on the app CPUs, device busy
# snapshots inside the measurement window, one phase1_results.csv row.
# Exit: 0 ok | 1 run failed | 2 usage | 75 host busy (retry later)
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
TAG=""; VARIANT=lean; MODE=0; APP_CPUS=0; DEV_CPUS=4; QD=128; QPAIRS=1; BDEV=fastpath; LAT_US=1
PLACEMENT=same-socket; COMPUTE=0; RUN_ID=1; WARMUP=5; TIME=20; TRACE="$RC_TRACE_DEFAULT"; PERF_STAT=0; LAT=1
ENV_SPEC=""; CONTROLLERS=1; TGT_VARIANT=""; WORKLOAD=randread; RWMIX=100; IO_SIZE=4096; ENGINE_CPUS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --tag) TAG="$2"; shift 2 ;;
    --variant) VARIANT="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --app-cpus) APP_CPUS="$2"; shift 2 ;;
    --dev-cpus) DEV_CPUS="$2"; shift 2 ;;
    --qd) QD="$2"; shift 2 ;;
    --qpairs) QPAIRS="$2"; shift 2 ;;
    --bdev) BDEV="$2"; shift 2 ;;
    --lat-us) LAT_US="$2"; shift 2 ;;
    --placement) PLACEMENT="$2"; shift 2 ;;
    --compute-per-io) COMPUTE="$2"; shift 2 ;;
    --run-id) RUN_ID="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;;
    --time) TIME="$2"; shift 2 ;;
    --trace) TRACE="$2"; shift 2 ;;
    --workload) WORKLOAD="$2"; shift 2 ;;
    --rwmix) RWMIX="$2"; shift 2 ;;
    --io-size) IO_SIZE="$2"; shift 2 ;;
    --perf-stat) PERF_STAT="$2"; shift 2 ;;
    --lat) LAT="$2"; shift 2 ;;
    --env) ENV_SPEC="$2"; shift 2 ;;
    --controllers) CONTROLLERS="$2"; shift 2 ;;
    --tgt-variant) TGT_VARIANT="$2"; shift 2 ;;
    --engine-cpus) ENGINE_CPUS="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[ -n "$TAG" ] || { echo "--tag required" >&2; exit 2; }
[ "$TIME" -ge 3 ] || { echo "--time must be >= 3" >&2; exit 2; }
case "$LAT" in 0|1) ;; *) echo "--lat must be 0 or 1" >&2; exit 2 ;; esac
case "$PERF_STAT" in 0|1|2) ;; *) echo "--perf-stat must be 0, 1 or 2" >&2; exit 2 ;; esac
# mode 6 (IAU-next P4.6) = the mode 5 target (IAU-engine threads) + host SPDK_IAU_TILE=1 (rc_host_mode_env, rc_target_mode)
case "$MODE" in 0|4|5|6) ;; *) echo "--mode must be 0, 4, 5 or 6" >&2; exit 2 ;; esac
if [ -n "$ENGINE_CPUS" ] && ! [[ "$ENGINE_CPUS" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]]; then
  echo "--engine-cpus must be a CPU list like 6,7 or 6-7" >&2; exit 2
fi
case "$MODE" in 5|6) [ -n "$ENGINE_CPUS" ] || { echo "--mode $MODE needs --engine-cpus" >&2; exit 2; } ;; esac
TGT_MODE=$(rc_target_mode "$MODE") || exit 2
if [ -n "$ENGINE_CPUS" ]; then
  for _c in $(rc_cpuset_expand "$ENGINE_CPUS"); do
    for _o in $(rc_cpuset_expand "$APP_CPUS,$DEV_CPUS"); do
      [ "$_c" != "$_o" ] || { echo "--engine-cpus $ENGINE_CPUS overlaps --app-cpus/--dev-cpus" >&2; exit 2; }
    done
  done
fi
case "$WORKLOAD" in randread|randwrite|randrw) ;; *) echo "--workload must be randread, randwrite or randrw" >&2; exit 2 ;; esac
case "$RWMIX" in ''|*[!0-9]*) echo "--rwmix must be an integer 0-100" >&2; exit 2 ;; esac
[ "$RWMIX" -le 100 ] || { echo "--rwmix must be an integer 0-100" >&2; exit 2; }
case "$IO_SIZE" in ''|*[!0-9]*) echo "--io-size must be an integer multiple of 512 in [512, 1048576]" >&2; exit 2 ;; esac
{ [ $((10#$IO_SIZE % 512)) -eq 0 ] && [ $((10#$IO_SIZE)) -ge 512 ] && [ $((10#$IO_SIZE)) -le 1048576 ]; } \
  || { echo "--io-size must be an integer multiple of 512 in [512, 1048576]" >&2; exit 2; }
[ "$WORKLOAD" = randrw ] || [ $((10#$RWMIX)) -eq 100 ] || { echo "--rwmix must be 100 unless --workload randrw" >&2; exit 2; }
if [ "$TRACE" != none ] && { [ "$WORKLOAD" != randread ] || [ $((10#$IO_SIZE)) -ne 4096 ]; }; then
  echo "the trace replay is 4 KiB reads; use --trace none for other workloads or io sizes" >&2; exit 2
fi
rc_select_variant "$VARIANT"
TGT_VARIANT="${TGT_VARIANT:-$VARIANT}"
RC_RPC_PY="$RC_ROOT/.build/spdk-host-$TGT_VARIANT/src/spdk/scripts/rpc.py"   # talk to the target with its own rpc.py
# >>> rc_point_env
# The point's host env (P4.6 M6): every inherited SPDK_IAU_* / SPDK_UNCORE_* goes first, so a caller's exported knob
# never leaks into a point; then the point's env column, checked against the mode (tile knobs mode 6 only, S2 mode 0
# only); rc_host_mode_env (after the target starts) adds the mode's own switches.
rc_scrub_host_env
rc_export_inv_env "$ENV_SPEC" "$MODE" || exit 2
# <<< rc_point_env
APP_N=$(rc_cpuset_count "$APP_CPUS")
if [ "$APP_N" -eq 1 ]; then SUB="core0_qp${QPAIRS}"; else SUB="core_count${APP_N}_qp${QPAIRS}"; fi
RUN_DIR="$RC_RESULTS/$TAG/$SUB"; LOG_DIR="$RUN_DIR/logs"; mkdir -p "$LOG_DIR"
STEM="s4096_q${QD}_r${RUN_ID}"
[ "$LAT" = 1 ] || STEM="${STEM}_nolat"
rc_write_fingerprint "$RUN_DIR" || exit 1
if [ "$TGT_VARIANT" != "$VARIANT" ]; then
  _tf="$RC_ROOT/.build/spdk-host-$TGT_VARIANT/fingerprint.txt"
  [ -f "$_tf" ] || { echo "missing $_tf; build it with scripts/realcore/inv/build_variant.sh $TGT_VARIANT (or scripts/image/build_spdk_host.sh --variant $TGT_VARIANT)" >&2; exit 1; }
  if [ -f "$RUN_DIR/fingerprint_tgt.txt" ] && ! cmp -s "$_tf" "$RUN_DIR/fingerprint_tgt.txt"; then
    echo "FINGERPRINT_MISMATCH $RUN_DIR/fingerprint_tgt.txt (Invariant D); use a new --tag" >&2; exit 1
  fi
  cp -f "$_tf" "$RUN_DIR/fingerprint_tgt.txt"
fi
rc_host_check "$APP_CPUS,$DEV_CPUS${ENGINE_CPUS:+,$ENGINE_CPUS}" > "$LOG_DIR/hostcheck_$STEM.txt" || { cat "$LOG_DIR/hostcheck_$STEM.txt" >&2; exit 75; }

# CONTROLLERS>1 deliberately forces RC_PG_BY_QID=0 even if the caller set 1 (qid->pg map collides across controllers).
_pg="${RC_PG_BY_QID:-1}"; [ "$CONTROLLERS" -gt 1 ] && _pg=0
TGT_ENG=(); [ -n "$ENGINE_CPUS" ] && TGT_ENG=(--engine-cpus "$ENGINE_CPUS")
# the target never sees SPDK_IAU_* (start_target.sh unsets every one for nvmf_tgt): only the host gets the tile knobs
RC_VARIANT="$TGT_VARIANT" RC_PG_BY_QID="$_pg" "$RC_LIB_DIR/start_target.sh" --dev-cpus "$DEV_CPUS" --mode "$TGT_MODE" \
  --bdev "$BDEV" --lat-us "$LAT_US" --controllers "$CONTROLLERS" "${TGT_ENG[@]}" \
  > "$LOG_DIR/target_start_$STEM.log" 2>&1 || { cat "$LOG_DIR/target_start_$STEM.log" >&2; exit 1; }

# Host-side mode switches mirror phase1_trace_replay.sh: Mode D = SPDK_UNCORE_MODE_C=1 + _D=1.
rc_host_mode_env "$MODE"
export SPDK_COMPUTE_PER_IO="$COMPUTE"
export PERF_QPAIR_ORDERED="${RC_PERF_QPAIR_ORDERED:-1}"
# The host cycle buffer is one global array: per-IO breakdown only with the instr build at 1 app core.
if [ "$VARIANT" = instr ] && [ "$APP_N" -eq 1 ]; then
  export SPDK_IO_CYCLE_ENABLE=1 SPDK_IO_CYCLE_OUT="$LOG_DIR/cycle_breakdown_$STEM.csv"
else
  export SPDK_IO_CYCLE_ENABLE=0; unset SPDK_IO_CYCLE_OUT
fi

# >>> rc_wait_perf
# Bounded wait on our own perf child: poll kill -0 once a second until $2 seconds after $3 (a $SECONDS stamp).
# Timeout: PERF_WATCHDOG line to stderr (and appended to log $4), SIGTERM to $1 only, <=10 s grace, then SIGKILL; rc 124.
rc_wait_perf() {
  local pid="$1" limit="$2" t0="$3" log="${4:-}" waited i rc
  while kill -0 "$pid" 2>/dev/null; do
    waited=$((SECONDS - t0))
    if [ "$waited" -ge "$limit" ]; then
      echo "PERF_WATCHDOG timeout pid=$pid after=${waited}s" >&2
      [ -n "$log" ] && echo "PERF_WATCHDOG timeout pid=$pid after=${waited}s" >> "$log"
      kill -TERM "$pid" 2>/dev/null
      for ((i = 0; i < 10; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
      kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return 124
    fi
    sleep 1
  done
  wait "$pid"; rc=$?
  return "$rc"
}
# <<< rc_wait_perf

TRID_ARGS=()
for ((i = 0; i < CONTROLLERS; i++)); do
  TRID_ARGS+=(-r "trtype:VFIOUSER traddr:$(rc_vfu_dir "$i") subnqn:$(rc_nqn "$i")")
done
TRACE_ARGS=(--trace-file "$TRACE"); [ "$TRACE" = none ] && TRACE_ARGS=()
APP_MASK=$(rc_cpuset_to_mask "$APP_CPUS")
LAT_ARG=(); [ "$LAT" = 1 ] && LAT_ARG=(-L)
MIX_ARG=(); [ "$WORKLOAD" = randrw ] && MIX_ARG=(-M "$RWMIX")
# shellcheck disable=SC2206
PERF_CMD=("$RC_BIN/spdk_nvme_perf" "${TRID_ARGS[@]}" -w "$WORKLOAD" -o "$IO_SIZE" "${MIX_ARG[@]}" -q "$QD" -t "$TIME"
          --warmup-time "$WARMUP" -c "$APP_MASK" -P "$QPAIRS" "${LAT_ARG[@]}" --transport-stats -Q 1
          "${TRACE_ARGS[@]}" $(rc_mem_args "$RC_PERF_MEM_MB") $RC_PERF_EXTRA)
if [ "$PERF_STAT" = 1 ]; then
  PERF_CMD=("$RC_PERF_TOOL" stat -e cycles,instructions,LLC-load-misses -x ';'
            -o "$LOG_DIR/perfstat_$STEM.txt" -- "${PERF_CMD[@]}")
fi
echo "PERF_WORKLOAD workload=$WORKLOAD rwmix=$RWMIX io_size=$IO_SIZE"
printf '%q ' "${PERF_CMD[@]}" > "$LOG_DIR/cmd_$STEM.txt"; echo >> "$LOG_DIR/cmd_$STEM.txt"
{ env | grep -E '^(SPDK_|IAU_|RC_)' | sort; echo "lat=$LAT"; echo "engine_cpus=$ENGINE_CPUS"; } > "$LOG_DIR/env_$STEM.txt"

"${PERF_CMD[@]}" > "$LOG_DIR/run_$STEM.log" 2>&1 < /dev/null &
PERF_PID=$!
PERF_T0=$SECONDS
sleep "$((WARMUP + 1))"; rc_rpc thread_get_stats > "$LOG_DIR/devstats_a_$STEM.json" 2>/dev/null
rc_rpc nvmf_subsystem_get_qpairs "$RC_NQN" > "$LOG_DIR/qpairs_$STEM.json" 2>/dev/null || true
for ((i = 1; i < CONTROLLERS; i++)); do
  rc_rpc nvmf_subsystem_get_qpairs "$(rc_nqn "$i")" > "$LOG_DIR/qpairs_${STEM}_c$i.json" 2>/dev/null || true
done
rc_rpc framework_get_reactors > "$LOG_DIR/reactors_$STEM.json" 2>/dev/null || true
if [ "$PERF_STAT" = 2 ]; then
  _w0=$(date +%s.%N)
  "$RC_PERF_TOOL" stat -x ';' -e cycles,instructions,task-clock -p "$PERF_PID" \
    -o "$LOG_DIR/perfwin_$STEM.txt" -- sleep "$((TIME - 2))" > /dev/null 2>&1
  _prc=$?
  if [ "$_prc" -ne 0 ]; then
    echo "WARN perf stat window failed rc=$_prc; falling back to sleep" >&2
    rm -f "$LOG_DIR/perfwin_$STEM.txt"
    _rem=$(python3 -I -c 'import sys,time; print("%.3f" % max(0.0, float(sys.argv[2]) - (time.time() - float(sys.argv[1]))))' "$_w0" "$((TIME - 2))")
    sleep "$_rem"
  fi
else
  sleep "$((TIME - 2))"
fi
rc_rpc thread_get_stats > "$LOG_DIR/devstats_b_$STEM.json" 2>/dev/null
rc_wait_perf "$PERF_PID" "$((WARMUP + TIME + ${RC_PERF_GRACE_S:-60}))" "$PERF_T0" "$LOG_DIR/run_$STEM.log"; PERF_RC=$?
RC_VARIANT="$TGT_VARIANT" "$RC_LIB_DIR/stop_target.sh" > /dev/null 2>&1
cp -f "$RC_TGT_LOG" "$LOG_DIR/nvmf_tgt_$STEM.log"

if [ "$PERF_RC" -ne 0 ] || ! grep -q '^Total' "$LOG_DIR/run_$STEM.log"; then
  echo "perf failed rc=$PERF_RC; see $LOG_DIR/run_$STEM.log" >&2
  tail -20 "$LOG_DIR/run_$STEM.log" >&2
  exit 1
fi
ROW=$(python3 -I "$RC_LIB_DIR/make_row.py" --perf-log "$LOG_DIR/run_$STEM.log" \
  --cycle-csv "$LOG_DIR/cycle_breakdown_$STEM.csv" --perf-stat "$LOG_DIR/perfstat_$STEM.txt" \
  --qd "$QD" --qpairs "$QPAIRS" --io-size "$IO_SIZE" --run-id "$RUN_ID" --duration "$TIME" \
  --trace "$TRACE" --core-count "$APP_N" --core-mask "$APP_MASK" \
  --out "$RUN_DIR/phase1_results.csv") || exit 1
BUSY=$(python3 -I "$RC_LIB_DIR/device_stats.py" "$LOG_DIR/devstats_a_$STEM.json" \
  "$LOG_DIR/devstats_b_$STEM.json" --out "$LOG_DIR/device_busy_$STEM.json" | sed -n 's/^MAX_BUSY=//p')
[ -n "$BUSY" ] || { echo "device_stats failed" >&2; exit 1; }
ACTIVE_PG=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["active_poll_groups"])' "$LOG_DIR/device_busy_$STEM.json")
[ -n "$ACTIVE_PG" ] || { echo "active_poll_groups missing" >&2; exit 1; }
IOPS=$(printf '%s\n' "$ROW" | sed -n 's/.*iops=\([0-9.]*\).*/\1/p')
[ -n "$IOPS" ] || { echo "iops missing from row" >&2; exit 1; }
rc_write_corebudget "$RUN_DIR" "$APP_CPUS" "$DEV_CPUS" "$PLACEMENT" "$MODE" "$VARIANT" "$STEM" "$BUSY" "$IOPS" "$ACTIVE_PG"
NONBIND=$(sed -n 's/.*nonbinding_point=\([A-Z]*\).*/\1/p' "$RUN_DIR/corebudget.txt" | tail -1)
CYC_KV="cycles_per_io=NA ghz=NA ipc=NA"
if [ "$PERF_STAT" = 2 ] && [ -f "$LOG_DIR/perfwin_$STEM.txt" ]; then
  CYC_KV=$(python3 -I "$RC_LIB_DIR/inv/perfstat_window.py" "$LOG_DIR/perfwin_$STEM.txt" \
           --iops "$IOPS" --window-s "$((TIME - 2))") || CYC_KV="cycles_per_io=NA ghz=NA ipc=NA"
fi
echo "POINT_RESULT $ROW busy_max=$BUSY nonbinding=$NONBIND $CYC_KV run_dir=$RUN_DIR"
