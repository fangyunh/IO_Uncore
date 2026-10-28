#!/usr/bin/env python3
"""Spec §4 L evidence check for an investigation campaign.

  check_l.py <campaign_dir> [--out F] [--expect-md] [--p3-tgt V,...]

Campaign runs pass -Q 1 to perf, so completion errors do not fail a point. This reads each ok
point's run log (<run_dir>/logs/run_<STEM>.log, STEM as in run_point.sh) and FAILs on error
text, lost IOs, marker imbalance (per INV_<NAME> family and qid: constructs vs
"all <k> cids returned at destroy" lines) or a marker count that differs from the env-derived
expectation. With IAU_INV_REC_BYTES set (23-l3-rec) it also reads the point's target log
(<run_dir>/logs/nvmf_tgt_<STEM>.log) and FAILs unless nvmf_tgt's "INV_REC target: env=V bytes=B"
line is there, carries the point's env value, and gives the same bytes as every host
"INV_REC qid=N bytes=B" construct line (a host/target size disagreement usually hangs perf).
Further families (see EXPECT / TGT_FAMILIES): INV_DESC (25, same size agreement as INV_REC), INV_ADDR
(27, host mode= must equal SPDK_INV_ADDR), INV_DBSPIN (28, host ns=/cycles= must equal the parsed envs,
any mode), INV_PF (30, construct line only, no destroy line: "INV_PF qid=N loop=tr|trfree", expected when
SPDK_INV_PF=1, mode 4 and a 32 B lever loop), INV_IDLE (24, target-only "INV_IDLE target: env=V enabled=0|1"), INV_TRACE_FAST (26, one
"entries=N" line per ns_ctx, all equal), INV_GRING (29, construct-only "INV_GRING qid=N joined entries=E ring=P" per IO qpair,
expected when IAU_INV_GLOBAL_RING is 1 or 2, SPDK_INV_TRFREE=1 and mode 4; plus one host "INV_GRING ring=P iova=I entries=E
mode=M" line (M = the env value) and one "INV_GRING ring=P consumed=C members=0 at destroy" line (C > 0) per poll-group ring,
every join's ring P among them, and one nvmf_tgt "INV_GRING target: iova=I entries=E" line per ring with the same IOVAs; E
from IAU_INV_GLOBAL_RING_ENTRIES, default 4096, on every line; the sum of C equals the sum of emitted= on the target's
IAU_STATS lines). MD_TRFREE / MD_TRACKERS (F3, the spdk/ host library itself, not an INV_ lever): construct-only
"MD_TRFREE qid=N fast_cids=F" per Mode D IO qpair when mode 4 or 5 and SPDK_MD_TRACKERS is not "1", "MD_TRACKERS qid=N" per
Mode D IO qpair when mode 4 or 5 and SPDK_MD_TRACKERS is "1"; every other mode prints neither. MD_DBCOST (P11 review fix M2,
any mode, check_host_values): exactly one "MD_DBCOST ssd_ns=S iau_ns=I ticks= iau_ticks=" line when SPDK_DB_COST_NS or
SPDK_DB_COST_IAU_NS parses > 0, S / I = those values (<= 0 -> 0); none when both are unset or <= 0. Logs with any MD_ line are
checked strictly against that; a log with none is ok unless --expect-md is given (post-F3 campaigns pass it; pre-F3
builds print no MD_ line). Counts follow perf's default ns/worker association (no --use-every-core): n_ctx = max(app cores,
controllers) ns_ctx, one poll group each, so the per-qpair families expect qpairs * n_ctx lines and INV_GRING n_ctx rings. Extra CSV columns: doorbells (sum of destroy doorbells=D),
flush_idle (sum of flush_idle=N on the target's IAU_STATS lines).
Perf argv (P0.7): for each ok point <run_dir>/logs/cmd_<STEM>.txt must carry "-w <workload>" and "-o <io_size>"
(and "-M <rwmix>" for randrw) matching the point's workload/rwmix/io_size columns (absent or empty = randread/100/4096);
a missing cmd file or a difference FAILs with "cmd argv mismatch: -w ...".
Opcode mix (P1.2): for each ok point whose points.csv row has a NON-EMPTY workload column, the target log
(<run_dir>/logs/nvmf_tgt_<STEM>.log) must carry "OPC_STATS qid=N reads=R writes=W other=O" lines (one per I/O SQ, both
modes, printed by targets from nx-p1b on); summed over the lines it FAILs if the log or the lines are missing, R+W == 0,
workload randread and W > 0, randwrite and R > 0, or randrw and |100*R/(R+W) - rwmix| > 1.0 (rwmix empty = 100; a
rwmix that is not a number in [0, 100] FAILs with "rwmix not a number in [0, 100]"). On mode 4 points it also FAILs
"opc/fetched mismatch" unless, per qid (summed over controllers), R+W+O equals fetched= of the target's IAU_STATS
lines. Rows with an absent or empty workload column (campaigns from before P0.7 added it) are not checked: their targets
predate the counters. gen_inv_points.py always fills the column, so a new campaign whose target predates nx-p1b FAILs here.
P3.9: any IAU_ENGINE_DETACH_TIMEOUT / IAU_ENGINE_BIND_TIMEOUT / "queues still bound at transport destroy" line in the target log FAILs, in any mode.
IAU-next P3 (testbed restructure): NX_DEVMODEL (present target "DEV_MODEL ctrlr=K model=M fastpath= channels= tmax=
lmin_ns=" lines: one per controller, model = IAU_DEV_MODEL or sq, settings = IAU_DEV_<KEY>_C<K> over IAU_DEV_<KEY> over
1/32/250000/1000, except that the FASTPATH default follows the point's bdev column: fastpath (or empty) -> 1, null|delay
-> 0, the IAU_DEV_FASTPATH start_target.sh gives the target (it overrides a point-env IAU_DEV_FASTPATH, so an explicit
one that disagrees with the bdev FAILs); mandatory for --p3-tgt target variants and on modes 5 and 6). NX_ENGINE (mode 5 = arm I1e): one host
MD_ENGINE and one target IAU_ENGINE_BIND / IAU_ENGINE_STATS line per IO qpair; per queue desc_fetched
== sqes_written == cqes_consumed == records_emitted (a P2.1 re-issue is just another descriptor fetch, so the relation
holds with retries; reissues= is read but must be 0, the engine cannot know), inflight/bad_cid/bad_db/host_ssd_db/bad_sqhd 0,
err_records (records with SCT/SC != 0) required: > 0 only when the point env sets SPDK_INV_EXPECT_RETRIES=1, and then the
point total must be > 0 (the retry path must have run),
the SSD's OPC_STATS (keyed by ctrlr= and qid) R+W+O == sqes_written and fp_done+O == cqes_consumed;
sum(host pcie_stats completions) == sum(records_emitted); IAU_ENGINE_START lcores == engine_cpus; host credits == bound
credits; BIND/STATS (ctrlr,qid) sets == the IO-qpair set; engine_cpus non-empty with >= 1 START; BIND engine/lcore match START;
DEV_MODEL ctrlr indices exactly 0..controllers-1; total records_emitted > 0; no legacy IAU_STATS line; any of these lines on another mode FAILs.
FP_SHARE (target "FP_SHARE ctrlr= qid= lcore= users=" lines, printed at bind in the ctrlr device model only): with
IAU_DEV_MODEL=ctrlr one line per IO qpair of each controller whose fast path is on (the DEV_MODEL fastpath
expectation; a controller without the fast path has no device model and prints none), the (ctrlr, qid) set = qids
1..n of each such controller, all qids of a controller on one lcore, users 1..n per controller; any FP_SHARE line
without IAU_DEV_MODEL=ctrlr FAILs. Worker group (SPDK_PERF_WORKER_GROUP=1): one
"PERF_WORKER_GROUP lcore= ns_ctx= qpairs=" line and one pcie_stats block per worker; without it no such line and one
pcie_stats block per ns_ctx when any is present; any "PERF_WORKER_GROUP lcore=N group destroy failed" line (a qpair
left in the worker's group at cleanup) FAILs, env or not.
NX_TILE (IAU-next P4.6; mode 6 = target IAU_UNCORE_MODE=5 + host SPDK_IAU_TILE=1, private rings until P4.8; the key lists
are the TILE_* tuples below, the C X lists' copies). Any mode: a v6 host knob (TILE_ONLY_KNOBS) on a non-6 point or
SPDK_IAU_S2 on a non-0 point ("v6 knob on a mode M point"); SPDK_PERF_FAULT=1 before P4.12 (TILE_PERF_FAULT_EVIDENCE
unset; afterwards a fault point = SPDK_PERF_FAULT=1 AND perf's fault evidence line, and either alone FAILs); a
host-library IAU_TILE_ line in the TARGET log (the target saw SPDK_IAU_*), IAU_ENGINE6_REFUSED / IAU_ENGINE6_BIND_TIMEOUT,
"IAU tile ring ... members still bound at transport destroy" or "IAU engine N stopped with M IAU tile rings" FAILs; on modes 0/4/5 any IAU_TILE_ (host log) or IAU_ENGINE6_
(target log) line FAILs ("<names> lines on a mode M point", as NX_ENGINE). Mode 6 (DEV_MODEL mandatory; the P3
NX_ENGINE lines must be absent, so a MD_ENGINE line FAILs there): one IAU_TILE_OPS line whose stride/rcons/rings/
ring_entries are the env's (C defaults 64/0/1/16384); no IAU_TILE_LOST / _RESET_REFUSED / _JOIN_REFUSED / _REFUSED /
_CONFIG_ERROR line; one IAU_TILE_QPAIR, IAU_TILE_STATS, IAU_TILE_GROUP_END per host IO qpair and one IAU_ENGINE6_BIND /
IAU_ENGINE6_STATS per member; at most one IAU_ENGINE6_PEAK per engine; IAU_TILE_GROUP once per QPAIR gid; IAU_ENGINE6_RING and _RING_END paired by (gid,ring) and
equal to the host's rings; group/ring stride = SPDK_IAU_DESC_STRIDE; fast= = SPDK_IAU_FAST, private= = not
SPDK_IAU_GROUP, ring < K; host (gid,route) set == engine member set, with equal qid/ring/gen/credits/nsid; host STATS ==
QPAIR set; engine STATS == BIND set; BIND/STATS (ctrlr,qid) = the IO-qpair set; IAU_ENGINE_START on exactly
engine_cpus, every RING/BIND on a started thread, BIND on its ring's engine, STATS engine = BIND engine; no legacy
IAU_STATS. Per engine STATS (every TILE_ENG_MEMBER_STATS key + inflight/parked/host_ssd_db/state present):
io_fetched+raw_fetched+reissues == sqes_written == cqes_consumed+dropped_busy; inflight, parked, bad_cid, bad_sqhd,
bad_ctrl, host_ssd_db, parked_dropped, rec_inval 0; rec_detached 1; state=gone; rec_err_parked+rec_err_final > 0 only
with expected errors: SPDK_INV_EXPECT_RETRIES=1 (then the total must be > 0) or a fault knob WITH its evidence
(IAU_ENG_FAULT_ERR_EVERY: exactly one IAU_ENG6_FAULT line with that every=, sum injected > 0, sum rec_err_* > 0;
IAU_DEV_FAULT_ERR_EVERY[_C<k>]: one DEV_FAULT ctrlr=k line with the effective every= for each controller it applies
to and none other, sum DEV_FAULT_STATS marked > 0; a knob in the env without that evidence FAILs); on a non-fault
point rec_timeout, rec_sqdel, rec_ack, rec_abort_req, dropped_busy, credit_stalls, sq_full_stalls 0; SSD doorbells
written; per (ctrlr,qid) SSD OPC_STATS R+W+O == sqes_written and, with the fast path, fp_done+O == cqes_consumed.
Per RING_END (every key present): bad_db, bad_kind, r_overflow 0 always; orphan_io, orphan_ack, stale_gen, members,
in_use 0 on non-fault points; raw_waits, peak_slots informational. Per ring: records_emitted == the members'
TILE_ENG_REC + orphan_io + orphan_ack (one record counter per eng6_emit site; the excess is 0..stale_gen, so exact
while stale_gen == 0); on a non-fault point the exact disposition identity of the group-rings paragraph (P4.8b I2,
private rings too). Per host STATS (TILE_HOST_QSTATS + lost + pend_left
present): lost 0, pend_left 0, detached 1, err / retries only with expected errors, aborted / timeouts / ctrl 0 on
non-fault points; GROUP_END keys present, stray 0 on every line of a non-fault point (a LOST or removed member's late
record counts stray), no counter decreasing from one of a gid's lines to the next (printed per member leave,
group-cumulative: the LAST line holds the totals, a declared P4.8 deviation). Non-fault: no "dropping the ring" /
"remap ... failed"
target line, no host "IAU tile: Delete I/O CQ failed". Three-way: host records (per gid its last GROUP_END records=) ==
sum of RING_END records_emitted, > 0. P4.10b (A19) IAU_TILE_CTX_SPILL (mode 6 only; "1" on, "" / "0" off, else FAIL): on needs exactly one IAU_ENGINE6_CTX_SPILL
on=1 line=64, no CTX_SPILL_REFUSED, per member one CTX_SPILL_BIND (A19 route block inside the region, blocks of a gid
disjoint) and one CTX_SPILL_STATS with stores == sqes_written - reissues and loads == cqes_consumed - bad_cid, totals > 0;
off with any CTX_SPILL line FAILs (the knob on silently); "1" on another mode FAILs (v6 knob on a mode M point).
A fault-knob line without ITS env FAILs (IAU_ENG6_FAULT without an
IAU_ENG_FAULT_* key, DEV_FAULT / DEV_FAULT_STATS without an IAU_DEV_FAULT_* key: the knob on silently).
NX_TILE group rings (P4.8b; mode 6 with SPDK_IAU_GROUP=1, perf's poll groups = one per worker with
SPDK_PERF_WORKER_GROUP=1, else one per ns_ctx). Evidence: IAU_TILE_OPS group=1 iff the knob (group=0 or no group= key with
the knob FAILs, group=1 without it FAILs); an OPS line with group= (a P4.8+ host library) requires the GROUP_END tail
TILE_HOST_END_TAIL. P4.8e: OPS db_batch= (the group pass's doorbell chunk) equals SPDK_IAU_DB_BATCH when set,
TILE_DEFAULT_DB_BATCH (64; a snapshot built before the P4.8f default change: TILE_DB_BATCH_DEFAULT_BY_VARIANT, 32) on a
group run without it, 0 without the group; an absent key is an older host library (0), where the knob FAILs (lever
evidence); a non-zero knob without SPDK_IAU_GROUP=1 FAILs (0 there is a no-op printing db_batch=0, decision m3).
Topology: one IAU_TILE_GROUP private=0 line per poll group with k = SPDK_IAU_RINGS (default TILE_DEFAULT_RINGS) and
entries = SPDK_IAU_RING_ENTRIES (default 16384), its members = the poll group's qpairs (PERF_WORKER_GROUP qpairs= / the
ns_ctx's), every QPAIR private=0 (the private= rule above), any JOIN_REFUSED FAILs
(TILE_HOST_BAD; so does any RESET_REFUSED). Every mode 6 point: route r on ring r mod k of its group (a drift pin only:
print_qpair() prints ring = route mod k by construction; the routing evidence is orphan_io 0 plus the per-ring
identities below), one member per route on non-fault points (a rejoin prints a route again, P4.12), a shared group's
routes 0..n-1 on non-fault points; IAU_ENGINE6_RING ring k of gid on engine (gid + k) mod n_engines (n_engines = the
engine_cpus count), its nk / d_entries / r_entries = the group's k / entries; engine lines join host groups by gid (the
GCB's gid word; the target keys rings by GCB IOVA and k); every IAU_ENGINE6_RING / _BIND / _RING_END / _RELEASE line
parsed (A10). Per member, every point: cqes_consumed == rec_cpl + rec_err_parked + rec_err_final + rec_abort_req +
bad_cid (member_cq(): one record kind or bad_cid per consumed CQE). Exactly once per group and in total: host records
(the gid's last GROUP_END records=) == the gid's RING_END records_emitted, and on non-fault points == its members' SSD
posted (OPC_STATS fp_done + other) + their rec_detached, or, without a usable fp_done (no fast path, or a mixed group),
their cqes_consumed + rec_detached (the three-way never drops to two-way). GROUP_END (non-fault): one line per QPAIR of
its gid, stray 0 on every line, kept 0 on the last (a state, not a counter). Dispositions (non-fault, per (gid, ring),
private and shared, exact): flushes + releases + reissues of its members + the ring's bad_route == its BIND lines (one
FLUSH each) + the host's REISSUE / RELEASE count (STATS reissues + releases) for its routes (each descriptor is fetched
once, by its live member or memberless); per host STATS (every point) reissues + releases <= err (one disposition per
PARKED record at most). Doorbells (every mode 6 point; perf's delay_cmd_submit, so interrupt mode on a mode 6 point
FAILs in check_cmd: -E, bundled or not, --enable-interrupt or a unique abbreviation of it, as getopt_long parses perf's
argv; exactly one complete PCIE block, polls / idle_polls / sq_mmio, per perf poll group is required, a missing one
FAILs): per perf PCIE block sq_mmio <= R x (polls - idle_polls) + K x floor(records / db_batch) + its rings +
queued_requests + members + host dispositions + abort controls (P8.H1-dbg, below; the P4.8e mid-pass term,
tile_db_mid_pass(): all K dirty rings after every full chunk of db_batch records of a ring; 0 at db_batch 0; records =
the shared gids' last GROUP_END records=, all of
them for a block, which names no gid), R = K for a shared group, the ns_ctx's qpairs for private rings (perf picks the
qpair round-robin per submission, so one qpair's busy poll can dirty every ring of its ns_ctx, whose next, possibly
idle, poll rings it); per shared gid's last GROUP_END the same with R = K (teardown included); db_per_busy_poll (perf's
sq_mmio / (polls - idle_polls)) is informational. IAU_ENGINE6_RELEASE (every mode 6 point): each names a BIND (ctrlr,
qid = cqid, gid, ring, route) with that BIND's gen, at most one per (gid, ring, route, gen); non-fault: rc 0, per ring
RELEASE lines == BIND - RING_END and RING_END reserved == BIND - RELEASE(rc 0): one per deleted route EXCEPT each ring
incarnation's last member (its detach unbinds the ring and frees the gring, sq_eng6_detach(); its Delete I/O CQ then
finds no listed gring, sq_eng6_cq_deleted()); a LOST or held route (no or a failed Delete I/O CQ) gets none either,
fault points only. The count assumes each ring's members are torn down one after the other (perf: synchronous deletes on
its poll group's one thread). A16 (the host-bound label) before P4.12 had ONE source on every point: perf's
idle_polls / polls (host_idle_frac, the PCIE transport block = the poll group's own PCIe stats); since P4.12 the source of
every worker-group point is perf's time-based idle (A16 time-based below) and host_idle_frac is informational there. Under group polling
those count one poll per perf poll-group call (nvme_iau_pg_process_completions(): idle when the pass delivered nothing;
perf's loop makes no direct member poll and the attention pass's generic calls count none), so the fraction still
answers "did the host's poll find work"; under the stock poll-group ops (private rings) they count one poll per member
qpair per call. A call is idle only if every member is, so for the same traffic the group fraction is <= the per-qpair
one: a group point reaches the 0.05 host-bound threshold more easily than a private one; compare labels across the two
with that bias in mind, never the magnitudes. group_idle_frac (group points only: the shared groups' last GROUP_END
idle_polls / polls) is informational and NOT the A16 source: g->polls also counts direct polls and the teardown polls
after perf's stats print.
NX_ENGINE and NX_TILE skip the SSD-posted rule without the fast path (bdev null|delay, IAU_DEV_FASTPATH[_C<k>]=0:
fp_done counts fast-path completions only).
IAU-next P4.12 additions (plan P4.12 rev 2, A16, A20, A25; reports/p4_12.md):
NX_FAULT (check_fault(), any mode): perf's fault mode, SPDK_PERF_FAULT=1 with its PERF_FAULT_CONFIG evidence line (I2's
fault point); the rules are check_fault()'s docstring (one PERF_FAULT line: dup 0, inflight_end 0, submitted ==
completed, no PERF_FAULT_DRAIN_BOUND (A20); each knob's evidence and only a set knob's; F1; the error knobs' DNR rules; ok ==
completed without a knob that can end an IO in error). A fault point also relaxes, in NX_TILE: perf's "completed with error"
text (a counted outcome), the fault-only tripwires (as before), the CTX_SPILL loads equality (<=, m12); P4.12b: SPDK_PERF_FAULT_RAW_INVAL_EVERY
was removed (the ns_cmd SGL split never reaches the payload builder), any point that sets it FAILs and rec_inval is a
tripwire again; a reset fault point (SPDK_PERF_FAULT_RESET_MS) allows
IAU_TILE_RESET_DEFERRED / _RESET_DISABLE (FAIL elsewhere). Rejoins (every point): IAU_TILE_QPAIR, IAU_ENGINE6_BIND and
IAU_ENGINE6_STATS count qpairs + the host STATS' rejoins, the host's detached = 1 + rejoins, and a point other than a
reset fault point needs rejoins 0 (P4.12b); the CTX_SPILL overlap rule is time-aware and its STATS paired per incarnation (m11).
NX_S2 (check_s2(), mode 0 + SPDK_IAU_S2=1): the evidence lines of reports/p4_11.md section 8; an IAU_S2_ line without
the knob FAILs, and so does SPDK_IAU_S2_WB (P11 review fix M3) on a point that is not S2. IAU_S2_QPAIR sqe=wb iff
SPDK_IAU_S2_WB=1, else sqe=nt; a line without sqe= (builds before P11) passes only while SPDK_IAU_S2_WB is unset. The fast-path rule (the P4.9 review's, both v6 SPDK_IAU_FAST and S2): FAST off -> sub_fast = declined = 0;
FAST on with perf's queue depth per qpair (-q over -P, rounded up) <= the qpair's credits (S2: fast_cids) -> no declines
and no request-backed IO (non-fault); else the qpair is "mixed" (S2: the point "S2 not fair", also with a timeout
latched) and the point's request-free share is reported.
A16 time-based (check_idle_time()): every worker-group point with perf's PERF_IDLE_TIME lines (one per worker) is labelled
by idle_time_frac = sum idle_tsc / sum window_tsc (host if <= 0.05, else latency); one without them keeps the poll-count
label marked "poll-count (pre-P4.12)" only when its perf build predates P4.12 (variant, PERF_IDLE_PRE_P412_NX; a later
build without the lines FAILs, P4.12b); other points keep "poll-count" (host_idle_frac).
P4.12b statuses (check_fault): err, other_err and inval are never allowed; err_dnr only with a target error knob of DNR 1
(and that knob needs err_dnr > 0); ABORT_ON_TIMEOUT needs aborted_req > 0 (T3); IAU_S2_RESET_DISABLE done=0 FAILs (A26).
P8.H1-dbg (reports/p8_h1_dbg.md; the T3 / R3 false FAILs of nx_p8f_faults): the doorbell bounds add the abort
controls the host wrote (host STATS ctrl= summed: all for a PCIE block, the gid's for its GROUP_END): an ABORT_TAG /
ABORT_COOKIE (D10) is a D-ring control written from an R_TIMEOUT or ack record's callback, a pass the group poll counts
idle (it delivered no IO completion), so its doorbell is outside R x (polls - idle_polls); each comes back once as an
ack (record, or the CLOSED walk's ack(0)), so ctrl= counts them on every route that does not end LOST. A shared
group's members are its IAU_TILE_QPAIR lines minus its host STATS' rejoins (a rejoin prints the line again on the same
gid). OPC_STATS (one per I/O SQ lifetime) pairs per incarnation: the i-th of a (ctrlr, qid) with its i-th
IAU_ENGINE6_STATS; more OPC_STATS lines than member incarnations FAILs.
Output CSV: point_id,arm,cell,mode,verdict,reason,doorbells,flush_idle,read_pct,err_records,bad_sqhd,tile_records,host_idle_frac,peak_slots,
group_idle_frac,db_per_busy_poll,idle_time_frac,a16_src,a16_bound,fast_share,fast_label,fault_stats (OUT_COLS; P4.12
appended the last six: the time-based idle, the A16 source and label, the request-free share and label, perf's fault
counters)
(err_records / bad_sqhd: sums of IAU_ENGINE_STATS err_records= / bad_sqhd= on mode 5, else empty; read_pct = 100*R/(R+W)
when the mix was checked; tile_records: the mode 6 host records; host_idle_frac (A16, every point with poll counts):
perf idle_polls / polls over its PCIE transport blocks, NOT CPU idle; peak_slots: the max IAU_ENGINE6_PEAK
peak_slots= (printed per engine at target shutdown only), else empty; group_idle_frac: group points only;
db_per_busy_poll: mode 6 points; see NX_TILE group rings). Summary 'CHECK_L ok=N fail=M' on stderr; exit 1
if any FAIL.
"""
import argparse
import csv
import math
import os
import re
import shlex
import sys

