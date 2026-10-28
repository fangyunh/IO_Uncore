#!/usr/bin/env bash
# make_s0_tree.sh: pristine upstream SPDK + the shared perf harness -> .build/spdk-upstream-harness (gitignored).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
UP="$ROOT/.build/spdk-upstream"; OUT="$ROOT/.build/spdk-upstream-harness"
[ "$(git -C "$UP" rev-parse --short=8 HEAD)" = 08e30b18 ] || { echo "upstream checkout is not 08e30b18" >&2; exit 1; }
[ -z "$(git -C "$UP" status --porcelain --untracked-files=no)" ] || { echo "upstream checkout is dirty" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
rsync -a --exclude .git "$UP/" "$OUT/"
cp "$ROOT/spdk/app/spdk_nvme_perf/perf.c" "$OUT/app/spdk_nvme_perf/perf.c"
if [ -f "$ROOT/scripts/realcore/next/s0_perf_compat.patch" ]; then
  patch -p1 -d "$OUT" --batch -s -i "$ROOT/scripts/realcore/next/s0_perf_compat.patch"
fi
sha256sum "$ROOT/spdk/app/spdk_nvme_perf/perf.c" "$OUT/app/spdk_nvme_perf/perf.c"
{
  echo "upstream_commit=$(git -C "$UP" rev-parse HEAD)"
  echo "upstream_dir=$UP"
  echo "harness_perf_c_sha256=$(sha256sum "$OUT/app/spdk_nvme_perf/perf.c" | cut -d' ' -f1)"
  if [ -f "$ROOT/scripts/realcore/next/s0_perf_compat.patch" ]; then
    echo "compat_patch=$(sha256sum "$ROOT/scripts/realcore/next/s0_perf_compat.patch" | cut -d' ' -f1)"
  else
    echo "compat_patch=none"
  fi
} > "$OUT/PROVENANCE"
