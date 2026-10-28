#!/usr/bin/env bash
# start_target.sh — launch nvmf_tgt as a vfio-user software NVMe controller.
# Usage: start_target.sh --dev-cpus 4-7 [--mode 0|4|5] [--bdev fastpath|null|delay] [--lat-us 1]
#                        [--qdepth 1024] [--max-qpairs 64] [--controllers 1] [--engine-cpus 6,7]
# --mode 5 (IAU-next P3, arm I1e): nvmf poll groups (SSD threads) on --dev-cpus run the vanilla NVMe path;
#   one IAU-engine SPDK thread per --engine-cpus core (IAU_ENGINE_CPUS) does the IAU's work. The reactor
#   mask is dev + engine CPUs, the main core the first dev CPU, and nvmf_set_config -m keeps poll groups
#   off the engine cores (the target starts with --wait-for-rpc). Other modes ignore --engine-cpus.
#   Device model per controller: IAU_DEV_<KEY>_C<i> overrides IAU_DEV_<KEY>; IAU_DEV_MODEL=sq|ctrlr.
# --controllers C: C independent vfio-user controllers (Null$i, cnode$i, $RC_RUNTIME/vfu$i); 1 = classic.
# --bdev fastpath (default): in-transport NVMeVirt-style device model (gem5 fast-path formula,
#   IAU_DEV_LMIN_NS/IAU_DEV_TMAX/IAU_DEV_CHANNELS); the Null0 bdev only supplies namespace identity.
# --bdev null|delay: generic nvmf request + bdev path (IAU_DEV_FASTPATH=0).
# Refused with exit 2 before any side effect: an inherited IAU_DEV_FASTPATH or IAU_DEV_FASTPATH_C<k> that
# contradicts --bdev (the target gets IAU_DEV_FASTPATH from --bdev), and an IAU_DEV_<KEY>_C<k> with k >= --controllers.
# Env: RC_VARIANT (lean|instr|inv-<name>|upstream variants) selects the build; IAU_* env vars pass through to the target.
# SPDK_IAU_* never reaches it (IAU-next P4.6): nvmf_tgt links libspdk_nvme, whose selector constructor reads them at
# load and aborts on a bad combination, so every SPDK_IAU_* name is unset for nvmf_tgt, even one the caller exported
# (harness mode 6 = this script's --mode 5 + host SPDK_IAU_TILE=1; the host alone gets the tile knobs).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
DEV_CPUS="4-7"; MODE=0; BDEV=fastpath; LAT_US=1; QDEPTH=1024; MAXQP=64; CONTROLLERS=1; ENGINE_CPUS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dev-cpus) DEV_CPUS="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --bdev) BDEV="$2"; shift 2 ;;
    --lat-us) LAT_US="$2"; shift 2 ;;
    --qdepth) QDEPTH="$2"; shift 2 ;;
    --max-qpairs) MAXQP="$2"; shift 2 ;;
    --controllers) CONTROLLERS="$2"; shift 2 ;;
    --engine-cpus) ENGINE_CPUS="$2"; shift 2 ;;
    *) rc_die "unknown argument $1" ;;
  esac
done
case "$MODE" in 0|4|5) ;; *) rc_die "--mode must be 0, 4 or 5" ;; esac
[ "$MODE" != 5 ] || [ -n "$ENGINE_CPUS" ] || rc_die "--mode 5 needs --engine-cpus"
[[ "$CONTROLLERS" =~ ^[1-9][0-9]*$ ]] || rc_die "--controllers must be a positive integer"
case "$BDEV" in
  fastpath) FASTPATH=1 ;;
  null|delay) FASTPATH=0 ;;
  *) rc_die "--bdev must be fastpath, null or delay (D1 default: no payload movement)" ;;
esac
# Device settings the target would silently not get, refused before any side effect (exit 2).
env_conflict() { echo "realcore: $*" >&2; exit 2; }
while IFS= read -r name; do
  case "$name" in
    IAU_DEV_FASTPATH|IAU_DEV_FASTPATH_C*)
      v="${!name}"
      if [ -n "$v" ] && { ! [[ "$v" =~ ^[0-9]+$ ]] || [ "$((10#$v))" -ne "$FASTPATH" ]; }; then
        env_conflict "$name=$v contradicts --bdev $BDEV (the target runs with IAU_DEV_FASTPATH=$FASTPATH)"
      fi ;;
  esac
  if [[ "$name" =~ ^IAU_DEV_[A-Z_]+_C([0-9]+)$ ]] && [ "$((10#${BASH_REMATCH[1]}))" -ge "$CONTROLLERS" ]; then
    env_conflict "$name names controller ${BASH_REMATCH[1]}, but --controllers is $CONTROLLERS"
  fi