BAD = ("completed with error", "lost io", "extra cids", "cannot service", "starting i/o failed",
       "mismatch", "no completion path",
       "does not map to outstanding cmd", "perf_watchdog")

# marker family -> predicate(env dict, mode) giving the expected destroy-ok count as a function of qpairs
# (None = no expectation for this point -> zero lines required). Add later levers here.
EXPECT = {
    "INV_THIN": lambda env, mode: env.get("SPDK_INV_THIN") == "1",
    "INV_TRFREE": lambda env, mode: env.get("SPDK_INV_TRFREE") == "1" and mode == 4,
    "INV_REC": lambda env, mode: mode == 4 and env.get("IAU_INV_REC_BYTES", "") != "",
    "INV_DESC": lambda env, mode: mode == 4 and env.get("IAU_INV_DESC_BYTES", "") != "",
    "INV_ADDR": lambda env, mode: mode == 4 and env.get("SPDK_INV_ADDR", "") in ("va", "vtophys"),
    "INV_DBSPIN": lambda env, mode: strtoll(env.get("SPDK_INV_DB_SPIN_NS", "")) > 0
    or strtoll(env.get("SPDK_INV_DB_SPIN_CYCLES", "")) > 0,
    # 30-l10-pf: mirrors the C: Mode D IO qpair whose final inv_flags routes completions to a 32 B lever
    # loop = no THIN, no REC16/REC64, and a bit from REC32, TRFREE, ADDR (va|vtophys) or DESC (env set, 32|40|64)
    # 29-l5-gring: per-IO-qpair join lines (the C rule: IAU_INV_GLOBAL_RING strtoll 1 or 2, on 20's trfree path)
    "INV_GRING": lambda env, mode: mode == 4 and strtoll(env.get("IAU_INV_GLOBAL_RING", "")) in (1, 2)
    and env.get("SPDK_INV_TRFREE") == "1",
    "INV_PF": lambda env, mode: env.get("SPDK_INV_PF") == "1" and mode == 4
    and env.get("SPDK_INV_THIN") != "1" and strtoll(env.get("IAU_INV_REC_BYTES", "")) not in (16, 64)
    and (strtoll(env.get("IAU_INV_REC_BYTES", "")) == 32 or env.get("SPDK_INV_TRFREE") == "1"
         or env.get("SPDK_INV_ADDR", "") in ("va", "vtophys")
         or (env.get("IAU_INV_DESC_BYTES", "") != "" and strtoll(env.get("IAU_INV_DESC_BYTES", "")) in (32, 40, 64))),
}

# families whose evidence is the construct line only (no destroy line): count constructs per qpair
NO_DESTROY = ("INV_PF", "INV_GRING")
# evidence-only family: not balanced, not counted (see check_flags)
EVIDENCE_ONLY = ("INV_FLAGS",)

# F3 tracker-free Mode D (spdk/ host library, nvme_pcie_qpair_construct): one construct line per Mode D IO qpair,
# no destroy line. Not INV_ lines, so the INV_ marker balance and the INV_FLAGS rule do not see them.
MD_EXPECT = {
    "MD_TRFREE": lambda env, mode: mode in (4, 5) and env.get("SPDK_MD_TRACKERS") != "1",
    "MD_TRACKERS": lambda env, mode: mode in (4, 5) and env.get("SPDK_MD_TRACKERS") == "1",
}
# searched anywhere in the line (a token can be glued to another worker's stdout), every occurrence counted
MD_MARK = re.compile(r"(MD_TRFREE|MD_TRACKERS) qid=(\d+)\b")

# target-line families with a host/target record-size agreement: family -> env var
TGT_FAMILIES = {"INV_REC": "IAU_INV_REC_BYTES", "INV_DESC": "IAU_INV_DESC_BYTES"}

# evidence tokens may be glued mid-line to another worker's stdout (e.g. "compute_INV_TRFREE qid=4 ..."),
# so they are searched anywhere in the line; no word boundary before INV_.
MARK = re.compile(r"(INV_[A-Z0-9_]+) qid=(\d+) (.*)$")
DESTROY = re.compile(r"^all \d+ cids returned at destroy")
IDLE_TGT = re.compile(r"INV_IDLE target: env=(\S*) enabled=([01])\b")
TF_LINE = re.compile(r"INV_TRACE_FAST entries=(\d+)\b")
ADDR_HOST = re.compile(r"INV_ADDR qid=\d+ mode=(\S+)")
FLAGS_HOST = re.compile(r"INV_FLAGS qid=(\d+) flags=(0x[0-9a-fA-F]+)")
PF_HOST = re.compile(r"INV_PF qid=\d+ loop=(\S+)")
SPIN_HOST = re.compile(r"INV_DBSPIN qid=\d+ ns=(-?\d+) cycles=(-?\d+)\b")
DBCOST = re.compile(r"MD_DBCOST ssd_ns=(-?\d+) iau_ns=(-?\d+) ticks=(\d+) iau_ticks=(\d+)")   # P11 review fix M2
SPIN_DEST = re.compile(r"INV_DBSPIN qid=\d+ all \d+ cids returned at destroy \(doorbells=(\d+)\)")
FLUSH_IDLE = re.compile(r"\bflush_idle=(\d+)")
GRING_RING = re.compile(r"INV_GRING ring=(0x[0-9a-fA-F]+) iova=(0x[0-9a-fA-F]+) entries=(\d+)(?: mode=(\d+))?")
GRING_JOIN = re.compile(r"INV_GRING qid=\d+ joined entries=(\d+)(?: ring=(0x[0-9a-fA-F]+))?")
TGT_EMITTED = re.compile(r"IAU_STATS qid=\d+ fetched=\d+ emitted=(\d+)")
TGT_FETCHED = re.compile(r"IAU_STATS qid=(\d+) fetched=(\d+)")
GRING_FREE = re.compile(r"INV_GRING ring=(0x[0-9a-fA-F]+) consumed=(\d+) members=(\d+) at destroy")
GRING_TGT = re.compile(r"INV_GRING target: iova=(0x[0-9a-fA-F]+) entries=(\d+)")
OPC_TGT = re.compile(r"OPC_STATS qid=(\d+) reads=(\d+) writes=(\d+) other=(\d+)")

# IAU-next P3 evidence (testbed restructure)
DEVMODEL = re.compile(r"DEV_MODEL ctrlr=(-?\d+) model=(\S+) fastpath=(\d+) channels=(\d+) tmax=(\d+) lmin_ns=(\d+)")
DEV_KEYS = (("FASTPATH", 3, "1"), ("CHANNELS", 4, "32"), ("TMAX", 5, "250000"), ("LMIN_NS", 6, "1000"))
ENG_BIND = re.compile(r"IAU_ENGINE_BIND ctrlr=(-?\d+) qid=(\d+) engine=(\d+) lcore=(\d+) nsid=(\d+) credits=(\d+)")
ENG_STATS = re.compile(r"IAU_ENGINE_STATS ctrlr=(-?\d+) qid=(\d+) engine=(\d+) (.*)$")
ENG_START = re.compile(r"IAU_ENGINE_START engine=(\d+) lcore=(\d+) want=(\d+)")
MD_ENGINE = re.compile(r"MD_ENGINE qid=(\d+) qcfg=0x[0-9a-fA-F]+ nsid=(\d+) credits=(\d+)")
OPC_P3 = re.compile(r"OPC_STATS qid=(\d+) reads=(\d+) writes=(\d+) other=(\d+) ctrlr=(-?\d+) fp_done=(\d+)")
WGROUP = re.compile(r"PERF_WORKER_GROUP lcore=(\d+) ns_ctx=(\d+) qpairs=(\d+)")
FP_SHARE = re.compile(r"FP_SHARE ctrlr=(-?\d+) qid=(\d+) lcore=(\d+) users=(\d+)")
WGROUP_DESTROY_FAIL = re.compile(r"PERF_WORKER_GROUP lcore=(\d+) .*\bdestroy failed")
ENGINE_TIMEOUT = re.compile(r"IAU_ENGINE_(?:DETACH|BIND)_TIMEOUT|\bIAU engine \d+: \d+ queues still bound at transport destroy")
PCIE_STATS = re.compile(r"pcie_stats: polls=\d+ completions=(\d+)")
NUMKV = re.compile(r"(\w+)=(\d+)")
ENG_NEED = ("err_records", "bad_sqhd", "desc_fetched", "sqes_written", "ssd_db_writes", "cqes_consumed", "cq_db_writes", "records_emitted",
            "inflight", "bad_cid", "bad_db", "host_ssd_db")

# IAU-next P4.6 NX_TILE (mode 6 = target IAU_UNCORE_MODE=5 + host SPDK_IAU_TILE=1). Every key list check_tile relies on,
# in C print order, is here and only here; tests/realcore/test_inv_check_l_tile.py greps the C X lists and formats and
# asserts these match them, and at run time a required key missing from a line FAILs the point (A10).
TILE_HOST_QSTATS = ("sub_fast", "sub_req", "sub_raw", "declined", "err", "retries", "reissues", "releases", "aborted",
                  "timeouts", "ctrl", "detached", "rejoins")         # NVME_IAU_QSTATS (spdk/lib/nvme/nvme_iau.h)
TILE_HOST_STATS_TAIL = ("lost", "pend_left")                            # print_stats() after the X list (nvme_iau.c)
TILE_HOST_END_KEYS = ("records", "stray", "polls", "doorbells", "kept")  # member_leave() IAU_TILE_GROUP_END
# P4.8: the GROUP_END tail after TILE_HOST_END_KEYS (a group's idle polls, sweeps, attention calls). P4.8b: required
# when the log's IAU_TILE_OPS line carries group= (a P4.8+ host library prints both); older logs have neither
TILE_HOST_END_TAIL = ("idle_polls", "sweeps", "attn_calls")
TILE_ENG_MEMBER_STATS = ("io_fetched", "raw_fetched", "reissues", "releases", "aborts", "flushes", "sqes_written",
                       "ssd_db_writes", "cqes_consumed", "cq_db_writes", "rec_cpl", "rec_err_parked", "rec_err_final",
                       "rec_timeout", "rec_abort_req", "rec_sqdel", "rec_ack", "rec_detached", "rec_inval", "marked",
                       "bad_cid", "bad_sqhd", "bad_ctrl", "injected", "dropped_busy", "parked_dropped",
                       "credit_stalls", "sq_full_stalls", "fuse_waits", "peak_inflight")
#                                                     IAU_ENG6_MEMBER_STATS (spdk/lib/nvmf/vfio_user_iau_tile.h)
TILE_ENG_MEMBER_TAIL = ("inflight", "parked", "host_ssd_db")     # iau_eng6_fmt_stats() after the X list (+ state=<name>)
TILE_ENG_RING_STATS = ("desc_fetched", "d_slots", "records_emitted", "bad_db", "bad_route", "bad_kind", "orphan_io",
                     "orphan_ack", "stale_gen", "raw_waits", "r_overflow", "peak_slots")   # IAU_ENG6_RING_STATS
TILE_ENG_RING_TAIL = ("members", "in_use", "reserved")           # iau_eng6_fmt_ring_end() after the X list
TILE_DEFAULT_STRIDE = 64            # IAU_TILE_DESC_BYTES (spdk/include/spdk_internal/iau_tile_abi.h), SPDK_IAU_DESC_STRIDE unset
TILE_DEFAULT_RING_ENTRIES = 16384   # NVME_IAU_RING_ENTRIES_DEFAULT (nvme_iau.h), SPDK_IAU_RING_ENTRIES unset
TILE_DEFAULT_RINGS = 1              # env_u32("SPDK_IAU_RINGS", 1, IAU_TILE_MAX_K, 1, ...) (nvme_iau.c), SPDK_IAU_RINGS unset
TILE_DEFAULT_DB_BATCH = 64          # NVME_IAU_DB_BATCH_DEFAULT (nvme_iau.h, P4.8f), SPDK_IAU_DB_BATCH unset on a group run
# host variants (snapshots) built while NVME_IAU_DB_BATCH_DEFAULT was 32 (P4.8e ffb4acc7 up to the P4.8f default
# change): their group points without the knob print db_batch=32 (the nx-p4a4 round trip's group legs)
TILE_DB_BATCH_DEFAULT_BY_VARIANT = {"nx-p4a4": 32}
# the rules over those keys (plan rev 2 P4.6 + controller corrections; rev 2's rec_aborted = rec_sqdel, the
# ABORTED_SQ_DELETION records, and its aborted_late = rec_abort_req; bad_route is NOT a tripwire, the header says a
# clean teardown produces it; raw_waits and peak_slots are informational)
TILE_ENG_ZERO = ("inflight", "parked", "bad_cid", "bad_sqhd", "bad_ctrl", "host_ssd_db", "parked_dropped", "rec_inval")
TILE_ENG_ERR = ("rec_err_parked", "rec_err_final")              # > 0 only with expected errors
TILE_ENG_NONFAULT_ZERO = ("rec_timeout", "rec_sqdel", "rec_ack", "rec_abort_req", "dropped_busy", "credit_stalls",
                        "sq_full_stalls")
TILE_RING_ZERO = ("bad_db", "bad_kind", "r_overflow")            # every point, fault points too
TILE_RING_NONFAULT_ZERO = ("orphan_io", "orphan_ack", "stale_gen")
TILE_HOST_ERR = ("err", "retries")                               # > 0 only with expected errors
# record identity (M2): each eng6_emit() site of vfio_user_iau_tile.c increments exactly one of these (the drift test
# pins the sites): records_emitted == sum of the members' TILE_ENG_REC + the ring's TILE_RING_REC; the memberless sites
# count stale_gen instead for a stale incarnation (and stale REISSUE / RELEASE / FLUSH count stale_gen with no record),
# so the excess is 0..stale_gen, exactly 0 while stale_gen == 0
TILE_ENG_REC = ("rec_cpl", "rec_err_parked", "rec_err_final", "rec_timeout", "rec_abort_req", "rec_sqdel", "rec_ack",
              "rec_detached", "rec_inval")
TILE_RING_REC = ("orphan_io", "orphan_ack")
TILE_RING_TEARDOWN_ZERO = ("members", "in_use")                  # RING_END after a clean teardown (non-fault)
# P4.8b I1, every point: member_cq() counts each consumed SSD CQE (cqes_consumed) and then exactly one of these
TILE_ENG_CQE = ("rec_cpl", "rec_err_parked", "rec_err_final", "rec_abort_req", "bad_cid")
# P4.8b I2, non-fault: every FLUSH / REISSUE / RELEASE descriptor the host writes is fetched once, by its live member
# (do_flush(), do_dispose(); their other outcomes count zero-required counters) or memberless (bad_route, stale_gen 0)
TILE_ENG_DISP = ("flushes", "releases", "reissues")
# P4.8b m3: engine families whose parsed lines must equal the lines naming them (A10: a format change FAILs)
TILE_ENG_RAW = (("IAU_ENGINE6_RING", re.compile(r"IAU_ENGINE6_RING\b")),
              ("IAU_ENGINE6_BIND", re.compile(r"IAU_ENGINE6_BIND\b")),
              ("IAU_ENGINE6_RING_END", re.compile(r"IAU_ENGINE6_RING_END\b")),
              ("IAU_ENGINE6_RELEASE", re.compile(r"IAU_ENGINE6_RELEASE\b")))
TILE_HOST_NONFAULT_ZERO = ("aborted", "timeouts", "ctrl", "rejoins")   # host STATS on a non-fault point (P4.12: rejoins)
# I1: the tile host knobs run on mode 6 only; SPDK_IAU_S2 (the P4.11 software fast path) on mode 0 only. Mirrors
# gen_inv_points.py TILE_ONLY and lib.sh rc_export_inv_env (the drift test compares them).
TILE_ONLY_KNOBS = ("SPDK_IAU_GROUP", "SPDK_IAU_FAST", "SPDK_IAU_RINGS", "SPDK_IAU_RING_ENTRIES", "SPDK_IAU_DESC_STRIDE",
                 "SPDK_IAU_RCONS", "SPDK_IAU_DB_BATCH")
TILE_S2_KNOB, TILE_S2_MODE = "SPDK_IAU_S2", 0
TILE_S2_WB_KNOB = "SPDK_IAU_S2_WB"   # P11 review fix M3: the S2 SQE in write-back stores; S2 points only
# IAU-next P4.12 NX_FAULT: perf's fault mode (spdk/app/spdk_nvme_perf/perf.c perf_fault_*, SPDK_PERF_FAULT=1). Its knobs
# in perf's order (g_perf_fault_knobs) with their PERF_FAULT_CONFIG keys, and the PERF_FAULT keys in print order; the
# drift test (tests/realcore/test_inv_check_l_p412.py) pins every list and regex here to perf.c.
PERF_FAULT_KNOBS = (("SPDK_PERF_FAULT_ABORT_EVERY", "abort_every"), ("SPDK_PERF_FAULT_TIMEOUT_US", "timeout_us"),
                    ("SPDK_PERF_FAULT_ABORT_ON_TIMEOUT", "abort_on_timeout"),
                    ("SPDK_PERF_FAULT_DISCONNECT_MS", "disconnect_ms"), ("SPDK_PERF_FAULT_RESET_MS", "reset_ms"))
PERF_FAULT_KEYS = ("submitted", "completed", "dup", "ok", "err", "err_dnr", "aborted_req", "aborted_sqdel", "other_err",
                   "timeouts", "abort_calls", "abort_cbs", "abort_rc_err", "disconnects", "inflight_end", "resets",
                   "reset_rc", "inval", "late", "bound_hits", "admin_timeouts", "poll_err", "admin_err")
PERF_FAULT_SHOW = ("ok", "err", "err_dnr", "aborted_req", "aborted_sqdel", "other_err", "inval", "timeouts",
                   "abort_calls", "abort_cbs", "disconnects", "resets", "dup", "inflight_end")   # the fault_stats column
PERF_FAULT_DRAIN_BOUND_MS = 10000   # perf.c PERF_FAULT_DRAIN_BOUND_MS (A20)
PERF_FAULT_REMOVED = "SPDK_PERF_FAULT_RAW_INVAL_EVERY"   # P4.12b: removed from perf (never reached the payload builder)
PERF_FAULT_CONFIG = re.compile(r"PERF_FAULT_CONFIG abort_every=(\d+) timeout_us=(\d+) abort_on_timeout=(\d+) "
                               r"disconnect_ms=(\d+) reset_ms=(\d+) drain_bound_ms=(\d+)")
PERF_FAULT_END = re.compile(r"PERF_FAULT lcore=(\d+)((?: \w+=-?\d+)*)")
PERF_FAULT_BOUND = re.compile(r"PERF_FAULT_DRAIN_BOUND lcore=(\d+) inflight=(\d+) bound_ms=(\d+)")
PERF_FAULT_ANY = re.compile(r"PERF_FAULT(?:_CONFIG|_DRAIN_BOUND)? ")
# a perf "completed with error" line is a counted outcome on a fault point (perf_fault_io_complete() -> io_complete())
FAULT_OK_BAD = ("completed with error",)
# I2: a fault point is SPDK_PERF_FAULT=1 AND perf's fault evidence line, PERF_FAULT_CONFIG (P4.12; None = before P4.12,
# when nothing read SPDK_PERF_FAULT and a point that set it FAILed)
TILE_PERF_FAULT_EVIDENCE = PERF_FAULT_CONFIG
# P4.12 NX_S2 (mode 0 + SPDK_IAU_S2=1, the P4.11 software fast path; spdk/lib/nvme/nvme_iau_s2.c, nvme_iau.h): the
# evidence lines of reports/p4_11.md section 8 (plan A25), drift-pinned to the C formats
S2_TRACKER_CIDS = 16                # NVME_IAU_S2_TRACKER_CIDS
S2_QSTATS = ("sub_fast", "sub_tr", "declined", "cpl", "err", "retries", "marked", "aborted", "timeouts", "leftovers",
             "rejoins", "aborts")   # NVME_IAU_S2_QSTATS (nvme_iau.h)
S2_STATS_TAIL = ("ended", "lost", "special")
# P4.11b (in flight while P4.12 was written, 2026-10-19: the P4.11 review's I1) adds a stray-CQE counter after aborts;
# check_l reads it when the line has it: 0 on every point (a stray is stock's "does not map", which FAILs as text too)
S2_OPTIONAL_ZERO = ("stray",)
S2_ZERO = ("lost", "special")       # every point
S2_ERR = ("err", "retries")         # > 0 only with expected errors (as TILE_HOST_ERR)
S2_NONFAULT_ZERO = ("marked", "aborted", "timeouts", "leftovers", "ended", "rejoins", "aborts")
S2_OPS = re.compile(r"IAU_S2_OPS s2=(\d+) tracker_cids=(\d+) pg=(\d+)")
S2_QPAIR = re.compile(r"IAU_S2_QPAIR qid=(\d+) fast_cids=(\d+) trackers=(\d+) nsid=(\d+) fast=(\d+) fast_nlb=(\d+) "
                      r"tmo_us=(\d+) pg=(-?\d+)(?: sqe=(\S+))?")   # sqe= from P11 (M3)
S2_STATS = re.compile(r"IAU_S2_STATS qid=(\d+) fast_cids=(\d+)\b(.*)$")
S2_PG_END = re.compile(r"IAU_S2_PG_END pg=(\d+) qid=(\d+) polls=(\d+) idle_polls=(\d+) sweeps=(\d+) attn_calls=(\d+)")
S2_BAD = re.compile(r"IAU_S2_(REFUSED|JOIN_REFUSED|RESET_REFUSED)\b")
S2_DEFERRED = re.compile(r"IAU_S2_(RESET_DEFERRED|RESET_DISABLE)\b")
S2_DISABLE = re.compile(r"IAU_S2_RESET_DISABLE qid=(\d+) done=(\d+)")   # A26: done=0 is the residual, a FAIL   # reset evidence: reset fault points only
S2_ANY = re.compile(r"IAU_S2_[A-Z0-9_]*[A-Z0-9]")
# P4.12 (A16 source refinement): work_fn_worker_group()'s time-based idle, one line per perf worker (perf.c
# perf_idle_time_print()); worker-group points from a perf before P4.12 have none and keep the poll-count source
# P4.12b: perf builds (host variants) from before P4.12 print no PERF_IDLE_TIME line; every later nx-* snapshot does, and a
# worker-group point of one without the line FAILs (keyed on the build, never on the line being absent). Non-nx-*
# variants (lean, inv-*, empty) all predate P4.12.
PERF_IDLE_PRE_P412_NX = ("nx-p1", "nx-p1b", "nx-p1c", "nx-p2", "nx-p3a", "nx-p4a", "nx-p4a2", "nx-p4a3", "nx-p4a4")
PERF_IDLE_TIME = re.compile(r"PERF_IDLE_TIME lcore=(\d+) idle_tsc=(\d+) window_tsc=(\d+) idle_time_frac=([0-9.]+)")
# P4.12: the tile host's reset evidence (nvme_iau.c), allowed on reset fault points only (SPDK_PERF_FAULT_RESET_MS)
TILE_RESET_ONLY = re.compile(r"IAU_TILE_(RESET_DEFERRED|RESET_DISABLE)\b")
# P4.10b (A19): the target ctx-spill emulation, mode 6 only (= target IAU_UNCORE_MODE=5 with tile queues); 0 / empty = off
TILE_SPILL_KNOB = "IAU_TILE_CTX_SPILL"
TILE_ENG_FAULT_EVERY = "IAU_ENG_FAULT_ERR_EVERY"                 # the engine knob (global only, b6_fault_load idx -1)
TILE_DEV_FAULT_EVERY = re.compile(r"^IAU_DEV_FAULT_ERR_EVERY(?:_C(\d+))?$")   # the device knob, _C<k> over the global
# host (nvme_iau.c, stderr / NVME_QPAIR_ERRLOG into the perf log)
TILE_OPS = re.compile(r"IAU_TILE_OPS tile=(\d+) stride=(\d+) rcons=(\d+) rings=(\d+) ring_entries=(\d+)(?: group=(\d+))?"
                    r"(?: db_batch=(\d+))?")
TILE_GROUP = re.compile(r"IAU_TILE_GROUP gid=(\d+) k=(\d+) entries=(\d+) stride=(\d+) private=(\d+)")
TILE_QPAIR = re.compile(r"IAU_TILE_QPAIR qid=(\d+) gid=(\d+) route=(\d+) ring=(\d+) gen=(\d+) credits=(\d+) nsid=(\d+) "
                      r"fast=(\d+) tmo_us=(\d+) private=(\d+)")
