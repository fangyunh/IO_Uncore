#!/usr/bin/env bash
# stop_target.sh — stop the nvmf_tgt started by start_target.sh (only ours).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
[ -f "$RC_TGT_PIDFILE" ] || { echo "TARGET_STOPPED (no pidfile)"; exit 0; }
PID="$(cat "$RC_TGT_PIDFILE")"
if kill -0 "$PID" 2>/dev/null; then
  OWNER="$(ps -o user= -p "$PID" | tr -d ' ')"
  CMD="$(tr '\0' ' ' < "/proc/$PID/cmdline" 2>/dev/null || true)"
  if [ "$OWNER" != "$(id -un)" ] || [[ "$CMD" != *nvmf_tgt* ]]; then
    rc_die "pid $PID is not our nvmf_tgt (owner=$OWNER cmd=$CMD); refusing to signal"
  fi
  rc_rpc spdk_kill_instance SIGTERM >/dev/null 2>&1 || kill -TERM "$PID"
  for _ in $(seq 1 100); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$PID" 2>/dev/null; then rc_die "nvmf_tgt pid $PID did not exit after SIGTERM"; fi
fi
rm -f "$RC_TGT_PIDFILE" "$RC_RPC_SOCK"
echo "TARGET_STOPPED"
