#!/usr/bin/env bash
# profile_point.sh — profile the app process of one investigation cell (spec I3).
# Usage: profile_point.sh --variant V --mode 0|4|5|6 --app-cpus 0 --dev-cpus 4,5 --qd 128 --qpairs 2
#                         [--env "K=V;K=V"] [--tgt-variant V] [--engine-cpus 6,7] --out DIR
#                         [--extra-stat "EVLIST1|EVLIST2|..."] [--mem-event EV] [--annotate "sym1,sym2"] [--time S]
#                         [--compute N]   (SPDK_COMPUTE_PER_IO for the co-resident kernel, default 0)
# perf runs 5 s warmup + 30 s. Inside the window: perf record (8 s, cycles), stat A (6 s), stat B (6 s),
# precise L2-miss sampling (4 s, best effort). Writes run.log, syms.txt, stat_a.txt, stat_b.txt,
# mem_l2_syms.txt, ann_cpl.txt, ann_sub.txt, ann_mem_cpl.txt, meta.txt into DIR.
# Opt-in options (default behavior and outputs unchanged when not given):
#   --extra-stat "L1|L2|..."  each '|'-separated comma event list is one more 6 s perf stat window (after stat_b,
#                             before mem sampling) -> stat_c1.txt, stat_c2.txt, ...; meta line 3 gets rc_stat_c=rc1,rc2,...
#                             (by convention each list starts with cycles,instructions; not enforced)
#   --mem-event EV            precise sampling event for the mem record (default mem_load_retired.l2_miss:pp);
#                             meta line 1 gets mem_event=EV only when given
#   --annotate "s1,s2"        also write ann_<sym>.txt (cycles perf.data) and ann_mem_<sym>.txt (mem_l2.data, if sampled)
#   --time S                  perf -t duration (default 30). Windows must fit: 6 (marker sleep) + 8 + 6 + 6 + 4
#                             + 6*Nextra + 2 (margin) <= 5 (warmup) + S, else exit 2 before starting anything.
#   --engine-cpus L           IAU-next P4.7d: the IAU-engine cores of a mode 5 or 6 point (required there, as in
#                             run_point.sh): the target starts in rc_target_mode (6 -> 5) with --engine-cpus L, and
#                             the host check covers L; L must not overlap --app-cpus / --dev-cpus. Modes 0/4 pass it
#                             on (start_target.sh prints ENGINE_CPUS_IGNORED). Meta line 1 gets engine_cpus=L.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"
VARIANT=""; MODE=0; APP=0; DEV=4; QD=128; QP=1; ENV_SPEC=""; TGT=""; OUT=""; LAT=1
EXTRA_STAT=""; MEM_EV=""; ANNOT=""; TIME=30; COMPUTE=0; ENG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) VARIANT="$2"; shift 2 ;; --mode) MODE="$2"; shift 2 ;;
    --app-cpus) APP="$2"; shift 2 ;; --dev-cpus) DEV="$2"; shift 2 ;;
    --qd) QD="$2"; shift 2 ;; --qpairs) QP="$2"; shift 2 ;;
    --env) ENV_SPEC="$2"; shift 2 ;; --tgt-variant) TGT="$2"; shift 2 ;;
    --lat) LAT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --extra-stat) EXTRA_STAT="$2"; shift 2 ;; --mem-event) MEM_EV="$2"; shift 2 ;;
    --annotate) ANNOT="$2"; shift 2 ;; --time) TIME="$2"; shift 2 ;;
    --compute) COMPUTE="$2"; shift 2 ;;
    --engine-cpus) ENG="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[ -n "$VARIANT" ] && [ -n "$OUT" ] || { echo "--variant and --out required" >&2; exit 2; }
case "$LAT" in 0|1) ;; *) echo "--lat must be 0 or 1" >&2; exit 2 ;; esac
case "$COMPUTE" in ''|*[!0-9]*) echo "--compute must be a non-negative integer" >&2; exit 2 ;; esac
case "$TIME" in ''|*[!0-9]*) echo "--time must be a positive integer" >&2; exit 2 ;; esac
if [ -n "$ENG" ] && ! [[ "$ENG" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]]; then
  echo "--engine-cpus must be a CPU list like 6,7 or 6-7" >&2; exit 2
fi
case "$MODE" in 5|6) [ -n "$ENG" ] || { echo "--mode $MODE needs --engine-cpus" >&2; exit 2; } ;; esac
TGT_MODE=$(rc_target_mode "$MODE") || exit 2
if [ -n "$ENG" ]; then
  for _c in $(rc_cpuset_expand "$ENG"); do
    for _o in $(rc_cpuset_expand "$APP,$DEV"); do
      [ "$_c" != "$_o" ] || { echo "--engine-cpus $ENG overlaps --app-cpus/--dev-cpus" >&2; exit 2; }
    done
  done