TILE_STATS = re.compile(r"IAU_TILE_STATS gid=(\d+) route=(\d+) qid=(\d+) gen=(\d+)\b(.*)$")
TILE_END = re.compile(r"IAU_TILE_GROUP_END gid=(\d+)\b(.*)$")
TILE_HOST_BAD = re.compile(r"IAU_TILE_(LOST|RESET_REFUSED|JOIN_REFUSED|REFUSED|CONFIG_ERROR)\b")
TILE_ANY_HOST = re.compile(r"IAU_TILE_[A-Z0-9_]*[A-Z0-9]")
# target (vfio_user.c, vfio_user_iau_tile.c: SPDK_NOTICELOG / SPDK_ERRLOG into the nvmf_tgt log)
ENG6_RING = re.compile(r"IAU_ENGINE6_RING gid=(\d+) ring=(\d+) nk=(\d+) engine=(\d+) lcore=(\d+) d_entries=(\d+) "
                       r"r_entries=(\d+) stride=(\d+)")
ENG6_BIND = re.compile(r"IAU_ENGINE6_BIND ctrlr=(-?\d+) qid=(\d+) gid=(\d+) route=(\d+) ring=(\d+) gen=(\d+) "
                       r"engine=(\d+) lcore=(\d+) nsid=(\d+) credits=(\d+) entries=(\d+) timeout_us=(\d+)")
ENG6_STATS = re.compile(r"IAU_ENGINE6_STATS ctrlr=(-?\d+) qid=(\d+) gid=(\d+) route=(\d+) ring=(\d+) engine=(\d+)\b(.*)$")
ENG6_END = re.compile(r"IAU_ENGINE6_RING_END gid=(\d+) ring=(\d+) engine=(\d+)\b(.*)$")
ENG6_PEAK = re.compile(r"IAU_ENGINE6_PEAK engine=(\d+) peak_slots=(\d+)")   # once per engine, at target shutdown
# P4.8b: a route's reservation released by the host's own Delete I/O CQ (sq_eng6_cq_deleted(), listed grings only)
ENG6_RELEASE = re.compile(r"IAU_ENGINE6_RELEASE ctrlr=(-?\d+) cqid=(\d+) gid=(\d+) ring=(\d+) route=(\d+) gen=(\d+) "
                          r"rc=(-?\d+)")
ENG6_BAD = re.compile(r"IAU_ENGINE6_(REFUSED|BIND_TIMEOUT)\b|IAU tile ring gid=\d+ ring=\d+: \d+ members (still bound at "
                      r"transport destroy)|IAU engine \d+ (stopped with) \d+ IAU tile rings")
ENG6_ANY = re.compile(r"IAU_ENGINE6_[A-Z0-9_]*[A-Z0-9]")
# P4.10b: one ON line at transport create, per member BIND (its A19 route block) and STATS (the emulated traffic)
SPILL_ON = re.compile(r"IAU_ENGINE6_CTX_SPILL on=(\d+) line=(\d+)")
SPILL_BIND = re.compile(r"IAU_ENGINE6_CTX_SPILL_BIND ctrlr=(-?\d+) qid=(\d+) gid=(\d+) route=(\d+) ring=(\d+) off=(\d+) "
                        r"slots=(\d+) block=(\d+) region=(\d+)")
SPILL_STATS = re.compile(r"IAU_ENGINE6_CTX_SPILL_STATS ctrlr=(-?\d+) qid=(\d+) gid=(\d+) route=(\d+) ring=(\d+) "
                         r"stores=(\d+) loads=(\d+)")
SPILL_REFUSED = re.compile(r"IAU_ENGINE6_CTX_SPILL_REFUSED\b")
SPILL_ANY = re.compile(r"IAU_ENGINE6_CTX_SPILL(?:_BIND|_STATS|_REFUSED)? ")
ENG6_FAULT = re.compile(r"IAU_ENG6_FAULT every=(\d+) status=(0x[0-9a-fA-F]+)")
DEV_FAULT = re.compile(r"DEV_FAULT ctrlr=(-?\d+) every=(\d+) status=(0x[0-9a-fA-F]+)")
DEV_FAULT_STATS = re.compile(r"DEV_FAULT_STATS ctrlr=(-?\d+) qid=(\d+) every=(\d+) marked=(\d+) posted=(\d+)")
# M2: a ring dropped (emit=false: its records end unconsumed) or a failed remap (vfio_user.c), a failed host Delete
# I/O CQ (nvme_iau.c): never on a clean teardown
ENG6_TEARDOWN = re.compile(r"IAU tile ring gid=\d+ ring=\d+: (?:.*; (dropping the ring) \(|(remap) to ctrlr=-?\d+ qid=\d+ failed)")
TILE_HOST_TEARDOWN = re.compile(r"IAU tile: Delete I/O CQ failed")
TILE_KV = re.compile(r"(\w+)=(\S+)")
# A16: perf's PCIE transport block (nvme_dump_pcie_statistics(), one per poll group): "\tpolls: N", "\tidle_polls: N"
POLLS_LINE = re.compile(r"^polls:\s+(\d+)$")
PCIE_KV = re.compile(r"^(\w+):\s+(\d+)$")
# m8: perf's interrupt mode (g_enable_interrupt, then delay_cmd_submit = 0, perf.c), parsed as getopt_long parses
# perf's argv: the short option 'E' (PERF_ENABLE_INTERRUPT), also bundled ("-GE"), and the long option or any unique
# abbreviation of it (getopt_long accepts a prefix that names one option: no other perf long option starts with
# "enable-i"); "--enable-interrupt=..." too (getopt_long refuses an argument to it, so perf would not run).
# PERF_GETOPT_SHORT and PERF_LONG_ARG (the long options that take an argument) are perf.c's, drift-pinned by
# test_inv_check_l_tile_group.py, so an option's separate argument is never read as options.
PERF_GETOPT_SHORT = "a:b:c:d:e:ghi:lmo:q:r:k:s:t:v:w:y:z:A:C:DEF:GHILM:NO:P:Q:RS:T:U:VZ:"
PERF_INTERRUPT_SHORT, PERF_INTERRUPT_LONG, PERF_INTERRUPT_MIN = "E", "enable-interrupt", "enable-i"
PERF_LONG_ARG = ("warmup-time", "allowed-pci-addr", "core-mask", "metadata", "shmem-grp-id", "io-size", "io-depth",
                 "transport", "keepalive", "hugemem-size", "time", "number-ios", "io-pattern", "disable-zcopy",
                 "buffer-alignment", "max-completion-per-poll", "zipf", "rwmixread", "io-unit-size", "num-qpairs",
                 "continue-on-error", "default-sock-impl", "logflag", "num-unused-qpairs", "enable-zcopy",
                 "iova-mode", "io-queue-size", "tls-version", "psk-path", "psk-identity ", "zerocopy-threshold",
                 "zerocopy-threshold-sock-impl", "transport-tos", "rdma-srq-size", "dhchap-key", "dhchap-ctrlr-key",
                 "vfio-vf-token", "log-level", "trace-file")
IDLE_POLLS_LINE = re.compile(r"^idle_polls:\s+(\d+)$")
HOST_BOUND_IDLE = 0.05   # A16: a host-cost claim needs the median idle_polls / polls <= this
EXTRAS0 = {"doorbells": "", "flush_idle": "", "err_records": "", "bad_sqhd": "", "tile_records": "", "host_idle_frac": "",
           "peak_slots": "", "group_idle_frac": "", "db_per_busy_poll": "", "idle_time_frac": "", "a16_src": "",
           "a16_bound": "", "fast_share": "", "fast_label": "", "fault_stats": ""}
# the output columns: the 16 before P4.12 unchanged, P4.12's appended
OUT_COLS = ["point_id", "arm", "cell", "mode", "verdict", "reason", "doorbells", "flush_idle", "read_pct", "err_records",
            "bad_sqhd", "tile_records", "host_idle_frac", "peak_slots", "group_idle_frac", "db_per_busy_poll",
            "idle_time_frac", "a16_src", "a16_bound", "fast_share", "fast_label", "fault_stats"]



FAST_DECLINE_TOL = 0.001    # P4.13-lite: declined (tracker / request-backed) share of a qpair's IOs tolerated with QD <= credits


def fast_rare(declined, total):
    """True when no IO was declined, or the declined share is within FAST_DECLINE_TOL."""
    return declined == 0 or (total > 0 and declined <= FAST_DECLINE_TOL * total)


def strtoll(v):
    m = re.match(r"\s*([+-]?\d+)", v or "")
    return int(m.group(1)) if m else 0


def cpu_count(s):
    n = 0
    for part in (s or "").replace("+", ",").split(","):
        if "-" in part:
            a, b = part.split("-")
            n += int(b) - int(a) + 1
        elif part.strip():
            n += 1
    return n


def cpu_list(s):
    out = []
    for part in (s or "").replace("+", ",").split(","):
        if "-" in part:
            a, b = part.split("-")
            out.extend(range(int(a), int(b) + 1))
        elif part.strip():
            out.append(int(part))
    return out


def dev_expect(env, idx, key, default):
    v = env.get("IAU_DEV_%s_C%d" % (key, idx), "") if idx >= 0 else ""
    if v == "":
        v = env.get("IAU_DEV_" + key, "")
    return str(strtoll(v)) if v != "" else default


def parse_env(s):
    d = {}
    for kv in (s or "").split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            d[k.strip()] = v.strip()
    return d


def stem(qd, run_id, lat):
    return "s4096_q%s_r%s%s" % (qd, run_id, "" if str(lat) == "1" else "_nolat")


def perf_interrupt(argv):
    """True if perf's argv (after the binary) selects interrupt mode, as getopt_long would parse it (m8)."""
    takes = set(c for i, c in enumerate(PERF_GETOPT_SHORT) if c != ":" and PERF_GETOPT_SHORT[i + 1:i + 2] == ":")
    i = 0
    while i < len(argv):
        t, i = argv[i], i + 1
        if t == "--":
            break
        if t.startswith("--"):
            name, eq, _ = t[2:].partition("=")
            if len(name) >= len(PERF_INTERRUPT_MIN) and PERF_INTERRUPT_LONG.startswith(name):
                return True
            hit = [n for n in PERF_LONG_ARG if n == name] or [n for n in PERF_LONG_ARG if n.startswith(name)]
            if hit and not eq:
                i += 1                                   # its separate argument
        elif t.startswith("-") and len(t) > 1:
            for j, c in enumerate(t[1:], 1):
                if c == PERF_INTERRUPT_SHORT:
                    return True
                if c in takes:
                    i += 1 if j == len(t) - 1 else 0     # the rest of the token, or the next one, is its argument
                    break
    return False


def check_cmd(path, p):
    """The perf argv recorded by run_point.sh must carry the point's workload knobs."""
    wl = p.get("workload") or "randread"
    mix = p.get("rwmix") or "100"
    size = p.get("io_size") or "4096"
    if not os.path.isfile(path):
        return "cmd file missing: %s" % path
    with open(path, errors="replace") as fh:
        argv = shlex.split(fh.read())
    idx = [i for i, t in enumerate(argv) if os.path.basename(t) == "spdk_nvme_perf"]
    if not idx:
        return "cmd argv mismatch: no spdk_nvme_perf"
    argv = argv[idx[0] + 1:]   # perf stat wrappers carry their own -o

    def val(flag):
        return argv[argv.index(flag) + 1] if flag in argv[:-1] else None
    bad = []
    for flag, want in (("-w", wl), ("-o", size)) + ((("-M", mix),) if wl == "randrw" else ()):
        if val(flag) != want:
            bad.append("%s %s (want %s)" % (flag, val(flag), want))
    r = "cmd argv mismatch: " + ", ".join(bad) if bad else ""
    if str(p.get("mode")) == "6" and perf_interrupt(argv):
        # P4.8b m8: there is no tile interrupt mode, and NX_TILE's doorbell bounds assume perf's delay_cmd_submit
        # (perf.c: delay_cmd_submit = !g_enable_interrupt): every submit would ring outside a poll
        r = "; ".join(x for x in (r, "NX_TILE perf interrupt mode (-E) on a mode 6 point: unsupported (no IAU tile "
                                     "interrupt mode; the doorbell bounds assume delay_cmd_submit)") if x)
    return r


def check_opc(tgt_log, p, mode):
    """P1.2: the target's per-opcode counts must match the point's workload (rows with a non-empty workload column
    only) and, on mode 4, the per-qid IAU_STATS fetched counts. Returns (reason, read_pct)."""
    wl = p.get("workload") or ""
    if wl == "":
        return "", ""
    if not tgt_log or not os.path.isfile(tgt_log):
        return "OPC_STATS target log missing: %s" % tgt_log, ""
    tl = read_lines(tgt_log)
    got = [m for m in (OPC_TGT.search(x) for x in tl) if m]
    if not got:
        return "OPC_STATS line missing in %s" % os.path.basename(tgt_log), ""
    r = sum(int(m.group(2)) for m in got)
    w = sum(int(m.group(3)) for m in got)
    reasons = []
    if mode == 4:
        opc_q, fet_q = {}, {}
        for m in got:
            opc_q[m.group(1)] = opc_q.get(m.group(1), 0) + int(m.group(2)) + int(m.group(3)) + int(m.group(4))
        for m in (TGT_FETCHED.search(x) for x in tl):
            if m:
                fet_q[m.group(1)] = fet_q.get(m.group(1), 0) + int(m.group(2))
        bad = [q for q in sorted(set(opc_q) | set(fet_q), key=int) if opc_q.get(q) != fet_q.get(q)]
        if bad:
            reasons.append("opc/fetched mismatch " + ",".join(
                "qid=%s(opc=%s fetched=%s)" % (q, opc_q.get(q, "none"), fet_q.get(q, "none")) for q in bad))
    if r + w == 0:
        return "; ".join(reasons + ["OPC_STATS reads+writes=0"]), ""
    pct = 100.0 * r / (r + w)
    if wl == "randread" and w > 0:
        reasons.append("OPC_STATS randread with writes=%d" % w)
    elif wl == "randwrite" and r > 0:
        reasons.append("OPC_STATS randwrite with reads=%d" % r)
    elif wl == "randrw":
        try:
            mix = float(p.get("rwmix") or "100")
        except ValueError:
            mix = None
        if mix is None or not math.isfinite(mix) or not 0.0 <= mix <= 100.0:
            mix = None
            reasons.append("rwmix not a number in [0, 100]: %r" % p.get("rwmix"))
        if mix is not None and abs(pct - mix) > 1.0:
            reasons.append("OPC_STATS read %.2f%% vs rwmix %g (tolerance 1.0)" % (pct, mix))
    return "; ".join(reasons), "%.2f" % pct


def read_lines(path):
    with open(path, errors="replace") as fh:
        return [line.strip() for line in fh]


def check_sizes(fam, evar, hlines, env, tl_lines, tgt_log):
    """Host and nvmf_tgt must agree on the record/descriptor size (23, 25)."""
    want = env.get(evar, "")
    if want == "":
        return []
    if not tgt_log or tl_lines is None:
        return ["%s target log missing: %s" % (fam, tgt_log)]
    rt = re.compile(r"%s target: env=(\S*) bytes=(\d+)\b" % fam)
    rh = re.compile(r"%s qid=\d+ bytes=(\d+)\b" % fam)
    tl = [m for m in (rt.search(x) for x in tl_lines) if m]
    if not tl:
        return ["%s target line missing in %s" % (fam, os.path.basename(tgt_log))]
    reasons = []
    tenv = sorted(set(m.group(1) for m in tl))
    tbytes = sorted(set(m.group(2) for m in tl))
    if tenv != [want]:
        reasons.append("%s target env=%s, point env=%s" % (fam, ",".join(tenv), want))
    if len(tbytes) != 1:
        reasons.append("%s target lines disagree bytes=%s" % (fam, ",".join(tbytes)))
    host = sorted(set(m.group(1) for m in (rh.search(x) for x in hlines) if m))
    bad = [b for b in host if b not in tbytes]
    if bad:
        reasons.append("%s size mismatch host bytes=%s target bytes=%s" % (fam, ",".join(bad), ",".join(tbytes)))
    return reasons


def check_idle(env, tl_lines, tgt_log):
    want = env.get("IAU_INV_IDLE_FLUSH", "")
    if want == "":
        found = [m for m in (IDLE_TGT.search(x) for x in (tl_lines or [])) if m]
        return ["INV_IDLE target line present but env unset"] if found else []
    if tl_lines is None:
        return ["INV_IDLE target log missing: %s" % tgt_log]
    tl = [m for m in (IDLE_TGT.search(x) for x in tl_lines) if m]
    if not tl:
        return ["INV_IDLE target line missing in %s" % os.path.basename(tgt_log)]
    reasons = []
    tenv = sorted(set(m.group(1) for m in tl))
    if tenv != [want]:
        reasons.append("INV_IDLE target env=%s, point env=%s" % (",".join(tenv), want))
    exp = "1" if strtoll(want) != 0 else "0"
    if any(m.group(2) != exp for m in tl):
        reasons.append("INV_IDLE enabled=%s expected %s" % (",".join(sorted(set(m.group(2) for m in tl))), exp))
    return reasons


def check_host_values(hlines, env, mode):
    reasons = []
    if EXPECT["INV_ADDR"](env, mode):
        bad = sorted(set(m.group(1) for m in (ADDR_HOST.search(x) for x in hlines) if m and m.group(1) != env["SPDK_INV_ADDR"]))
        if bad:
            reasons.append("INV_ADDR mode mismatch host=%s env=%s" % (",".join(bad), env["SPDK_INV_ADDR"]))
    if EXPECT["INV_DBSPIN"](env, mode):
        wn, wc = strtoll(env.get("SPDK_INV_DB_SPIN_NS", "")), strtoll(env.get("SPDK_INV_DB_SPIN_CYCLES", ""))
        bad = sorted(set("ns=%s,cycles=%s" % m.groups() for m in (SPIN_HOST.search(x) for x in hlines)
                         if m and (int(m.group(1)), int(m.group(2))) != (wn, wc)))
        if bad:
            reasons.append("INV_DBSPIN value mismatch host %s env ns=%d,cycles=%d" % ("|".join(bad), wn, wc))
    reasons += check_dbcost(hlines, env)
    if EXPECT["INV_PF"](env, mode):
        want = "trfree" if env.get("SPDK_INV_TRFREE") == "1" else "tr"
        bad = sorted(set(m.group(1) for m in (PF_HOST.search(x) for x in hlines) if m and m.group(1) != want))
        if bad:
            reasons.append("INV_PF loop mismatch host=%s expected %s" % (",".join(bad), want))
    return reasons


def check_dbcost(hlines, env):
    """P11 review fix M2, any mode: one MD_DBCOST line carrying the knobs iff SPDK_DB_COST_NS or SPDK_DB_COST_IAU_NS > 0
    (the C parses them with strtol; <= 0 is off); with both off no line (a lever that switches itself on FAILs)."""
    want = tuple(max(strtoll(env.get(k, "")), 0) for k in ("SPDK_DB_COST_NS", "SPDK_DB_COST_IAU_NS"))
    lines = [x for x in hlines if "MD_DBCOST" in x]
    if want == (0, 0):
        return ["MD_DBCOST lines %d without SPDK_DB_COST_NS / SPDK_DB_COST_IAU_NS > 0" % len(lines)] if lines else []
    if len(lines) != 1:
        return ["MD_DBCOST lines %d, expected 1 (SPDK_DB_COST_NS=%d SPDK_DB_COST_IAU_NS=%d)" % ((len(lines),) + want)]
    m = DBCOST.search(lines[0])
    if m is None or (int(m.group(1)), int(m.group(2))) != want:
        return ["MD_DBCOST host %r, env ssd_ns=%d iau_ns=%d" % ((lines[0].strip(),) + want)]
    return []


def expected_inv_flags(env, mode):
    """The IO-qpair inv_flags the host library computes at construct, mirroring the C order
    (30-l10-pf tree): 20 TRFREE, 22 THIN (an assignment: clears TRFREE; mode 0 = B-thin 0x02,
    mode 4 = I-thin 0x06, other modes stay stock), 23 REC (|=, set on thin qpairs too), 25 DESC
    (|=, only when the env is non-empty and 32|40|64), 27 ADDR (|=). Only mode 0 and mode 4 (Mode D)
    are modelled; Mode E bits are excluded in C and the harness never runs Mode E. Combinations that
    fail IO-qpair construct (ADDR with TRFREE/THIN or with DESC=32, REC!=32 on Mode E) are not
    modelled: such a point never reaches the checker with a log."""
    f = 0
    if mode == 4 and strtoll(env.get("SPDK_INV_TRFREE", "")) == 1:
        f |= 0x01
    if strtoll(env.get("SPDK_INV_THIN", "")) == 1 and mode in (0, 4):
        f = 0x02 | (0x04 if mode == 4 else 0)
    if mode == 4:
        f |= {16: 0x08, 32: 0x20, 64: 0x10}.get(strtoll(env.get("IAU_INV_REC_BYTES", "")), 0)
        d = env.get("IAU_INV_DESC_BYTES", "")
        if d != "" and strtoll(d) in (32, 40, 64):
            f |= 0x40
        if env.get("SPDK_INV_ADDR", "") in ("va", "vtophys"):
            f |= 0x80
    return f


def check_flags(hlines, env, mode, qpairs):  # qpairs = total IO qpairs (see check_log)
    """30-l10-pf guard. expected == 0: no INV_FLAGS line allowed. expected != 0: one line per IO
    qpair, each carrying exactly the expected flags."""
    exp = expected_inv_flags(env, mode)
    got = [(m.group(1), int(m.group(2), 16)) for m in (FLAGS_HOST.search(x) for x in hlines) if m]
    if exp == 0:
        if got:
            return ["unexpected lever bits: %s" % ",".join("qid=%s flags=0x%02x" % g for g in got)]
        return []
    reasons = []
    if len(got) != qpairs:
        reasons.append("INV_FLAGS lines %d, expected %d" % (len(got), qpairs))
    bad = sorted(set("0x%02x" % f for _, f in got if f != exp))
    if bad:
        reasons.append("INV_FLAGS %s != expected 0x%02x" % (",".join(bad), exp))
    return reasons


def check_gring(hlines, env, mode, tl_lines, tgt_log, groups):
    """29-l5-gring: the shared-ring lines beyond the per-qpair join count (checked as a family).
    groups = expected poll-group rings (n_ctx, see check_log)."""
    rings = [m for m in (GRING_RING.search(x) for x in hlines) if m]
    frees = [m for m in (GRING_FREE.search(x) for x in hlines) if m]
    joins = [m for m in (GRING_JOIN.search(x) for x in hlines) if m]
    tgt = [m for m in (GRING_TGT.search(x) for x in (tl_lines or [])) if m]
    if not EXPECT["INV_GRING"](env, mode):
        return ["INV_GRING ring/target lines present but not expected"] if (rings or frees or tgt) else []
    ev = env.get("IAU_INV_GLOBAL_RING_ENTRIES", "")
    want = str(strtoll(ev)) if ev != "" else "4096"
    reasons = []
    if len(rings) != groups:
        reasons.append("INV_GRING ring lines %d, expected %d" % (len(rings), groups))
    wmode = str(strtoll(env.get("IAU_INV_GLOBAL_RING", "")))
    modes = sorted(set(m.group(4) or "none" for m in rings))
    if modes and modes != [wmode]:
        reasons.append("INV_GRING ring mode %s, expected %s" % (",".join(modes), wmode))
    rptr = set(int(m.group(1), 16) for m in rings)
    if any(m.group(2) is None or int(m.group(2), 16) not in rptr for m in joins):
        reasons.append("INV_GRING join line with a ring that has no creation line")
    ents = sorted(set([m.group(3) for m in rings] + [m.group(1) for m in joins] + [m.group(2) for m in tgt]))
    if ents and ents != [want]:
        reasons.append("INV_GRING entries %s, expected %s" % (",".join(ents), want))
    hptr = sorted(int(m.group(1), 16) for m in rings)
    fptr = sorted(int(m.group(1), 16) for m in frees)
    if fptr != hptr:
        reasons.append("INV_GRING destroy lines %d for %d rings" % (len(frees), len(rings)))
    if any(int(m.group(2)) == 0 or int(m.group(3)) != 0 for m in frees):
        reasons.append("INV_GRING ring destroyed with consumed=0 or members left")
    if tl_lines is None:
        reasons.append("INV_GRING target log missing: %s" % tgt_log)
    elif not tgt:
        reasons.append("INV_GRING target line missing in %s" % os.path.basename(tgt_log))
    elif sorted(int(m.group(1), 16) for m in tgt) != sorted(int(m.group(2), 16) for m in rings):
        reasons.append("INV_GRING target iovas %s != host ring iovas %s" % (
            ",".join(sorted(m.group(1) for m in tgt)), ",".join(sorted(m.group(2) for m in rings))))
    if tl_lines is not None:
        # every record the device emitted (all Mode D SQs are gring SQs here) was consumed by a ring
        emitted = [int(m.group(1)) for m in (TGT_EMITTED.search(x) for x in tl_lines) if m]
        consumed = sum(int(m.group(2)) for m in frees)
        if not emitted:
            reasons.append("INV_GRING target IAU_STATS lines missing")
        elif sum(emitted) != consumed:
            reasons.append("INV_GRING host consumed %d != target emitted %d" % (consumed, sum(emitted)))
    return reasons


