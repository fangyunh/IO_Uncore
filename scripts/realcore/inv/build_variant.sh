#!/usr/bin/env bash
# build_variant.sh <inv-variant> — build an investigation overlay variant from its series file
# scripts/realcore/inv/series/<variant>.txt (one patch file name per line; '#' starts a comment).
set -euo pipefail
INV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$INV_DIR/../../.." && pwd)"
V="${1:?usage: build_variant.sh inv-<name>}"
[[ "$V" =~ ^inv-[a-z0-9-]+$ ]] || { echo "bad variant name $V" >&2; exit 2; }
S="$INV_DIR/series/$V.txt"
[ -f "$S" ] || { echo "no series file $S" >&2; exit 2; }
ARGS=()
while IFS= read -r line || [ -n "$line" ]; do
  p="${line%%#*}"; p="${p#"${p%%[![:space:]]*}"}"; p="${p%"${p##*[![:space:]]}"}"
  [ -n "$p" ] || continue
  [ -f "$INV_DIR/patches/$p" ] || { echo "missing patch $INV_DIR/patches/$p" >&2; exit 2; }
  ARGS+=(--patch "$INV_DIR/patches/$p")
done < "$S"
exec "$ROOT/scripts/image/build_spdk_host.sh" --variant "$V" "${ARGS[@]}"
