#!/usr/bin/env bash
# build_spdk_host.sh — build the SPDK fork for HOST-NATIVE real-core runs
# (vfio-user target nvmf_tgt + initiator spdk_nvme_perf).
#
# spdk/ stays the single source of truth. This script mirrors the tracked and
# untracked-but-not-ignored files of spdk/ into .build/spdk-host-<variant>/src/
# (gitignored) and builds there inside an Ubuntu 22.04 container running as the
# invoking user, so the root-owned in-tree guest build of build_spdk_docker.sh
# is never touched. Ubuntu 22.04 matches the host glibc/OpenSSL 3.
#
# Variants (same source, different compile flags):
#   lean  : -DSPDK_NVME_IO_CYCLE_OFF  -> host per-IO cycle probes compiled out
#           (headline IOPS / cycles-per-IO; no probe bias between modes)
#   instr : probes compiled in        -> per-stage breakdown runs
#   inv-<name> : lean flags + overlay patches (--patch, repeatable) applied to a copy of spdk/;
#                the tree is seeded from .build/spdk-host-lean so only patched files rebuild
#   upstream   : an upstream SPDK checkout given by --src (read-only reference, no patches);
#                always reconfigures and rebuilds fully
#   nx-<name>  : lean flags + current spdk/ tree; immutable snapshot (refuses to rebuild, exit 3)
#   upstream-<name> : lean flags + tree from --src; immutable snapshot
#   ut-<name>  : unit-test tree, never a measurement binary (no apps are built; lib.sh refuses ut-*):
#                current spdk/ tree, -DSPDK_NVME_IO_CYCLE_OFF (the lean code paths), configured
#                --enable-unit-tests --enable-debug (asserts on, -O0, like upstream CI) in the CUnit image
#                (docker/realcore/ut.Dockerfile); builds only `make module` + test/unit/lib/nvme; mutable (rebuilt
#                in place, --clean for fresh). Refuses with exit 75 while a campaign runs. Exit 91 = configure
#                failed, 90 = lib/module build failed, 1 = a ut did not build (BUILD_UT_INCOMPLETE).
#   --extra-cflags "FLAGS" is appended to any variant's CFLAGS
#
# Usage: scripts/image/build_spdk_host.sh [--variant lean|instr|inv-<name>|nx-<name>|upstream|upstream-<name>|ut-<name>]
#        [--patch FILE]... [--src DIR] [--extra-cflags FLAGS] [--clean] [--jobs N]
# Output: .build/spdk-host-<variant>/src/spdk/build/bin/{nvmf_tgt,spdk_nvme_perf,spdk_nvme_identify}
#         .build/spdk-host-<variant>/fingerprint.txt
#         .build/spdk-host-<variant>/patched_files.txt   (inv-<name> variants only)
#         ut-<name>: .build/spdk-host-<variant>/src/spdk/test/unit/lib/nvme/*/*_ut (run them with
#                    scripts/realcore/next/run_spdk_nvme_ut.sh) + fingerprint.txt (removed at the start of every
#                    build, rewritten at its end); UT_DRYRUN=1 stops after the checks
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
VARIANT=lean
CLEAN=0
JOBS=16
PATCHES=()
SRC_OVERRIDE=""
USER_CFLAGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) VARIANT="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --patch) PATCHES+=("$(realpath "$2")"); shift 2 ;;
    --src) SRC_OVERRIDE="$(realpath "$2")"; shift 2 ;;
    --extra-cflags) [ $# -ge 2 ] || { echo "--extra-cflags needs a value" >&2; exit 2; }; USER_CFLAGS="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
case "$VARIANT" in
  lean) EXTRA_CFLAGS="-DSPDK_NVME_IO_CYCLE_OFF" ;;
  instr) EXTRA_CFLAGS="" ;;
  inv-*) [[ "$VARIANT" =~ ^inv-[a-z0-9-]+$ ]] || { echo "bad variant name $VARIANT" >&2; exit 2; }
         EXTRA_CFLAGS="-DSPDK_NVME_IO_CYCLE_OFF" ;;
  nx-*) [[ "$VARIANT" =~ ^nx-[a-z0-9-]+$ ]] || { echo "bad variant name $VARIANT" >&2; exit 2; }
        EXTRA_CFLAGS="-DSPDK_NVME_IO_CYCLE_OFF" ;;
  upstream) EXTRA_CFLAGS=""
            [ -n "$SRC_OVERRIDE" ] && [ -d "$SRC_OVERRIDE" ] || { echo "--variant upstream needs --src DIR" >&2; exit 2; } ;;
  upstream-*) [[ "$VARIANT" =~ ^upstream-[a-z0-9-]+$ ]] || { echo "bad variant name $VARIANT" >&2; exit 2; }
              EXTRA_CFLAGS="-DSPDK_NVME_IO_CYCLE_OFF"
              [ -n "$SRC_OVERRIDE" ] && [ -d "$SRC_OVERRIDE" ] || { echo "--variant $VARIANT needs --src DIR" >&2; exit 2; } ;;
  ut-*) [[ "$VARIANT" =~ ^ut-[a-z0-9-]+$ ]] || { echo "bad variant name $VARIANT" >&2; exit 2; }
        EXTRA_CFLAGS="-DSPDK_NVME_IO_CYCLE_OFF" ;;
  *) echo "--variant must be lean, instr, inv-<name>, nx-<name>, upstream, upstream-<name> or ut-<name>" >&2; exit 2 ;;
