#!/usr/bin/env bash
# mkpatch.sh — author an investigation overlay patch against spdk/ without touching spdk/.
#   mkpatch.sh start <NN-name> [--base a.diff,b.diff] <spdk-relative file>...
#       Copies the files (plus every file the base patches touch) from spdk/ into
#       .build/inv-work/<NN-name>/{a,b}/ and applies the base patches to both. Edit under b/.
#   mkpatch.sh finish <NN-name>
#       Writes scripts/realcore/inv/patches/<NN-name>.diff = <NN-name>.header + `diff -ruN a b`.
# Env (tests only): INV_SPDK_DIR, INV_WORK_DIR, INV_PATCH_DIR.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SPDK_DIR="${INV_SPDK_DIR:-$ROOT/spdk}"
WORK_ROOT="${INV_WORK_DIR:-$ROOT/.build/inv-work}"
PATCH_DIR="${INV_PATCH_DIR:-$ROOT/scripts/realcore/inv/patches}"
die() { echo "mkpatch: $*" >&2; exit 1; }
touched() { sed -n 's#^+++ b/\([^[:space:]]*\).*#\1#p' "$1"; }
cmd="${1:-}"; name="${2:-}"
[ -n "$cmd" ] && [ -n "$name" ] || die "usage: mkpatch.sh start|finish <NN-name> ..."
[[ "$name" =~ ^[0-9][0-9]-[a-z0-9-]+$ ]] || die "name must look like NN-some-name"
W="$WORK_ROOT/$name"
case "$cmd" in
  start)
    shift 2
    BASE=""
    if [ "${1:-}" = --base ]; then BASE="${2:?mkpatch: --base needs a comma-separated list}"; shift 2; fi
    [ $# -gt 0 ] || die "list at least one spdk-relative file"
    for f in "$@"; do
      case "$f" in
        -*|/*|*..*) die "bad file argument '$f' (must be spdk-relative, no leading - or /, no ..)" ;;
      esac
    done
    rm -rf "$W"; mkdir -p "$W/a" "$W/b"
    files=("$@")
    bases=()
    if [ -n "$BASE" ]; then IFS=',' read -ra bases <<< "$BASE"; fi
    for p in "${bases[@]}"; do
      [ -f "$PATCH_DIR/$p" ] || die "no base patch $PATCH_DIR/$p"
      while read -r f; do files+=("$f"); done < <(touched "$PATCH_DIR/$p")
    done
    while read -r f; do
      [ -n "$f" ] || continue
      for side in a b; do
        mkdir -p "$W/$side/$(dirname "$f")"
        if [ -f "$SPDK_DIR/$f" ]; then
          cp "$SPDK_DIR/$f" "$W/$side/$f"
        elif [ "$side" = a ] && [ ! -e "$W/a/$f" ]; then
          echo "mkpatch: note: $f not in spdk/; treated as a new file" >&2
        fi
      done
    done < <(printf '%s\n' "${files[@]}" | sort -u)
    for p in "${bases[@]}"; do
      for side in a b; do
        patch -p1 -s -d "$W/$side" --batch --no-backup-if-mismatch -i "$PATCH_DIR/$p" || die "base $p does not apply"
      done
    done
    echo "$BASE" > "$W/.base"
    echo "MKPATCH_STARTED edit files under $W/b, then run: mkpatch.sh finish $name"
    ;;
  finish)
    [ -d "$W/a" ] || die "no work dir $W (run start first)"
    hdr="$PATCH_DIR/$name.header"
    [ -f "$hdr" ] || die "missing header $hdr"
    for key in inv-patch purpose safety breaks env base; do
      grep -q "^# $key: " "$hdr" || die "header lacks '# $key: '"
    done
    hv() { sed -n "s/^# $1: *//p" "$hdr" | head -n1 | sed 's/[[:space:]]*$//'; }
    [ "$(hv inv-patch)" = "$name" ] || die "header '# inv-patch:' is '$(hv inv-patch)', expected '$name'"
    case "$(hv safety)" in SAFE*|UNSAFE*) ;; *) die "header '# safety:' must start with SAFE or UNSAFE" ;; esac
    hb="$(hv base)"; [ "$hb" != none ] || hb=""
    rb="$(tr -d '[:space:]' < "$W/.base" 2>/dev/null || true)"
    hb="$(printf '%s' "$hb" | tr -d '[:space:]')"
    [ "$hb" = "$rb" ] || die "header '# base:' ('${hb:-none}') differs from start --base ('${rb:-none}')"
    set +e; (cd "$W" && diff -ruN a b) > "$W/body.diff"; rc=$?; set -e
    [ "$rc" -ne 0 ] || die "no differences between a/ and b/"
    [ "$rc" -eq 1 ] || die "diff failed (rc=$rc)"
    { cat "$hdr"; [ -z "$(tail -c1 "$hdr")" ] || echo; cat "$W/body.diff"; } > "$PATCH_DIR/$name.diff"
    echo "MKPATCH_WROTE $PATCH_DIR/$name.diff"
    ;;
  *) die "unknown command $cmd" ;;
esac
