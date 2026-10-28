#!/usr/bin/env bash
# lib.sh — shared settings and helpers for the real-core (vfio-user) harness.
# Source it:  . "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
RC_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RC_ROOT="$(cd "$RC_LIB_DIR/../.." && pwd)"
RC_RUNTIME="${RC_RUNTIME:-$RC_ROOT/.build/run}"
RC_RPC_SOCK="$RC_RUNTIME/tgt.sock"
RC_VFU_DIR="$RC_RUNTIME/vfu0"
RC_NQN="nqn.2026-10.io.realcore:cnode0"
RC_TGT_PIDFILE="$RC_RUNTIME/nvmf_tgt.pid"
RC_TGT_LOG="$RC_RUNTIME/nvmf_tgt.log"
RC_PERF_TOOL="${RC_PERF_TOOL:-/usr/lib/linux-tools-5.15.0-198/perf}"
RC_TRACE_DEFAULT="$RC_ROOT/artifacts/bigann/diskann_bigann_trace.bin"
RC_RESULTS="${RC_RESULTS:-$RC_ROOT/results/realcore}"
RC_HUGE="${RC_HUGE:-0}"                 # 0 = --no-huge (memfd-backed), 1 = hugepages (admin, spec App. A)
RC_TGT_MEM_MB="${RC_TGT_MEM_MB:-2048}"
RC_PERF_MEM_MB="${RC_PERF_MEM_MB:-2048}"
RC_PERF_EXTRA="${RC_PERF_EXTRA:-}"      # set by the T1.6 hugepage decision (e.g. "-g")
RC_TGT_EXTRA="${RC_TGT_EXTRA:-}"

rc_die() { echo "realcore: $*" >&2; exit 1; }

rc_select_variant() {   # lean | instr | inv-* | nx-* | upstream*; never ut-* (unit-test trees, no measurement binaries)
  case "$1" in
    ut-*) rc_die "variant $1 is a unit-test tree (build_spdk_host.sh ut-*), never a measurement binary" ;;
  esac
  RC_VARIANT="$1"
  RC_BUILD="$RC_ROOT/.build/spdk-host-$RC_VARIANT"
  RC_SPDK="$RC_BUILD/src/spdk"
  RC_BIN="$RC_SPDK/build/bin"
  RC_RPC_PY="$RC_SPDK/scripts/rpc.py"
  export RC_VARIANT
}
rc_select_variant "${RC_VARIANT:-lean}"