esac
EXTRA_CFLAGS="$EXTRA_CFLAGS${USER_CFLAGS:+ $USER_CFLAGS}"
if [ "${#PATCHES[@]}" -gt 0 ] && [[ "$VARIANT" != inv-* ]]; then
  echo "--patch is only valid with an inv-<name> variant" >&2; exit 2
fi
if [ -n "$SRC_OVERRIDE" ] && [[ "$VARIANT" != upstream && "$VARIANT" != upstream-* ]]; then
  echo "--src is only valid with the upstream variants" >&2; exit 2
fi
OUT_DIR="$ROOT_DIR/.build/spdk-host-$VARIANT"
SRC_ROOT="$OUT_DIR/src"
SPDK_HOST="$SRC_ROOT/spdk"
IMAGE="realcore-spdk-build:22.04"
# Immutable snapshots: guard intentionally precedes --clean (rm -rf) and docker build.
# Keys on the binaries too, so a snapshot whose fingerprint was deleted is not rebuilt.
if [[ "$VARIANT" == nx-* || "$VARIANT" == upstream-* ]] && { [ -f "$OUT_DIR/fingerprint.txt" ] || [ -e "$SPDK_HOST/build/bin/nvmf_tgt" ]; }; then
  echo "$VARIANT is an immutable snapshot and already exists: $OUT_DIR" >&2; exit 3
fi
UTV=0
RUN_IMAGE="$IMAGE"
CFG="--disable-tests --disable-unit-tests --disable-examples"
if [[ "$VARIANT" == ut-* ]]; then
  # Separate CUnit image (the measurement image and its gcc stay untouched); debug build like upstream CI
  # (asserts on). The lean -DSPDK_NVME_IO_CYCLE_OFF stays: the uts #include nvme_pcie_common.c / nvme_ns_cmd.c,
  # and the flag removes the probe fields from struct nvme_request and the probe blocks, so the uts test the
  # code paths the lean measurement binaries run.
  UTV=1; RUN_IMAGE="realcore-spdk-ut:22.04"; CFG="--enable-unit-tests --enable-debug --disable-tests --disable-examples"
fi
# No ut build while a campaign runs (builds stay off the measurement window); separate pgrep, no launch on this line.
if [ "$UTV" = 1 ] && pgrep -f 'scripts/realcore/[c]ampaign\.sh' >/dev/null; then
  echo "WAIT: a campaign is running; ut builds refuse (exit 75)" >&2; exit 75
fi
# ut-<name> trees are mutable (the guard above keys only on nx-*/upstream-*); dry run for the hermetic test.
if [ "${UT_DRYRUN:-0}" = 1 ] && [ "$UTV" = 1 ]; then
  echo "UT_DRYRUN ok variant=$VARIANT image=$RUN_IMAGE configure=\"--with-vfio-user --without-nvme-cuse $CFG\" extra_cflags=\"$EXTRA_CFLAGS\""
  exit 0
