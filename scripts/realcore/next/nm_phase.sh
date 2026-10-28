#!/usr/bin/env bash
# nm_phase.sh: the linked-binary 64 B phase check (IAU-next P4.4 review I1; the P4 plan's "nm phase check").
# Compares, symbol by symbol, the address mod 64 of functions in two LINKED binaries, e.g. a snapshot's nvmf_tgt
# (.build/spdk-host-nx-p3a/src/spdk/build/bin/nvmf_tgt) and a scratch build's. The object-level guards
# (tests/realcore/test_{nvmf,pcie}_hot_layout.sh) are a proxy; this is the final word on the linked layout.
# Usage: nm_phase.sh BASE_BIN NEW_BIN [--set nvmf_tgt|perf] [--sym NAME]... [--pin0 NAME]...
#   default symbols: the target's per-IO and poll-loop set (reactor, SPDK thread, bdev completion, the five
#   guarded vfio_user functions, the device model, the P3 engine) = --set nvmf_tgt
#   --set perf: spdk_nvme_perf's set since P4.4 (the perf loop, the four ns_cmd entries, qpair / transport / poll
#   group, spdk_get_ticks, the 8 guarded pcie functions; 30 symbols) plus, as --pin0, the tile host functions P4.10c
#   pinned at 64 B starts (nvme_iau.c NVME_IAU_HOT_ALIGN) and P4.11's dispatcher, tile body and S2 functions (17)
#   --pin0 NAME: NAME must start at phase 0 in NEW_BIN (an absolute check: its base phase is printed, not compared,
#   and it may be absent from BASE_BIN, e.g. a snapshot from before the pin or without the object); P4.10f (the
#   P4.10c/d/e review's m8): every other NAME.<suffix> in NEW_BIN (a split: NAME.part.N) must start at phase 0 too,
#   one "NM_PHASE sym=<s> part=<name> new=<addr>/<mod64> <pin0 ok|PIN0 CHANGED>" line each
#   NAME also matches NAME.<suffix> (GCC's .constprop / .isra / .part clones; never .cold)
# Output: one "NM_PHASE sym=<s> base=<addr>/<mod64> new=<addr>/<mod64> <ok|CHANGED|MISSING|absent>" per symbol
# (pinned: "<pin0 ok|PIN0 CHANGED|MISSING>"), then "NM_PHASE OK" (exit 0) or "NM_PHASE FAIL" (exit 1). absent = in
# neither binary (not counted); MISSING = in one only (fails; for --pin0, missing from NEW_BIN). Exit 2 on bad
# arguments. Read-only.
set -u
[ $# -ge 2 ] || { echo "usage: nm_phase.sh BASE_BIN NEW_BIN [--set nvmf_tgt|perf] [--sym NAME]... [--pin0 NAME]..." >&2; exit 2; }
A="$1"; B="$2"; shift 2
[ -f "$A" ] && [ -f "$B" ] || { echo "no such binary: $A or $B" >&2; exit 2; }
SET_NVMF_TGT=(reactor_run thread_poll spdk_thread_poll spdk_bdev_io_complete
  nvmf_vfio_user_sq_poll nvmf_vfio_user_poll_group_poll post_completion handle_cmd_req iau_stage_cpl
  fp_schedule fp_pop_due iau_eng_q_poll iau_engine_poll)
SET_PERF=(work_fn submit_single_io io_complete task_complete nvme_submit_io nvme_check_io nvme_setup_payload
  nvme_verify_io spdk_nvme_ns_cmd_read spdk_nvme_ns_cmd_write spdk_nvme_ns_cmd_read_with_md
  spdk_nvme_ns_cmd_write_with_md _nvme_ns_cmd_rw nvme_qpair_submit_request _nvme_qpair_submit_request
  spdk_nvme_qpair_process_completions nvme_transport_qpair_submit_request nvme_transport_qpair_process_completions
  spdk_nvme_poll_group_process_completions nvme_transport_poll_group_process_completions
  nvme_pcie_poll_group_process_completions spdk_get_ticks nvme_pcie_md_poll nvme_pcie_md_submit
  nvme_pcie_qpair_process_completions_stock nvme_pcie_qpair_submit_request_stock nvme_pcie_qpair_process_completions
  nvme_pcie_qpair_submit_request nvme_pcie_qpair_submit_tracker nvme_pcie_qpair_complete_tracker)
# P4.10c: the tile host functions pinned at 64 B starts (nvme_iau.c NVME_IAU_HOT_ALIGN); P4.11: the fast path's dispatcher
# nvme_iau_fast_rw (x86-64 asm at nvme_iau.o's start) and its tile body tile_fast_rw, and the S2 ablation's hot functions
# (nvme_iau_s2.c NVME_IAU_S2_HOT_ALIGN)
SET_PERF_PIN0=(nvme_iau_qpair_submit_request nvme_iau_qpair_submit_request48 submit_other submit_full nvme_iau_fast_rw
  nvme_iau_qpair_process_completions nvme_iau_pg_process_completions shared_poll slow_record nvme_iau_req_done
  tile_fast_rw nvme_iau_s2_fast_rw nvme_iau_s2_qpair_submit_request nvme_iau_s2_process_completions
  nvme_iau_s2_pg_process_completions s2_cpl_cold s2_cpl_tracker)
SYMS=(); PIN0=()
while [ $# -gt 0 ]; do
  case "$1" in
    --sym) [ $# -ge 2 ] || { echo "--sym needs a name" >&2; exit 2; }; SYMS+=("$2"); shift 2 ;;
    --pin0) [ $# -ge 2 ] || { echo "--pin0 needs a name" >&2; exit 2; }; PIN0+=("$2"); shift 2 ;;
    --set) [ $# -ge 2 ] || { echo "--set needs a name" >&2; exit 2; }
      case "$2" in
        nvmf_tgt) SYMS+=("${SET_NVMF_TGT[@]}") ;;
        perf) SYMS+=("${SET_PERF[@]}"); PIN0+=("${SET_PERF_PIN0[@]}") ;;
        *) echo "unknown set: $2" >&2; exit 2 ;;
      esac
      shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ "${#SYMS[@]}" -gt 0 ] || [ "${#PIN0[@]}" -gt 0 ] || SYMS=("${SET_NVMF_TGT[@]}")