def check_md(hlines, env, mode, qpairs, expect_md=False):  # qpairs = total IO qpairs (see check_log)
    """F3: the Mode D host-path evidence, MD_TRFREE (default) or MD_TRACKERS (SPDK_MD_TRACKERS=1), one construct
    line per IO qpair on mode 4 and 5 points and none elsewhere. Any MD_ line present -> checked strictly. None present ->
    ok (a pre-F3 build), unless expect_md (--expect-md, post-F3 campaigns), where mode 4 and 5 then FAIL."""
    counts = dict((fam, 0) for fam in MD_EXPECT)
    for line in hlines:
        for m in MD_MARK.finditer(line):
            counts[m.group(1)] += 1
    if sum(counts.values()) == 0 and not expect_md:
        return []
    reasons = []
    for fam, pred in MD_EXPECT.items():
        want = qpairs if pred(env, mode) else 0
        if counts[fam] != want:
            reasons.append("%s construct lines %d, expected %d" % (fam, counts[fam], want))
    return reasons


def check_trace_fast(hlines, env, n_ctx):
    ent = [m.group(1) for m in (TF_LINE.search(x) for x in hlines) if m]
    want = n_ctx if env.get("SPDK_INV_TRACE_FAST") == "1" else 0
    reasons = []
    if len(ent) != want:
        reasons.append("INV_TRACE_FAST lines %d, expected %d" % (len(ent), want))
    if len(set(ent)) > 1:
        reasons.append("INV_TRACE_FAST entries differ: %s" % ",".join(sorted(set(ent))))
    return reasons


def bdev_fastpath(bdev):
    """The IAU_DEV_FASTPATH start_target.sh sets in the target env for --bdev (empty = its default, fastpath)."""
    return "0" if (bdev or "fastpath") in ("null", "delay") else "1"


def check_devmodel(env, tl, tgt_log, controllers, required, bdev="fastpath"):
    """P3 NX_DEVMODEL: one DEV_MODEL line per controller carrying the env's model (IAU_DEV_MODEL, default sq)
    and settings (IAU_DEV_<KEY>_C<k> over IAU_DEV_<KEY> over the defaults; the FASTPATH default is the point's
    bdev's, bdev_fastpath()). Present lines are always checked; `required` (a --p3-tgt variant, or a mode 5 or 6
    point) makes them mandatory."""
    lines = [m for m in (DEVMODEL.search(x) for x in (tl or [])) if m]
    if not lines:
        if not required:
            return []
        if tl is None:
            return ["DEV_MODEL target log missing: %s" % tgt_log]
        return ["DEV_MODEL lines 0, expected %d" % controllers]
    reasons = []
    if len(lines) != controllers:
        reasons.append("DEV_MODEL lines %d, expected %d" % (len(lines), controllers))
    idxs = sorted(int(m.group(1)) for m in lines)
    if idxs != list(range(controllers)):
        reasons.append("DEV_MODEL ctrlr indices %s, expected 0..%d once each" % (idxs, controllers - 1))
    want_model = env.get("IAU_DEV_MODEL") or "sq"
    for m in lines:
        idx = int(m.group(1))
        if m.group(2) != want_model:
            reasons.append("DEV_MODEL ctrlr=%d model=%s, expected %s" % (idx, m.group(2), want_model))
        for key, grp, default in DEV_KEYS:
            want = dev_expect(env, idx, key, bdev_fastpath(bdev) if key == "FASTPATH" else default)
            if m.group(grp) != want:
                reasons.append("DEV_MODEL ctrlr=%d %s=%s, expected %s" % (idx, key.lower(), m.group(grp), want))
    return reasons


def check_fpshare(env, tl, tgt_log, qpairs, controllers, bdev="fastpath"):
    """P3.8 FP_SHARE, qpairs = total IO qpairs. In the ctrlr device model every IO SQ of a controller shares one
    channel model, so its SSD thread must be one lcore: one FP_SHARE line per IO qpair of each controller whose fast
    path is on (dev_expect FASTPATH over the bdev's value, as check_devmodel; sq_fp_bind() prints nothing without the
    SQ's fast-path model), the (ctrlr, qid) set is qids 1..qpairs/controllers of each such controller, one lcore per
    controller, and users (the sharers counted at bind) are 1..n per controller. Any FP_SHARE line without
    IAU_DEV_MODEL=ctrlr FAILs."""
    lines = [m for m in (FP_SHARE.search(x) for x in (tl or [])) if m]
    if (env.get("IAU_DEV_MODEL") or "sq") != "ctrlr":
        return ["FP_SHARE lines without IAU_DEV_MODEL=ctrlr"] if lines else []
    if tl is None:
        return ["FP_SHARE target log missing: %s" % tgt_log]
    reasons = []
    fp_on = [c for c in range(controllers) if dev_expect(env, c, "FASTPATH", bdev_fastpath(bdev)) != "0"]
    if controllers <= 0 or qpairs % controllers:
        reasons.append("FP_SHARE IO qpairs %d not divisible over %d controllers" % (qpairs, controllers))
    else:
        per = qpairs // controllers
        if len(lines) != per * len(fp_on):
            reasons.append("FP_SHARE lines %d, expected %d (one per IO qpair of a fast-path controller)"
                           % (len(lines), per * len(fp_on)))
        want = sorted((c, q) for c in fp_on for q in range(1, per + 1))
        keys = sorted((int(m.group(1)), int(m.group(2))) for m in lines)
        if keys != want:
            dup = sorted(set(k for k in keys if keys.count(k) > 1))
            reasons.append("FP_SHARE (ctrlr,qid) set: duplicate %s missing %s unexpected %s"
                           % (dup, sorted(set(want) - set(keys)), sorted(set(keys) - set(want))))
    by = {}
    for m in lines:
        by.setdefault(int(m.group(1)), []).append(m)
    for c in sorted(by):
        lcores = sorted(set(int(m.group(3)) for m in by[c]))
        if len(lcores) != 1:
            reasons.append("FP_SHARE ctrlr=%d qids on lcores %s, expected one lcore" % (c, lcores))
        users = [int(m.group(4)) for m in by[c]]
        if sorted(users) != list(range(1, len(users) + 1)):
            reasons.append("FP_SHARE ctrlr=%d users %s, expected 1..%d" % (c, users, len(users)))
    return reasons


def check_engine(hlines, mode, tl, tgt_log, qpairs, engine_cpus, controllers=1, env=None, extras=None, bdev="fastpath"):
    """P3 NX_ENGINE: exactly-once evidence of the IAU-engine path (mode 5, arm I1e); qpairs = total IO qpairs.
    A P2.1 retry re-issue is just a new descriptor fetch, so per queue desc_fetched == sqes_written ==
    cqes_consumed == records_emitted holds with retries too; reissues= is still read but must be 0 (the engine cannot
    know). A duplicate or stray final completion per cid shows up as bad_cid. err_records (records with SCT/SC != 0)
    must be 0 per queue unless the point's env sets SPDK_INV_EXPECT_RETRIES=1, and with the flag the point total must
    be > 0 (the retry path has to have run). bad_sqhd (out-of-range sqhd in a CQE) must be 0. Coverage: the BIND and
    STATS (ctrlr, qid) sets must each equal the IO-qpair set (qpairs / controllers qids 1.. per controller), and
    engine_cpus must be non-empty with at least one IAU_ENGINE_START; BIND engine/lcore must match START and
    engine_cpus; STATS engine must match its BIND; the total records_emitted must be > 0."""
    env = env or {}
    binds = [m for m in (ENG_BIND.search(x) for x in (tl or [])) if m]
    stats = [m for m in (ENG_STATS.search(x) for x in (tl or [])) if m]
    starts = [m for m in (ENG_START.search(x) for x in (tl or [])) if m]
    mdeng = [m for x in hlines for m in MD_ENGINE.finditer(x)]
    if mode != 5:
        present = [n for n, v in (("MD_ENGINE", mdeng), ("IAU_ENGINE_BIND", binds), ("IAU_ENGINE_STATS", stats)) if v]
        return ["%s lines on a mode %d point" % (",".join(present), mode)] if present else []
    if tl is None:
        return ["NX_ENGINE target log missing: %s" % tgt_log]
    reasons = []
    for name, got in (("MD_ENGINE", mdeng), ("IAU_ENGINE_BIND", binds), ("IAU_ENGINE_STATS", stats)):
        if len(got) != qpairs:
            reasons.append("NX_ENGINE %s lines %d, expected %d" % (name, len(got), qpairs))
    want_cpus = sorted(cpu_list(engine_cpus))
    got_cpus = sorted(int(m.group(2)) for m in starts)
    if not want_cpus:
        reasons.append("NX_ENGINE mode 5 point has empty engine_cpus")
    if not starts:
        reasons.append("NX_ENGINE no IAU_ENGINE_START line")
    if got_cpus != want_cpus or any(m.group(2) != m.group(3) for m in starts):
        reasons.append("NX_ENGINE engine threads on lcores %s, expected %s" % (got_cpus, want_cpus))
    start_map = dict((m.group(1), m.group(2)) for m in starts)
    for m in binds:
        if start_map.get(m.group(3)) != m.group(4) or int(m.group(4)) not in want_cpus:
            reasons.append("NX_ENGINE BIND ctrlr=%s qid=%s engine=%s lcore=%s not a started engine thread on engine_cpus"
                           % m.group(1, 2, 3, 4))
    per = qpairs // controllers if controllers > 0 and qpairs % controllers == 0 else None
    if per is None:
        reasons.append("NX_ENGINE IO qpairs %d not divisible over %d controllers" % (qpairs, controllers))
    else:
        want_set = sorted((str(c), str(q)) for c in range(controllers) for q in range(1, per + 1))
        for name, got in (("BIND", binds), ("STATS", stats)):
            keys = sorted((m.group(1), m.group(2)) for m in got)
            if keys != want_set:
                dup = sorted(set(k for k in keys if keys.count(k) > 1))
                miss = sorted(set(want_set) - set(keys))
                extra = sorted(set(keys) - set(want_set))
                reasons.append("NX_ENGINE %s (ctrlr,qid) set: duplicate %s missing %s unexpected %s" % (name, dup, miss, extra))
    bind_eng = dict(((m.group(1), m.group(2)), m.group(3)) for m in binds)
    if sorted(m.group(3) for m in mdeng) != sorted(m.group(6) for m in binds):
        reasons.append("NX_ENGINE host credits %s != bound credits %s"
                       % (sorted(m.group(3) for m in mdeng), sorted(m.group(6) for m in binds)))
    if any(TGT_FETCHED.search(x) for x in tl):
        reasons.append("NX_ENGINE legacy IAU_STATS lines on a mode 5 point")
    opc = {}
    for m in (OPC_P3.search(x) for x in tl):
        if m:
            opc[(m.group(5), m.group(1))] = (int(m.group(2)) + int(m.group(3)) + int(m.group(4)),
                                             int(m.group(6)), int(m.group(4)))
    emitted = 0
    err_total = bad_sqhd_total = 0
    expect_retries = env.get("SPDK_INV_EXPECT_RETRIES") == "1"
    for m in stats:
        kv = dict((k, int(v)) for k, v in NUMKV.findall(m.group(4)))
        key = (m.group(1), m.group(2))
        tag = "ctrlr=%s qid=%s" % key
        missing = [k for k in ENG_NEED if k not in kv]
        if missing:
            reasons.append("NX_ENGINE %s missing %s" % (tag, ",".join(missing)))
            continue
        if kv.get("reissues", 0) != 0:
            reasons.append("NX_ENGINE %s reissues=%d, engine reports re-issues it cannot know" % (tag, kv["reissues"]))
        err_total += kv["err_records"]
        bad_sqhd_total += kv["bad_sqhd"]
        if kv["err_records"] and not expect_retries:
            reasons.append("NX_ENGINE %s err_records=%d without SPDK_INV_EXPECT_RETRIES=1" % (tag, kv["err_records"]))
        if key in bind_eng and bind_eng[key] != m.group(3):
            reasons.append("NX_ENGINE %s STATS engine=%s != BIND engine=%s" % (tag, m.group(3), bind_eng[key]))
        seq = (kv["desc_fetched"], kv["sqes_written"], kv["cqes_consumed"], kv["records_emitted"])
        if len(set(seq)) != 1:
            reasons.append("NX_ENGINE %s fetched/sqes/cqes/records %d/%d/%d/%d" % ((tag,) + seq))
        for k in ("inflight", "bad_cid", "bad_db", "host_ssd_db", "bad_sqhd"):
            if kv[k] != 0:
                reasons.append("NX_ENGINE %s %s=%d" % (tag, k, kv[k]))
        if kv["records_emitted"] > 0 and (kv["ssd_db_writes"] == 0 or kv["cq_db_writes"] == 0):
            reasons.append("NX_ENGINE %s IOs without SSD doorbell writes" % tag)
        o = opc.get(key)
        if o is None:
            reasons.append("NX_ENGINE %s has no OPC_STATS line with ctrlr= and fp_done=" % tag)
        else:
            if o[0] != kv["sqes_written"]:
                reasons.append("NX_ENGINE %s SSD fetched %d != engine sqes %d" % (tag, o[0], kv["sqes_written"]))
            # M3: fp_done counts fast-path completions only (0 without the fast path, bdev null|delay)
            if dev_expect(env, int(m.group(1)), "FASTPATH", bdev_fastpath(bdev)) != "0" and o[1] + o[2] != kv["cqes_consumed"]:
                reasons.append("NX_ENGINE %s SSD posted %d != engine cqes %d" % (tag, o[1] + o[2], kv["cqes_consumed"]))
        emitted += kv["records_emitted"]
    if extras is not None:
        extras["err_records"] = str(err_total)
        extras["bad_sqhd"] = str(bad_sqhd_total)
    if expect_retries and err_total == 0:
        reasons.append("NX_ENGINE SPDK_INV_EXPECT_RETRIES=1 but err_records total 0 (retry path did not run)")
    if emitted == 0:
        reasons.append("NX_ENGINE total records_emitted 0 (zero-IO point)")
    host = sum(int(m.group(1)) for m in (PCIE_STATS.search(x) for x in hlines) if m)
    if host != emitted:
        reasons.append("NX_ENGINE host consumed %d != target emitted %d" % (host, emitted))
    return reasons


def tile_kv(tail):
    """key=value pairs of an evidence line's tail: int for a decimal value, else the text."""
    return dict((k, int(v) if v.isdigit() else v) for k, v in TILE_KV.findall(tail or ""))


def tile_missing(kv, keys, text=()):
    """Required keys absent (or not a number): a renamed key FAILs instead of silently passing (A10)."""
    return [k for k in keys if not isinstance(kv.get(k), int)] + [k for k in text if k not in kv]


def tile_knob_misuse(env, mode):
    """I1: the env keys that do not belong to the point's mode (v6 host knobs off mode 6, SPDK_IAU_S2 off mode 0)."""
    bad = [k for k in TILE_ONLY_KNOBS if k in env and mode != 6]
    bad += [TILE_SPILL_KNOB] if tile_spill_on(env) and mode != 6 else []
    return sorted(bad + ([TILE_S2_KNOB] if TILE_S2_KNOB in env and mode != TILE_S2_MODE else []))


def tile_spill_on(env):
    """P4.10b: IAU_TILE_CTX_SPILL=1 (unset, empty and 0 are off; the target refuses any other value)."""
    return env.get(TILE_SPILL_KNOB, "") == "1"