fi

docker build -q -t "$IMAGE" -f "$ROOT_DIR/docker/realcore/build.Dockerfile" \
  "$ROOT_DIR/docker/realcore" >/dev/null
RUN_REF="$RUN_IMAGE"
if [ "$UTV" = 1 ]; then
  docker build -q --cpuset-cpus "8-15,24-31" -t "$RUN_IMAGE" -f "$ROOT_DIR/docker/realcore/ut.Dockerfile" \
    "$ROOT_DIR/docker/realcore" >/dev/null
  RUN_REF="$(docker image inspect -f '{{.Id}}' "$RUN_IMAGE")"   # build and fingerprint by id
  # From here on the tree may change: no fingerprint until the build finishes (the runner refuses a tree without one).
  rm -f "$OUT_DIR/fingerprint.txt"
fi

if [ "$CLEAN" -eq 1 ]; then rm -rf "$SRC_ROOT" "$OUT_DIR/patched_files.txt"; fi
if [[ "$VARIANT" == upstream || "$VARIANT" == upstream-* ]]; then
  mkdir -p "$SPDK_HOST"
  rsync -a --delete --exclude .git "$SRC_OVERRIDE/" "$SPDK_HOST/"
else
  if [[ "$VARIANT" == inv-* ]] && [ ! -d "$SPDK_HOST" ] && [ -d "$ROOT_DIR/.build/spdk-host-lean/src/spdk" ]; then
    mkdir -p "$OUT_DIR"
    cp -a "$ROOT_DIR/.build/spdk-host-lean/src" "$SRC_ROOT"   # seed: reuse lean objects
  elif [[ "$VARIANT" == inv-* ]] && [ ! -d "$SPDK_HOST" ]; then
    echo "note: no lean seed at $ROOT_DIR/.build/spdk-host-lean/src/spdk; full build" >&2
  fi
  mkdir -p "$SRC_ROOT"
  ( cd "$ROOT_DIR" && git ls-files -z --cached --others --exclude-standard -- spdk ) \
    | rsync -a --from0 --files-from=- --ignore-missing-args "$ROOT_DIR/" "$SRC_ROOT/"
  # spdk/.gitignore ignores source files like dpdk-cmdline-gen.py (pattern dpdk-*);
  # DPDK's meson build needs them, so mirror ignored non-build files under spdk/dpdk too.
  ( cd "$ROOT_DIR" && git ls-files -z --others --ignored --exclude-standard -- spdk/dpdk ) \
    | grep -zEv '^spdk/dpdk/(build|build-tmp)/|__pycache__|\.(o|d|a|so)$' \
    | rsync -a --from0 --files-from=- --ignore-missing-args "$ROOT_DIR/" "$SRC_ROOT/"
  if [[ "$VARIANT" == inv-* ]]; then
    # rsync restored pristine copies of tracked files an earlier build patched, with the repo's OLD
    # mtimes; touch them so make rebuilds their objects. Files an earlier patch CREATED are not in
    # the repo and rsync leaves them behind; remove them so a re-apply starts from pristine.
    if [ -f "$OUT_DIR/patched_files.txt" ]; then
      while read -r rel; do
        if [ -e "$ROOT_DIR/spdk/$rel" ]; then
          [ -f "$SPDK_HOST/$rel" ] && touch "$SPDK_HOST/$rel"
        else
          rm -f "$SPDK_HOST/$rel"
        fi
      done < "$OUT_DIR/patched_files.txt"
    fi
    for p in "${PATCHES[@]}"; do
      patch -N -p1 -d "$SPDK_HOST" --dry-run --batch -s -i "$p" >/dev/null \
        || { echo "patch does not apply: $p" >&2; exit 1; }
      # record the touched files BEFORE applying, so a mid-series failure still leaves a record
      { [ -f "$OUT_DIR/patched_files.txt" ] && cat "$OUT_DIR/patched_files.txt"
        sed -n -e 's#^+++ b/\([^[:space:]]*\).*#\1#p' -e 's#^--- a/\([^[:space:]]*\).*#\1#p' "$p"
        true; } | sort -u > "$OUT_DIR/patched_files.txt.new"
      mv "$OUT_DIR/patched_files.txt.new" "$OUT_DIR/patched_files.txt"
      patch -N -p1 -d "$SPDK_HOST" --batch -s --no-backup-if-mismatch -i "$p"
    done
    [ -f "$OUT_DIR/patched_files.txt" ] || : > "$OUT_DIR/patched_files.txt"
  fi
