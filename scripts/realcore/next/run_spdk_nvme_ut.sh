#!/usr/bin/env bash
# run_spdk_nvme_ut.sh --variant ut-<name> [--baseline FILE] [--logs DIR]: run SPDK's own nvme unit tests of a ut tree.
# The tree is .build/spdk-host-ut-<name> (built by scripts/image/build_spdk_host.sh --variant ut-<name>).
# The run uses the CUnit image realcore-spdk-ut:22.04 (by its id) on CPUs 8-15,24-31, with the tree mounted
# read-only and no network.
#
# Preconditions, else "SPDK_NVME_UT_REFUSED reason=..." and exit 1:
#   - the tree's fingerprint.txt exists with kind=unit-test-tree and ut_make_rc=0;
#   - the runtime image id equals the fingerprint's image_id.
# The runner enumerates the 17 EXPECTED dirs (spdk/test/unit/lib/nvme/Makefile DIRS-y; rdma and cuse are off).
# Output: the SPDK_NVME_UT_TREE and SPDK_NVME_UT_IMAGE header lines, then one line per expected ut:
#   UT <d> PASS|FAIL rc=<exit> suites=<ran>/<failed>/<total> tests=<ran>/<passed>/<failed>/<total>
#                    asserts=<ran>/<passed>/<failed>/<total> [why=<reasons>]
#   UT <d> FAIL rc=- why=sha256_mismatch   (binary differs from the fingerprint; not run)
#   UT <d> MISSING                         (no binary; counts as a failure, P4 G-ut)
#   UT <d> EXTRA not_run                   (a *_ut outside EXPECTED; printed, not run, not counted)
# A FAIL is followed by its failing CUnit tests and asserts (or the log tail), indented "  | ".
# A ut PASSes only if all of these hold:
#   - exit 0 and a CUnit summary;
#   - suites ran == total, 0 failed;
#   - tests ran == total > 0, 0 failed;
#   - 0 failed asserts;
#   - its sha256 equals the fingerprint's;
#   - with --baseline FILE (a previous output, e.g. p4_ut_base.txt), tests ran >= that ut's tests ran there.
# The exit code alone is the assert failure count mod 256, so it is not enough.
# Each ut runs with stdin </dev/null under `timeout -k $UT_KILL_AFTER $UT_TIMEOUT` (default 30 / 600 s).
# The last line is: SPDK_NVME_UT pass=<X> fail=<Y> missing=<Z> n=17 [failed=<d,...>]
# A run counts as passing only if its exit is 0 and its last line is exactly
#   SPDK_NVME_UT pass=<X> fail=0 missing=0 n=17
# --logs DIR keeps every ut's full log as DIR/<d>.log.
# Exit codes: 0 all pass; 1 any fail / missing / refusal / runner error; 2 usage / no tree / no image / no baseline;
# 75 while a campaign runs.
# Test hooks (tests/realcore/test_run_spdk_nvme_ut.sh only): UT_RUNNER_LOCAL=1 runs the same inner script with local
# bash instead of `docker run`; UT_BUILD_ROOT replaces .build.
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V=""; LOGS=""; BASEF=""
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) V="${2:-}"; shift 2 || shift ;;
    --logs) LOGS="${2:-}"; shift 2 || shift ;;
    --baseline) BASEF="${2:-}"; shift 2 || shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ "$V" =~ ^ut-[a-z0-9-]+$ ]] || { echo "usage: $0 --variant ut-<name> [--baseline FILE] [--logs DIR]" >&2; exit 2; }
if pgrep -f 'scripts/realcore/[c]ampaign\.sh' >/dev/null; then echo "WAIT: a campaign is running" >&2; exit 75; fi
OUT="${UT_BUILD_ROOT:-$ROOT/.build}/spdk-host-$V"; S="$OUT/src/spdk"; FP="$OUT/fingerprint.txt"
[ -d "$S/test/unit/lib/nvme" ] || { echo "no ut tree $S (build_spdk_host.sh --variant $V)" >&2; exit 2; }
if [ -n "$BASEF" ]; then
  [ -f "$BASEF" ] && [ -r "$BASEF" ] || { echo "no baseline file $BASEF" >&2; exit 2; }
  BASEF="$(cd "$(dirname "$BASEF")" && pwd)/$(basename "$BASEF")"