def tile_spill_evidence(env, tl, mems, binds, fault=False):
    """P4.10b (A19): the ctx-spill knob and its evidence. On (IAU_TILE_CTX_SPILL=1): exactly one ON line with line=64, no
    CTX_SPILL_REFUSED, one CTX_SPILL_BIND and one CTX_SPILL_STATS per member (the (ctrlr,qid) set of IAU_ENGINE6_BIND);
    every block is the A19 route block of its slots (block == 256-rounded 64 s + 256 s, off a multiple of 256, inside
    the region) and the live blocks of one gid do not overlap (P4.12, the P4.10a/b review's m11: in target-log order a
    block is live from its CTX_SPILL_BIND until its route's reservation ends, an IAU_ENGINE6_RELEASE rc=0 of the route
    (iau_tile_ctx_free()) or its ring's IAU_ENGINE6_RING_END (gring_unlist() -> iau_tile_ctx_ring_free()); a route's own rebind,
    a rejoin, replaces its block); per member incarnation (the i-th CTX_SPILL_STATS of a (ctrlr,qid) with its i-th
    IAU_ENGINE6_STATS, m11) stores == sqes_written - reissues (one 64 B store per fetch) and loads == cqes_consumed -
    bad_cid (one 64 B load per completion; on a fault point <=, m12: the detach drain's member_cq() is the knob-off copy,
    so CQEs consumed there with IO still in flight load nothing); the totals are > 0. Off: no CTX_SPILL line of any kind
    (the knob was on silently); a value other than 0 / 1 FAILs. mems: the members' (IAU_ENGINE6_STATS match, kv) pairs
    in log order; binds: the IAU_ENGINE6_BIND matches."""
    v = env.get(TILE_SPILL_KNOB, "")
    r = []
    if v not in ("", "0", "1"):
        r.append("NX_TILE %s=%s is not 0 or 1" % (TILE_SPILL_KNOB, v))
    anyl = [x for x in tl if SPILL_ANY.search(x)]
    if not tile_spill_on(env):
        if anyl:
            r.append("NX_TILE %d IAU_ENGINE6_CTX_SPILL line(s) without %s=1 (the knob was on silently)" % (len(anyl), TILE_SPILL_KNOB))
        return r
    on = [m for x in tl for m in [SPILL_ON.search(x)] if m]
    if len(on) != 1 or on[0].group(1) != "1" or on[0].group(2) != "64":
        r.append("NX_TILE %s=1: IAU_ENGINE6_CTX_SPILL on=1 line=64 lines: %s, expected exactly 1" % (TILE_SPILL_KNOB, [m.group(0) for m in on]))
    ref = [x for x in tl if SPILL_REFUSED.search(x)]
    if ref:
        r.append("NX_TILE %s=1: IAU_ENGINE6_CTX_SPILL_REFUSED x%d (the GCB's ctx region cannot serve the bind)" % (TILE_SPILL_KNOB, len(ref)))
    sb = [m for x in tl for m in [SPILL_BIND.search(x)] if m]
    ss = [m for x in tl for m in [SPILL_STATS.search(x)] if m]
    want = sorted(m.group(1, 2) for m in binds)
    for name, got in (("IAU_ENGINE6_CTX_SPILL_BIND", sb), ("IAU_ENGINE6_CTX_SPILL_STATS", ss)):
        keys = sorted(m.group(1, 2) for m in got)
        if keys != want:
            r.append("NX_TILE %s (ctrlr,qid) set %s != IAU_ENGINE6_BIND set %s" % (name, keys, want))
    live = {}       # gid -> {route: (off, end, qid, ring)}: the blocks held at this point of the target log
    for x in tl:
        m = SPILL_BIND.search(x)
        if m:
            off, slots, block, region = (int(m.group(i)) for i in (6, 7, 8, 9))
            lines = 64 * slots
            want_block = ((lines + 255) // 256) * 256 + 256 * slots
            if slots < 1 or block != want_block or off % 256 != 0 or off + block > region:
                r.append("NX_TILE CTX_SPILL_BIND ctrlr=%s qid=%s off=%d slots=%d block=%d region=%d is not an A19 route "
                         "block inside the region (block %d, off a multiple of 256)"
                         % (m.group(1), m.group(2), off, slots, block, region, want_block))
            held = live.setdefault(m.group(3), {})
            for rt, (a, b, q, _) in sorted(held.items(), key=lambda kv: kv[1][0]):
                if rt != m.group(4) and a < off + block and off < b:
                    r.append("NX_TILE CTX_SPILL blocks of gid=%s overlap: qid %s [%d,%d) and qid %s [%d,%d)"
                             % (m.group(3), q, a, b, m.group(2), off, off + block))
            held[m.group(4)] = (off, off + block, m.group(2), m.group(5))
            continue
        m = ENG6_RELEASE.search(x)
        if m and m.group(7) == "0":
            live.get(m.group(3), {}).pop(m.group(5), None)
            continue
        m = ENG6_END.search(x)
        if m:
            held = live.get(m.group(1), {})
            for rt in [rt for rt, v in held.items() if v[3] == m.group(2)]:
                del held[rt]
    kvs = {}
    for m, kv in mems:
        kvs.setdefault(m.group(1, 2), []).append(kv)
    seen = {}
    for m in ss:
        i = seen[m.group(1, 2)] = seen.get(m.group(1, 2), -1) + 1
        lst = kvs.get(m.group(1, 2), [])
        kv = lst[i] if i < len(lst) else None
        if kv is None:
            continue
        stores, loads = int(m.group(6)), int(m.group(7))
        if stores != kv["sqes_written"] - kv["reissues"]:
            r.append("NX_TILE ctrlr=%s qid=%s CTX_SPILL stores %d != sqes_written - reissues %d" % (m.group(1), m.group(2), stores, kv["sqes_written"] - kv["reissues"]))
        want = kv["cqes_consumed"] - kv["bad_cid"]
        if loads != want and not (fault and loads < want):
            r.append("NX_TILE ctrlr=%s qid=%s CTX_SPILL loads %d %s cqes_consumed - bad_cid %d" % (
                m.group(1), m.group(2), loads, "!=" if not fault else ">", want))
    if ss and (sum(int(m.group(6)) for m in ss) == 0 or sum(int(m.group(7)) for m in ss) == 0):
        r.append("NX_TILE %s=1 but CTX_SPILL stores or loads total 0" % TILE_SPILL_KNOB)
    return r


def tile_perf_fault_evidence(hlines):
    return TILE_PERF_FAULT_EVIDENCE is not None and any(TILE_PERF_FAULT_EVIDENCE.search(x) for x in hlines)


def tile_fault_point(env, hlines):
    """I2: a fault point (the P4.12 perf fault mode) = SPDK_PERF_FAULT=1 AND perf's fault evidence line; only there
    may timeouts, aborts, acks, drops, orphans, stalls and the teardown leftovers occur."""
    return env.get("SPDK_PERF_FAULT") == "1" and tile_perf_fault_evidence(hlines)


def check_perf_fault(env, hlines):
    """I2, any mode: SPDK_PERF_FAULT=1 FAILs before P4.12 (TILE_PERF_FAULT_EVIDENCE unset); afterwards the env and
    perf's evidence line must come together."""
    on = env.get("SPDK_PERF_FAULT") == "1"
    if TILE_PERF_FAULT_EVIDENCE is None:
        return ["NX_TILE SPDK_PERF_FAULT before P4.12 (nothing reads it yet)"] if on else []
    ev = tile_perf_fault_evidence(hlines)
    if on and not ev:
        return ["NX_TILE SPDK_PERF_FAULT=1 without perf's fault evidence line"]
    if ev and not on:
        return ["NX_TILE perf fault evidence line without SPDK_PERF_FAULT=1"]
    return []


def fault_kv(tail):
    """key=value pairs of a PERF_FAULT line's tail, every value an int (reset_rc may be negative)."""
    return dict((k, int(v)) for k, v in re.findall(r"(\w+)=(-?\d+)", tail or ""))


def fault_err_knobs(env, controllers=1):
    """The target error knobs a point runs, as [(name, dnr)] for each with every > 0: the engine knob
    IAU_ENG_FAULT_ERR_{EVERY,DNR} (global) and the device knob IAU_DEV_FAULT_ERR_{EVERY,DNR}[_C<k>] per controller
    (_C<k> over the global for every key, b6_fault_load())."""
    out = []
    if strtoll(env.get(TILE_ENG_FAULT_EVERY, "")) > 0:
        out.append(("IAU_ENG_FAULT_ERR", strtoll(env.get("IAU_ENG_FAULT_ERR_DNR", ""))))
    for c in range(max(1, controllers)):
        ev = env.get("IAU_DEV_FAULT_ERR_EVERY_C%d" % c, "") or env.get("IAU_DEV_FAULT_ERR_EVERY", "")
        if strtoll(ev) > 0:
            dnr = env.get("IAU_DEV_FAULT_ERR_DNR_C%d" % c, "") or env.get("IAU_DEV_FAULT_ERR_DNR", "")
            out.append(("IAU_DEV_FAULT_ERR_C%d" % c, strtoll(dnr)))
    return out


def fault_abort_capable(env, mode):
    """F1: an abort by cookie (spdk_nvme_ctrlr_cmd_abort_ext(), SPDK_PERF_FAULT_ABORT_EVERY) needs the host's
    iterate_requests, which stock vfio-user has not: the IAU tile host (mode 6) and S2 (mode 0 + SPDK_IAU_S2=1) have."""
    return mode == 6 or (mode == TILE_S2_MODE and env.get(TILE_S2_KNOB) == "1")


def check_fault(hlines, env, mode, controllers=1, extras=None):
    """IAU-next P4.12 NX_FAULT, any mode: perf's fault mode (SPDK_PERF_FAULT=1, perf.c perf_fault_*). Without it: no
    SPDK_PERF_FAULT_* knob (perf refuses one) and no PERF_FAULT* line; a value other than 0 / 1 FAILs. A fault point
    (the env and PERF_FAULT_CONFIG, check_perf_fault()): one PERF_FAULT_CONFIG carrying the env's knobs (unset = 0) and
    drain_bound_ms PERF_FAULT_DRAIN_BOUND_MS; one PERF_FAULT line with every key; dup 0, inflight_end 0, submitted ==
    completed > 0, no PERF_FAULT_DRAIN_BOUND line and bound_hits 0 (A20: the exit drain bound hit, its IO abandoned);
    per knob, its evidence and nothing of an unset one: ABORT_EVERY: abort_calls > 0 and abort_cbs == abort_calls, and
    only where an abort by cookie exists (fault_abort_capable(), F1); ABORT_ON_TIMEOUT likewise; neither: no abort
    call or callback; TIMEOUT_US: timeouts > 0, else no timeout (IO or admin); DISCONNECT_MS: disconnects 1, else 0;
    RESET_MS: resets 1 with reset_rc 0, else 0; SPDK_PERF_FAULT_RAW_INVAL_EVERY (removed, P4.12b) FAILs; a
    target error knob (fault_err_knobs()) with dnr 1: err_dnr > 0, with dnr 0: err 0 (every error retried); statuses
    (P4.12b I2): err > 0 needs a DNR 0 error knob, err_dnr > 0 a DNR 1 one, other_err always FAILs, inval > 0 needs an
    error knob; ABORT_ON_TIMEOUT needs aborted_req > 0 (I3, T3); no knob that can end an IO in error (aborts, disconnect,
    reset, an error knob): ok == completed. abort_rc_err, late, admin_timeouts, poll_err and admin_err are reported
    (fault_stats), not judged. extras
    fault_stats: PERF_FAULT_SHOW's counters."""
    r = []
    v = env.get("SPDK_PERF_FAULT", "")
    if v not in ("", "0", "1"):
        r.append("NX_FAULT SPDK_PERF_FAULT=%s is not 0 or 1 (perf refuses it)" % v)
    knobs = dict((k, strtoll(env.get(k, ""))) for k, _ in PERF_FAULT_KNOBS)
    if env.get(PERF_FAULT_REMOVED, "") != "":
        r.append("NX_FAULT %s is removed (P4.12b: the ns_cmd SGL split never reaches the payload builder)"
                 % PERF_FAULT_REMOVED)
    cfgs = [m for x in hlines for m in [PERF_FAULT_CONFIG.search(x)] if m]
    ends = [m for x in hlines for m in [PERF_FAULT_END.search(x)] if m]
    bounds = [m for x in hlines for m in [PERF_FAULT_BOUND.search(x)] if m]
    if v != "1":
        on = sorted(k for k, _ in PERF_FAULT_KNOBS if env.get(k, "") != "")
        if on:
            r.append("NX_FAULT %s without SPDK_PERF_FAULT=1 (perf refuses them)" % ",".join(on))
        n = sum(1 for x in hlines if PERF_FAULT_ANY.search(x))
        if n:
            r.append("NX_FAULT %d perf fault-mode line(s) without SPDK_PERF_FAULT=1" % n)
        return r
    if not cfgs:
        return r        # check_perf_fault() FAILs the point: the fault mode did not run
    if len(cfgs) != 1:
        r.append("NX_FAULT PERF_FAULT_CONFIG lines %d, expected 1" % len(cfgs))
    for i, (name, key) in enumerate(PERF_FAULT_KNOBS):
        if int(cfgs[0].group(i + 1)) != knobs[name]:
            r.append("NX_FAULT PERF_FAULT_CONFIG %s=%s, expected %d (%s)" % (key, cfgs[0].group(i + 1), knobs[name],
                                                                              name))
    if int(cfgs[0].group(6)) != PERF_FAULT_DRAIN_BOUND_MS:
        r.append("NX_FAULT PERF_FAULT_CONFIG drain_bound_ms=%s, expected %d" % (cfgs[0].group(6),
                                                                                PERF_FAULT_DRAIN_BOUND_MS))
    if len(ends) != 1:
        return r + ["NX_FAULT PERF_FAULT lines %d, expected 1 (printed once after the workers joined)" % len(ends)]
    kv = fault_kv(ends[0].group(2))
    miss = [k for k in PERF_FAULT_KEYS if k not in kv]
    if miss:
        return r + ["NX_FAULT PERF_FAULT missing %s" % ",".join(miss)]
    if extras is not None:
        extras["fault_stats"] = " ".join("%s=%d" % (k, kv[k]) for k in PERF_FAULT_SHOW)
    if kv["dup"]:
        r.append("NX_FAULT dup=%d (an IO called back more than once)" % kv["dup"])
    if kv["inflight_end"]:
        r.append("NX_FAULT inflight_end=%d (IO without a callback at exit)" % kv["inflight_end"])
    if kv["submitted"] != kv["completed"]:
        r.append("NX_FAULT submitted %d != completed %d" % (kv["submitted"], kv["completed"]))
    if kv["submitted"] == 0:
        r.append("NX_FAULT submitted 0 (zero-IO fault point)")
    if bounds or kv["bound_hits"]:
        r.append("NX_FAULT exit drain bound hit (A20): %d ns_ctx abandoned %d IO after %d ms (bound_hits=%d, late=%d)"
                 % (len(bounds), sum(int(m.group(2)) for m in bounds), PERF_FAULT_DRAIN_BOUND_MS, kv["bound_hits"],
                    kv["late"]))
    aborts = knobs["SPDK_PERF_FAULT_ABORT_EVERY"] > 0 or knobs["SPDK_PERF_FAULT_ABORT_ON_TIMEOUT"] > 0
    if knobs["SPDK_PERF_FAULT_ABORT_EVERY"] > 0 and not fault_abort_capable(env, mode):
        r.append("NX_FAULT SPDK_PERF_FAULT_ABORT_EVERY on a mode %d point without SPDK_IAU_S2=1 (F1: stock vfio-user "
                 "has no iterate_requests, an abort by cookie needs the IAU tile or the S2 host)" % mode)
    if aborts:
        if kv["abort_calls"] == 0:
            r.append("NX_FAULT abort knob set but abort_calls 0 (the lever did not run)")
        if kv["abort_cbs"] != kv["abort_calls"]:
            r.append("NX_FAULT abort_cbs %d != abort_calls %d" % (kv["abort_cbs"], kv["abort_calls"]))
    elif kv["abort_calls"] or kv["abort_cbs"] or kv["abort_rc_err"]:
        r.append("NX_FAULT abort_calls=%d abort_cbs=%d abort_rc_err=%d without an abort knob" % (
            kv["abort_calls"], kv["abort_cbs"], kv["abort_rc_err"]))
    if knobs["SPDK_PERF_FAULT_TIMEOUT_US"] > 0:
        if kv["timeouts"] == 0:
            r.append("NX_FAULT SPDK_PERF_FAULT_TIMEOUT_US=%d but timeouts 0" % knobs["SPDK_PERF_FAULT_TIMEOUT_US"])
    elif kv["timeouts"] or kv["admin_timeouts"]:
        r.append("NX_FAULT timeouts=%d admin_timeouts=%d without SPDK_PERF_FAULT_TIMEOUT_US" % (kv["timeouts"],
                                                                                              kv["admin_timeouts"]))
    want = 1 if knobs["SPDK_PERF_FAULT_DISCONNECT_MS"] > 0 else 0
    if kv["disconnects"] != want:
        r.append("NX_FAULT disconnects=%d, expected %d (SPDK_PERF_FAULT_DISCONNECT_MS)" % (kv["disconnects"], want))
    want = 1 if knobs["SPDK_PERF_FAULT_RESET_MS"] > 0 else 0
    if kv["resets"] != want:
        r.append("NX_FAULT resets=%d, expected %d (SPDK_PERF_FAULT_RESET_MS)" % (kv["resets"], want))
    if want and kv["reset_rc"] != 0:
        r.append("NX_FAULT reset_rc=%d (spdk_nvme_ctrlr_reset() failed)" % kv["reset_rc"])
    errk = fault_err_knobs(env, controllers)
    for name, dnr in errk:
        if dnr and kv["err_dnr"] == 0:
            r.append("NX_FAULT %s_DNR=1 but err_dnr 0" % name)
        if not dnr and kv["err"]:
            r.append("NX_FAULT %s_DNR=0 but err=%d (every error should have been retried)" % (name, kv["err"]))
    # P4.12b I2: a status is explained by a knob or it FAILs. err (dnr 0) is retried by the library, other_err (a path or
    # vendor status) and inval (INVALID_FIELD, which only a target error knob can send) are never expected, err_dnr
    # needs a target error knob with DNR 1; aborted_* are the abort / disconnect / reset outcomes
    dnr1 = any(d for _, d in errk)
    if kv["err"] and not any(not d for _, d in errk):
        r.append("NX_FAULT err=%d (DNR 0) without a target error knob with DNR 0" % kv["err"])
    if kv["err_dnr"] and not dnr1:
        r.append("NX_FAULT err_dnr=%d without a target error knob with DNR 1" % kv["err_dnr"])
    if kv["other_err"]:
        r.append("NX_FAULT other_err=%d (a path or vendor status no knob explains)" % kv["other_err"])
    if kv["inval"] and not errk:
        r.append("NX_FAULT inval=%d (INVALID_FIELD) without a target error knob" % kv["inval"])
    # P4.12b I3 (the P4.13 table, T3): an abort by cid on a timeout ends the IO ABORTED_BY_REQUEST
    if knobs["SPDK_PERF_FAULT_ABORT_ON_TIMEOUT"] > 0 and kv["aborted_req"] == 0:
        r.append("NX_FAULT SPDK_PERF_FAULT_ABORT_ON_TIMEOUT=1 but aborted_req 0 (no timed-out IO was aborted)")
    if not (aborts or errk or knobs["SPDK_PERF_FAULT_DISCONNECT_MS"] or knobs["SPDK_PERF_FAULT_RESET_MS"]) \
            and kv["ok"] != kv["completed"]:
        r.append("NX_FAULT ok %d != completed %d without a knob that can end an IO in error" % (kv["ok"],
                                                                                            kv["completed"]))
    return r


def qd_per_qpair(qd, qpairs_ctx):
    """perf's nominal queue depth per IO qpair: -q (per ns_ctx) spread round robin over the ns_ctx's -P qpairs
    (perf.c nvme_submit_io()), rounded up; None when unknown."""
    try:
        qd, q = int(qd), int(qpairs_ctx)
    except (TypeError, ValueError):
        return None
    return -(-qd // q) if qd > 0 and q > 0 else None


def check_s2(hlines, env, mode, qpairs, tl=None, controllers=1, qd_q=None, extras=None):
    """IAU-next P4.12 NX_S2 (reports/p4_11.md section 8, plan A25). Any point: an IAU_S2_ line without SPDK_IAU_S2=1 on
    mode 0 FAILs. An S2 point (mode 0, SPDK_IAU_S2=1; qpairs = the IO qpairs): one IAU_S2_OPS s2=1 tracker_cids=16 pg=1
    (the IAU_TILE_ lines of a mode 0 point FAIL in check_tile); no IAU_S2_REFUSED / _JOIN_REFUSED / _RESET_REFUSED;
    IAU_S2_RESET_DEFERRED (and P4.11b's IAU_S2_RESET_DISABLE) on reset fault points only (SPDK_PERF_FAULT_RESET_MS);
    one IAU_S2_QPAIR per IO qpair connect
    (qpairs + the STATS lines' rejoins), each fast=1 (the namespace qualifies), in a poll group (pg >= 0, perf polls
    by group), tmo_us = SPDK_PERF_FAULT_TIMEOUT_US on a fault point that sets it, else 0; one IAU_S2_STATS (every key)
    and one IAU_S2_PG_END per IO qpair, the STATS qids those of the QPAIR lines; per STATS lost 0, special 0, sub_fast > 0
    (every point; and stray 0 when the line carries it, P4.11b); err / retries > 0 only with expected errors (SPDK_INV_EXPECT_RETRIES=1, or the device error knob
    with its evidence), and with SPDK_INV_EXPECT_RETRIES=1 their retries total > 0; on non-fault points marked,
    aborted, timeouts, leftovers, ended, rejoins and aborts 0, and (the P4.9 review's fast-path rule, as the IAU's) with
    the per-qpair queue depth <= fast_cids declined 0 and sub_tr 0, above it declines go to trackers (reported, not
    failed). extras: fast_share = sub_fast / (sub_fast + sub_tr); fast_label "S2 not fair" when a qpair's queue depth
    exceeds its fast_cids or a timeout is latched (tmo_us > 0; the controller's P6 note on the P4.11 review: the C6
    comparison needs timeouts off and the queue depth per qpair <= fast_cids), else request-free."""
    r = []
    on = mode == TILE_S2_MODE and env.get(TILE_S2_KNOB) == "1"
    names = sorted(set(m.group(0) for x in hlines for m in S2_ANY.finditer(x)))
    if not on:
        if names:
            r.append("NX_S2 %s lines without SPDK_IAU_S2=1 on mode 0" % ",".join(names))
        if TILE_S2_WB_KNOB in env:
            r.append("NX_S2 %s without SPDK_IAU_S2=1 on mode 0" % TILE_S2_WB_KNOB)
        return r
    fault = tile_fault_point(env, hlines)
    find = lambda rx: [m for x in hlines for m in [rx.search(x)] if m]
    ops, qps, sts, pge = find(S2_OPS), find(S2_QPAIR), find(S2_STATS), find(S2_PG_END)
    if len(ops) != 1:
        r.append("NX_S2 IAU_S2_OPS lines %d, expected 1" % len(ops))
    for m in ops:
        if m.groups() != ("1", str(S2_TRACKER_CIDS), "1"):
            r.append("NX_S2 IAU_S2_OPS s2=%s tracker_cids=%s pg=%s, expected s2=1 tracker_cids=%d pg=1" % (
                m.group(1), m.group(2), m.group(3), S2_TRACKER_CIDS))
    bad = {}
    for x in hlines:
        for m in S2_BAD.finditer(x):
            bad["IAU_S2_" + m.group(1)] = bad.get("IAU_S2_" + m.group(1), 0) + 1
    r += ["NX_S2 %s in host log (x%d)" % kv for kv in sorted(bad.items())]
    reset_pt = fault and strtoll(env.get("SPDK_PERF_FAULT_RESET_MS", "")) > 0
    nd = {}
    for x in hlines:
        for m in S2_DEFERRED.finditer(x):
            nd["IAU_S2_" + m.group(1)] = nd.get("IAU_S2_" + m.group(1), 0) + 1
    if not reset_pt:
        r += ["NX_S2 %s x%d outside a reset fault point (SPDK_PERF_FAULT_RESET_MS)" % kv for kv in sorted(nd.items())]
    for m in find(S2_DISABLE):
        if int(m.group(2)) == 0:
            r.append("NX_S2 IAU_S2_RESET_DISABLE qid=%s done=0 (A26: the leftovers were not drained, the residual)"
                     % m.group(1))
    kvs = []
    for m in sts:
        kv = tile_kv(m.group(3))
        miss = tile_missing(kv, S2_QSTATS + S2_STATS_TAIL)
        if miss:
            r.append("NX_S2 qid=%s IAU_S2_STATS missing %s" % (m.group(1), ",".join(miss)))
        else:
            kvs.append((m, kv))
    rejoins = sum(kv["rejoins"] for _, kv in kvs)
    if len(qps) != qpairs + rejoins:
        r.append("NX_S2 IAU_S2_QPAIR lines %d, expected %d (one per IO qpair connect: %d qpairs + %d rejoins)"
                 % (len(qps), qpairs + rejoins, qpairs, rejoins))
    for name, got in (("IAU_S2_STATS", sts), ("IAU_S2_PG_END", pge)):
        if len(got) != qpairs:
            r.append("NX_S2 %s lines %d, expected %d (one per IO qpair)" % (name, len(got), qpairs))
    a, b = sorted(m.group(1) for m in sts), sorted(m.group(1) for m in qps)
    if (a != b) if rejoins == 0 else (set(a) != set(b)):
        r.append("NX_S2 IAU_S2_STATS qids %s != IAU_S2_QPAIR qids %s" % (a, b))
    # P4.12b: the lines pair per qid: one STATS and one PG_END each, QPAIR lines = 1 + that qid's rejoins, and the
    # STATS' fast_cids is the QPAIR's
    pe = sorted(m.group(2) for m in pge)
    if pe != a:
        r.append("NX_S2 IAU_S2_PG_END qids %s != IAU_S2_STATS qids %s" % (pe, a))
    for m, kv in kvs:
        q = m.group(1)
        want, got = 1 + kv["rejoins"], sum(1 for x in qps if x.group(1) == q)
        if a.count(q) == 1 and got != want:
            r.append("NX_S2 qid=%s IAU_S2_QPAIR lines %d, expected %d (1 + rejoins %d)" % (q, got, want, kv["rejoins"]))
        for x in qps:
            if x.group(1) == q and x.group(2) != m.group(2):
                r.append("NX_S2 qid=%s IAU_S2_QPAIR fast_cids=%s != IAU_S2_STATS fast_cids=%s" % (q, x.group(2),
                                                                                                m.group(2)))
                break
    tmo = strtoll(env.get("SPDK_PERF_FAULT_TIMEOUT_US", "")) if fault else 0
    sqe = "wb" if env.get(TILE_S2_WB_KNOB) == "1" else "nt"
    fcids, tmo_on = {}, False
    for m in qps:
        tag = "qid=%s" % m.group(1)
        if m.group(5) != "1":
            r.append("NX_S2 %s IAU_S2_QPAIR fast=%s (the namespace does not qualify: no request-free path)"
                     % (tag, m.group(5)))
        if int(m.group(8)) < 0:
            r.append("NX_S2 %s IAU_S2_QPAIR pg=%s (not in a poll group; perf polls by group)" % (tag, m.group(8)))
        if int(m.group(7)) != tmo:
            r.append("NX_S2 %s IAU_S2_QPAIR tmo_us=%s, expected %d" % (tag, m.group(7), tmo))
        if m.group(9) is None:      # a build before P11 prints no sqe= (always streaming stores)
            if env.get(TILE_S2_WB_KNOB, "") != "":
                r.append("NX_S2 %s IAU_S2_QPAIR without sqe= but %s is set" % (tag, TILE_S2_WB_KNOB))
        elif m.group(9) != sqe:
            r.append("NX_S2 %s IAU_S2_QPAIR sqe=%s, expected %s" % (tag, m.group(9), sqe))
        fcids[m.group(1)] = int(m.group(2))
        tmo_on = tmo_on or int(m.group(7)) > 0
    dr, dok = dev_fault_evidence(env, tl, controllers, "NX_S2")
    r += dr
    expect_err = env.get("SPDK_INV_EXPECT_RETRIES") == "1" or dok
    mixed = False
    for m, kv in kvs:
        tag = "qid=%s" % m.group(1)
        r += ["NX_S2 %s %s=%d" % (tag, z, kv[z]) for z in S2_ZERO + S2_OPTIONAL_ZERO if kv.get(z)]
        if kv["sub_fast"] == 0:
            r.append("NX_S2 %s sub_fast=0 (no request-free IO)" % tag)
        if not expect_err and any(kv[z] for z in S2_ERR):
            r.append("NX_S2 %s err=%d retries=%d without expected errors" % (tag, kv["err"], kv["retries"]))
        fc = int(m.group(2))
        over = qd_q is not None and qd_q > fc
        mixed = mixed or over
        # P4.13-lite (controller, 2026-10-19): a closed-loop resubmit from inside a completion can meet a transient
        # credit / queued-list state, so a few declines happen with QD <= fast_cids; they complete exactly once on
        # the tracker path. Tolerated up to FAST_DECLINE_TOL of the qpair's IOs; above it the point FAILs / is mixed
        rare = fast_rare(kv["declined"] + kv["sub_tr"], kv["sub_fast"] + kv["sub_tr"])
        mixed = mixed or not rare   # P4.12b: the label also follows the evidence
        if not fault:
            r += ["NX_S2 %s %s=%d on a non-fault point" % (tag, z, kv[z]) for z in S2_NONFAULT_ZERO if kv[z]]
            if qd_q is not None and not over and not rare:
                r.append("NX_S2 %s declined=%d sub_tr=%d with the queue depth per qpair %d <= fast_cids %d" % (
                    tag, kv["declined"], kv["sub_tr"], qd_q, fc))
        elif not reset_pt and kv["rejoins"]:      # P4.12b: only a reset rejoins
            r.append("NX_S2 %s rejoins=%d outside a reset fault point (SPDK_PERF_FAULT_RESET_MS)" % (tag, kv["rejoins"]))
    if env.get("SPDK_INV_EXPECT_RETRIES") == "1" and kvs and sum(kv["retries"] for _, kv in kvs) == 0:
        r.append("NX_S2 SPDK_INV_EXPECT_RETRIES=1 but retries total 0 (the retry path did not run)")
    if extras is not None and kvs:
        f, t = sum(kv["sub_fast"] for _, kv in kvs), sum(kv["sub_tr"] for _, kv in kvs)
        if f + t:
            extras["fast_share"] = "%.4f" % (f / (f + t))
        # the controller's P6 note on the P4.11 review: the C6 comparison needs timeouts off and the queue depth
        # per qpair <= fast_cids, else the point is "S2 not fair" (S2's IO queues behind its 16 tracker cids; its
        # timeout scan is O(fast cids)); P4.12b: also whenever a decline or a tracker submit was observed
        extras["fast_label"] = "S2 not fair" if mixed or tmo_on else "request-free"
    return r


# P8.H2-fix: perf builds from nx-p8a print compute_calls (completions that ran the co-resident kernel) on the
# compute_stats line; earlier builds counted compute_ops in a shared non-atomic global (lost updates at k > 1).
COMPUTE_STATS = re.compile(r"compute_stats: compute_ops=(\d+) compute_per_io=(\d+)(?: compute_calls=(\d+))?")
COMPUTE_CALLS_NX = ("nx-p8a",)


def check_compute(hlines, variant=""):
    """P8.H2-fix: compute_ops == compute_per_io x compute_calls, exactly (the kernel adds compute_per_io per call);
    compute_per_io 0 needs compute_ops == 0. Every compute_stats line of a log (one per pcie_stats block) carries the
    same totals. A variant in COMPUTE_CALLS_NX must print compute_calls; earlier builds print none and are skipped."""
    ms = [m.groups() for x in hlines for m in [COMPUTE_STATS.search(x)] if m]
    if not ms:
        return []
    r = []
    if len(set(ms)) > 1:
        r.append("compute_stats lines disagree: %s" % " | ".join(sorted(set(
            "ops=%s per_io=%s calls=%s" % (o, c, n) for o, c, n in ms))))
    o, c, n = ms[0]
    o, c = int(o), int(c)
    if n is None:
        if variant in COMPUTE_CALLS_NX:
            r.append("compute_stats without compute_calls on %s" % variant)
        elif c == 0 and o != 0:
            r.append("compute_ops %d with compute_per_io 0" % o)
        return r
    n = int(n)
    if o != c * n:
        r.append("compute_ops %d != compute_per_io %d x compute_calls %d (= %d; %.3f kernels/call)"
                 % (o, c, n, c * n, (o / n) if n else 0.0))
    return r


def perf_idle_time_frac(hlines):
    """P4.12 (A16): the time-based idle of a worker-group point, the PERF_IDLE_TIME lines' summed idle_tsc over their
    summed window_tsc; None without a line (a perf from before P4.12, or no worker group) or a zero window."""
    lines = [m for x in hlines for m in [PERF_IDLE_TIME.search(x)] if m]
    win = sum(int(m.group(3)) for m in lines)
    return sum(int(m.group(2)) for m in lines) / win if lines and win > 0 else None


def perf_prints_idle_time(variant):
    """P4.12b: True when the host variant's perf prints PERF_IDLE_TIME on a worker-group run (a P4.12 or later build:
    an nx-* snapshot after PERF_IDLE_PRE_P412_NX); every other variant, the empty one included, predates it."""
    return bool(variant) and variant.startswith("nx-") and variant not in PERF_IDLE_PRE_P412_NX


def check_idle_time(hlines, wgroup, workers, extras=None, variant=""):
    """IAU-next P4.12 (A16's source refinement, the controller's decision of 2026-10-18): the host-bound test of every
    worker-group point is perf's time-based idle, idle_time_frac = the sum of the PERF_IDLE_TIME lines' idle_tsc over
    the sum of their window_tsc (one line per worker; each line's printed fraction its own ticks' ratio, idle <=
    window, window > 0), <= HOST_BOUND_IDLE -> host, else latency. A worker-group point without the lines (a perf from
    before P4.12) keeps the poll-count source, marked "poll-count (pre-P4.12)"; a point without the worker group keeps
    perf's poll-count idle (host_idle_frac, "poll-count") and has no such line. extras: idle_time_frac, a16_src,
    a16_bound (from the source's value; empty without one)."""
    r = []
    lines = [m for x in hlines for m in [PERF_IDLE_TIME.search(x)] if m]
    val, src = None, "poll-count"
    if not wgroup:
        if lines:
            r.append("PERF_IDLE_TIME lines without SPDK_PERF_WORKER_GROUP=1")
        val = host_idle_frac(hlines)
    elif not lines:
        src, val = "poll-count (pre-P4.12)", host_idle_frac(hlines)
        if perf_prints_idle_time(variant):
            r.append("PERF_IDLE_TIME lines 0, expected %d (variant %s prints one per worker on a worker-group run)"
                     % (workers, variant))
    else:
        src = "time"
        if len(lines) != workers:
            r.append("PERF_IDLE_TIME lines %d, expected %d (one per worker)" % (len(lines), workers))
        lc = sorted(m.group(1) for m in lines)
        dup = sorted(set(x for x in lc if lc.count(x) > 1))
        if dup:
            r.append("PERF_IDLE_TIME lcore(s) %s printed more than once" % ",".join(dup))
        for m in lines:
            i, w, f = int(m.group(2)), int(m.group(3)), float(m.group(4))
            if w == 0 or i > w or abs(f - i / w) > 0.00005 + 1e-9:
                r.append("PERF_IDLE_TIME lcore=%s idle_tsc=%d window_tsc=%d idle_time_frac=%s is inconsistent"
                         % (m.group(1), i, w, m.group(4)))
        val = perf_idle_time_frac(hlines)
        if extras is not None and val is not None:
            extras["idle_time_frac"] = "%.4f" % val
    if extras is not None:
        extras["a16_src"] = src
        extras["a16_bound"] = "" if val is None else "host" if val <= HOST_BOUND_IDLE else "latency"
    return r


def tile_knob_evidence(env, tl, mkv, controllers):
    """I2: the fault knobs of the env with their evidence: (reasons, errors_allowed). Engine knob
    IAU_ENG_FAULT_ERR_EVERY: exactly one IAU_ENG6_FAULT line, with every= the env's, sum injected > 0, sum rec_err_* >
    0. Device knob IAU_DEV_FAULT_ERR_EVERY[_C<k>] (_C<k> over the global, b6_fault_load()): one DEV_FAULT ctrlr=k
    line with every= the effective value for each controller with every > 0 and none for another, sum DEV_FAULT_STATS
    marked > 0. Errors are allowed only by a knob whose every requirement holds. mkv: the members' STATS dicts."""
    r, allowed = [], False
    if TILE_ENG_FAULT_EVERY in env:
        want = strtoll(env[TILE_ENG_FAULT_EVERY])
        lines = [m for x in tl for m in [ENG6_FAULT.search(x)] if m]
        match = [m for m in lines if int(m.group(1)) == want]
        ok = len(lines) == 1 and len(match) == 1
        if not ok:
            r.append("NX_TILE %s=%s: IAU_ENG6_FAULT lines with every=%d: %d, expected 1%s"
                     % (TILE_ENG_FAULT_EVERY, env[TILE_ENG_FAULT_EVERY], want, len(match),
                        " (%d with another every=)" % (len(lines) - len(match)) if len(lines) != len(match) else ""))
        if sum(kv["injected"] for kv in mkv) == 0:
            ok = False
            r.append("NX_TILE %s=%s but injected total 0" % (TILE_ENG_FAULT_EVERY, env[TILE_ENG_FAULT_EVERY]))
        if sum(kv[z] for kv in mkv for z in TILE_ENG_ERR) == 0:
            ok = False
            r.append("NX_TILE %s=%s but rec_err_parked+rec_err_final total 0" % (TILE_ENG_FAULT_EVERY, env[TILE_ENG_FAULT_EVERY]))
        allowed = allowed or ok
    dr, dok = dev_fault_evidence(env, tl, controllers)
    r += dr
    allowed = allowed or dok
    return r, allowed


def dev_fault_evidence(env, tl, controllers, tag="NX_TILE"):
    """The device knob IAU_DEV_FAULT_ERR_EVERY[_C<k>] (_C<k> over the global, b6_fault_load()) with its evidence:
    (reasons, ok). One DEV_FAULT ctrlr=k line with every= the effective value for each controller with every > 0 and
    none for another, sum DEV_FAULT_STATS marked > 0; ok = the knob is set and every requirement holds (its errors are
    expected). No knob: ([], False)."""
    r, ok = [], False
    if any(TILE_DEV_FAULT_EVERY.match(k) for k in env):
        want = []
        for c in range(controllers):
            v = env.get("IAU_DEV_FAULT_ERR_EVERY_C%d" % c, "")
            v = v if v != "" else env.get("IAU_DEV_FAULT_ERR_EVERY", "")
            if strtoll(v) > 0:
                want.append((c, strtoll(v)))
        got = sorted((int(m.group(1)), int(m.group(2))) for x in tl or [] for m in [DEV_FAULT.search(x)] if m)
        ok = got == want
        if not ok:
            r.append("%s IAU_DEV_FAULT_ERR_EVERY: DEV_FAULT (ctrlr,every) %s != expected %s" % (tag, got, want))
        if sum(int(m.group(4)) for x in tl or [] for m in [DEV_FAULT_STATS.search(x)] if m) == 0:
            ok = False
            r.append("%s IAU_DEV_FAULT_ERR_EVERY but DEV_FAULT_STATS marked total 0" % tag)
    return r, ok


def tile_env_num(env, name, default):
    """The value the host library parses for name (C default when unset or empty), as printed."""
    v = env.get(name, "")
    return str(default) if v == "" else str(int(v)) if v.isdigit() else v


def tile_records(hlines, tl):
    """(host records, engine records) for the three-way check. A host group's records= is group-cumulative and
    printed at every member's leave (IAU_TILE_GROUP_END; a private group has one; a declared P4.8 deviation), so its
    LAST line per gid counts (check_tile FAILs a counter that decreases from one line to the next); the engine prints
    one RING_END per ring incarnation, summed."""
    host = {}
    for x in hlines:
        m = TILE_END.search(x)
        if m and isinstance(tile_kv(m.group(2)).get("records"), int):
            host[m.group(1)] = tile_kv(m.group(2))["records"]
    eng = 0
    for x in tl or []:
        m = ENG6_END.search(x)
        if m and isinstance(tile_kv(m.group(4)).get("records_emitted"), int):
            eng += tile_kv(m.group(4))["records_emitted"]
    return sum(host.values()), eng


def tile_group_records(hlines, tl, env=None, bdev="fastpath"):
    """P4.8b, the three-way per group: {gid: {"host": the gid's last IAU_TILE_GROUP_END records=, "engine": the
    sum of its IAU_ENGINE6_RING_END records_emitted, "ssd": the SSD posted completions (OPC_STATS fp_done + other,
    keyed by ctrlr= and qid) of its members (its IAU_ENGINE6_BIND (ctrlr, qid)s), None without a member, when a member
    has no such OPC_STATS line or its controller has no fast path (fp_done counts fast-path completions only),
    "cqes": its members' cqes_consumed (the SSD leg when "ssd" is None: the engine's count of the SSD's CQEs, I1),
    "detached": its members' rec_detached}}. Engine lines name the host's gid (the GCB's gid word, iau_tile_gcb_check()).
    On a non-fault point host == engine == ssd + detached: the engine emits one record per consumed CQE (member_cq())
    and one R_DETACHED per member, every other record kind is 0 there."""
    env, out = env or {}, {}
    g = lambda gid: out.setdefault(gid, {"host": 0, "engine": 0, "ssd": None, "cqes": 0, "detached": 0})
    for x in hlines:
        m = TILE_END.search(x)
        if m and isinstance(tile_kv(m.group(2)).get("records"), int):
            g(m.group(1))["host"] = tile_kv(m.group(2))["records"]
    opc = {}
    for x in tl or []:
        m = ENG6_END.search(x)
        if m and isinstance(tile_kv(m.group(4)).get("records_emitted"), int):
            g(m.group(1))["engine"] += tile_kv(m.group(4))["records_emitted"]
        m = OPC_P3.search(x)
        if m:
            opc[(m.group(5), m.group(1))] = int(m.group(4)) + int(m.group(6))
        m = ENG6_STATS.search(x)
        if m and isinstance(tile_kv(m.group(7)).get("rec_detached"), int):
            g(m.group(3))["detached"] += tile_kv(m.group(7))["rec_detached"]
        if m and isinstance(tile_kv(m.group(7)).get("cqes_consumed"), int):
            g(m.group(3))["cqes"] += tile_kv(m.group(7))["cqes_consumed"]
    mem = {}
    for x in tl or []:
        m = ENG6_BIND.search(x)
        if m:
            mem.setdefault(m.group(3), []).append(m.group(1, 2))
    for gid, cq in mem.items():
        post = [opc.get(k) for k in cq]
        if None not in post and all(dev_expect(env, int(c), "FASTPATH", bdev_fastpath(bdev)) != "0" for c, _ in cq):
            g(gid)["ssd"] = sum(post)
    return out


def host_idle_frac(hlines):
    """A16: perf's idle_polls / polls summed over its PCIE transport blocks (one per poll group); None without
    polls. It is not CPU idle (CLAUDE.md): the share of polls that found nothing to complete."""
    polls = idle = 0
    for x in hlines:
        m = POLLS_LINE.match(x)
        if m:
            polls += int(m.group(1))
        m = IDLE_POLLS_LINE.match(x)
        if m:
            idle += int(m.group(1))
    return idle / polls if polls > 0 else None


def tile_db_mid_pass(k, records, n):
    """P4.8e, the doorbells a shared group's passes may ring after full chunks (shared_pass(): all K dirty rings after
    every full chunk of db_batch = n records of a ring), over `records` consumed records: K x floor(records / n), 0 at
    n = 0 (pass-end doorbells only). With the busy-poll term K per busy pass this is the per-pass bound K + K x
    floor(R / n) for a pass of R records (a sum of floors <= the floor of the sum). Confirmed for the round-robin
    pass of the P4.8e fix round (72533ba5): per busy pass <= K x (1 + rounds with a full chunk) <= K + K x floor(R /
    n). The one place to change if the group pass's chunk order changes the bound."""
    return k * (records // n) if n > 0 else 0


def pcie_blocks(hlines):
    """perf's PCIE transport blocks (nvme_dump_pcie_statistics(), one per poll group) in log order: each the block's
    "key: N" counters (polls, idle_polls, queued_requests, sq_mmio_doorbell_updates, ...) plus sq_mmio= of its
    pcie_stats line, which closes it."""
    out, cur = [], None
    for x in hlines:
        if x == "PCIE transport:":
            cur = {}
            continue
        if cur is None:
            continue
        m = PCIE_STATS.search(x)
        if m:
            cur.update((k, int(v)) for k, v in NUMKV.findall(x[x.index("pcie_stats:"):]) if k == "sq_mmio")
            out.append(cur)
            cur = None
            continue
        m = PCIE_KV.match(x)
        if m:
            cur[m.group(1)] = int(m.group(2))
    return out


def check_tile(hlines, mode, tl, tgt_log, qpairs, engine_cpus, controllers=1, env=None, extras=None, bdev="fastpath",
             app_cpus="0", variant="", qd_q=None):
    """IAU-next P4.6 NX_TILE (mode 6 = target mode 5 + host SPDK_IAU_TILE=1; P4.8 group rings with SPDK_IAU_GROUP=1, their
    rules P4.8b); qpairs = total IO qpairs, app_cpus as check_log's (perf's poll groups). Any mode: a v6 knob off its
    mode (I1), SPDK_PERF_FAULT before P4.12 (I2), a host-library IAU_TILE_ line
    in the TARGET log (the target saw SPDK_IAU_*), an IAU_ENGINE6_REFUSED / _BIND_TIMEOUT line or a v6 ring leak at
    transport destroy FAILs; on a non-6 mode any IAU_TILE_ / IAU_ENGINE6_ line FAILs (as NX_ENGINE: "<names> lines on a
    mode M point"). Mode 6: see the module doc."""
    env = env or {}
    r = []
    bad = tile_knob_misuse(env, mode)
    if bad:
        r.append("NX_TILE v6 knob on a mode %d point: %s" % (mode, ",".join(bad)))
    r += check_perf_fault(env, hlines)
    leak = sorted(set(m.group(0) for x in (tl or []) for m in TILE_ANY_HOST.finditer(x)))
    if leak:
        r.append("NX_TILE host-library %s lines in the target log (the target saw SPDK_IAU_*)" % ",".join(leak))
    hits = {}
    for x in tl or []:
        for m in ENG6_BAD.finditer(x):
            name = "IAU_ENGINE6_" + m.group(1) if m.group(1) else \
                "v6 rings still bound at transport destroy" if m.group(2) else "engine stopped with v6 rings"
            hits[name] = hits.get(name, 0) + 1
    r += ["NX_TILE %s in target log (x%d)" % kv for kv in sorted(hits.items())]
    if mode != 6:
        names = sorted(set(m.group(0) for x in hlines for m in TILE_ANY_HOST.finditer(x))
                       | set(m.group(0) for x in (tl or []) for m in ENG6_ANY.finditer(x)))
        if names:
            r.append("%s lines on a mode %d point" % (",".join(names), mode))
        return r
    if tl is None:
        return r + ["NX_TILE target log missing: %s" % tgt_log]
    fault = tile_fault_point(env, hlines)
    group_on = env.get("SPDK_IAU_GROUP") == "1"
    # perf's poll groups (one shared group each with SPDK_IAU_GROUP=1): one per worker with SPDK_PERF_WORKER_GROUP=1,
    # else one per ns_ctx (n_ctx = max(app cores, controllers), check_log)
    wgroup = env.get("SPDK_PERF_WORKER_GROUP") == "1"
    workers = max(1, cpu_count(app_cpus))
    n_ctx = max(workers, controllers)
    n_pg = workers if wgroup else n_ctx
    want_stride = tile_env_num(env, "SPDK_IAU_DESC_STRIDE", TILE_DEFAULT_STRIDE)
    find = lambda rx, lines: [m for x in lines for m in [rx.search(x)] if m]
    ops, groups, hq, hs, he = (find(rx, hlines) for rx in (TILE_OPS, TILE_GROUP, TILE_QPAIR, TILE_STATS, TILE_END))
    rings, binds, stats, ends, starts, peaks, rels = (find(rx, tl) for rx in (ENG6_RING, ENG6_BIND, ENG6_STATS, ENG6_END,
                                                                               ENG_START, ENG6_PEAK, ENG6_RELEASE))
    # host evidence: the lever's one OPS line with the env's values, and no refusal or loss line
    if len(ops) != 1:
        r.append("NX_TILE IAU_TILE_OPS lines %d, expected 1" % len(ops))
    want_ops = (("tile", "1"), ("stride", want_stride), ("rcons", "1" if env.get("SPDK_IAU_RCONS") == "1" else "0"),
                ("rings", tile_env_num(env, "SPDK_IAU_RINGS", 1)),
                ("ring_entries", tile_env_num(env, "SPDK_IAU_RING_ENTRIES", TILE_DEFAULT_RING_ENTRIES)))
    for m in ops:
        for i, (key, want) in enumerate(want_ops):
            if m.group(i + 1) != want:
                r.append("NX_TILE IAU_TILE_OPS %s=%s, expected %s" % (key, m.group(i + 1), want))
    # P4.8b, the group lever's evidence: OPS group= (every P4.8+ host library prints it) is 1 iff SPDK_IAU_GROUP=1
    p48 = any(m.group(6) is not None for m in ops)      # a P4.8+ host library: the GROUP_END tail keys are required
    for m in ops:
        if group_on and m.group(6) != "1":
            r.append("NX_TILE SPDK_IAU_GROUP=1 but IAU_TILE_OPS group=%s (the group lever did not switch on)"
                     % ("absent" if m.group(6) is None else m.group(6)))
        elif not group_on and m.group(6) not in (None, "0"):
            r.append("NX_TILE IAU_TILE_OPS group=%s without SPDK_IAU_GROUP=1 (a lever on silently)" % m.group(6))
    # P4.8e, the group pass's doorbell policy (SPDK_IAU_DB_BATCH, nvme_iau_cfg_load(): 0..65535, default
    # NVME_IAU_DB_BATCH_DEFAULT on a group run; a non-zero value needs SPDK_IAU_TILE=1 and SPDK_IAU_GROUP=1, 0 without
    # the group is a no-op, decision m3). OPS db_batch= is printed from P4.8e on: absent = an older host library
    # (pass-end doorbells only, 0), where the knob has no effect and so FAILs (lever evidence); present = the env's
    # value, else the host variant's default on a group run (TILE_DEFAULT_DB_BATCH, or TILE_DB_BATCH_DEFAULT_BY_VARIANT for a
    # snapshot built before the P4.8f default change), else 0
    knob_db = env.get("SPDK_IAU_DB_BATCH", "")
    if knob_db != "" and not group_on and tile_env_num(env, "SPDK_IAU_DB_BATCH", 0) != "0":
        r.append("NX_TILE SPDK_IAU_DB_BATCH=%s without SPDK_IAU_GROUP=1 (only 0 is allowed without the group: a group-pass "
                 "knob)" % knob_db)
    n_db = 0
    for m in ops[:1]:
        if m.group(7) is None:
            if knob_db != "":
                r.append("NX_TILE SPDK_IAU_DB_BATCH=%s but IAU_TILE_OPS has no db_batch= (the lever did not switch on)"
                         % knob_db)
            continue
        n_db = int(m.group(7))
        dflt_db = TILE_DB_BATCH_DEFAULT_BY_VARIANT.get(variant, TILE_DEFAULT_DB_BATCH)
        want_db = tile_env_num(env, "SPDK_IAU_DB_BATCH", dflt_db) if knob_db != "" else \
            str(dflt_db) if group_on else "0"
        if m.group(7) != want_db:
            r.append("NX_TILE IAU_TILE_OPS db_batch=%s, expected %s (SPDK_IAU_DB_BATCH%s)"
                     % (m.group(7), want_db, "" if knob_db != "" else " unset: %s" % ("the group default" if group_on
                                                                                     else "0 without the group")))
    hbad = {}
    for x in hlines:
        for m in TILE_HOST_BAD.finditer(x):
            hbad["IAU_TILE_" + m.group(1)] = hbad.get("IAU_TILE_" + m.group(1), 0) + 1
    r += ["NX_TILE %s in host log (x%d)" % kv for kv in sorted(hbad.items())]
    # P4.12: the reset evidence (n4's deferral, A22's disable run by a delete) only on a reset fault point
    nreset = {}
    for x in hlines:
        for m in TILE_RESET_ONLY.finditer(x):
            nreset["IAU_TILE_" + m.group(1)] = nreset.get("IAU_TILE_" + m.group(1), 0) + 1
    reset_pt = fault and strtoll(env.get("SPDK_PERF_FAULT_RESET_MS", "")) > 0
    if not reset_pt:
        r += ["NX_TILE %s x%d outside a reset fault point (SPDK_PERF_FAULT_RESET_MS)" % kv for kv in sorted(nreset.items())]
    # P4.12: a reset point's rejoins (the host STATS' rejoins=, one per reconnect, printed at the qpair's delete)
    rejoins = sum(x for x in (tile_kv(m.group(5)).get("rejoins") for m in hs) if isinstance(x, int))
    # line counts: one per host IO qpair (STATS, GROUP_END: at its delete) or per member incarnation (QPAIR at each
    # connect, BIND, engine STATS: qpairs + rejoins); at most one PEAK per engine (printed at its stop)
    for name, got, want in (("IAU_TILE_QPAIR", hq, qpairs + rejoins), ("IAU_TILE_STATS", hs, qpairs),
                            ("IAU_TILE_GROUP_END", he, qpairs), ("IAU_ENGINE6_BIND", binds, qpairs + rejoins),
                            ("IAU_ENGINE6_STATS", stats, qpairs + rejoins)):
        if len(got) != want:
            r.append("NX_TILE %s lines %d, expected %d" % (name, len(got), want))
    pe = [m.group(1) for m in peaks]
    r += ["NX_TILE IAU_ENGINE6_PEAK lines for engine %s: %d (at most one)" % (e, pe.count(e))
          for e in sorted(set(pe), key=int) if pe.count(e) > 1]
    # (no "engine peak >= its rings' peak" rule: the engine samples the sum of ring in_use once per pass, end of pass,
    # a ring its in_use at every slot allocation, so a slot taken and freed inside one pass reaches only the ring's)
    if sorted(m.group(1) for m in groups) != sorted(set(m.group(2) for m in hq)):
        r.append("NX_TILE IAU_TILE_GROUP gids %s != IAU_TILE_QPAIR gids %s (one GROUP line per group)"
                 % (sorted(m.group(1) for m in groups), sorted(set(m.group(2) for m in hq))))
    # P4.8b topology: one shared group (private=0) per perf poll group with k = SPDK_IAU_RINGS and entries =
    # SPDK_IAU_RING_ENTRIES, holding that poll group's qpairs (PERF_WORKER_GROUP qpairs= / the ns_ctx's qpairs)
    gk = dict((m.group(1), (int(m.group(2)), int(m.group(3)))) for m in groups)     # gid -> (k, entries)
    shared = [m for m in groups if m.group(5) == "0"]
    hshared = set(m.group(1) for m in shared)
    nmem = {}                                   # gid -> its IAU_TILE_QPAIR lines: one per connect (member incarnations)
    for m in hq:
        nmem[m.group(2)] = nmem.get(m.group(2), 0) + 1
    # P8.H1-dbg: a rejoin prints its qpair's QPAIR line again on the same gid (nvme_iau_ctrlr_connect_qpair(): the
    # reconnect keeps iq->g, print_qpair() at every connect), so a gid's members are its QPAIR lines minus its host
    # STATS' rejoins (each member's first join plus one line per rejoin)
    grj = {}
    for m in hs:
        x = tile_kv(m.group(5)).get("rejoins")
        grj[m.group(1)] = grj.get(m.group(1), 0) + (x if isinstance(x, int) else 0)
    if group_on:
        want_k = tile_env_num(env, "SPDK_IAU_RINGS", TILE_DEFAULT_RINGS)
        want_e = tile_env_num(env, "SPDK_IAU_RING_ENTRIES", TILE_DEFAULT_RING_ENTRIES)
        if len(shared) != n_pg:
            r.append("NX_TILE IAU_TILE_GROUP private=0 lines %d, expected %d (one shared group per perf poll group)"
                     % (len(shared), n_pg))
        for m in shared:
            if m.group(2) != want_k:
                r.append("NX_TILE gid=%s IAU_TILE_GROUP k=%s, expected %s (SPDK_IAU_RINGS)" % (m.group(1), m.group(2), want_k))
            if m.group(3) != want_e:
                r.append("NX_TILE gid=%s IAU_TILE_GROUP entries=%s, expected %s (SPDK_IAU_RING_ENTRIES)"
                         % (m.group(1), m.group(3), want_e))
        got = sorted(nmem.get(m.group(1), 0) - grj.get(m.group(1), 0) for m in shared)
        want = sorted(int(m.group(3)) for x in hlines for m in [WGROUP.search(x)] if m) if wgroup else \
            [qpairs // n_ctx] * n_ctx if qpairs % n_ctx == 0 else None
        if got != want:
            r.append("NX_TILE shared group members %s, expected %s (the qpairs of each perf poll group)" % (got, want))
    ring_keys, end_keys = sorted((m.group(1), m.group(2)) for m in rings), sorted((m.group(1), m.group(2)) for m in ends)
    if ring_keys != end_keys:
        r.append("NX_TILE IAU_ENGINE6_RING (gid,ring) %s != IAU_ENGINE6_RING_END (gid,ring) %s" % (ring_keys, end_keys))
    # configuration the env selects: stride, private groups (SPDK_IAU_GROUP), fast (SPDK_IAU_FAST), K rings
    bad = sorted(set(m.group(4) for m in groups if m.group(4) != want_stride) | set(m.group(8) for m in rings
                                                                                  if m.group(8) != want_stride))
    if bad:
        r.append("NX_TILE group/ring stride %s, expected %s" % (",".join(bad), want_stride))
    want_fast, want_priv = "1" if env.get("SPDK_IAU_FAST") == "1" else "0", "0" if group_on else "1"
    bad = sorted(set(m.group(8) for m in hq if m.group(8) != want_fast))
    if bad:
        r.append("NX_TILE IAU_TILE_QPAIR fast=%s, expected %s (SPDK_IAU_FAST)" % (",".join(bad), want_fast))
    bad = sorted(set([m.group(10) for m in hq if m.group(10) != want_priv]
                     + [m.group(5) for m in groups if m.group(5) != want_priv]))
    if bad:
        r.append("NX_TILE private=%s, expected %s (SPDK_IAU_GROUP)" % (",".join(bad), want_priv))
    nk = (strtoll(env.get("SPDK_IAU_RINGS", "")) or 1) if group_on else 1
    bad = sorted(set(int(m.group(4)) for m in hq if int(m.group(4)) >= nk))
    if bad:
        r.append("NX_TILE IAU_TILE_QPAIR ring index %s >= K=%d" % (",".join(map(str, bad)), nk))
    # P4.8b: route r of a group on ring r mod k (k of its GROUP line), one member per route; a shared group's routes
    # 0..n-1 on a non-fault point (pg_join: the lowest free route; nothing leaves before perf's teardown)
    routes = {}
    for m in hq:
        gid, route, ring = m.group(2), int(m.group(3)), int(m.group(4))
        k = gk.get(gid, (0, 0))[0]
        if k > 0 and ring != route % k:
            r.append("NX_TILE qid=%s gid=%s route=%d on ring %d, expected route mod k=%d = %d"
                     % (m.group(1), gid, route, ring, k, route % k))
        routes.setdefault(gid, []).append(route)
    for gid in sorted(routes, key=int):
        rts = sorted(routes[gid])
        dup = sorted(set(x for x in rts if rts.count(x) > 1))
        if dup and not fault:             # m7: a rejoin (fault points, P4.12) prints a route's QPAIR line again
            r.append("NX_TILE gid=%s routes %s on more than one IAU_TILE_QPAIR" % (gid, dup))
        elif not dup and not fault and gid in hshared and rts != list(range(len(rts))):
            r.append("NX_TILE gid=%s routes %s, expected 0..%d (lowest free route at each join)" % (gid, rts, len(rts) - 1))
    # member sets: host (gid,route) == engine members; host STATS == host QPAIR; engine STATS == BIND; the IO qpairs
    hkeys = sorted((m.group(2), m.group(3)) for m in hq)
    bkeys = sorted((m.group(3), m.group(4)) for m in binds)
    if hkeys != bkeys:
        r.append("NX_TILE host (gid,route) %s != engine member (gid,route) %s" % (hkeys, bkeys))
    else:
        hmem = dict(((m.group(2), m.group(3)), m.group(1, 4, 5, 6, 7)) for m in hq)
        bmem = dict(((m.group(3), m.group(4)), m.group(2, 5, 6, 10, 9)) for m in binds)
        bad = sorted(x for x in hmem if hmem[x] != bmem.get(x))
        if bad:
            r.append("NX_TILE host QPAIR != engine BIND (qid,ring,gen,credits,nsid) at (gid,route) %s" % bad)
    a, b = sorted(m.group(1, 2, 3) for m in hs), sorted(m.group(2, 3, 1) for m in hq)
    if (a != b) if rejoins == 0 else (set(a) != set(b)):    # P4.12: a rejoin prints its QPAIR line again
        r.append("NX_TILE IAU_TILE_STATS (gid,route,qid) != IAU_TILE_QPAIR: %s vs %s" % (a, b))
    a, b = sorted(m.group(1, 2, 3, 4) for m in stats), sorted(m.group(1, 2, 3, 4) for m in binds)
    if a != b:
        r.append("NX_TILE IAU_ENGINE6_STATS (ctrlr,qid,gid,route) != IAU_ENGINE6_BIND: %s vs %s" % (a, b))
    per = qpairs // controllers if controllers > 0 and qpairs % controllers == 0 else None
    if per is None:
        r.append("NX_TILE IO qpairs %d not divisible over %d controllers" % (qpairs, controllers))
    else:
        want_set = sorted((str(c), str(q)) for c in range(controllers) for q in range(1, per + 1))
        for name, got in (("IAU_ENGINE6_BIND", binds), ("IAU_ENGINE6_STATS", stats)):
            keys = sorted(m.group(1, 2) for m in got)
            if rejoins:     # P4.12: a member incarnation per rejoin (the count is checked above)
                keys = sorted(set(keys))
            if keys != want_set:
                r.append("NX_TILE %s (ctrlr,qid) set: duplicate %s missing %s unexpected %s"
                         % (name, sorted(set(x for x in keys if keys.count(x) > 1)), sorted(set(want_set) - set(keys)),
                            sorted(set(keys) - set(want_set))))
    host_rings = sorted(set(m.group(2, 4) for m in hq))
    if sorted(set(ring_keys)) != host_rings:
        r.append("NX_TILE engine rings (gid,ring) %s != host rings %s" % (sorted(set(ring_keys)), host_rings))
    # engine threads (as NX_ENGINE): started on exactly engine_cpus; rings and members on started threads
    want_cpus = sorted(cpu_list(engine_cpus))
    got_cpus = sorted(int(m.group(2)) for m in starts)
    if not want_cpus:
        r.append("NX_TILE mode 6 point has empty engine_cpus")
    if not starts:
        r.append("NX_TILE no IAU_ENGINE_START line")
    if got_cpus != want_cpus or any(m.group(2) != m.group(3) for m in starts):
        r.append("NX_TILE engine threads on lcores %s, expected %s" % (got_cpus, want_cpus))
    start_map = dict((m.group(1), m.group(2)) for m in starts)
    for m in rings:
        if start_map.get(m.group(4)) != m.group(5) or int(m.group(5)) not in want_cpus:
            r.append("NX_TILE IAU_ENGINE6_RING gid=%s ring=%s engine=%s lcore=%s not a started engine thread on "
                     "engine_cpus" % m.group(1, 2, 4, 5))
    # P4.8b: ring k of group gid on engine (gid + k) mod n_engines (iau_tile_pick_engine()); its nk and entries are the
    # host group's k and entries
    for m in rings:
        gid, ring, n_e = int(m.group(1)), int(m.group(2)), len(want_cpus)
        if n_e and int(m.group(4)) != (gid + ring) % n_e:
            r.append("NX_TILE IAU_ENGINE6_RING gid=%d ring=%d on engine %s, expected (gid + ring) mod %d engines = %d"
                     % (gid, ring, m.group(4), n_e, (gid + ring) % n_e))
        k, e = gk.get(m.group(1), (None, None))
        if k is not None and int(m.group(3)) != k:
            r.append("NX_TILE IAU_ENGINE6_RING gid=%d ring=%d nk=%s, expected the group's k=%d" % (gid, ring, m.group(3), k))
        if e is not None and (int(m.group(6)) != e or int(m.group(7)) != e):
            r.append("NX_TILE IAU_ENGINE6_RING gid=%d ring=%d d_entries=%s r_entries=%s, expected the group's entries=%d"
                     % (gid, ring, m.group(6), m.group(7), e))
    ring_eng = dict(((m.group(1), m.group(2)), m.group(4)) for m in rings)
    for m in binds:
        if start_map.get(m.group(7)) != m.group(8) or int(m.group(8)) not in want_cpus:
            r.append("NX_TILE IAU_ENGINE6_BIND ctrlr=%s qid=%s engine=%s lcore=%s not a started engine thread on "
                     "engine_cpus" % m.group(1, 2, 7, 8))
        if ring_eng.get(m.group(3, 5)) != m.group(7):
            r.append("NX_TILE IAU_ENGINE6_BIND ctrlr=%s qid=%s engine=%s is not its ring's engine %s"
                     % (m.group(1), m.group(2), m.group(7), ring_eng.get(m.group(3, 5))))
    if any(TGT_FETCHED.search(x) for x in tl):
        r.append("NX_TILE legacy IAU_STATS lines on a mode 6 point")
    # per member STATS (printed after detach): every required key first, then the fault knobs' evidence (I2: only an
    # evidenced knob, or SPDK_INV_EXPECT_RETRIES=1, allows error records), then the relations
    mems = []
    for m in stats:
        kv = tile_kv(m.group(7))
        miss = tile_missing(kv, TILE_ENG_MEMBER_STATS + TILE_ENG_MEMBER_TAIL, ("state",))
        if miss:
            r.append("NX_TILE ctrlr=%s qid=%s IAU_ENGINE6_STATS missing %s" % (m.group(1), m.group(2), ",".join(miss)))
        else:
            mems.append((m, kv))
    knob_r, knob_ok = tile_knob_evidence(env, tl, [kv for _, kv in mems], controllers)
    r += knob_r
    r += tile_spill_evidence(env, tl, mems, binds, fault)
    expect_err = env.get("SPDK_INV_EXPECT_RETRIES") == "1" or knob_ok
    # P4.12b: perf's RAW_INVAL_EVERY knob is gone (it never reached the payload builder), so a refused RAW (DV29,
    # rec_inval) is a tripwire on every point again; the refusal is pinned by the harness and the C gate vectors
    eng_zero = TILE_ENG_ZERO
    bind_eng = dict((m.group(1, 2), m.group(7)) for m in binds)
    # P8.H1-dbg: the SSD prints one OPC_STATS per I/O SQ lifetime (armed at Create I/O SQ, vfio_user.c; printed once at
    # delete_sq_done() / free_qp(), after sq_eng_detach() printed that incarnation's IAU_ENGINE6_STATS), and on mode 6
    # each SQ lifetime binds one member incarnation (BIND = qpairs + rejoins, above): the i-th OPC_STATS of a (ctrlr,
    # qid) is the SSD side of its i-th IAU_ENGINE6_STATS (a reset's rejoin is a second pair; as CTX_SPILL_STATS, m11)
    opc = {}
    for x in tl:
        m = OPC_P3.search(x)
        if m:
            opc.setdefault((m.group(5), m.group(1)), []).append(
                (int(m.group(2)) + int(m.group(3)) + int(m.group(4)), int(m.group(6)), int(m.group(4))))
    n_st = {}
    for m, _ in mems:
        n_st[m.group(1, 2)] = n_st.get(m.group(1, 2), 0) + 1
    r += ["NX_TILE ctrlr=%s qid=%s OPC_STATS lines %d != IAU_ENGINE6_STATS lines %d (one OPC_STATS per I/O SQ lifetime, "
          "each with one member incarnation)" % (key + (len(opc[key]), n_st[key]))
          for key in sorted(opc) if key in n_st and len(opc[key]) > n_st[key]]
    seen = {}
    err_total = 0
    for m, kv in mems:
        tag = "ctrlr=%s qid=%s" % m.group(1, 2)
        if bind_eng.get(m.group(1, 2), m.group(6)) != m.group(6):
            r.append("NX_TILE %s STATS engine=%s != BIND engine=%s" % (tag, m.group(6), bind_eng[m.group(1, 2)]))
        if kv["io_fetched"] + kv["raw_fetched"] + kv["reissues"] != kv["sqes_written"]:
            r.append("NX_TILE %s io_fetched+raw_fetched+reissues %d != sqes_written %d"
                     % (tag, kv["io_fetched"] + kv["raw_fetched"] + kv["reissues"], kv["sqes_written"]))
        if kv["sqes_written"] != kv["cqes_consumed"] + kv["dropped_busy"]:
            r.append("NX_TILE %s sqes_written %d != cqes_consumed+dropped_busy %d"
                     % (tag, kv["sqes_written"], kv["cqes_consumed"] + kv["dropped_busy"]))
        # P4.8b I1 (every point): member_cq() counts each SSD CQE, then exactly one record kind or bad_cid
        if kv["cqes_consumed"] != sum(kv[z] for z in TILE_ENG_CQE):
            r.append("NX_TILE %s cqes_consumed %d != %s %d (one record or bad_cid per consumed CQE)"
                     % (tag, kv["cqes_consumed"], "+".join(TILE_ENG_CQE), sum(kv[z] for z in TILE_ENG_CQE)))
        r += ["NX_TILE %s %s=%d" % (tag, z, kv[z]) for z in eng_zero if kv[z]]
        if kv["rec_detached"] != 1:
            r.append("NX_TILE %s rec_detached=%d, expected 1" % (tag, kv["rec_detached"]))
        errs = sum(kv[z] for z in TILE_ENG_ERR)
        err_total += errs
        if errs and not expect_err:
            r.append("NX_TILE %s rec_err_parked+rec_err_final=%d without expected errors (SPDK_INV_EXPECT_RETRIES=1, or "
                     "a fault knob with its evidence)" % (tag, errs))
        if not fault:
            r += ["NX_TILE %s %s=%d on a non-fault point" % (tag, z, kv[z]) for z in TILE_ENG_NONFAULT_ZERO if kv[z]]
        if kv["state"] != "gone":
            r.append("NX_TILE %s state=%s, expected gone (STATS after detach)" % (tag, kv["state"]))
        if kv["sqes_written"] > 0 and (kv["ssd_db_writes"] == 0 or kv["cq_db_writes"] == 0):
            r.append("NX_TILE %s IOs without SSD doorbell writes" % tag)
        i = seen[m.group(1, 2)] = seen.get(m.group(1, 2), -1) + 1
        o = opc.get(m.group(1, 2), [])
        o = o[i] if i < len(o) else None
        if o is None:
            r.append("NX_TILE %s has no OPC_STATS line with ctrlr= and fp_done=" % tag)
        else:
            if o[0] != kv["sqes_written"]:
                r.append("NX_TILE %s SSD fetched %d != engine sqes_written %d" % (tag, o[0], kv["sqes_written"]))
            # M3: fp_done counts fast-path completions only (0 without the fast path, bdev null|delay)
            if dev_expect(env, int(m.group(1)), "FASTPATH", bdev_fastpath(bdev)) != "0" and o[1] + o[2] != kv["cqes_consumed"]:
                r.append("NX_TILE %s SSD posted %d != engine cqes_consumed %d" % (tag, o[1] + o[2], kv["cqes_consumed"]))
    if env.get("SPDK_INV_EXPECT_RETRIES") == "1" and err_total == 0:
        r.append("NX_TILE SPDK_INV_EXPECT_RETRIES=1 but rec_err_parked+rec_err_final total 0 (retry path did not run)")
    # per RING_END; the per-ring sums: the record identity (any point) and one FLUSH per private route (non-fault)
    hpriv = set(m.group(2) for m in hq if m.group(10) == "1")
    hdisp = dict((m.group(1), 0) for m in hs)
    hctrl = dict((m.group(1), 0) for m in hs)                         # gid -> abort controls written (doorbell bounds)
    qring = dict((m.group(2, 3), m.group(4)) for m in hq)             # host (gid, route) -> ring
    rdisp = {}                                                        # (gid, ring) -> host dispositions written
    for m in hs:
        kv = tile_kv(m.group(5))
        dsp = sum(kv[z] for z in ("releases", "reissues") if isinstance(kv.get(z), int))
        hdisp[m.group(1)] += dsp
        hctrl[m.group(1)] += kv["ctrl"] if isinstance(kv.get("ctrl"), int) else 0
        key = (m.group(1), qring.get(m.group(1, 2)))
        rdisp[key] = rdisp.get(key, 0) + dsp
    nbind = {}                                                        # (gid, ring) -> members bound (BIND lines)
    for m in binds:
        nbind[m.group(3, 5)] = nbind.get(m.group(3, 5), 0) + 1
    msum, mdisp = {}, {}
    for m, kv in mems:
        key = m.group(3, 5)
        msum[key] = msum.get(key, 0) + sum(kv[z] for z in TILE_ENG_REC)
        mdisp[key] = mdisp.get(key, 0) + sum(kv[z] for z in TILE_ENG_DISP)
    rsum = {}
    for m in ends:
        tag = "gid=%s ring=%s" % m.group(1, 2)
        kv = tile_kv(m.group(4))
        miss = tile_missing(kv, TILE_ENG_RING_STATS + TILE_ENG_RING_TAIL)
        if miss:
            r.append("NX_TILE %s IAU_ENGINE6_RING_END missing %s" % (tag, ",".join(miss)))
            continue
        r += ["NX_TILE %s %s=%d" % (tag, z, kv[z]) for z in TILE_RING_ZERO if kv[z]]
        if not fault:
            r += ["NX_TILE %s %s=%d on a non-fault point" % (tag, z, kv[z])
                  for z in TILE_RING_NONFAULT_ZERO + TILE_RING_TEARDOWN_ZERO if kv[z]]
        tot = rsum.setdefault(m.group(1, 2), dict((z, 0) for z in ("records_emitted", "stale_gen", "bad_route", "n",
                                                                   "reserved") + TILE_RING_REC))
        for z in tot:
            tot[z] += 1 if z == "n" else kv[z]
    for key in sorted(rsum):
        tot = rsum[key]
        emitted, stale, bad_route, n_end = tot["records_emitted"], tot["stale_gen"], tot["bad_route"], tot["n"]
        tag = "gid=%s ring=%s" % key
        want = msum.get(key, 0) + sum(tot[z] for z in TILE_RING_REC)
        if not 0 <= emitted - want <= stale:
            r.append("NX_TILE %s records_emitted %d != member records + orphans %d" % (tag, emitted, want) if stale == 0
                     else "NX_TILE %s records_emitted %d - member records + orphans %d not in 0..stale_gen %d"
                     % (tag, emitted, want, stale))
        if not fault and (key[0] in hpriv or key[0] in hshared):
            # P4.8b I2, exact per (gid, ring), private and shared: the host writes one FLUSH per member incarnation
            # (flush_once(); BIND lines) and one REISSUE / RELEASE per disposition (its STATS reissues / releases);
            # the engine fetches each once, by its live member (do_flush() flushes, do_dispose() releases /
            # reissues, every other outcome a zero-required counter: bad_ctrl, rec_sqdel, rec_inval, sq_full_stalls)
            # or memberless (do_desc(): bad_route; stale_gen 0 on a non-fault point), the last member's final fetch
            # included, so nothing stays unfetched
            fb, nb, dsp = mdisp.get(key, 0) + bad_route, nbind.get(key, 0), rdisp.get(key, 0)
            if fb != nb + dsp:
                r.append("NX_TILE %s flushes+releases+reissues+bad_route %d != BIND %d + host dispositions %d (every "
                         "FLUSH / REISSUE / RELEASE the host wrote is fetched once)" % (tag, fb, nb, dsp))
    # P4.8b RELEASE (sq_eng6_cq_deleted(): the host's own Delete I/O CQ of a route detached in the current controller
    # incarnation, with that bind's gen; only listed grings, so a ring incarnation's last member, whose detach unbinds
    # and frees the gring, gets none): each names a bound member, at most one per route incarnation, rc=0 on a
    # non-fault point; per ring (non-fault) RELEASE lines == BIND - RING_END and RING_END reserved == BIND - RELEASE
    bgen = {}
    for m in binds:
        bgen.setdefault((m.group(1), m.group(2), m.group(3), m.group(5), m.group(4)), []).append(m.group(6))
    nrel, nrel_ok, relk = {}, {}, {}
    for m in rels:
        c, q, gid, ring, route, gen, rc = m.groups()
        tag = "ctrlr=%s cqid=%s gid=%s ring=%s route=%s gen=%s" % (c, q, gid, ring, route, gen)
        gens = bgen.get((c, q, gid, ring, route))
        if gens is None:
            r.append("NX_TILE IAU_ENGINE6_RELEASE %s matches no IAU_ENGINE6_BIND (ctrlr,qid,gid,ring,route)" % tag)
        elif gen not in gens:
            r.append("NX_TILE IAU_ENGINE6_RELEASE %s: its IAU_ENGINE6_BIND gen is %s" % (tag, ",".join(sorted(set(gens)))))
        if not fault and rc != "0":
            r.append("NX_TILE IAU_ENGINE6_RELEASE %s rc=%s on a non-fault point" % (tag, rc))
        relk[(gid, ring, route, gen)] = relk.get((gid, ring, route, gen), 0) + 1
        nrel[(gid, ring)] = nrel.get((gid, ring), 0) + 1
        nrel_ok[(gid, ring)] = nrel_ok.get((gid, ring), 0) + (rc == "0")
    r += ["NX_TILE IAU_ENGINE6_RELEASE gid=%s ring=%s route=%s gen=%s x%d (at most one per released route)" % (k + (n,))
          for k, n in sorted(relk.items()) if n > 1]
    for key in sorted(rsum) if not fault else ():
        nb, ne, nr = nbind.get(key, 0), rsum[key]["n"], nrel.get(key, 0)
        if nr != nb - ne:
            r.append("NX_TILE gid=%s ring=%s IAU_ENGINE6_RELEASE lines %d, expected %d (BIND %d - RING_END %d: every member "
                     "but each ring incarnation's last)" % (key + (nr, nb - ne, nb, ne)))
        if rsum[key]["reserved"] != nb - nrel_ok.get(key, 0):
            r.append("NX_TILE gid=%s ring=%s RING_END reserved %d, expected %d (BIND %d - RELEASE %d)"
                     % (key + (rsum[key]["reserved"], nb - nrel_ok.get(key, 0), nb, nrel_ok.get(key, 0))))
    # per host STATS / GROUP_END
    for m in hs:
        tag = "qid=%s gid=%s route=%s" % m.group(3, 1, 2)
        kv = tile_kv(m.group(5))
        miss = tile_missing(kv, TILE_HOST_QSTATS + TILE_HOST_STATS_TAIL)
        if miss:
            r.append("NX_TILE %s IAU_TILE_STATS missing %s" % (tag, ",".join(miss)))
            continue
        if kv["lost"]:
            r.append("NX_TILE %s lost=%d" % (tag, kv["lost"]))
        if kv["pend_left"]:
            r.append("NX_TILE %s pend_left=%d" % (tag, kv["pend_left"]))
        if kv["detached"] != 1 + kv["rejoins"]:
            r.append("NX_TILE %s detached=%d, expected %d (one R_DETACHED per route incarnation: 1 + rejoins)"
                     % (tag, kv["detached"], 1 + kv["rejoins"]))
        # P4.8b I2: error_record() counts err once per PARKED record and writes at most one REISSUE / RELEASE for it
        if kv["reissues"] + kv["releases"] > kv["err"]:
            r.append("NX_TILE %s reissues+releases %d > err %d (at most one disposition per PARKED record)"
                     % (tag, kv["reissues"] + kv["releases"], kv["err"]))
        if not expect_err and any(kv[z] for z in TILE_HOST_ERR):
            r.append("NX_TILE %s err=%d retries=%d without expected errors" % (tag, kv["err"], kv["retries"]))
        if not fault:
            r += ["NX_TILE %s %s=%d on a non-fault point" % (tag, z, kv[z]) for z in TILE_HOST_NONFAULT_ZERO if kv[z]]
        elif not reset_pt and kv["rejoins"]:      # P4.12b: only a reset rejoins; a disconnect frees its qpair
            r.append("NX_TILE %s rejoins=%d outside a reset fault point (SPDK_PERF_FAULT_RESET_MS)" % (tag, kv["rejoins"]))
    # P4.12, the P4.9 review's fast-path rule as the controller adopted it: SPDK_IAU_FAST off -> sub_fast 0 and
    # declined 0 on every qpair (the request-free path never ran: a silent-lever tripwire); on -> sub_fast > 0 on every
    # qpair and, on a non-fault point whose queue depth per qpair (qd_q, perf's -q over -P) is <= the qpair's credits
    # (its last QPAIR line), declined 0 and sub_req 0 (every IO request-free); a qpair above its credits is "mixed"
    # (DV9: the overflow declines to the request path), reported with the point's request-free share
    qcred = dict((m.group(2, 3), int(m.group(6))) for m in hq)
    fast_on, fsum, rsum_req, mixed = env.get("SPDK_IAU_FAST") == "1", 0, 0, False
    for m in hs:
        kv = tile_kv(m.group(5))
        if tile_missing(kv, ("sub_fast", "sub_req", "declined")):
            continue
        tag = "qid=%s gid=%s route=%s" % m.group(3, 1, 2)
        if not fast_on:
            if kv["sub_fast"] or kv["declined"]:
                r.append("NX_TILE %s sub_fast=%d declined=%d without SPDK_IAU_FAST=1 (the request-free path ran)"
                         % (tag, kv["sub_fast"], kv["declined"]))
            continue
        fsum, rsum_req = fsum + kv["sub_fast"], rsum_req + kv["sub_req"]
        if kv["sub_fast"] == 0:
            r.append("NX_TILE %s sub_fast=0 with SPDK_IAU_FAST=1 (no request-free IO)" % tag)
        cred = qcred.get(m.group(1, 2))
        over = qd_q is not None and cred is not None and qd_q > cred
        rare = fast_rare(kv["declined"] + kv["sub_req"], kv["sub_fast"] + kv["sub_req"])   # P4.13-lite, as NX_S2
        mixed = mixed or over or not rare   # P4.12b: the label follows the evidence too
        if not fault and qd_q is not None and cred is not None and not over and not rare:
            r.append("NX_TILE %s declined=%d sub_req=%d with SPDK_IAU_FAST=1 and the queue depth per qpair %d <= credits "
                     "%d" % (tag, kv["declined"], kv["sub_req"], qd_q, cred))
    if extras is not None and fast_on:
        if fsum + rsum_req:
            extras["fast_share"] = "%.4f" % (fsum / (fsum + rsum_req))
        extras["fast_label"] = "mixed" if mixed else "request-free"
    # GROUP_END: printed at every member leave with group-cumulative counters (P4.8 deviation), so the LAST line per
    # gid holds the group's totals; a counter that decreases from one of the gid's lines to the next FAILs (the line
    # is not cumulative and its last value no total). kept is a state, not a counter (it may go 1 -> 0): its last line
    # only (m4)
    gend, nend = {}, {}                             # gid -> its last GROUP_END line's counters / its GROUP_END lines
    for m in he:
        kv = tile_kv(m.group(2))
        nend[m.group(1)] = nend.get(m.group(1), 0) + 1
        miss = tile_missing(kv, TILE_HOST_END_KEYS + (TILE_HOST_END_TAIL if p48 else ()))
        if miss:
            r.append("NX_TILE gid=%s IAU_TILE_GROUP_END missing %s" % (m.group(1), ",".join(miss)))
            continue
        # stray: a record with no member (a LOST or removed member's late record) only on a fault point
        if not fault and kv["stray"]:
            r.append("NX_TILE gid=%s stray=%d on a non-fault point" % (m.group(1), kv["stray"]))
        prev = gend.get(m.group(1), {})
        r += ["NX_TILE gid=%s IAU_TILE_GROUP_END %s=%d after %d (not group-cumulative)" % (m.group(1), z, kv[z], prev[z])
              for z in TILE_HOST_END_KEYS + TILE_HOST_END_TAIL
              if z != "kept" and isinstance(kv.get(z), int) and kv[z] < prev.get(z, 0)]
        gend[m.group(1)] = dict((z, v) for z, v in kv.items() if isinstance(v, int))
    for gid in sorted(gend, key=int):
        if not fault and gend[gid].get("kept"):
            r.append("NX_TILE gid=%s kept=%d on a non-fault point (a LOST member's memory kept)" % (gid, gend[gid]["kept"]))
    if not fault:   # m2: one GROUP_END per member leave
        r += ["NX_TILE gid=%s IAU_TILE_GROUP_END lines %d, expected %d (one per IAU_TILE_QPAIR of the gid)"
              % (gid, nend.get(gid, 0), nmem.get(gid, 0)) for gid in sorted(set(nend) | set(nmem), key=int)
              if nend.get(gid, 0) != nmem.get(gid, 0)]
    # P4.8b I3 doorbell bounds (perf's delay_cmd_submit assumed; interrupt mode FAILs in check_cmd). A doorbell rings
    # a ring whose D tail moved since its last ring, at the end of a poll of it (group_pass(), the group pass, a
    # direct poll; at most once per ring per poll) or in flush_once() / a drain loop. The tail moves by a submit: in a
    # callback, so inside a BUSY poll (one that consumed / delivered records), by the attention or generic
    # resubmission of a queued request (stat queued_requests counts every refusal), by a disposition (ctrl()), by a
    # FLUSH (one per member), or by perf's initial fill (once per ring). Each busy poll dirties at most R rings, R =
    # the rings its callbacks can reach: K for a shared group (one pass delivers every member's records); for
    # private rings the ns_ctx's qpairs, because perf picks the qpair round-robin per submission (perf.c
    # nvme_submit_io(): last_qpair), so a completion consumed on one qpair dirties another's ring, which its next,
    # possibly idle, poll rings. Hence doorbells <= R x (polls - idle_polls) + rings + queued + members + host
    # dispositions, per perf PCIE block (the poll group's stats during the run, every mode 6 point) and per shared
    # gid's last GROUP_END (teardown included, R = K). P4.8e: a shared pass also rings all K dirty rings after every
    # full chunk of db_batch = N records of a ring (shared_pass()); a ring with r records has at most floor(r / N)
    # full chunks, so a pass adds <= K x floor(records / N) and the run <= K x floor(the group's records / N) (the
    # sum of floors <= the floor of the sum; records = its last GROUP_END records=, every consume counted, drains
    # included, which have no chunks: an upper bound). N = 0: no mid-pass doorbells. A PCIE block cannot be mapped
    # to its gid from the logs (GROUP lines name no lcore), so its term uses every shared gid's records: exact with
    # one perf worker, an upper bound with several
    blocks = pcie_blocks(hlines)
    # every mode 6 point: exactly one complete PCIE block per perf poll group (one per worker with the worker group,
    # else one per ns_ctx; --transport-stats), else the doorbell bound has nothing to bound (a missing line FAILs)
    full = [b for b in blocks if all(z in b for z in ("polls", "idle_polls", "sq_mmio"))]
    if len(blocks) != n_pg or len(full) != len(blocks):
        r.append("NX_TILE PCIE transport blocks %d (complete: polls, idle_polls, sq_mmio %d), expected %d (one per perf "
                 "poll group)" % (len(blocks), len(full), n_pg))
    queued = sum(b.get("queued_requests", 0) for b in blocks)
    disp_all = sum(hdisp.values())
    # P8.H1-dbg: an ABORT_TAG / ABORT_COOKIE control (D10) moves the D tail outside every busy poll: perf's timeout
    # callback aborts by cid from the R_TIMEOUT record's slow_record(), and abort_q_issue() sends a control queued behind
    # the two control credits from the ack record's, in passes that deliver no IO completion, which the group poll counts
    # idle (n_for == 0); so each abort control the host wrote may need its own doorbell. It comes back exactly once, as
    # its ack record or the CLOSED walk's ack(0) (end_unfetched()), counted in the host STATS ctrl= (a route's drain
    # waits for ctrl_inflight 0; only a LOST route or a removed controller ends controls unacked, and LOST FAILs above)
    ctrl_all = sum(hctrl.values())
    wgq = [int(m.group(3)) for x in hlines for m in [WGROUP.search(x)] if m] if wgroup else []
    qctx = qpairs // n_ctx if n_ctx and qpairs % n_ctx == 0 else qpairs
    k_grp = int(want_k) if group_on and str(want_k).isdigit() else 1
    mid_all = tile_db_mid_pass(k_grp, sum(gend[g].get("records", 0) for g in hshared & set(gend)), n_db) \
        if group_on else 0
    for i, b in enumerate(blocks):
        if "polls" not in b or "idle_polls" not in b or "sq_mmio" not in b:
            continue
        busy = b["polls"] - b["idle_polls"]
        rb, ra = (k_grp, k_grp) if group_on else \
            (qctx, (wgq[i] if len(wgq) == len(blocks) else qpairs) if wgroup else qctx)
        bound = rb * busy + mid_all + ra + b.get("queued_requests", 0) + qpairs + disp_all + ctrl_all
        if b["sq_mmio"] > bound:
            r.append("NX_TILE PCIE block %d sq_mmio %d > R x (polls - idle_polls) + K x floor(records / db_batch) + rings "
                     "+ queued + members + host dispositions + abort controls = %d (R=%d: one doorbell per ring a busy "
                     "poll or a full chunk can dirty; abort controls %d)" % (i, b["sq_mmio"], bound, rb, ctrl_all))
    for gid in sorted(hshared & set(gend), key=int):
        d, k = gend[gid], gk[gid][0]
        if "idle_polls" not in d:
            continue
        bound = k * (d.get("polls", 0) - d["idle_polls"]) + tile_db_mid_pass(k, d.get("records", 0), n_db) + k + \
            queued + nmem.get(gid, 0) + hdisp.get(gid, 0) + hctrl.get(gid, 0)
        if d.get("doorbells", 0) > bound:
            r.append("NX_TILE gid=%s doorbells %d > K x (polls - idle_polls) + K x floor(records / db_batch) + K + queued "
                     "+ members + host dispositions + abort controls = %d (one doorbell per ring a busy poll or a full "
                     "chunk can dirty; abort controls %d)" % (gid, d["doorbells"], bound, hctrl.get(gid, 0)))
    # m3 (A10): every engine line of a family the rules rely on parsed (a format change FAILs, not passes)
    for name, rx in TILE_ENG_RAW:
        raw = sum(1 for x in tl if rx.search(x))
        got = len({"IAU_ENGINE6_RING": rings, "IAU_ENGINE6_BIND": binds, "IAU_ENGINE6_RING_END": ends,
                   "IAU_ENGINE6_RELEASE": rels}[name])
        if raw != got:
            r.append("NX_TILE %s lines %d, parsed %d (an unparsed format: A10)" % (name, raw, got))
    # clean teardown (non-fault): no ring dropped, no failed remap, no failed host Delete I/O CQ
    if not fault:
        td = {}
        for x in tl:
            for m in ENG6_TEARDOWN.finditer(x):
                name = "v6 ring dropped" if m.group(1) else "v6 ring remap failed"
                td[name] = td.get(name, 0) + 1
        r += ["NX_TILE %s in target log (x%d)" % kv for kv in sorted(td.items())]
        n = sum(1 for x in hlines if TILE_HOST_TEARDOWN.search(x))
        if n:
            r.append("NX_TILE host Delete I/O CQ failed in host log (x%d)" % n)
    # three-way exactly once, per group (P4.8b) and in total: host records == engine records (> 0) == SSD posted +
    # R_DETACHED (non-fault, fast path; the SSD == engine relations per member are above)
    hrec, erec = tile_records(hlines, tl)
    grec = tile_group_records(hlines, tl, env, bdev)
    # (I1: without a usable fp_done, no fast path or a mixed group, the SSD leg is the engine's cqes_consumed, so the
    # check never drops to two-way; the per-member SSD fetched == sqes_written stays)
    leg = lambda d: ("SSD posted", d["ssd"]) if d["ssd"] is not None else ("engine cqes_consumed", d["cqes"])
    for gid in sorted(grec, key=int):
        d = grec[gid]
        if d["host"] != d["engine"]:
            r.append("NX_TILE gid=%s host records %d != engine records_emitted %d" % (gid, d["host"], d["engine"]))
        name, v = leg(d)
        if not fault and d["host"] != v + d["detached"]:
            r.append("NX_TILE gid=%s host records %d != %s %d + R_DETACHED %d" % (gid, d["host"], name, v, d["detached"]))
    if hrec != erec:
        r.append("NX_TILE host records %d != engine records_emitted %d" % (hrec, erec))
    if not fault and grec:
        legs = [leg(d) for d in grec.values()]
        name = legs[0][0] if len(set(n for n, _ in legs)) == 1 else "SSD posted / engine cqes_consumed"
        v, det = sum(x for _, x in legs), sum(d["detached"] for d in grec.values())
        if hrec != v + det:
            r.append("NX_TILE host records %d != %s %d + R_DETACHED %d" % (hrec, name, v, det))
    if hrec == 0 and erec == 0:
        r.append("NX_TILE zero records (zero-IO point)")
    # a fault knob's evidence without ITS env: the knob was on silently
    for name, rx, pfx in (("IAU_ENG6_FAULT", ENG6_FAULT, "IAU_ENG_FAULT_"), ("DEV_FAULT", DEV_FAULT, "IAU_DEV_FAULT_"),
                          ("DEV_FAULT_STATS", DEV_FAULT_STATS, "IAU_DEV_FAULT_")):
        if any(rx.search(x) for x in tl) and not any(k.startswith(pfx) for k in env):
            r.append("NX_TILE fault knob line %s without an %s* env" % (name, pfx))
    if extras is not None:
        extras["tile_records"] = str(hrec)
        if peaks:
            extras["peak_slots"] = str(max(int(m.group(2)) for m in peaks))
        # P4.8b: the shared groups' own idle_polls / polls (group passes + direct polls, teardown included; NOT CPU
        # idle); informational next to perf's host_idle_frac, which stays the A16 source
        gp = [gend[g] for g in sorted(hshared & set(gend)) if "idle_polls" in gend[g] and "polls" in gend[g]]
        if group_on and gp and sum(d["polls"] for d in gp) > 0:
            extras["group_idle_frac"] = "%.4f" % (sum(d["idle_polls"] for d in gp) / sum(d["polls"] for d in gp))
        # I3, informational: perf's doorbells per busy poll over its PCIE blocks (1.0 for one ring per busy poll)
        bb = [b for b in blocks if "sq_mmio" in b and "polls" in b and "idle_polls" in b]
        if bb and sum(b["polls"] - b["idle_polls"] for b in bb) > 0:
            extras["db_per_busy_poll"] = "%.4f" % (sum(b["sq_mmio"] for b in bb)
                                                   / sum(b["polls"] - b["idle_polls"] for b in bb))
    return r


def check_engine_timeouts(tl):
    """P3.9: a target log with IAU_ENGINE_DETACH_TIMEOUT / IAU_ENGINE_BIND_TIMEOUT (an engine thread that did not
    answer a bind or unbind within 1 s: a leaked queue) or the transport-destroy "queues still bound" error FAILs
    the point in any mode. No target log (or no such line) is no finding."""
    hits = {}
    for x in tl or []:
        m = ENGINE_TIMEOUT.search(x)
        if m:
            k = "IAU_ENGINE_BIND_TIMEOUT" if "BIND" in m.group(0) else \
                "IAU_ENGINE_DETACH_TIMEOUT" if "DETACH" in m.group(0) else "IAU engine queues still bound at transport destroy"
            hits[k] = hits.get(k, 0) + 1
    return ["%s in target log (x%d)" % (k, n) for k, n in sorted(hits.items())]


def check_wgroup(hlines, wgroup, workers, n_ctx, qpairs):  # qpairs = per ns_ctx
    """P3: SPDK_PERF_WORKER_GROUP=1 -> one PERF_WORKER_GROUP end line and one pcie_stats block per worker, the
    ns_ctx summing to n_ctx and the qpairs to qpairs * n_ctx; otherwise no such line and (when any pcie_stats
    line exists) one block per ns_ctx."""
    wg = [m for m in (WGROUP.search(x) for x in hlines) if m]
    nst = sum(1 for x in hlines if PCIE_STATS.search(x))
    reasons = []
    dfail = [m.group(1) for m in (WGROUP_DESTROY_FAIL.search(x) for x in hlines) if m]
    if dfail:
        reasons.append("PERF_WORKER_GROUP group destroy failed lcore=%s" % ",".join(dfail))
    if not wgroup:
        if wg:
            reasons.append("PERF_WORKER_GROUP lines without SPDK_PERF_WORKER_GROUP=1")
        if nst and nst != n_ctx:
            reasons.append("pcie_stats blocks %d, expected %d (one per ns_ctx)" % (nst, n_ctx))
        return reasons
    if len(wg) != workers:
        reasons.append("PERF_WORKER_GROUP lines %d, expected %d" % (len(wg), workers))
    if wg and sum(int(m.group(2)) for m in wg) != n_ctx:
        reasons.append("PERF_WORKER_GROUP ns_ctx %d, expected %d" % (sum(int(m.group(2)) for m in wg), n_ctx))
    if wg and sum(int(m.group(3)) for m in wg) != qpairs * n_ctx:
        reasons.append("PERF_WORKER_GROUP qpairs %d, expected %d" % (sum(int(m.group(3)) for m in wg), qpairs * n_ctx))
    if nst != workers:
        reasons.append("pcie_stats blocks %d, expected %d (one per worker group)" % (nst, workers))
    return reasons


def check_log(path, env, mode, qpairs, tgt_log=None, app_cpus="0", controllers=1, expect_md=False,
              engine_cpus="", p3_tgt=False, bdev="fastpath", variant="", qd=None):
    """Returns (reason string, extras dict with doorbells/flush_idle).
    qpairs is per ns_ctx: perf (spdk_nvme_perf.c associate_workers_with_ns, default association; the
    harness never passes --use-every-core) creates n_ctx = max(namespaces, workers) ns_ctx, one
    namespace per controller, each with its own poll group and that many IO qpairs, so the expected
    marker count is qpairs * n_ctx. Balance stays per qid (qids may repeat across controllers;
    construct and destroy counts then repeat equally)."""
    workers = max(1, cpu_count(app_cpus))
    n_ctx = max(workers, controllers)
    wgroup = env.get("SPDK_PERF_WORKER_GROUP") == "1"
    qpairs_ctx = qpairs
    qpairs = qpairs * n_ctx
    extras = dict(EXTRAS0)
    if not os.path.isfile(path):
        return "log missing: %s" % path, extras
    with open(path, errors="replace") as fh:
        text = fh.read()
    low = text.lower()
    hlines = [x.strip() for x in text.splitlines()]
    # P4.12: on a fault point (SPDK_PERF_FAULT=1 with perf's evidence) perf's "completed with error" is a counted
    # outcome (NX_FAULT judges the statuses); every other error text still FAILs
    fault_pt = tile_fault_point(env, hlines)
    reasons = [b for b in BAD if b in low and not (fault_pt and b in FAULT_OK_BAD)]
    cons, dest = {}, {}
    for line in hlines:
        m = MARK.search(line)
        if not m or "*error*" in line.lower():
            continue
        fam, qid, rest = m.group(1), m.group(2), m.group(3)
        tgt = dest if DESTROY.match(rest) else cons
        tgt.setdefault(fam, {}).setdefault(qid, 0)
        tgt[fam][qid] += 1
    fams = set(cons) | set(dest) | set(EXPECT)
    for fam in sorted(fams):
        if fam in EVIDENCE_ONLY:
            continue
        c, d = cons.get(fam, {}), dest.get(fam, {})
        nodestroy = fam in NO_DESTROY
        bad = [] if nodestroy else [q for q in sorted(set(c) | set(d)) if c.get(q, 0) != d.get(q, 0)]
        if bad:
            reasons.append("%s imbalance qid=%s" % (fam, ",".join("%s(%d/%d)" % (q, c.get(q, 0), d.get(q, 0)) for q in bad)))
        n = sum((c if nodestroy else d).values())
        want = qpairs if (fam in EXPECT and EXPECT[fam](env, mode)) else 0
        if n != want:
            reasons.append("%s %s lines %d, expected %d" % (fam, "construct" if nodestroy else "destroy-ok", n, want))
    tl = read_lines(tgt_log) if tgt_log and os.path.isfile(tgt_log) else None
    for fam, evar in TGT_FAMILIES.items():
        reasons += check_sizes(fam, evar, hlines, env, tl, tgt_log)
    reasons += check_idle(env, tl, tgt_log)
    reasons += check_host_values(hlines, env, mode)
    reasons += check_flags(hlines, env, mode, qpairs)
    reasons += check_md(hlines, env, mode, qpairs, expect_md)
    reasons += check_trace_fast(hlines, env, n_ctx)
    reasons += check_gring(hlines, env, mode, tl, tgt_log, workers if wgroup else n_ctx)
    if wgroup and EXPECT["INV_GRING"](env, mode):
        reasons.append("INV_GRING with SPDK_PERF_WORKER_GROUP=1 is not supported")
    reasons += check_wgroup(hlines, wgroup, workers, n_ctx, qpairs_ctx)
    reasons += check_devmodel(env, tl, tgt_log, controllers, p3_tgt or mode in (5, 6), bdev)
    reasons += check_fpshare(env, tl, tgt_log, qpairs, controllers, bdev)
    reasons += check_engine(hlines, mode, tl, tgt_log, qpairs, engine_cpus, controllers, env, extras, bdev)
    qd_q = qd_per_qpair(qd, qpairs_ctx)
    reasons += check_tile(hlines, mode, tl, tgt_log, qpairs, engine_cpus, controllers, env, extras, bdev, app_cpus,
                        variant, qd_q)
    reasons += check_fault(hlines, env, mode, controllers, extras)
    reasons += check_s2(hlines, env, mode, qpairs, tl, controllers, qd_q, extras)
    reasons += check_idle_time(hlines, wgroup, workers, extras, variant)
    reasons += check_compute(hlines, variant)
    reasons += check_engine_timeouts(tl)
    db = [int(m.group(1)) for m in (SPIN_DEST.search(x) for x in hlines) if m]
    if db:
        extras["doorbells"] = str(sum(db))
    fi = [int(m.group(1)) for x in (tl or []) if "IAU_STATS" in x for m in [FLUSH_IDLE.search(x)] if m]
    if fi:
        extras["flush_idle"] = str(sum(fi))
    idle = host_idle_frac(hlines)
    if idle is not None:
        extras["host_idle_frac"] = "%.4f" % idle
    return "; ".join(reasons), extras


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("campaign_dir")
    ap.add_argument("--out")
    ap.add_argument("--expect-md", action="store_true",
                    help="F3 builds: a mode 4 or 5 point whose log has no MD_TRFREE/MD_TRACKERS line FAILs")
    ap.add_argument("--p3-tgt", default="", help="comma list of target variants that must print the P3 DEV_MODEL evidence")
    a = ap.parse_args()
    p3set = set(x for x in a.p3_tgt.split(",") if x)
    with open(os.path.join(a.campaign_dir, "points.csv")) as fh:
        points = list(csv.DictReader(fh))
    ok_rows = {}
    with open(os.path.join(a.campaign_dir, "ledger.csv")) as fh:
        for r in csv.DictReader(fh):
            if r.get("status") == "ok":
                ok_rows[r["point_id"]] = r
    out, nok, nfail = [], 0, 0
    for p in points:
        pid = p["point_id"]
        if pid not in ok_rows:
            continue
        rd = ok_rows[pid].get("run_dir") or ""
        extras = dict(EXTRAS0)
        read_pct = ""
        if not rd or not os.path.isabs(rd) or not os.path.isdir(rd):
            reason = "run_dir relative or missing: %r" % rd
        else:
            st = stem(p["qd"], p["run_id"], p["lat"])
            log = os.path.join(rd, "logs", "run_%s.log" % st)
            tgt = os.path.join(rd, "logs", "nvmf_tgt_%s.log" % st)
            reason, extras = check_log(log, parse_env(p.get("env")), int(p["mode"]), int(p["qpairs"]), tgt,
                                       p.get("app_cpus", "0"), int(p.get("controllers") or 1), a.expect_md,
                                       p.get("engine_cpus") or "", (p.get("tgt_variant") or p["variant"]) in p3set,
                                       p.get("bdev") or "fastpath", p.get("variant") or "", p.get("qd"))
            cmd = check_cmd(os.path.join(rd, "logs", "cmd_%s.txt" % st), p)
            opc, read_pct = check_opc(tgt, p, int(p["mode"]))
            reason = "; ".join(x for x in (reason, cmd, opc) if x)
        v = "FAIL" if reason else "ok"
        nfail += v == "FAIL"
        nok += v == "ok"
        row = dict(extras, point_id=pid, arm=p.get("arm", ""), cell=p.get("cell", ""), mode=p["mode"], verdict=v,
                   reason=reason, read_pct=read_pct)
        out.append([row[c] for c in OUT_COLS])
    fh = open(a.out, "w", newline="") if a.out else sys.stdout
    w = csv.writer(fh, lineterminator="\n")
    w.writerow(OUT_COLS)
    w.writerows(out)
    if a.out:
        fh.close()
    print("CHECK_L ok=%d fail=%d" % (nok, nfail), file=sys.stderr)
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
