/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.3: the IAU tile engine core (spec §3.2-§3.4; P4 plan rev 2 D3, D6-D13, D17, D20 as amended by
 * A1, A10, A15). Pure C, no SPDK runtime: the testbed's stand-in for the tile's SSD-facing engine on IAU host
 * interface tile (wire formats: spdk/include/spdk_internal/iau_tile_abi.h, the one definition).
 *
 * Objects.
 * - struct iau_eng6_ring: one ring pair k of one group (GCB): the host's 64 B (or 48 B) descriptor ring D_k
 *   with its doorbell (GCB db[k].d_tail) and the host's 32 B record ring R_k. Route r is served by ring
 *   r mod K (D3, fixed when the route connects); the ring's engine thread is the single consumer of D_k and
 *   the single producer of R_k (D20 condition: one producer per ring).
 * - struct iau_eng6_member: one route (one SSD I/O SQ/CQ pair) of a ring. It snapshots its configuration
 *   (credits, nsid, timeout_us, entries, lba_shift, member gen) at bind (V9); nothing is re-read from the
 *   GCB or QCFG2 afterwards. It owns the route's slot pool: entries slot records (struct iau_eng6_slot,
 *   the D17 record), a FIFO of free slots, one 256 B-aligned PRP list per slot (IAU_TILE_PRP_MAX entries) and
 *   the host's echo slab (entries x 64 B, D8). Per-member arrays are a testbed simplification of the
 *   tile's one shared slot pool (DV16); STATS report peak_inflight per member and peak_slots per ring
 *   (Little's-law evidence for P5).
 *
 * Threads and lifetime. A ring and its members belong to one engine thread: every call below except
 * iau_eng6_ring_init(), iau_eng6_member_init() and iau_eng6_member_fini() (on a member not in a ring) runs on
 * that thread (P4.4 sends messages with the P3 handshake). Nothing here locks.
 *   ring_init -> member_init -> member_add (bind) -> ring_poll ... -> [member_stop] -> member_detach (GONE:
 *   the member leaves the ring, its route's need stays reserved) -> fmt_stats -> member_fini ... and later
 *   route_release (frees the reservation).
 * member_stop() is phase 1 of the V9 two-phase detach (controller disable, before the IAU page is zeroed);
 * member_detach() is phase 2 (inside delete_sq_done(), on the engine thread, after the SSD stopped serving
 * the SQ). A member may be detached without a stop. iau_eng6_ring_remap() repoints a ring at the same host
 * memory mapped through another controller (owner loss).
 *
 * Reservations (the reap rule, review I2; P4.4 implements the trigger). A GONE route keeps its route_need_d /
 * route_need_r reserved in the ring (resv[route] = its credits, resv_gen[route] = its member gen) until
 *   (a) the host's own Admin Delete I/O CQ of that route's CQ (D11 step 4, after the host's drain):
 *       iau_eng6_route_release(r, route, gen) with the gen of the incarnation it deletes (H1: a late release of
 *       an older incarnation is -ESTALE and frees nothing);
 *   (b) the same route binds again (a rejoin with a new member gen replaces the reservation, member_add); or
 *   (c) the ring is reset (all members gone: the host re-creates the ring with zeroed memory and positions,
 *       and P4.4 inits a new ring).
 * A target-internal CQ deletion (CC.EN disable, reset / shutdown) does NOT release it. Until then no other route
 * can bind into that need (-ENOSPC): the GONE route's host may still have descriptors and records in flight.
 * A route deleted target-internally that its host never rejoins keeps its reservation until the ring is reset
 * (fail-safe). Option for P4.4 / P4.8: release it once the host clears that route's GCB member line after its
 * drain (valid 0), which the host does only after the preconditions below.
 * Host preconditions (P4.5 / P4.8 implement them; the engine cannot check them): a rejoin (b) and a release
 * (a) of a route require that route's queue_depth == 0, ctrl_inflight == 0 and GCB d_head >= the D position
 * just after the route's last descriptor (RELEASE / REISSUE / FLUSH emit no record but occupy D slots), so no
 * descriptor or record of the old incarnation is still in flight. After IAU_TILE_LOST (the 1 s drain timeout)
 * the host never rejoins or releases that route: it is terminal until a ring reset.
 * A private ring (its last member detaches with emit, P4.5 fix A): every descriptor written before the
 * detach-time d_tail gets exactly one record before R_DETACHED, and R_DETACHED is the ring's last record (the
 * final fetch in member_detach(); the ring leaves its engine in the same step) and carries rflags CLOSED; the
 * host ends the commands still in flight when it reads a CLOSED R_DETACHED (their descriptors came after that
 * d_tail), and its rejoin is a ring reset (new memory positions, d_seq0 = r_seq0 = 0), which replaces the
 * d_head precondition.
 * Stale descriptors (H3): every descriptor carries the route's member gen in d0 cflags [15:13]; one whose gen
 * differs from the bound member's is handled as for a route without a member (IO / RAW: one final
 * ABORTED_SQ_DELETION, tag NONE; ABORT_*: ack 0; REISSUE / RELEASE / FLUSH ignored) and counted in stale_gen
 * (not in orphan_io / orphan_ack / bad_route): an old incarnation's unfetched descriptor never reaches a
 * rejoined route's SSD queue. stale_gen is 0 when the host keeps the preconditions above.
 *
 * Route (member) states:
 *   ACTIVE  --FLUSH-->        FLUSHED  parked slots disposed (final ABORTED_SQ_DELETION each)
 *   ACTIVE  --member_stop-->  STOPPED  parked slots kept (the host holds their PARKED records)
 *   STOPPED --FLUSH-->        FLUSHED  parked slots disposed
 *   any but GONE --member_detach--> GONE   (out of the ring; the route's need stays reserved)
 * Only ACTIVE issues SQEs: on a FLUSHED or STOPPED route a new IO / RAW gets an immediate final
 * ABORTED_SQ_DELETION (tag NONE), a REISSUE of a parked slot a final ABORTED_SQ_DELETION, an ABORT_* the
 * ack 0, an error CQE a final error record (never parked); in-flight commands complete normally. A FLUSH
 * is honoured once per route (D7); a second is bad_ctrl. An IO or RAW for a route that is GONE or has no
 * member gets a final ABORTED_SQ_DELETION (orphan_io), an ABORT_* the ack 0 (orphan_ack); REISSUE,
 * RELEASE and FLUSH there are ignored (bad_route).
 *
 * Slot states: FREE --IO/RAW issued--> BUSY --success CQE--> final CPL --> FREE
 *   BUSY --CQE of an abort-marked slot (any status)--> final ABORTED_BY_REQUEST (DNR) --> FREE
 *   BUSY --error CQE, route ACTIVE--> PARKED record (rflags PARKED; IO kind: the SQE rebuilt from the replay
 *        fields into echo[slot]) --> PARKED
 *   BUSY --error CQE, route not ACTIVE--> final error record --> FREE
 *   BUSY --error CQE of a fused command (fuse != 0; IO kind or RAW)--> final error record --> FREE (DV21:
 *        fused commands are not retried, the error is delivered: a re-issued FIRST alone would break the pair)
 *   PARKED --REISSUE (IO kind, ACTIVE)--> BUSY (same tag, SQE rebuilt from the replay, retries + 1, ts32 and
 *        notified kept)
 *   PARKED --REISSUE (route not ACTIVE)--> final ABORTED_SQ_DELETION --> FREE
 *   PARKED --REISSUE of a RAW slot--> bad_ctrl + final ABORTED_SQ_DELETION --> FREE (RAW is never replayed:
 *        the host retries a RAW itself, RELEASE + a new RAW from req->cmd, D8)
 *   PARKED --RELEASE--> FREE (no record: the PARKED record is the command's last; the host writes RELEASE
 *        before its callback, D8)
 *   PARKED --FLUSH--> final ABORTED_SQ_DELETION --> FREE
 *   BUSY --member_detach--> final ABORTED_SQ_DELETION (dropped_busy; also the commands the device model
 *        dropped at sq_fp_free(), F5) --> FREE;  PARKED --member_detach--> FREE (parked_dropped, no record)
 * A late-abort mark is honoured whatever the route state when its CQE arrives (FLUSHED, STOPPED, the detach
 * drain), and a timed BUSY command gets its notice on FLUSHED and STOPPED routes too (it is still at the SSD).
 * ABORT_COOKIE / ABORT_TAG (late abort, C1/DV5) mark BUSY slots only (abort_pending; never a PARKED one;
 * ABORT_TAG checks the tag's gen; a slot already marked is not counted again) and are acked at once:
 * cdw0 = slots marked, tag NONE, rflags ACK. TIMEOUT (D9): a BUSY slot of a timed command (cflags class
 * IAU_TILE_CF_TMO on a route with timeout_us != 0) whose (now32 - ts32) >= timeout_us (u32 microseconds,
 * wrap-safe) gets one R_TIMEOUT notice per command lifetime (notified survives REISSUE); the scan runs every
 * IAU_ENG6_TMO_SCAN_US.
 * Guarantee (C2): every credit-charged descriptor (IO, RAW, ABORT_*) gets exactly one final record, or for an
 * error a PARKED record that the host disposes (REISSUE -> a later final, RELEASE -> none); nothing follows a
 * final record for that tag. On a shared ring (other members stay) only the orphan records (tag NONE) of
 * descriptors written before the host saw R_DETACHED follow it (A15: the host's queue_depth == 0 drain covers
 * them). On a ring whose LAST member detaches with emit (a private ring, P4.5 fix A): every descriptor written
 * before the detach-time d_tail gets exactly one record BEFORE R_DETACHED, and R_DETACHED is the ring's last
 * record (the final fetch in member_detach(), the unbind in the same engine message); a descriptor written
 * after that d_tail is never fetched, and the host ends it when it reads R_DETACHED. Slots in use (BUSY + PARKED)
 * never exceed credits at a fetch (A1 / D13: an unfetched RELEASE may follow a credit the host already
 * returned); the engine stalls the ring instead (credit_stalls, a tripwire that is 0 on every non-fault
 * point), as it does for a full SSD SQ (sq_full_stalls, likewise 0).
 *
 * Tags (D17): gen [15:12] | slot [11:0]; entries <= IAU_TILE_MAX_SLOTS (4095), so no tag is TAG_NONE. Every slot's
 * gen starts at the member gen (so a rejoined route's first tags differ from its last incarnation's), advances
 * when the slot is freed, and the free slots are a FIFO (handed out 0, 1, 2, ...): a tag recurs only after
 * 16 x entries allocations, the widest ABA distance for ABORT_TAG / REISSUE / RELEASE and a stray CQE
 * (bad_cid).
 * Records: w3.sqhd is the engine's D index when the record was written: informational only (the host uses the
 * GCB d_head), and always < 65536 because d_entries <= IAU_ENG6_RING_MAX.
 *
 * Sizing (A1): member_add refuses a route unless iau_tile_ring_fits(sum route_need_d, sum route_need_r,
 * d_entries, r_entries) holds over the ring's routes (-ENOSPC). Optional tripwire: with the GCB flag
 * IAU_TILE_GCB_F_RCONS, r_consumed points at GCB ring[k].r_consumed (a host-written monotonic count of records
 * consumed; the host stores it before its next d_tail store), and a record that would overwrite one the host
 * has not consumed is counted (r_overflow; still written).
 *
 * Ordering (D20; x86 TSO on WB mappings, no NT/WC stores on any publish path, one producer per ring):
 * - D ring: the engine loads d_tail with acquire semantics and reads no descriptor word before that load.
 *   The host must store its descriptors before d_tail with spdk_wmb() or a release store (P4.5).
 * - SSD SQ: the SQE words, then the SQ tail doorbell as a release store (spdk_wmb() first off x86).
 * - SSD CQ: the CQE status word with acquire semantics, then the rest; the CQ-head doorbell is a release
 *   store after the CQE loads.
 * - R ring: iau_tile_rec_publish() (w0, w1, w2, then w3 with the phase as a release store). The echo slab is
 *   written before the PARKED record that names it.
 * - GCB ring[k].d_head (the monotonic count of D slots consumed, A15) is a release store after each fetch
 *   batch, and also before any record emitted while fetching (an ack, an orphan or not-issued abort, a FLUSH
 *   disposal), so a host that sees a record also sees the D slots consumed before it: its assert
 *   "written - d_head < d_entries" holds at the A1 bound. The internal ring index is d_idx (never d_head).
 * On x86 all of these are plain moves plus compiler barriers; ThreadSanitizer sees the same orderings.
 *
 * Fused commands (D20): the SSD SQ doorbell never exposes a FUSE_FIRST without its SECOND; commands before
 * the FIRST are still rung (fuse_waits counts the holds). A FIRST whose SECOND never comes (FLUSH, stop) is
 * never rung and ends as ABORTED_SQ_DELETION at detach.
 *
 * Evidence lines (IAU_ENG6_LINE_MAX holds any; snprintf semantics, no trailing newline):
 *   IAU_ENGINE6_RING gid=<g> ring=<k> nk=<K> engine=<e> lcore=<l> d_entries=<n> r_entries=<n> stride=<64|48>
 *   IAU_ENGINE6_BIND ctrlr=<c> qid=<q> gid=<g> route=<r> ring=<k> gen=<m> engine=<e> lcore=<l> nsid=<n>
 *     credits=<n> entries=<n> timeout_us=<n>
 *   IAU_ENGINE6_STATS ctrlr=<c> qid=<q> gid=<g> route=<r> ring=<k> engine=<e> <IAU_ENG6_MEMBER_STATS key=value,
 *     in list order> inflight=<n> parked=<n> state=<active|flushed|stopped|gone> host_ssd_db=<0|1>
 *   IAU_ENGINE6_RING_END gid=<g> ring=<k> engine=<e> <IAU_ENG6_RING_STATS key=value, in list order>
 *     members=<n> in_use=<n> reserved=<routes with a reservation>
 * The STATS keys are defined once, by the two X lists below (check_l.py NX_TILE keeps static copies; the P4.6 drift
 * test tests/realcore/test_inv_check_l_tile.py greps their X(name) tokens here); print STATS after member_detach so
 * dropped_busy, parked_dropped and rec_detached are final (host_ssd_db is latched at detach).
 */
#ifndef SPDK_VFIO_USER_IAU_TILE_H
#define SPDK_VFIO_USER_IAU_TILE_H

#include "spdk/stdinc.h"
#include "spdk/nvme_spec.h"
#include "spdk_internal/iau_tile_abi.h"

#define IAU_ENG6_POLL_BUDGET	64u	/* per poll: CQEs per member, descriptors per ring (as P3) */
#define IAU_ENG6_TMO_SCAN_US	100u	/* timeout scan period (D9) */
#define IAU_ENG6_RING_MIN	4u
#define IAU_ENG6_RING_MAX	65536u		/* D / R ring entries (w3.sqhd = the D index fits 16 bits) */
#define IAU_ENG6_LINE_MAX	2048

/*
 * Member STATS counters (one line per route), in print order:
 *   io_fetched, raw_fetched  IO / RAW descriptors fetched for the route (any state; RAW counted once)
 *   reissues, releases       REISSUEs that wrote an SQE; RELEASEs that freed a parked slot
 *   aborts, flushes          ABORT_* and FLUSH descriptors fetched (any state; flushes > 1 is bad_ctrl)
 *   sqes_written, ssd_db_writes, cqes_consumed, cq_db_writes   the SSD side
 *   rec_cpl                  final success records (CQE success)
 *   rec_err_parked           PARKED error records;  rec_err_final  final error records (route not ACTIVE)
 *   rec_timeout              TIMEOUT notices
 *   rec_abort_req            final ABORTED_BY_REQUEST (late abort at the CQE)
 *   rec_sqdel                final ABORTED_SQ_DELETION (IO/RAW not issued, FLUSH disposal, REISSUE refused,
 *                            detach)
 *   rec_ack, rec_detached    ABORT_* acks; DETACHED records (1 per detached route)
 *   rec_inval                final INVALID_FIELD (an IO descriptor without an IO-kind SQE, RAW at stride 48)
 *   marked                   slots marked abort_pending (the sum of the acks' cdw0)
 *   bad_cid, bad_sqhd        CQE for no BUSY slot of that gen; CQE sqhd >= sq_entries
 *   bad_ctrl                 REISSUE / RELEASE of a tag that is not PARKED (or wrong gen), REISSUE of a RAW
 *                            slot, a second FLUSH
 *   injected                 errors injected by the fault knob
 *   dropped_busy, parked_dropped   slots BUSY / PARKED at detach
 *   credit_stalls, sq_full_stalls  tripwires (0 unless the host breaks its credits or the SSD its SQHD)
 *   fuse_waits               SQ doorbells held back for an open fused pair
 *   peak_inflight            max slots in use (BUSY + PARKED)
 * Relation (after detach): sqes_written == cqes_consumed - bad_cid + dropped_busy (each SQE ends in exactly
 * one CQE or a drop); sqes_written - reissues = the IO / RAW descriptors issued.
 */
#define IAU_ENG6_MEMBER_STATS(X) \
	X(io_fetched) X(raw_fetched) X(reissues) X(releases) X(aborts) X(flushes) \
	X(sqes_written) X(ssd_db_writes) X(cqes_consumed) X(cq_db_writes) \
	X(rec_cpl) X(rec_err_parked) X(rec_err_final) X(rec_timeout) X(rec_abort_req) X(rec_sqdel) \
	X(rec_ack) X(rec_detached) X(rec_inval) X(marked) \
	X(bad_cid) X(bad_sqhd) X(bad_ctrl) X(injected) X(dropped_busy) X(parked_dropped) \
	X(credit_stalls) X(sq_full_stalls) X(fuse_waits) X(peak_inflight)
/*
 * Ring STATS (RING_END line):
 *   desc_fetched     descriptors processed (a RAW counts once);  d_slots  D slots consumed (a RAW counts 2)
 *   records_emitted  records written to R
 *   bad_db           polls that read d_tail >= d_entries (any value > 0 is the failure)
 *   bad_route        REISSUE / RELEASE / FLUSH for a route without a live member; NOT a tripwire: a clean
 *                    teardown produces it (dispositions written before the host saw R_DETACHED)
 *   bad_kind         descriptor kinds 7..15 (no record)
 *   orphan_io, orphan_ack   IO / RAW and ABORT_* for a route without a live member (each got its record)
 *   stale_gen        descriptors (any kind) whose d0 member gen differs from the bound member's (H3; handled as
 *                    for a route without a member)
 *   raw_waits        polls that found a RAW header without its SQE slot published
 *   r_overflow       records written over one the host had not consumed (r_consumed tripwire; 0 if off)
 *   peak_slots       max slots in use over the ring's routes
 */
#define IAU_ENG6_RING_STATS(X) \
	X(desc_fetched) X(d_slots) X(records_emitted) X(bad_db) X(bad_route) X(bad_kind) \
	X(orphan_io) X(orphan_ack) X(stale_gen) X(raw_waits) X(r_overflow) X(peak_slots)

#define IAU_ENG6_STAT_FIELD(n) uint64_t n;
struct iau_eng6_mstats {
	IAU_ENG6_MEMBER_STATS(IAU_ENG6_STAT_FIELD)
};
struct iau_eng6_rstats {
	IAU_ENG6_RING_STATS(IAU_ENG6_STAT_FIELD)
};
#undef IAU_ENG6_STAT_FIELD
#define IAU_ENG6_STAT_ONE(n) +1
enum {
	IAU_ENG6_MEMBER_NSTATS = 0 IAU_ENG6_MEMBER_STATS(IAU_ENG6_STAT_ONE),
	IAU_ENG6_RING_NSTATS = 0 IAU_ENG6_RING_STATS(IAU_ENG6_STAT_ONE),
};
#undef IAU_ENG6_STAT_ONE
/* the key strings, in list order (generated from the X lists in vfio_user_iau_tile.c) */
extern const char *const iau_eng6_member_stat_keys[IAU_ENG6_MEMBER_NSTATS];
extern const char *const iau_eng6_ring_stat_keys[IAU_ENG6_RING_NSTATS];
uint64_t iau_eng6_member_stat(const struct iau_eng6_mstats *st, unsigned i);	/* value of key i */
uint64_t iau_eng6_ring_stat(const struct iau_eng6_rstats *st, unsigned i);

/* slot states */
enum { IAU_ENG6_S_FREE = 0, IAU_ENG6_S_BUSY = 1, IAU_ENG6_S_PARKED = 2 };
/* member (route) states */
enum { IAU_ENG6_M_ACTIVE = 0, IAU_ENG6_M_FLUSHED = 1, IAU_ENG6_M_STOPPED = 2, IAU_ENG6_M_GONE = 3 };
/* slot bits */
#define IAU_ENG6_B_NOTIFIED	0x01u	/* a TIMEOUT notice was emitted in this command's lifetime */
#define IAU_ENG6_B_ABORT	0x02u	/* abort_pending: the CQE becomes ABORTED_BY_REQUEST */
#define IAU_ENG6_B_TMO		0x04u	/* a timed command (cflags class) on a route with timeout_us != 0 */
#define IAU_ENG6_F_RAW		0x80u	/* slot flags: a RAW command (no replay) */

/* THE tile slot record (D17; the P5 iau_slot_pool entry), 48 B in the testbed. */
struct iau_eng6_slot {
	uint64_t cb_fn, cb_arg;	/* the record's w0 / w1 */
	uint64_t slba, buf;	/* replay: d1, d2 */
	uint32_t ts32;		/* first-issue time, us (wrap-safe compare); kept by REISSUE */
	uint16_t nlb;		/* replay: 0-based */
	uint8_t opc;		/* replay */
	uint8_t flags;		/* replay: fuse | FUA | LR (IAU_TILE_F_REPLAY_MASK); IAU_ENG6_F_RAW */
	uint8_t state;		/* IAU_ENG6_S_* */
	uint8_t gen;		/* tag gen [15:12]; advances when the slot is freed */
	uint8_t retries;	/* REISSUEs of this command (w2 [55:48]) */
	uint8_t bits;		/* IAU_ENG6_B_* */
	uint16_t route;		/* the shared pool's owner field (implicit with per-member arrays) */
	uint16_t rsvd;
};
SPDK_STATIC_ASSERT(sizeof(struct iau_eng6_slot) == 48, "the v6 slot record is 48 B");

/* engine fault knob (IAU_ENG_FAULT_ERR_*, P4.4): every err_every-th successful first-issue completion of a
 * route is reported with err_status (an error: SC or SCT != 0; phase bit 0) */
struct iau_eng6_fault {
	uint32_t err_every;	/* 0 = off */
	uint16_t err_status;
};

struct iau_eng6_ring_cfg {
	uint32_t gid;				/* the group (GCB) id, for evidence lines */
	uint16_t k, nk;				/* this ring's index and the group's K: route r is here iff r % nk == k */
	const volatile void *d;			/* D_k: d_entries x d_stride */
	uint32_t d_entries, d_stride;		/* IAU_ENG6_RING_MIN..MAX; 64 or 48 */
	const volatile uint32_t *db;		/* GCB db[k].d_tail (host-written slot index) */
	volatile uint64_t *d_head_out;		/* GCB ring[k].d_head (engine-written monotonic D count) */
	volatile void *r;			/* R_k: r_entries x 32 B */
	uint32_t r_entries;			/* IAU_ENG6_RING_MIN..MAX */
	uint64_t d_seq0, r_seq0;		/* resume positions: D slots consumed and records written so far (0 for
						 * a new group; the engine starts at slot d_seq0 % d_entries and record
						 * r_seq0 % r_entries with phase 1 ^ (r_seq0 / r_entries) & 1). The GCB
						 * d_head must already equal d_seq0 (the previous engine's last store):
						 * the engine stores d_head only after it consumes a slot */
	uint32_t gcb_flags;			/* the GCB header flags: IAU_TILE_GCB_F_* only (-EINVAL otherwise) */
	const volatile uint64_t *r_consumed;	/* GCB ring[k].r_consumed: required iff gcb_flags has
						 * IAU_TILE_GCB_F_RCONS (the r_overflow tripwire), else ignored */
};

struct iau_eng6_member_cfg {
	uint16_t route;				/* < IAU_TILE_MAX_ROUTES; route % nk == the ring's k */
	uint16_t qid;				/* the SSD I/O queue (1..IAU_TILE_QIDS - 1), for evidence lines */
	int ctrlr;				/* controller index, for evidence lines */
	uint8_t gen;				/* member gen (<= IAU_TILE_MEMBER_GEN_MASK) */
	uint8_t lba_shift;			/* 9..31 */
	uint32_t entries;			/* slots = SSD SQ entries: 2..IAU_TILE_MAX_SLOTS */
	uint32_t credits;			/* IO credits: 1..entries - 1, <= cq_entries - 1 */
	uint32_t nsid;				/* the route's namespace (!= 0) */
	uint32_t timeout_us;			/* 0 = no timeouts (D9) */
	volatile void *ssd_sq;			/* sq_entries x 64 B (== entries) */
	const volatile void *ssd_cq;		/* cq_entries x 16 B (>= 2) */
	uint32_t sq_entries, cq_entries;
	volatile uint32_t *ssd_sq_db;		/* the SSD's SQ tail doorbell */
	volatile uint32_t *ssd_cq_db;		/* the SSD's CQ head doorbell */
	const volatile uint32_t *host_sink;	/* the host's redirected CQ-head doorbell (must stay 0) */
	volatile void *echo;			/* entries x 64 B (D8): rebuilt SQEs of parked IO-kind slots */
	struct iau_eng6_fault fault;
};

struct iau_eng6_member {
	/* configuration snapshot (V9) */
	uint16_t route, qid;
	int ctrlr;
	uint8_t gen, lba_shift, state, cq_phase;
	uint32_t entries, credits, nsid, timeout_us;
	uint32_t sq_entries, cq_entries;
	struct iau_eng6_fault fault;
	/* SSD side (NULL once GONE) */
	volatile struct spdk_nvme_cmd *sq;
	const volatile struct spdk_nvme_cpl *cq;
	volatile uint32_t *sq_db, *cq_db;
	const volatile uint32_t *host_sink;
	volatile struct spdk_nvme_cmd *echo;
	uint32_t sq_tail, sq_db_last, sq_head_seen, cq_head;
	uint32_t fuse_tail;			/* the SQ tail before an open FUSE_FIRST (the highest safe doorbell) */
	uint8_t fuse_open, sq_dirty, foreign_db_latched, rsvd0;
	/* slot pool */
	struct iau_eng6_slot *slots;		/* entries */
	uint16_t *free_ring;			/* entries; FIFO of free slot indices */
	uint64_t *prp;				/* entries x IAU_TILE_PRP_MAX, 256 B-aligned per slot */
	uint32_t free_head, free_tail, n_free, n_parked;
	uint64_t first_cpls;			/* successful first-issue completions (fault knob) */
	struct iau_eng6_mstats st;
	/* P4.10b, after everything the hot paths read: the spill emulation (only the _spill poll touches them) */
	volatile uint8_t *ctx;			/* slot line i at ctx + 64 * i; NULL = off */
	uint64_t spill_stores, spill_loads, spill_sink;
};

struct iau_eng6_ring {
	uint32_t gid;
	uint16_t k, nk;
	const volatile uint8_t *d;
	const volatile uint32_t *db;
	volatile uint64_t *d_head_out;
	volatile struct iau_tile_rec *r;
	const volatile uint64_t *r_consumed;	/* NULL unless the GCB flag IAU_TILE_GCB_F_RCONS */
	bool rcons_on;
	uint32_t d_entries, stride, r_entries;
	int8_t woff[8];				/* word offset of d0..d7 in a slot (-1 = absent) */
	uint32_t d_idx;				/* next D slot (index; the GCB d_head is d_seq) */
	uint64_t d_seq;				/* D slots consumed, monotonic */
	uint64_t d_cur, d_pub;			/* fetch: d_seq after the descriptor in hand; the last d_head stored */
	uint32_t r_tail;			/* next R slot */
	uint8_t r_phase;
	uint64_t r_seq;				/* records written, monotonic */
	uint64_t need_d, need_r;		/* sum of route_need_* over the ring's routes */
	uint64_t tmo_next;			/* next timeout scan, us */
	uint32_t in_use;			/* slots in use over the ring's routes */
	uint32_t n, n_dirty;
	struct iau_eng6_member *list[IAU_TILE_MAX_ROUTES];	/* the members, in no particular order */
	struct iau_eng6_member *route[IAU_TILE_MAX_ROUTES];	/* by route (NULL = none) */
	struct iau_eng6_member *dirty[IAU_TILE_MAX_ROUTES];	/* members with SQEs not yet rung, this poll */
	uint32_t resv[IAU_TILE_MAX_ROUTES];	/* credits whose need a GONE route keeps reserved (0 = none, I2) */
	uint8_t resv_gen[IAU_TILE_MAX_ROUTES];	/* the member gen of the reserving incarnation (H1) */
	uint32_t n_resv;
	uint8_t last_gen[IAU_TILE_MAX_ROUTES];	/* the member gen of each route's last incarnation (A15) */
	uint8_t gen_seen[IAU_TILE_MAX_ROUTES];
	struct iau_eng6_rstats st;
};

/* -EINVAL for a NULL pointer, entries outside IAU_ENG6_RING_MIN..MAX, a stride other than 64 / 48, nk 0 or
 * > IAU_TILE_MAX_K, k >= nk, an unknown GCB flag bit, IAU_TILE_GCB_F_RCONS without r_consumed; r is not touched
 * then. */
int iau_eng6_ring_init(struct iau_eng6_ring *r, const struct iau_eng6_ring_cfg *cfg);
/* Owner loss (P4.4 ring_remap): the same host memory through another mapping. -EINVAL on NULL. r_consumed is
 * used only if the ring was initialised with IAU_TILE_GCB_F_RCONS (then it must not be NULL). */
int iau_eng6_ring_remap(struct iau_eng6_ring *r, const volatile void *d, const volatile uint32_t *db,
			volatile uint64_t *d_head_out, volatile void *rec, const volatile uint64_t *r_consumed);
/* Validates cfg (the tile's registration check) and allocates the slot pool. -EINVAL: route >=
 * IAU_TILE_MAX_ROUTES, qid 0 or >= IAU_TILE_QIDS, entries < 2 or > IAU_TILE_MAX_SLOTS, sq_entries != entries,
 * cq_entries < 2, credits 0 or > entries - 1 or > cq_entries - 1, nsid 0, lba_shift outside 9..31, gen >
 * IAU_TILE_MEMBER_GEN_MASK, a fault knob whose status is not an error, a NULL pointer; m is not touched then.
 * -ENOMEM on allocation failure. */
int iau_eng6_member_init(struct iau_eng6_member *m, const struct iau_eng6_member_cfg *cfg);
/* Bind: -EINVAL route % nk != k (or m not ACTIVE / already in a ring); -EEXIST the route has a live member;
 * -ESTALE the member gen equals the route's last incarnation on this ring (A15: the host bumps it on rejoin;
 * the ring remembers it while the ring lives); -ENOSPC the ring invariant (A1) would break, counting every
 * reservation (a rejoin replaces its own route's reservation). */
int iau_eng6_member_add(struct iau_eng6_ring *r, struct iau_eng6_member *m);
/* V9 phase 1: stop issuing (ACTIVE -> STOPPED); no-op otherwise. */
void iau_eng6_member_stop(struct iau_eng6_ring *r, struct iau_eng6_member *m);
/* V9 phase 2 (the SSD no longer serves the SQ): with emit, drain the SSD CQ (every posted CQE, normal
 * records), a final ABORTED_SQ_DELETION for every BUSY slot, PARKED slots dropped, then R_DETACHED; without
 * emit (the ring's own memory is gone) no CQ access, no doorbell or CQ-sink read (host_ssd_db reads 0), no D
 * fetch and no record. State GONE, SSD and echo pointers NULL; the member leaves the ring (the ring no longer
 * references it: print its STATS, then fini it) and its route's need stays reserved under its member gen (see
 * Reservations). The ring's LAST member with emit (P4.5 fix A, the private-ring rule): after it left, d_tail is
 * read once and D is fetched up to it (every descriptor: the memberless rule's one final record, no SSD
 * command; GCB d_head stored), and only then R_DETACHED with rflags CLOSED (the only record that carries it),
 * the ring's last record: the caller must take the ring off its engine in the same engine-thread step, with no
 * poll in between (iau_tile_eng_member_last_out()). A member leaving a ring that others keep polling gets the early
 * R_DETACHED, without CLOSED.
 * Returns the records written (>= 0); 0 for a member already GONE; -EINVAL (nothing done) for a live member
 * that is not in this ring. */
int iau_eng6_member_detach(struct iau_eng6_ring *r, struct iau_eng6_member *m, bool emit);
/* Frees a GONE route's reservation (the host's Admin Delete I/O CQ of the incarnation with member gen gen, I2).
 * 0; -ESTALE the reservation belongs to another incarnation (gen differs: nothing freed, H1); -ENOENT none;
 * -EBUSY the route has a live member; -EINVAL route >= IAU_TILE_MAX_ROUTES. */
int iau_eng6_route_release(struct iau_eng6_ring *r, uint16_t route, uint8_t gen);
/* Frees the pool (never on a member still in a ring). */
void iau_eng6_member_fini(struct iau_eng6_member *m);
/* One poll at time now_us (microseconds, the caller's clock: the injectable clock of the tests): each live
 * member's SSD CQ (<= IAU_ENG6_POLL_BUDGET CQEs, one CQ-head doorbell), then D (<= IAU_ENG6_POLL_BUDGET
 * descriptors, one SQ doorbell per member written to, GCB d_head), then the timeout scan if due. Returns
 * CQEs consumed + descriptors processed + notices emitted. */
uint32_t iau_eng6_ring_poll(struct iau_eng6_ring *r, uint64_t now_us);

/* The same poll with the A19 / P4.10b ctx-spill emulation (IAU_TILE_CTX_SPILL=1; compiled from this source with
 * IAU_ENG6_SPILL defined, vfio_user_iau_tile_spill.c): one 64 B store of the slot line into the member's ctx block when a
 * descriptor takes a slot (before its SQE is written), one 64 B load of it for every valid completion (before the
 * record is written). Members without cfg->ctx see no extra access. The knob-off poll above has none of it. */
uint32_t iau_eng6_ring_poll_spill(struct iau_eng6_ring *r, uint64_t now_us);
#define IAU_ENG6_SPILL_LINE	64u
/* Arms the spill emulation of an ACTIVE member that is not in a ring yet: ctx = the route's slot lines in the GCB ctx
 * region (slots x 64 B, 64 B-aligned), ctx_bytes their size, slots the route's slots s (P4.10e: credits <= slots <=
 * entries; vfio_user.c passes s = credits, the RTL's reset SLOT_POLICY). With slots < entries the member's slot FIFO
 * hands out slots 0..slots-1 only, so every slot line lies in the block (the tile's ltag < slots_r). -EINVAL for
 * NULL, a misaligned pointer, slots outside credits..entries, fewer than slots x 64 B, or a slot FIFO no longer as
 * member_init left it (m is not touched then). Only iau_eng6_ring_poll_spill reads ctx; the knob-off poll ignores
 * it (the FIFO restriction is armed only with the knob on). */
int iau_eng6_member_set_ctx(struct iau_eng6_member *m, volatile void *ctx, uint32_t ctx_bytes, uint32_t slots);
/* IAU_ENGINE6_CTX_SPILL_STATS ctrlr= qid= gid= route= stores= loads=; 0 (nothing written) for a member without ctx */
int iau_eng6_fmt_spill(char *buf, size_t len, const struct iau_eng6_ring *r, const struct iau_eng6_member *m);

static inline uint32_t
iau_eng6_inflight(const struct iau_eng6_member *m)
{
	return m->entries - m->n_free;
}
static inline uint32_t
iau_eng6_ring_in_use(const struct iau_eng6_ring *r)
{
	return r->in_use;
}
/* 1 if anyone but the engine wrote this route's SSD doorbells (live while bound, latched at detach) */
uint32_t iau_eng6_member_foreign_db(const struct iau_eng6_member *m);

int iau_eng6_fmt_ring(char *buf, size_t len, const struct iau_eng6_ring *r, uint32_t engine, uint32_t lcore);
int iau_eng6_fmt_bind(char *buf, size_t len, const struct iau_eng6_ring *r, const struct iau_eng6_member *m,
		      uint32_t engine, uint32_t lcore);
int iau_eng6_fmt_stats(char *buf, size_t len, const struct iau_eng6_ring *r, const struct iau_eng6_member *m,
		       uint32_t engine);
int iau_eng6_fmt_ring_end(char *buf, size_t len, const struct iau_eng6_ring *r, uint32_t engine);

#endif /* SPDK_VFIO_USER_IAU_TILE_H */