fi
refuse() { echo "SPDK_NVME_UT_REFUSED reason=$1"; exit 1; }
[ -f "$FP" ] || refuse "no_fingerprint ($FP)"
grep -qE '^kind=unit-test-tree( |$)' "$FP" || refuse "not_a_ut_tree_fingerprint"
grep -qE '^ut_make_rc=0( |$)' "$FP" || refuse "ut_build_incomplete ($(grep -m1 '^ut_make_rc=' "$FP" || echo 'no ut_make_rc'))"
IMAGE="realcore-spdk-ut:22.04"
RID="$(docker image inspect -f '{{.Id}}' "$IMAGE" 2>/dev/null)" || { echo "no image $IMAGE (build_spdk_host.sh --variant $V builds it)" >&2; exit 2; }
FID="$(sed -n 's/^image=.* image_id=\([^ ]*\).*/\1/p' "$FP" | head -1)"
echo "SPDK_NVME_UT_TREE variant=$V $(grep -hE '^(repo_head|spdk_worktree_sha256|ut_make_rc)=' "$FP" | tr '\n' ' ')"
echo "SPDK_NVME_UT_IMAGE runtime=$RID fingerprint=${FID:-none}"
[ -n "$FID" ] && [ "$RID" = "$FID" ] || refuse "image_id_mismatch"
EXPECTED=(nvme nvme_ctrlr nvme_ctrlr_cmd nvme_ctrlr_ocssd_cmd nvme_ns nvme_ns_cmd nvme_ns_ocssd_cmd nvme_pcie
          nvme_poll_group nvme_qpair nvme_quirks nvme_tcp nvme_transport nvme_io_msg nvme_pcie_common nvme_fabric
          nvme_opal)
[ -n "$LOGS" ] && { mkdir -p "$LOGS" && LOGS="$(cd "$LOGS" && pwd)" || exit 2; }
UT_TIMEOUT="${UT_TIMEOUT:-600}"; UT_KILL_AFTER="${UT_KILL_AFTER:-30}"

# The inner script reads the tree at $TREE, the fingerprint at $FP, the optional baseline at $BASE, and writes
# logs to $LOGDIR (optional). It is fed on stdin (bash -s), which is why every ut runs with </dev/null.
INNER="$(cat <<'EOS'
cd "$TREE" || exit 3
tmp="$(mktemp -d)"
pass=0; fail=0; missing=0; bad=""
for d in $EXPECTED; do
  t="test/unit/lib/nvme/$d.c/${d}_ut"
  if [ ! -x "$t" ]; then echo "UT $d MISSING"; missing=$((missing + 1)); bad="$bad,$d"; continue; fi
  want="$(sed -n "s/^sha256_${d}_ut=//p" "$FP" | head -1)"
  have="$(sha256sum "$t" | cut -d' ' -f1)"
  if [ -z "$want" ] || [ "$want" != "$have" ]; then
    echo "UT $d FAIL rc=- why=sha256_mismatch"; fail=$((fail + 1)); bad="$bad,$d"; continue
  fi
  log="$tmp/$d.log"
  timeout -k "$UT_KILL_AFTER" "$UT_TIMEOUT" "$t" </dev/null >"$log" 2>&1; rc=$?
  [ -n "$LOGDIR" ] && cp "$log" "$LOGDIR/$d.log"
  # CUnit basic summary: "suites Total Ran n/a Failed Inactive"; "tests|asserts Total Ran Passed Failed Inactive"
  sum="$(awk '/^Run Summary:/ { s = 1; next }
    s && $1 == "suites"  { st = $2; sr = $3; sf = $5 }
    s && $1 == "tests"   { tt = $2; tr = $3; tp = $4; tf = $5 }
    s && $1 == "asserts" { at = $2; ar = $3; ap = $4; af = $5 }
    END { if (st == "" || tt == "" || at == "") print "none";
          else print st, sr, sf, tt, tr, tp, tf, at, ar, ap, af }' "$log")"
  why=""
  [ "$rc" -eq 0 ] || why="$why,rc"
  if [ "$sum" = none ]; then
    desc="summary=none"; why="$why,no_summary"
  else
    set -- $sum
    desc="suites=$2/$3/$1 tests=$5/$6/$7/$4 asserts=$9/${10}/${11}/$8"
    [ "$2" = "$1" ] || why="$why,suites_not_run"
    [ "$3" = 0 ] || why="$why,suites_failed"
    [ "$4" -gt 0 ] 2>/dev/null || why="$why,zero_tests"
    [ "$5" = "$4" ] || why="$why,tests_not_run"
    [ "$7" = 0 ] || why="$why,tests_failed"
    [ "${11}" = 0 ] || why="$why,asserts_failed"
    if [ -n "$BASE" ]; then
      # tests= field of this ut's line in the baseline output; its first number is tests ran (any format version)
      bl="$(awk -v d="$d" '$1 == "UT" && $2 == d { for (i = 3; i <= NF; i++) if ($i ~ /^tests=/) {
              split(substr($i, 7), a, "/"); print a[1]; exit } }' "$BASE")"
      if [ -n "$bl" ] && [ "$5" -lt "$bl" ] 2>/dev/null; then why="$why,tests_below_baseline_$bl"; fi
    fi
  fi
  if [ -z "$why" ]; then echo "UT $d PASS rc=$rc $desc"; pass=$((pass + 1)); continue; fi
  echo "UT $d FAIL rc=$rc $desc why=${why#,}"; fail=$((fail + 1)); bad="$bad,$d"
  # awk terminates every line, so a log cut off mid-line (a crash) cannot swallow the next UT line
  if [ "$sum" = none ]; then tail -5 "$log" | awk '{ print "  | " $0 }'
  else grep -E '\.\.\.FAILED|^ +[0-9]+\. ' "$log" | head -20 | awk '{ print "  | " $0 }'; fi
