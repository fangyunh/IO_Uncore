#!/usr/bin/env bash
# md_poll_asm.sh: normalized x86 disassembly of named functions in two builds of
# spdk/lib/nvme/nvme_pcie_common.c, and their diff (IAU-next P2: "the success path keeps its
# instructions").
#   --revs BEFORE AFTER     compile the file at two git revisions (AFTER may be WORKTREE) with the
#                           lean host build's code-generation flags (make -n of nvme_pcie_common.o in
#                           .build/spdk-host-<v>/src/spdk/lib/nvme; warning flags dropped) and the
#                           lean tree's spdk/config.h, with host gcc (= the build image's gcc 11.4),
#                           so the objects match the build's and addr= is authoritative
#   --objs BEFORE.o AFTER.o compare two existing objects, e.g. the snapshots'
#                           .build/spdk-host-<v>/src/spdk/lib/nvme/nvme_pcie_common.o (authoritative)
#   --sym NAME              repeatable, default nvme_pcie_md_poll; NAME.cold (the part GCC moved
#                           to .text.unlikely) is compared too, on its own line, when either
#                           object has it
#   --out DIR               default .build/tmp/md_poll_asm
#   --file NAME             with --revs: the spdk/lib/nvme source to compile (default nvme_pcie_common.c; IAU-next
#                           P4.9 / P4.9b: nvme_ns_cmd.c and nvme_iau.c for tests/realcore/test_ns_cmd_hot_layout.sh);
#                           the lib/nvme headers it
#                           includes by quote (nvme_internal.h, nvme_pcie_internal.h, nvme_iau.h, nvme_text_pad.h) are
#                           staged with it, each as the revision has it
# Per symbol it writes DIR/<sym>.before.s, DIR/<sym>.after.s, DIR/<sym>.diff and prints
#   MD_POLL_ASM sym=<sym> insns=<b>/<a> jcc=<b>/<a> calls=<b>/<a> addr=<b>/<a> diff=<changed lines>
# addr is the symbol's offset in its section, so layout changes stay visible.  Normalization drops
# addresses, raw bytes, in-function jump offsets, comments, nops, string-literal label numbers and
# section-relative relocation offsets; it keeps only the relocations inside the symbol's own address
# range (objdump -r --disassemble=SYM also prints every earlier relocation of the section); it
# writes the source-line argument of a log call as $LINE (a `mov $imm,%edx` from which the code
# reaches a call or tail jump to spdk_log / spdk_vlog / __assert_fail with %edx not written again,
# following unconditional jumps: __LINE__), so moved source lines are not counted; and a branch or
# call to a relocation shows its target as <reloc> (objdump would name whatever symbol follows the
# instruction).  The diff then holds real instruction changes.
# addr= is "-" for a .cold part missing from one object.  Exit 2 on bad arguments, 1 if a named
# symbol is missing from an object.  Read-only on the repo tree.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="$ROOT/.build/tmp/md_poll_asm"
MODE=""; A=""; B=""; SYMS=(); FILE=nvme_pcie_common.c
usage() { echo "usage: md_poll_asm.sh --revs BEFORE AFTER|WORKTREE | --objs A.o B.o [--sym NAME]... [--out DIR] [--file NAME]" >&2; exit 2; }
while [ $# -gt 0 ]; do
  case "$1" in
    --revs|--objs) [ $# -ge 3 ] || usage; MODE="$1"; A="$2"; B="$3"; shift 3 ;;
    --sym) [ $# -ge 2 ] || usage; SYMS+=("$2"); shift 2 ;;
    --out) [ $# -ge 2 ] || usage; OUT="$2"; shift 2 ;;
    --file) [ $# -ge 2 ] || usage; FILE="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$MODE" ] || usage
case "$FILE" in */*|"") usage ;; *.c) ;; *) usage ;; esac
OBJ="${FILE%.c}.o"
[ "${#SYMS[@]}" -gt 0 ] || SYMS=(nvme_pcie_md_poll)
mkdir -p "$OUT"

stage() {  # $1 = git rev or WORKTREE, $2 = dir: $FILE and the lib/nvme headers the sources include by quote
  local f
  mkdir -p "$2"
  for f in "$FILE" nvme_pcie_internal.h nvme_internal.h nvme_iau.h nvme_text_pad.h; do
    rm -f "$2/$f"
    if [ "$1" = WORKTREE ]; then
      if [ -f "$ROOT/spdk/lib/nvme/$f" ]; then cp "$ROOT/spdk/lib/nvme/$f" "$2/$f"; fi
    elif git -C "$ROOT" cat-file -e "$1:spdk/lib/nvme/$f" 2>/dev/null; then
      git -C "$ROOT" show "$1:spdk/lib/nvme/$f" > "$2/$f"
    fi
  done
  [ -f "$2/$FILE" ] || { echo "no $FILE at $1" >&2; exit 2; }
}
compile() {  # $1 = staged dir -> $1/$OBJ
  local cfg
  cfg="$(ls "$ROOT/.build/spdk-host-lean/src/spdk/include/spdk/config.h" "$ROOT/spdk/include/spdk/config.h" 2>/dev/null | head -1)"
  [ -n "$cfg" ] || { echo "no spdk/config.h (build the lean host tree)" >&2; exit 2; }
  mkdir -p "$OUT/cfg/spdk"; cp "$cfg" "$OUT/cfg/spdk/config.h"
  gcc -DSPDK_NVME_IO_CYCLE_OFF -g -fno-strict-aliasing -march=native -mno-avx512f -fno-lto \
      -D_GNU_SOURCE -fPIC -fstack-protector -fno-common -DNDEBUG -O2 -U_FORTIFY_SOURCE \
      -D_FORTIFY_SOURCE=2 -pthread -std=gnu11 \
      -I"$OUT/cfg" -I"$ROOT/spdk/include" -I"$ROOT/spdk/lib/nvme" \
      -c "$1/$FILE" -o "$1/$OBJ"
}
NORM_PY=$(cat <<'PY'
import re, subprocess, sys
obj, sym = sys.argv[1], sys.argv[2]
lo = hi = None
for l in subprocess.run(["objdump", "-t", obj], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) >= 6 and f[-1] == sym and " F " in l:
        lo = int(f[0], 16); hi = lo + int(f[-2], 16)
if lo is None:
    sys.exit(0)
dis = subprocess.run(["objdump", "-d", "-r", "--no-show-raw-insn", "--disassemble=" + sym, obj],
                     capture_output=True, text=True).stdout.splitlines()
out = []	# [text, address or None for a relocation line, jmp target address or None]
for l in dis:
    m = re.match(r"^\s+([0-9a-f]+): (R_X86_64_[A-Z0-9_]+)\s+(.*)$", l)
    if m:
        if lo <= int(m.group(1), 16) < hi:	# the symbol's own relocations only
            t = re.sub(r"\.LC[0-9]+", ".LC", m.group(3))
            t = re.sub(r"(\.(text|rodata)[.A-Za-z0-9_]*)[+-]0x[0-9a-f]+", r"\1", t)
            out.append(["  reloc %s %s" % (m.group(2), t.rstrip()), None, None])
        continue
    m = re.match(r"^ +([0-9a-f]+):\t(.*)$", l)
    if not m:
        continue
    raw = m.group(2)
    # lea of a symbol in the same section (a callback address): the displacement is layout, the symbol is not
    raw = re.sub(r"^(lea +)-?0x[0-9a-f]+(\(%rip\),%\w+) +# [0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>$", r"\1<\3>\2", raw)
    t = re.sub(r"[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>", r"<\1>", raw)
    t = re.sub(r"\s+#.*$", "", t)
    t = re.sub(r"\.LC[0-9]+", ".LC", t)
    t = re.sub(r"(\.(text|rodata)[.A-Za-z0-9_]*)[+-]0x[0-9a-f]+", r"\1", t).rstrip()
    if re.match(r"^(nop|data16|cs nop|xchg +%ax,%ax)", t):
        continue
    j = re.match(r"^jmp +([0-9a-f]+) <", raw)
    out.append([t, int(m.group(1), 16), int(j.group(1), 16) if j else None])
# The source-line argument of a log call: a `mov $imm,%edx` from which the code reaches a call (or a
# tail jump) to a log function with %edx not written again, following unconditional in-function
# jumps (GCC merges the calls of a log macro's branches into one call site).
LOG = re.compile(r"^  reloc R_X86_64_\w+ (spdk_log|spdk_vlog|__assert_fail)\b")
EDX_W = re.compile(r"(,%(rdx|edx|dx|dl|dh)$)|^(pop +%rdx|cltd|cqto|rdtscp?|cpuid|mul|imul +%|imul +[^,]*$|div|idiv)")
at = {e[1]: i for i, e in enumerate(out) if e[1] is not None}
def reaches_log(i):
    seen = set()
    for _ in range(64):
        if i >= len(out) or i in seen:
            return False
        seen.add(i)
        t, _a, tgt = out[i]
        if t.startswith("  reloc "):
            i += 1
            continue
        nxt_reloc = i + 1 < len(out) and out[i + 1][1] is None
        if re.match(r"^(call|jmp) ", t) and nxt_reloc and LOG.match(out[i + 1][0]):
            return True
        if t.startswith("jmp ") and tgt is not None and not nxt_reloc and tgt in at:
            i = at[tgt]
            continue
        if re.match(r"^(j|call|ret|ud2|int3|hlt)", t) or EDX_W.search(t):
            return False
        i += 1
    return False
for i, e in enumerate(out):
    if re.match(r"^mov +\$0x[0-9a-f]+,%edx$", e[0]) and reaches_log(i + 1):
        e[0] = "mov    $LINE,%edx"
# A branch or call whose target is a relocation: objdump shows whatever symbol follows the
# instruction, which is noise (the relocation line names the real target).
for i, e in enumerate(out):
    if i + 1 < len(out) and e[1] is not None and out[i + 1][1] is None:
        e[0] = re.sub(r"^((j[a-z]+|call) +)<[^>]*>$", r"\1<reloc>", e[0])
out = [e[0] for e in out]
print("\n".join(out))
PY
)
norm() {  # $1 = object, $2 = symbol -> normalized listing (empty if the symbol is missing)
  python3 -c "$NORM_PY" "$1" "$2" || true
}
saddr() {  # $1 = object, $2 = symbol -> 0x<offset in its section>
  local a
  a=$(objdump -t "$1" | awk -v s="$2" '$NF == s && / F / { print $1; exit }')
  if [ -n "$a" ]; then printf '0x%x\n' "$((16#$a))"; else echo -; fi
}
has() {  # $1 = object, $2 = symbol: true if the object defines that function symbol
  objdump -t "$1" | awk -v s="$2" '$NF == s && / F / { f = 1 } END { exit !f }'
}
if [ "$MODE" = --revs ]; then
  stage "$A" "$OUT/before"; stage "$B" "$OUT/after"
  compile "$OUT/before"; compile "$OUT/after"
  OA="$OUT/before/$OBJ"; OB="$OUT/after/$OBJ"
else
  OA="$A"; OB="$B"
  { [ -f "$OA" ] && [ -f "$OB" ]; } || { echo "missing object" >&2; exit 2; }
fi
ins() { grep -vc '^  reloc ' "$1" || true; }
jcc() { grep -E '^j[a-z]+ ' "$1" | grep -vc '^jmp' || true; }
cal() { grep -c '^call' "$1" || true; }
one() {  # $1 = symbol, $2 = 1 if it may be missing from one object (a .cold part)
  local s="$1" d
  norm "$OA" "$s" > "$OUT/$s.before.s"; norm "$OB" "$s" > "$OUT/$s.after.s"
  if [ "$2" != 1 ] && { [ ! -s "$OUT/$s.before.s" ] || [ ! -s "$OUT/$s.after.s" ]; }; then
    echo "MD_POLL_ASM sym=$s missing in one object" >&2; exit 1
  fi
  diff -u "$OUT/$s.before.s" "$OUT/$s.after.s" > "$OUT/$s.diff" || true
  d=$(grep -cE '^[-+]([^-+]|$)' "$OUT/$s.diff" || true)
  echo "MD_POLL_ASM sym=$s insns=$(ins "$OUT/$s.before.s")/$(ins "$OUT/$s.after.s") jcc=$(jcc "$OUT/$s.before.s")/$(jcc "$OUT/$s.after.s") calls=$(cal "$OUT/$s.before.s")/$(cal "$OUT/$s.after.s") addr=$(saddr "$OA" "$s")/$(saddr "$OB" "$s") diff=$d"
}
for s in "${SYMS[@]}"; do
  one "$s" 0
  case " ${SYMS[*]} " in *" $s.cold "*) continue ;; esac
  case "$s" in *.cold) continue ;; esac
  if has "$OA" "$s.cold" || has "$OB" "$s.cold"; then one "$s.cold" 1; fi
done