fi
EXTRA_LISTS=()
[ -n "$EXTRA_STAT" ] && IFS='|' read -r -a EXTRA_LISTS <<< "$EXTRA_STAT"
NEXTRA=${#EXTRA_LISTS[@]}
NEED=$((6 + 8 + 6 + 6 + 4 + 6 * NEXTRA + 2))   # marker sleep + record + stat A + stat B + mem + extras + margin
HAVE=$((5 + TIME))                              # warmup + -t
if [ "$NEED" -gt "$HAVE" ]; then
  echo "profile_point: windows do not fit: need $NEED s (6 sleep + 8 + 6 + 6 + 4 + 6*$NEXTRA extra + 2 margin) but perf runs 5 + $TIME = $HAVE s; raise --time to at least $((NEED - 5))" >&2
  exit 2
fi
mkdir -p "$OUT"
rc_select_variant "$VARIANT"
TGT="${TGT:-$VARIANT}"
RC_RPC_PY="$RC_ROOT/.build/spdk-host-$TGT/src/spdk/scripts/rpc.py"
rc_scrub_host_env                               # as run_point.sh: no inherited SPDK_IAU_* / SPDK_UNCORE_*
rc_export_inv_env "$ENV_SPEC" "$MODE" || exit 2   # the mode-bound knobs are checked against --mode
rc_host_check "$APP,$DEV${ENG:+,$ENG}" > "$OUT/hostcheck.txt" || { cat "$OUT/hostcheck.txt" >&2; exit 75; }
PID=""
cleanup() {   # idempotent: kills only our perf PID, stops the target
  if [ -n "$PID" ] && kill -0 "$PID" 2> /dev/null; then kill "$PID" 2> /dev/null; wait "$PID" 2> /dev/null; fi
  RC_VARIANT="$TGT" "$RC_LIB_DIR/stop_target.sh" > /dev/null 2>&1
}
TGT_ENG=(); [ -n "$ENG" ] && TGT_ENG=(--engine-cpus "$ENG")
if ! RC_VARIANT="$TGT" "$RC_LIB_DIR/start_target.sh" --dev-cpus "$DEV" --mode "$TGT_MODE" --bdev fastpath --lat-us 1 \
  "${TGT_ENG[@]}" > "$OUT/target.log" 2>&1; then
  cat "$OUT/target.log" >&2
  RC_VARIANT="$TGT" "$RC_LIB_DIR/stop_target.sh" > /dev/null 2>&1
  exit 1
fi
trap cleanup EXIT INT TERM
rc_host_mode_env "$MODE"
export SPDK_COMPUTE_PER_IO="$COMPUTE" PERF_QPAIR_ORDERED=1 SPDK_IO_CYCLE_ENABLE=0
TRID="trtype:VFIOUSER traddr:$(rc_vfu_dir 0) subnqn:$(rc_nqn 0)"
P="$RC_PERF_TOOL"
LAT_ARG=(); [ "$LAT" = 1 ] && LAT_ARG=(-L)
stdbuf -oL -eL "$RC_BIN/spdk_nvme_perf" -r "$TRID" -w randread -o 4096 -q "$QD" -t "$TIME" --warmup-time 5 \
  -c "$(rc_cpuset_to_mask "$APP")" -P "$QP" "${LAT_ARG[@]}" --transport-stats -Q 1 --trace-file "$RC_TRACE_DEFAULT" \
  $(rc_mem_args "$RC_PERF_MEM_MB") $RC_PERF_EXTRA > "$OUT/run.log" 2>&1 < /dev/null &
PID=$!
# Marker printed by perf.c once workers launch (spdk/app/spdk_nvme_perf/perf.c).
MARKER="Initialization complete. Launching workers."
FOUND=0
for _ in $(seq 600); do
  grep -qF "$MARKER" "$OUT/run.log" 2> /dev/null && { FOUND=1; break; }
  kill -0 "$PID" 2> /dev/null || break
  sleep 0.1
done
if [ "$FOUND" != 1 ]; then
  echo "profile_point: FAIL: perf init marker not seen within 60 s (or perf exited first); see $OUT/run.log" >&2
  exit 1
fi
if ! kill -0 "$PID" 2> /dev/null; then
  echo "profile_point: FAIL: init marker seen but perf had already exited (buffered output or run too short)" >&2
  exit 1
fi
sleep 6   # 5 s warmup + 1 s
RC_REC=NA; RC_SA=NA; RC_SB=NA; RC_MEM=NA; MEM=skipped; ALIVE=1
alive() { kill -0 "$PID" 2> /dev/null || ALIVE=0; [ "$ALIVE" = 1 ]; }
if alive; then
  "$P" record -F 15000 -p "$PID" -o "$OUT/perf.data" -- sleep 8 > "$OUT/record.log" 2>&1; RC_REC=$?
fi
if alive; then
  "$P" stat -x ';' -o "$OUT/stat_a.txt" -p "$PID" \
    -e cycles,instructions,branch-misses,mem_load_retired.l1_miss,mem_load_retired.l2_miss,mem_load_retired.l3_miss \
    -- sleep 6 > /dev/null 2>&1; RC_SA=$?
fi
if alive; then
  "$P" stat -x ';' -o "$OUT/stat_b.txt" -p "$PID" \
    -e cycles,instructions,mem_load_l3_hit_retired.xsnp_hitm,cycle_activity.stalls_l1d_miss,cycle_activity.stalls_l2_miss,cycle_activity.stalls_l3_miss \
    -- sleep 6 > /dev/null 2>&1; RC_SB=$?
fi
RC_C=""
for i in "${!EXTRA_LISTS[@]}"; do
  if alive; then
    "$P" stat -x ';' -o "$OUT/stat_c$((i + 1)).txt" -p "$PID" -e "${EXTRA_LISTS[$i]}" \
      -- sleep 6 > /dev/null 2>&1; RC_C="${RC_C:+$RC_C,}$?"
  else
    RC_C="${RC_C:+$RC_C,}NA"
  fi
done
if alive; then
  MEM=ok
  "$P" record -e "${MEM_EV:-mem_load_retired.l2_miss:pp}" -c 2003 -d -p "$PID" -o "$OUT/mem_l2.data" -- sleep 4 \
    > "$OUT/mem_record.log" 2>&1; RC_MEM=$?
  [ "$RC_MEM" -eq 0 ] || MEM=unavailable
fi
wait "$PID"; RC=$?
PID=""
RC_VARIANT="$TGT" "$RC_LIB_DIR/stop_target.sh" > /dev/null 2>&1
cp -f "$RC_TGT_LOG" "$OUT/nvmf_tgt.log" 2> /dev/null
[ -f "$OUT/perf.data" ] && "$P" report --stdio --sort sym -i "$OUT/perf.data" > "$OUT/syms.txt" 2> /dev/null
[ -f "$OUT/perf.data" ] && "$P" annotate --stdio -i "$OUT/perf.data" nvme_pcie_qpair_process_completions > "$OUT/ann_cpl.txt" 2> /dev/null
[ -f "$OUT/perf.data" ] && "$P" annotate --stdio -i "$OUT/perf.data" nvme_pcie_qpair_submit_request > "$OUT/ann_sub.txt" 2> /dev/null
if [ "$MEM" = ok ]; then
  "$P" report --stdio --sort sym -i "$OUT/mem_l2.data" > "$OUT/mem_l2_syms.txt" 2> /dev/null
  "$P" report --stdio --sort symbol_daddr -i "$OUT/mem_l2.data" > "$OUT/mem_l2_daddr.txt" 2> /dev/null || true
  "$P" annotate --stdio -i "$OUT/mem_l2.data" nvme_pcie_qpair_process_completions > "$OUT/ann_mem_cpl.txt" 2> /dev/null
fi
if [ -n "$ANNOT" ]; then
  IFS=',' read -r -a ANN_SYMS <<< "$ANNOT"
  for sym in "${ANN_SYMS[@]}"; do
    [ -n "$sym" ] || continue
    [ -f "$OUT/perf.data" ] && "$P" annotate --stdio -i "$OUT/perf.data" "$sym" > "$OUT/ann_$sym.txt" 2> /dev/null
    [ "$MEM" = ok ] && "$P" annotate --stdio -i "$OUT/mem_l2.data" "$sym" > "$OUT/ann_mem_$sym.txt" 2> /dev/null
  done
fi
IOPS=$(awk '/^Total/{for(i=1;i<=NF;i++) if($i==":"){print $(i+1); exit}}' "$OUT/run.log")
{ echo "variant=$VARIANT tgt=$TGT mode=$MODE app=$APP dev=$DEV qd=$QD qpairs=$QP lat=$LAT env=$ENV_SPEC${MEM_EV:+ mem_event=$MEM_EV}$([ "$COMPUTE" != 0 ] && echo " compute=$COMPUTE")${ENG:+ engine_cpus=$ENG}"
  echo "perf_rc=$RC iops=${IOPS:-NA} mem_sampling=$MEM stat_window_s=6"
  echo "rc_record=$RC_REC rc_stat_a=$RC_SA rc_stat_b=$RC_SB rc_mem=$RC_MEM app_alive_through_sampling=$ALIVE${EXTRA_STAT:+ rc_stat_c=$RC_C}"; } > "$OUT/meta.txt"
rm -f "$OUT/perf.data" "$OUT/mem_l2.data"   # large; the text reports are kept
cat "$OUT/meta.txt"
[ "$RC" -eq 0 ] && [ -n "$IOPS" ] && [ "$ALIVE" = 1 ] && [ "$RC_REC" = 0 ] && [ "$RC_SA" = 0 ] && [ "$RC_SB" = 0 ] && [ -s "$OUT/syms.txt" ]