done
for f in test/unit/lib/nvme/*/*_ut; do
  [ -e "$f" ] || continue
  n="$(basename "$f")"; n="${n%_ut}"
  case " $EXPECTED " in *" $n "*) ;; *) echo "UT $n EXTRA not_run" ;; esac
done
rm -rf "$tmp"
n=$((pass + fail + missing))
echo "SPDK_NVME_UT pass=$pass fail=$fail missing=$missing n=$n${bad:+ failed=${bad#,}}"
[ "$fail" -eq 0 ] && [ "$missing" -eq 0 ]
EOS
)"

CAP="$(mktemp)"; trap 'rm -f "$CAP"' EXIT
if [ "${UT_RUNNER_LOCAL:-0}" = 1 ]; then
  env TREE="$S" FP="$FP" BASE="$BASEF" LOGDIR="$LOGS" EXPECTED="${EXPECTED[*]}" UT_TIMEOUT="$UT_TIMEOUT" \
    UT_KILL_AFTER="$UT_KILL_AFTER" bash -s <<<"$INNER" | tee "$CAP"
  rc=${PIPESTATUS[0]}
else
  MNT=(-v "$S":/spdk:ro -v "$FP":/fp/fingerprint.txt:ro)
  [ -n "$BASEF" ] && MNT+=(-v "$BASEF":/fp/baseline.txt:ro)
  [ -n "$LOGS" ] && MNT+=(-v "$LOGS":/utlogs)
  docker run --rm -i --network none --user "$(id -u):$(id -g)" -e HOME=/tmp --cpuset-cpus "8-15,24-31" \
    -e TREE=/spdk -e FP=/fp/fingerprint.txt -e BASE="${BASEF:+/fp/baseline.txt}" -e LOGDIR="${LOGS:+/utlogs}" \
    -e EXPECTED="${EXPECTED[*]}" -e UT_TIMEOUT="$UT_TIMEOUT" -e UT_KILL_AFTER="$UT_KILL_AFTER" \
    "${MNT[@]}" -w /spdk "$RID" bash -s <<<"$INNER" | tee "$CAP"
  rc=${PIPESTATUS[0]}
fi
last="$(tail -1 "$CAP")"
if [ "$rc" -eq 0 ]; then
  [[ "$last" =~ ^SPDK_NVME_UT\ pass=[0-9]+\ fail=0\ missing=0\ n=${#EXPECTED[@]}$ ]] && exit 0
  echo "SPDK_NVME_UT runner error: exit 0 but the last line is '$last'" >&2; exit 1
fi
[ "$rc" -eq 1 ] || echo "SPDK_NVME_UT runner error rc=$rc" >&2
exit 1
