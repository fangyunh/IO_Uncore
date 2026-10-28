#!/usr/bin/env bash
# campaign.sh — run every point of a points CSV through run_point.sh, resumably.
# Usage: campaign.sh <points.csv> <campaign_name>
# Writes results/realcore/<campaign_name>/{ledger.csv,campaign.log,points.csv}.
# A point already marked ok in the ledger is skipped (resume after interruption).
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
POINTS="$1"; NAME="$2"
CDIR="$RC_RESULTS/$NAME"; mkdir -p "$CDIR"
cp -f "$POINTS" "$CDIR/points.csv"
LEDGER="$CDIR/ledger.csv"; LOG="$CDIR/campaign.log"
[ -f "$LEDGER" ] || echo "point_id,status,iops,busy_max,p50_us,p99_us,mmio_per_io,run_dir,finished_at,cycles_per_io,ghz" > "$LEDGER"
LEDGER_NEW=0; head -1 "$LEDGER" | grep -q ',cycles_per_io,ghz$' && LEDGER_NEW=1
kv() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | tail -1; }
tail -n +2 "$POINTS" | while IFS=, read -r point_id tag variant mode app_cpus dev_cpus qd qpairs bdev lat_us placement compute run_id warmup time lat env controllers tgt_variant trace arm cell workload rwmix io_size engine_cpus _rest; do
  if grep -q "^${point_id},ok," "$LEDGER"; then continue; fi
  EXTRA=()
  [ -n "${env:-}" ] && EXTRA+=(--env "$env")
  [ -n "${controllers:-}" ] && EXTRA+=(--controllers "$controllers")
  [ -n "${tgt_variant:-}" ] && EXTRA+=(--tgt-variant "$tgt_variant")
  [ -n "${trace:-}" ] && EXTRA+=(--trace "$trace")
  [ -n "${workload:-}" ] && EXTRA+=(--workload "$workload")
  [ -n "${rwmix:-}" ] && EXTRA+=(--rwmix "$rwmix")
  [ -n "${io_size:-}" ] && EXTRA+=(--io-size "$io_size")
  [ -n "${engine_cpus:-}" ] && EXTRA+=(--engine-cpus "${engine_cpus//+/,}")
  [ -n "${RC_CAMPAIGN_PERF_STAT:-}" ] && EXTRA+=(--perf-stat "$RC_CAMPAIGN_PERF_STAT")
  attempt=0
  while :; do
    attempt=$((attempt + 1))
    OUT=$("$RC_LIB_DIR/run_point.sh" --tag "$tag" --variant "$variant" --mode "$mode" \
      --app-cpus "${app_cpus//+/,}" --dev-cpus "${dev_cpus//+/,}" --qd "$qd" --qpairs "$qpairs" \
      --bdev "$bdev" --lat-us "$lat_us" --placement "$placement" --compute-per-io "$compute" \
      --run-id "$run_id" --warmup "$warmup" --time "$time" --lat "${lat:-1}" "${EXTRA[@]}" 2>&1 < /dev/null)
    rc=$?
    { echo "=== [$(date -Is)] $point_id attempt $attempt rc=$rc"; echo "$OUT"; } >> "$LOG"
    if [ "$rc" -eq 75 ] && [ "$attempt" -lt 120 ]; then sleep 60; continue; fi
    break
  done
  RES=$(echo "$OUT" | grep '^POINT_RESULT' | tail -1)
  if [ "$rc" -eq 0 ] && [ -n "$RES" ]; then st=ok; else st="fail_rc$rc"; fi
  echo "$point_id,$st,$(kv "$RES" iops),$(kv "$RES" busy_max),$(kv "$RES" p50_us),$(kv "$RES" p99_us),$(kv "$RES" mmio_per_io),$(kv "$RES" run_dir),$(date -Is)$([ "$LEDGER_NEW" = 1 ] && echo ",$(kv "$RES" cycles_per_io),$(kv "$RES" ghz)")" >> "$LEDGER"
done
echo "CAMPAIGN_DONE $NAME ok=$(grep -c ',ok,' "$LEDGER") fail=$(grep -c ',fail_' "$LEDGER")"