fi

RC=0
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  --cpuset-cpus "8-15,24-31" \
  -e EXTRA_CFLAGS="$EXTRA_CFLAGS" -e JOBS="$JOBS" -e CFG="$CFG" -e UTV="$UTV" \
  -v "$SPDK_HOST":/spdk -w /spdk "$RUN_REF" bash -c '
    set -euo pipefail
    if [ "$UTV" = 1 ]; then
      # Reconfigure whenever the configure arguments differ from the stamp of the last successful configure.
      want="--with-vfio-user --without-nvme-cuse $CFG"
      if [ ! -f mk/config.mk ] || [ "$(cat .realcore_ut_configure 2>/dev/null)" != "$want" ]; then
        rm -f .realcore_ut_configure
        ./configure $want || exit 91
        printf "%s\n" "$want" > .realcore_ut_configure
      fi
      # Stale binaries (a ut that no longer builds, a dir that left DIRS-y) must not survive into this build.
      rm -f test/unit/lib/nvme/*/*_ut
      # `module` generates config.h, version.h and build/ and pulls in lib, dpdkbuild, vfiouserbuild, isalbuild.
      # mk/spdk.subdirs.mk recurses with $(MAKECMDGOALS), so a bare `make module` asks isalbuild etc. for a
      # `module` target and fails; the empty command-line override (inherited by every sub-make) gives each
      # sub-make its default goal, i.e. a plain `make` limited to the module closure (no app binaries).
      CFLAGS="$EXTRA_CFLAGS" make -j"$JOBS" module MAKECMDGOALS= || exit 90
      # -k: every ut that compiles is built and fingerprinted even if one does not (ut_make_rc != 0, and the
      # runner refuses such a tree)
      CFLAGS="$EXTRA_CFLAGS" make -k -j"$JOBS" -C test/unit/lib/nvme
    else
      if ! grep -q "^CONFIG_VFIO_USER?=y" mk/config.mk 2>/dev/null; then
        ./configure --with-vfio-user --without-nvme-cuse $CFG
      fi
      CFLAGS="$EXTRA_CFLAGS" make -j"$JOBS"
    fi
  ' || RC=$?

if [ "$UTV" = 1 ]; then
  # 0 = all built; 2 = make -k reported a ut build failure (lib/module built); 91 / 90 = configure / module failed
  case "$RC" in
    0|2) ;;
    91) echo "ut configure failed (exit 91) variant=$VARIANT" >&2; exit 91 ;;
    90) echo "ut lib/module build failed (exit 90) variant=$VARIANT" >&2; exit 90 ;;
    *) echo "ut build failed rc=$RC variant=$VARIANT" >&2; exit 1 ;;
  esac
  UTS=()
  while IFS= read -r -d "" f; do UTS+=("$f"); done < <(cd "$SPDK_HOST" && find test/unit/lib/nvme -mindepth 2 -maxdepth 2 -type f -name '*_ut' -perm -u+x -print0 | sort -z)
  {
    echo "variant=$VARIANT"
    echo "kind=unit-test-tree not_a_measurement_snapshot=1"
    echo "extra_cflags=$EXTRA_CFLAGS"
    echo "repo_head=$(git -C "$ROOT_DIR" rev-parse HEAD)"
    echo "spdk_worktree_sha256=$( (git -C "$ROOT_DIR" diff HEAD -- spdk; git -C "$ROOT_DIR" status --porcelain -- spdk) | sha256sum | cut -d' ' -f1)"
    echo "image=$RUN_IMAGE image_id=$RUN_REF"
    echo "cunit=$(docker run --rm --network none --user "$(id -u):$(id -g)" --cpuset-cpus "8-15,24-31" "$RUN_REF" dpkg-query -W libcunit1-dev | tr '\t' ' ')"
    echo "configure=--with-vfio-user --without-nvme-cuse $CFG"
    echo "config=$(grep -E '^CONFIG_(VFIO_USER|UNIT_TESTS|TESTS|ARCH|DEBUG)\?=' "$SPDK_HOST/mk/config.mk" | tr '\n' ' ')"
    echo "ut_make_rc=$RC ut_built=${#UTS[@]}"
    for f in "${UTS[@]}"; do echo "sha256_$(basename "$f")=$(sha256sum "$SPDK_HOST/$f" | cut -d' ' -f1)"; done
  } > "$OUT_DIR/fingerprint.txt"
  cat "$OUT_DIR/fingerprint.txt"
  if [ "$RC" -ne 0 ]; then echo "BUILD_UT_INCOMPLETE variant=$VARIANT ut_built=${#UTS[@]} (make -k rc=$RC)" >&2; exit 1; fi
  echo "BUILD_OK variant=$VARIANT ut_built=${#UTS[@]} tree=$SPDK_HOST"
  exit 0
fi
[ "$RC" -eq 0 ] || exit "$RC"

BIN="$SPDK_HOST/build/bin"
for b in nvmf_tgt spdk_nvme_perf spdk_nvme_identify; do
  [ -x "$BIN/$b" ] || { echo "missing $BIN/$b" >&2; exit 1; }
done
{
  echo "variant=$VARIANT"
  echo "extra_cflags=$EXTRA_CFLAGS"
  echo "repo_head=$(git -C "$ROOT_DIR" rev-parse HEAD)"
  if [[ "$VARIANT" == upstream || "$VARIANT" == upstream-* ]]; then
    echo "spdk_worktree_sha256=n/a"
    echo "src_dir=$SRC_OVERRIDE"
    echo "src_tree_sha256=$(cd "$SPDK_HOST" && find . -path ./build -prune -o -type f \( -name '*.c' -o -name '*.h' -o -name 'Makefile' -o -name '*.mk' \) -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
  else
    echo "spdk_worktree_sha256=$( (git -C "$ROOT_DIR" diff HEAD -- spdk; git -C "$ROOT_DIR" status --porcelain -- spdk) | sha256sum | cut -d' ' -f1)"
  fi
  for b in nvmf_tgt spdk_nvme_perf spdk_nvme_identify; do
    echo "sha256_$b=$(sha256sum "$BIN/$b" | cut -d' ' -f1)"
  done
  echo "config=$(grep -E '^CONFIG_(VFIO_USER|ARCH|DEBUG)' "$SPDK_HOST/mk/config.mk" | tr '\n' ' ')"
  for p in "${PATCHES[@]}"; do echo "patch=$(basename "$p") sha256=$(sha256sum "$p" | cut -d' ' -f1)"; done
  if [[ "$VARIANT" == upstream || "$VARIANT" == upstream-* ]]; then
    # Only trust git when SRC_OVERRIDE is itself a repo toplevel (an rsync copy inside the main repo would
    # otherwise resolve to the enclosing repo HEAD); else fall back to the tree's PROVENANCE file.
    up_commit=unknown
    if [ "$(git -C "$SRC_OVERRIDE" rev-parse --show-toplevel 2>/dev/null || true)" = "$(realpath "$SRC_OVERRIDE")" ]; then
      up_commit="$(git -C "$SRC_OVERRIDE" rev-parse HEAD 2>/dev/null || echo unknown)"
    elif [ -f "$SRC_OVERRIDE/PROVENANCE" ]; then
      up_commit="$(sed -n 's/^upstream_commit=//p' "$SRC_OVERRIDE/PROVENANCE" | head -1)"
      up_commit="${up_commit:-unknown}"
    fi
    echo "upstream_src=$SRC_OVERRIDE upstream_commit=$up_commit"
    if [ -f "$SRC_OVERRIDE/PROVENANCE" ]; then sed 's/^/provenance_/' "$SRC_OVERRIDE/PROVENANCE"; fi
  fi
} > "$OUT_DIR/fingerprint.txt"
cat "$OUT_DIR/fingerprint.txt"
echo "BUILD_OK variant=$VARIANT bin=$BIN"