rc_cpuset_expand() {    # "0-3,16" -> "0 1 2 3 16"
  local part a b i out=()
  local -a parts
  IFS=',' read -ra parts <<< "$1"
  for part in "${parts[@]}"; do
    if [[ "$part" == *-* ]]; then
      a=${part%-*}; b=${part#*-}
      for ((i = a; i <= b; i++)); do out+=("$i"); done
    elif [ -n "$part" ]; then
      out+=("$part")
    fi
  done
  echo "${out[@]}"
}
rc_cpuset_count() { local -a c; read -ra c <<< "$(rc_cpuset_expand "$1")"; echo "${#c[@]}"; }
rc_cpuset_to_mask() {
  local m=0 c
  for c in $(rc_cpuset_expand "$1"); do m=$(( m | (1 << c) )); done
  printf '0x%x\n' "$m"
}

rc_mem_args() {         # $1 = MB
  if [ "$RC_HUGE" -eq 1 ]; then echo "-s $1"; else echo "--no-huge -s $1"; fi
}

rc_rpc() { python3 "$RC_RPC_PY" -s "$RC_RPC_SOCK" "$@"; }

# Exit 0 if every CPU in the set was < 10 % busy over 1 s, else print HOST_BUSY and exit 1.
# The same sample also logs one "HOST_QUIET cpu=N util=X" line per measurement CPU (0-7,16-23);
# co-tenant load on CPUs outside the set is logged only, never fatal (review C-01, R2-12).
# RC_PROC_STAT=<a>[:<b>] substitutes the stat files for the first/second snapshot (unit test).
rc_host_check() {
  python3 -I - "$(rc_cpuset_expand "$1")" <<'PY'
import os, sys, time
want = [int(c) for c in sys.argv[1].split()]
paths = os.environ.get("RC_PROC_STAT", "/proc/stat").split(":")
def snap(path):
    d = {}
    with open(path) as fh:
        for line in fh:
            if line.startswith("cpu") and line[3].isdigit():
                f = line.split()
                v = [int(x) for x in f[1:]]
                d[int(f[0][3:])] = (sum(v), v[3] + v[4])
    return d
a = snap(paths[0]); time.sleep(1.0); b = snap(paths[-1])
bad = []
for c in sorted(set(want) | set(range(8)) | set(range(16, 24))):
    if c not in a or c not in b:
        continue
    tot = b[c][0] - a[c][0]; idle = b[c][1] - a[c][1]
    util = 0.0 if tot <= 0 else 100.0 * (tot - idle) / tot
    print("HOST_QUIET cpu=%d util=%.1f" % (c, util))
    if c in want and util > 10.0:
        bad.append("cpu%d=%.0f%%" % (c, util))
if bad:
    print("HOST_BUSY " + " ".join(bad)); sys.exit(1)
print("HOST_QUIET")
PY
}

# Copy the variant fingerprint into a run dir; refuse if the run dir already
# holds a different fingerprint (spec Invariant D: one binary per campaign tag).
rc_write_fingerprint() {   # $1 = run dir
  local src="$RC_BUILD/fingerprint.txt" dst="$1/fingerprint.txt"
  [ -f "$src" ] || rc_die "missing $src (run scripts/image/build_spdk_host.sh --variant $RC_VARIANT)"
  if [ -f "$dst" ] && ! cmp -s "$src" "$dst"; then
    echo "FINGERPRINT_MISMATCH $dst differs from $src (Invariant D); use a new --tag" >&2
    return 1
  fi
  cp -f "$src" "$dst"
}

# Append one core-budget line per point (spec §6, §7.3).
rc_write_corebudget() {    # run_dir app_cpus dev_cpus placement mode variant stem busy_max iops active_pg
  local verdict busyv cap
  if [ "$5" = 4 ]; then cap="${RC_DEV_THREAD_CAPACITY_M4:-11330851}"; else cap="${RC_DEV_THREAD_CAPACITY_M0:-12740683}"; fi
  cap="${RC_DEV_THREAD_CAPACITY:-$cap}"
  verdict=$(python3 -I "$RC_LIB_DIR/nonbinding.py" thread --iops "$9" --active-pg "${10}" \
    --capacity "$cap" | sed -n 's/^NONBINDING_THREAD=\([A-Z]*\).*/\1/p')
  busyv=$(python3 -I "$RC_LIB_DIR/nonbinding.py" point --busy "$8" | sed -n 's/^NONBINDING_POINT=\([A-Z]*\).*/\1/p')
  echo "stem=$7 mode=$5 variant=$6 placement=$4 app_cpus=$2 dev_cpus=$3 busy_max=$8 busy_check=$busyv nonbinding_point=$verdict" >> "$1/corebudget.txt"
}

rc_nqn()     { echo "nqn.2026-10.io.realcore:cnode$1"; }   # controller i (0 = RC_NQN)
rc_vfu_dir() { echo "$RC_RUNTIME/vfu$1"; }                  # controller i socket dir (0 = RC_VFU_DIR)

# Host-side mode switches (mirror phase1_trace_replay.sh): Mode D = SPDK_UNCORE_MODE_C=1 + _D=1.
# mode 5 (IAU-next P3, arm I1e) = Mode D + SPDK_IAU_ENGINE=1 (register with and ring the IAU engine).
# mode 6 (IAU-next P4.6) = SPDK_IAU_TILE=1 alone (IAU tile host; no Mode C/D, no SPDK_IAU_ENGINE: the host library refuses
# them together); every other mode unsets SPDK_IAU_TILE. The tile knobs of an env column (rc_export_inv_env) are kept.
# The TARGET never sees SPDK_IAU_* (start_target.sh unsets them for nvmf_tgt).
rc_host_mode_env() {    # $1 = mode (0|4|5|6)
  unset SPDK_UNCORE_MODE_B SPDK_UNCORE_MODE_C SPDK_UNCORE_MODE_D SPDK_UNCORE_MODE_E \
        SPDK_UNCORE_SQE16 SPDK_UNCORE_HOST_CID SPDK_UNCORE_MULTIPAGE SPDK_IAU_ENGINE SPDK_IAU_TILE
  case "$1" in
    4) export SPDK_UNCORE_MODE_C=1 SPDK_UNCORE_MODE_D=1 ;;
    5) export SPDK_UNCORE_MODE_C=1 SPDK_UNCORE_MODE_D=1 SPDK_IAU_ENGINE=1 ;;
    6) export SPDK_IAU_TILE=1 ;;
  esac
}

