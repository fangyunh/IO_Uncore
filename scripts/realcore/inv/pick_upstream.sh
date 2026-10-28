#!/usr/bin/env bash
# pick_upstream.sh — clone upstream SPDK read-only into .build/spdk-upstream and pin the commit
# closest to the fork: among first-parent master commits whose VERSION file equals the fork's (26.05.0-pre),
# minimise the number of fork files (lib include app module examples scripts test, no IAU markers)
# whose git blob hash differs from the commit's blob.
# Usage: pick_upstream.sh [--step N]   (samples every Nth commit, default 5, then refines +-N)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
UP="$ROOT/.build/spdk-upstream"; STEP=5
[ "${1:-}" = --step ] && STEP="$2"
[ -d "$UP/.git" ] || git clone https://github.com/spdk/spdk.git "$UP"   # full clone: scoring reads many blobs
git -C "$UP" fetch -q origin master
DIRS=(lib include app module examples scripts test)
TMPD="$ROOT/.build/tmp"; mkdir -p "$TMPD"; FORKMAP="$TMPD/pick_upstream_fork.map"
# fork files with no IAU markers (never touched by the fork) and their git blob hashes
CLEAN=()
while IFS= read -r f; do
  case "$f" in *.o|*.d|*.a|*.so|*.pyc) continue;; esac
  grep -qiE 'uncore|iau|realcore|gem5|io_cycle' "$ROOT/spdk/$f" 2>/dev/null && continue
  CLEAN+=("$f")
done < <(cd "$ROOT/spdk" && find "${DIRS[@]}" -type f)
[ "${#CLEAN[@]}" -ge 100 ] || { echo "too few untouched reference files: ${#CLEAN[@]}" >&2; exit 1; }
paste <(printf '%s\n' "${CLEAN[@]}") <(printf '%s\n' "${CLEAN[@]/#/$ROOT/spdk/}" | git hash-object --stdin-paths) > "$FORKMAP"
score() {   # $1 = commit; prints the number of clean fork files whose blob differs from the commit's
  git -C "$UP" ls-tree -r "$1" -- "${DIRS[@]}" | awk -F'\t' -v map="$FORKMAP" '
    BEGIN { while ((getline l < map) > 0) { split(l, a, "\t"); fork[a[1]] = a[2] } }
    { split($1, m, " "); up[$2] = m[3] }
    END { n = 0; for (f in fork) if (up[f] != fork[f]) n++; print n }'
}
differs() { # $1 = commit; paths of clean fork files whose blob differs
  git -C "$UP" ls-tree -r "$1" -- "${DIRS[@]}" | awk -F'\t' -v map="$FORKMAP" '
    BEGIN { while ((getline l < map) > 0) { split(l, a, "\t"); fork[a[1]] = a[2] } }
    { split($1, m, " "); up[$2] = m[3] }
    END { for (f in fork) if (up[f] != fork[f]) print f }' | sort
}
# version window: first-parent commits whose VERSION file reads 26.05.0-pre (upstream moved the version
# from include/spdk/version.h to a top-level VERSION file in Dec 2024). FIRST = the bump to 26.05.0-pre,
# window end = the commit before the next bump.
FORKVER="$(cat "$ROOT/spdk/VERSION")"
mapfile -t BUMPS < <(git -C "$UP" log --first-parent --reverse --format=%H origin/master -- VERSION)
FIRST=""; NEXT=""
for c in "${BUMPS[@]}"; do
  v="$(git -C "$UP" show "$c:VERSION" 2>/dev/null || true)"
  if [ "$v" = "$FORKVER" ]; then FIRST="$c"; elif [ -n "$FIRST" ] && [ -z "$NEXT" ]; then NEXT="$c"; fi
done
[ -n "$FIRST" ] || { echo "no $FORKVER window found" >&2; exit 1; }
mapfile -t WIN < <(git -C "$UP" rev-list --first-parent --reverse "${NEXT:+$NEXT^}" 2>/dev/null | sed -n "/^$FIRST/,\$p")
[ "${#WIN[@]}" -gt 0 ] || mapfile -t WIN < <(git -C "$UP" rev-list --first-parent --reverse origin/master | sed -n "/^$FIRST/,\$p")
best=""; bests=999999999
for ((i = 0; i < ${#WIN[@]}; i += STEP)); do
  s=$(score "${WIN[$i]}"); if [ "$s" -lt "$bests" ]; then bests=$s; best=$i; fi
done
lo=$((best - STEP < 0 ? 0 : best - STEP)); hi=$((best + STEP >= ${#WIN[@]} ? ${#WIN[@]} - 1 : best + STEP))
for ((i = lo; i <= hi; i++)); do
  s=$(score "${WIN[$i]}"); if [ "$s" -lt "$bests" ]; then bests=$s; best=$i; fi
done
C="${WIN[$best]}"
git -C "$UP" checkout -q "$C"
git -C "$UP" submodule update --init --recursive -q
echo "$C diff_files=$bests clean_files=${#CLEAN[@]}" > "$UP/PINNED"
echo "differs=$(differs "$C" | tr '\n' ' ')" >> "$UP/PINNED"
echo "UPSTREAM_PINNED $C diff_files=$bests"