done < <(compgen -e)
[ -x "$RC_BIN/nvmf_tgt" ] || rc_die "nvmf_tgt not built for variant $RC_VARIANT"
"$RC_LIB_DIR/stop_target.sh" >/dev/null 2>&1 || true
mkdir -p "$RC_RUNTIME"
rm -f "$RC_RPC_SOCK"
for ((i = 0; i < CONTROLLERS; i++)); do mkdir -p "$(rc_vfu_dir "$i")"; rm -f "$(rc_vfu_dir "$i")"/*; done
START_ARGS=()
if [ "$MODE" = 5 ]; then
  MASK="$(rc_cpuset_to_mask "$DEV_CPUS,$ENGINE_CPUS")"
  PG_MASK="$(rc_cpuset_to_mask "$DEV_CPUS")"
  MAIN_CPU="$(rc_cpuset_expand "$DEV_CPUS" | awk '{print $1}')"
  START_ARGS=(--wait-for-rpc -p "$MAIN_CPU")
else
  [ -z "$ENGINE_CPUS" ] || echo "ENGINE_CPUS_IGNORED mode=$MODE engine_cpus=$ENGINE_CPUS"
  ENGINE_CPUS=""
  MASK="$(rc_cpuset_to_mask "$DEV_CPUS")"
fi
# shellcheck disable=SC2046
# IAU_ENGINE_CPUS reaches the target only in mode 5; modes 0/4 get it unset (even if the caller exported it).
TGT_ENVV=(-u IAU_ENGINE_CPUS)
[ "$MODE" != 5 ] || TGT_ENVV=("IAU_ENGINE_CPUS=$ENGINE_CPUS")
TGT_SCRUB=()   # env -u options, before any NAME=VALUE (env stops option parsing at the first assignment)
while IFS= read -r name; do case "$name" in SPDK_IAU_*) TGT_SCRUB+=(-u "$name") ;; esac; done < <(compgen -e)
env "${TGT_SCRUB[@]}" "${TGT_ENVV[@]}" IAU_UNCORE_MODE="$MODE" IAU_PG_BY_QID="${RC_PG_BY_QID:-1}" IAU_DEV_FASTPATH="$FASTPATH" \
  setsid "$RC_BIN/nvmf_tgt" -m "$MASK" "${START_ARGS[@]}" $(rc_mem_args "$RC_TGT_MEM_MB") \
  -r "$RC_RPC_SOCK" $RC_TGT_EXTRA > "$RC_TGT_LOG" 2>&1 < /dev/null &
echo $! > "$RC_TGT_PIDFILE"
for _ in $(seq 1 100); do [ -S "$RC_RPC_SOCK" ] && break; sleep 0.1; done
# On any failure after the launch (init, transport, bdevs, subsystems, listeners, vfio-user socket) stop the
# target we just started, so no half-configured nvmf_tgt survives (set -e alone would exit and leave it running).
init_fail() {   # $1 = message
  "$RC_LIB_DIR/stop_target.sh" >/dev/null 2>&1 || true
  tail -20 "$RC_TGT_LOG" >&2 2>/dev/null || true
  rc_die "$1"
}
[ -S "$RC_RPC_SOCK" ] || init_fail "nvmf_tgt RPC socket did not appear"
if [ "$MODE" = 5 ]; then
  rc_rpc nvmf_set_config -m "$PG_MASK" >/dev/null || init_fail "nvmf_set_config -m $PG_MASK failed"
  rc_rpc framework_start_init >/dev/null || init_fail "framework_start_init failed"
fi
rc_rpc framework_wait_init >/dev/null || init_fail "framework_wait_init failed"
rc_rpc nvmf_create_transport -t VFIOUSER -q "$QDEPTH" -m "$MAXQP" >/dev/null \
  || init_fail "nvmf_create_transport failed"
for ((i = 0; i < CONTROLLERS; i++)); do
  # 40 GiB covers the 38.15 GiB span of the BigANN trace; a null bdev uses no memory.
  rc_rpc bdev_null_create "Null$i" 40960 4096 >/dev/null || init_fail "bdev_null_create Null$i failed"
  NS_BDEV="Null$i"
  if [ "$BDEV" = delay ]; then
    rc_rpc bdev_delay_create -b "Null$i" -d "Delay$i" -r "$LAT_US" -t "$LAT_US" -w "$LAT_US" -n "$LAT_US" >/dev/null \
      || init_fail "bdev_delay_create Delay$i failed"
    NS_BDEV="Delay$i"
  fi
  rc_rpc nvmf_create_subsystem "$(rc_nqn "$i")" -a -s "REALCORE$i" -m 1 >/dev/null \
    || init_fail "nvmf_create_subsystem $(rc_nqn "$i") failed"
  rc_rpc nvmf_subsystem_add_ns "$(rc_nqn "$i")" "$NS_BDEV" >/dev/null \
    || init_fail "nvmf_subsystem_add_ns $(rc_nqn "$i") $NS_BDEV failed"
  rc_rpc nvmf_subsystem_add_listener "$(rc_nqn "$i")" -t VFIOUSER -a "$(rc_vfu_dir "$i")" -s 0 >/dev/null \
    || init_fail "nvmf_subsystem_add_listener $(rc_nqn "$i") failed"
  for _ in $(seq 1 50); do [ -S "$(rc_vfu_dir "$i")/cntrl" ] && break; sleep 0.1; done
  [ -S "$(rc_vfu_dir "$i")/cntrl" ] || init_fail "vfio-user socket $(rc_vfu_dir "$i")/cntrl did not appear"
done
echo "TARGET_READY variant=$RC_VARIANT mode=$MODE bdev=$BDEV controllers=$CONTROLLERS dev_cpus=$DEV_CPUS engine_cpus=${ENGINE_CPUS:-none} mask=$MASK pid=$(cat "$RC_TGT_PIDFILE")"