# The target mode (start_target.sh --mode) of a harness mode: mode 6 runs the mode 5 target (IAU-engine threads,
# which serve a tile queue when the host registers one); 0/4/5 are their own; anything else is refused (rc 2).
rc_target_mode() {      # $1 = harness mode (0|4|5|6)
  case "$1" in
    0|4|5) echo "$1" ;;
    6) echo 5 ;;
    *) echo "no target mode for harness mode $1" >&2; return 2 ;;
  esac
}

# Export "K=V;K=V" investigation knobs; only IAU_DEV_/IAU_CQ_/IAU_INV_/IAU_ENG_FAULT_/SPDK_INV_/SPDK_PERF_/SPDK_MD_
# names and the IAU tile host knobs (exact names) are allowed; SPDK_IAU_TILE / SPDK_IAU_ENGINE are the mode's
# (rc_host_mode_env). The tile host knobs need harness mode 6 and SPDK_IAU_S2 mode 0 (P4.6 I1: a mistyped I1v6n48 mode
# must not run I1); without a mode both are refused. Mirrors gen_inv_points.py ENV_PREFIXES / TILE_ONLY / S2_KNOB /
# MODE_SET and check_l.py TILE_ONLY_KNOBS. IAU_TILE_CTX_SPILL (P4.10b) mirrors gen_inv_points.py SPILL_KNOB / check_l.py TILE_SPILL_KNOB.
rc_export_inv_env() {   # $1 = spec string (may be empty), $2 = the harness mode (0|4|5|6)
  local kv mode="${2:-}"
  local -a kvs
  [ -n "$1" ] || return 0
  IFS=';' read -ra kvs <<< "$1"
  for kv in "${kvs[@]}"; do
    case "$kv" in
      SPDK_IAU_TILE=*|SPDK_IAU_ENGINE=*) echo "env entry not allowed: $kv (${kv%%=*} is set by the mode)" >&2; return 2 ;;
      IAU_DEV_*=*|IAU_CQ_*=*|IAU_INV_*=*|IAU_ENG_FAULT_*=*|SPDK_INV_*=*|SPDK_PERF_*=*|SPDK_MD_*=*) export "$kv" ;;
      SPDK_IAU_GROUP=*|SPDK_IAU_FAST=*|SPDK_IAU_RINGS=*|SPDK_IAU_RING_ENTRIES=*|SPDK_IAU_DESC_STRIDE=*|SPDK_IAU_RCONS=*|SPDK_IAU_DB_BATCH=*)
        [ "$mode" = 6 ] || { echo "env entry not allowed: $kv (a v6 knob on a mode ${mode:-?} point: mode 6 only)" >&2; return 2; }
        export "$kv" ;;
      IAU_TILE_CTX_SPILL=*)   # P4.10b (A19): the target ctx spill; 0|1, 1 on harness mode 6 only (= target mode 5 + v6 queues)
        case "${kv#*=}" in
          ""|0) ;;
          1) [ "$mode" = 6 ] || { echo "env entry not allowed: $kv (the ctx spill needs v6 queues: mode 6 only, not mode ${mode:-?})" >&2; return 2; } ;;
          *) echo "env entry not allowed: $kv (IAU_TILE_CTX_SPILL is 0 or 1)" >&2; return 2 ;;
        esac
        export "$kv" ;;
      SPDK_IAU_S2=*)
        [ "$mode" = 0 ] || { echo "env entry not allowed: $kv (S2 is a mode 0 knob, not mode ${mode:-?})" >&2; return 2; }
        export "$kv" ;;
      SPDK_IAU_S2_WB=*)   # P11 (review M3): the S2 SQE store type; cfg_load also refuses it without SPDK_IAU_S2=1
        [ "$mode" = 0 ] || { echo "env entry not allowed: $kv (S2 is a mode 0 knob, not mode ${mode:-?})" >&2; return 2; }
        export "$kv" ;;
      SPDK_DB_COST_NS=*|SPDK_DB_COST_IAU_NS=*) export "$kv" ;;   # P11 (review M2): doorbell-cost spin, any mode
      *) echo "env entry not allowed: $kv" >&2; return 2 ;;
    esac
  done
}

# Unset every inherited SPDK_IAU_* and SPDK_UNCORE_* (P4.6 M6): a point's host env is the point's env column and its
# mode's switches only, never a knob the caller's shell exported (cfg_load refuses SPDK_IAU_GROUP=1 in any mode).
# rc_host_mode_env unsets the SPDK_UNCORE_* names the host library reads today; this also covers future ones.
rc_scrub_host_env() {
  local n
  while IFS= read -r n; do
    case "$n" in SPDK_IAU_*|SPDK_UNCORE_*|SPDK_DB_COST_*) unset "$n" ;; esac
  done < <(compgen -e)
}