addr() {  # $1 = binary, $2 = name -> hex address of the function NAME or NAME.<suffix> (not .cold), or empty
  nm "$1" | awk -v s="$2" '$2 ~ /^[tTwW]$/ && ($3 == s || index($3, s ".") == 1) && $3 !~ /\.cold$/ { print $1; exit }'
}
fail=0
for s in "${SYMS[@]}"; do
  a=$(addr "$A" "$s"); b=$(addr "$B" "$s")
  if [ -z "$a" ] && [ -z "$b" ]; then
    echo "NM_PHASE sym=$s base=-/- new=-/- absent"; continue
  fi
  if [ -z "$a" ] || [ -z "$b" ]; then
    echo "NM_PHASE sym=$s base=${a:--}/- new=${b:--}/- MISSING"; fail=1; continue
  fi
  ma=$(( 0x$a % 64 )); mb=$(( 0x$b % 64 ))
  if [ "$ma" -eq "$mb" ]; then st=ok; else st=CHANGED; fail=1; fi
  printf 'NM_PHASE sym=%s base=0x%x/%d new=0x%x/%d %s\n' "$s" $((0x$a)) "$ma" $((0x$b)) "$mb" "$st"
done
parts() {  # $1 = binary, $2 = name -> "address name" of every other function NAME.<suffix> (not .cold) but $3
  nm "$1" | awk -v s="$2" -v x="$3" '$2 ~ /^[tTwW]$/ && index($3, s ".") == 1 && $3 !~ /\.cold$/ && $1 != x { print $1, $3 }'
}
for s in "${PIN0[@]}"; do
  a=$(addr "$A" "$s"); b=$(addr "$B" "$s")
  if [ -z "$b" ]; then
    echo "NM_PHASE sym=$s base=${a:--}/- new=-/- pin0 MISSING"; fail=1; continue
  fi
  mb=$(( 0x$b % 64 ))
  if [ -n "$a" ]; then ab=$(printf '0x%x/%d' $((0x$a)) $(( 0x$a % 64 ))); else ab=-/-; fi
  if [ "$mb" -eq 0 ]; then st="pin0 ok"; else st="PIN0 CHANGED"; fail=1; fi
  printf 'NM_PHASE sym=%s base=%s new=0x%x/%d %s\n' "$s" "$ab" $((0x$b)) "$mb" "$st"
  # P4.10f (the P4.10c/d/e review's m8): a split (NAME.part.N, another clone) is pinned too: every other NAME.<suffix>
  # in NEW_BIN must start at phase 0 as well, one line each
  while read -r pa pn; do
    [ -n "$pa" ] || continue
    mp=$(( 0x$pa % 64 ))
    if [ "$mp" -eq 0 ]; then st="pin0 ok"; else st="PIN0 CHANGED"; fail=1; fi
    printf 'NM_PHASE sym=%s part=%s new=0x%x/%d %s\n' "$s" "$pn" $((0x$pa)) "$mp" "$st"
  done <<< "$(parts "$B" "$s" "$b")"
done
if [ "$fail" -eq 0 ]; then echo "NM_PHASE OK"; exit 0; else echo "NM_PHASE FAIL"; exit 1; fi
