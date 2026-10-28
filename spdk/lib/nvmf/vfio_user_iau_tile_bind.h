/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.4: the pure-C glue between vfio_user.c (mode 5 serving IAU host interface tile queues) and the tile
 * engine core (vfio_user_iau_tile.h). No SPDK runtime: vfio_user.c does the DMA maps, the engine-thread messages
 * (the P3 handshake) and the log lines; everything it decides is here, so tests/realcore/test_vfio_user_iau_tile_bind.c
 * can check it with a real engine ring.
 *
 * Which path serves an I/O SQ (iau_tile_queue_decide(), at Create I/O SQ connect, from QCFG[q] and QCFG2[q] in the
 * IAU page, spdk/include/spdk_internal/iau_tile_abi.h):
 *   mode 5, QCFG registered, QCFG2 0       -> IAU_TILE_Q_ENG40: the P3 40 B engine path (I1e), unchanged
 *   mode 5, QCFG registered, QCFG2 valid   -> IAU_TILE_Q_TILE: a member of the tile group ring named by QCFG2
 *   mode 5, QCFG2 malformed                -> -EPROTO   (Create I/O SQ INVALID_FIELD)
 *   mode 5, QCFG not registered            -> -ENOENT   (as P3)
 *   modes 0 / 4, QCFG 0 and QCFG2 0         -> IAU_TILE_Q_NONE
 *   modes 0 / 4, QCFG or QCFG2 written      -> -EEXIST   (nobody would read the host's rings)
 *   a reserved QCFG bit (any mode)          -> -EINVAL
 * QCFG keeps carrying credits and nsid for both engine paths; QCFG2 adds the GCB IOVA, the route and the member
 * gen. The GCB member line must agree with both (iau_tile_gcb_check()).
 *
 * Group rings. A tile route r of a GCB is served by ring k = r mod K of that GCB (D3); one bound (GCB, k) is one
 * engine ring (struct iau_eng6_ring) on engine thread (gid + k) mod n_engines. A new ring starts at d_seq0 =
 * r_seq0 = 0 and needs the host's zeroed positions (iau_tile_gcb_fresh()); a ring lives while it has members, and a
 * re-registration after its last member left is a new ring (the host resets the memory and positions, P4.3
 * decision; the engine's reservations die with the old ring). A second member of a bound ring must describe the
 * same ring (iau_tile_gcb_same_ring()).
 *
 * Reservations (the I2 reap rule; vfio_user.c drives it through struct iau_tile_note, one per route of a bound ring):
 * the engine keeps a GONE route's need reserved under its member gen. vfio_user.c calls iau_eng6_route_release()
 * ONLY when the host's own Admin Delete I/O CQ of that route's CQ succeeds in the CURRENT controller incarnation
 * (iau_tile_note_cq_deleted() names the route and the gen recorded at bind). Controllers are named by a key, not an
 * address (iau_tile_ctrlr_key()): a controller gets a new key at its first tile bind after it is created or disabled
 * (CC.EN 1 -> 0, shutdown: iau_tile_ctrlr_key_reset()), so neither a reset with a re-created CQ of the same id nor a
 * new controller at a recycled address can release an old route. A detach while the controller goes away
 * (disconnect, reset, a freed controller) forgets the key at once (iau_tile_note_detach(ctrlr_going)). A
 * target-internal CQ deletion never calls it; a rejoin of the route replaces the reservation (the engine's
 * member_add) and the note.
 *
 * Ring source (I3): a ring polls D / R and the GCB words through ONE member's mappings, its source. The host's IOVA
 * space is one domain per target (rings are keyed by the GCB IOVA across controllers, M10), so every member maps
 * the same ring memory through its own controller. iau_tile_src_plan() decides, before a member detaches or a DMA
 * region goes, whether the source stays, moves to another member whose mapping survives, or (no mapping survives:
 * host death, or the host freeing live ring memory) the whole ring is dropped with emit=false for every member.
 * vfio_user.c runs it as a pre-pass over every ring before a region removal detaches any member, so a member's
 * emit never depends on the order in which the members detach (iau_tile_member_emit()).
 *
 * Time (D9). The engine takes microseconds: iau_tile_clock_us() converts ticks once per engine pass, with a 128-bit
 * multiply (no division, no overflow over the whole 64-bit tick range).
 *
 * Per-engine peak (D17, P4.3 decision): an engine thread polls its rings through struct iau_tile_eng_rings; each
 * pass sums iau_eng6_ring_in_use() over the rings and keeps the maximum (IAU_ENGINE6_PEAK engine= peak_slots=).
 *
 * Fault knobs.
 *  - IAU_ENG_FAULT_ERR_{EVERY,SC,SCT,DNR} (mode 5, the tile engine's knob, process-wide; iau_tile_eng_fault_load()):
 *    EVERY 0..1e9 decimal (0 = off), SC 0..255 decimal or 0x-hex (default 0x82 NAMESPACE_NOT_READY), SCT 0..7
 *    (default 0), DNR 0|1 (default 0). With EVERY != 0, SC or SCT must be non-zero. Unset or empty = default.
 *  - IAU_DEV_FAULT_ERR_{EVERY,SC,SCT,DNR}[_C<k>] (the mode-0 device-model knob, D19; iau_tile_dev_fault_load()):
 *    devcfg rules (vfio_user_devcfg.h): IAU_DEV_<KEY>_C<k> for controller k, else IAU_DEV_<KEY>, else the
 *    default; plain decimal; the same ranges and defaults as the engine knob. iau_tile_dev_fault_scan() checks every
 *    such variable at transport create and resolves each controller it names.
 * Every parser: 0, or -EINVAL with err naming the first offending variable.
 */
#ifndef SPDK_VFIO_USER_IAU_TILE_BIND_H
#define SPDK_VFIO_USER_IAU_TILE_BIND_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "vfio_user_iau.h"
#include "vfio_user_iau_tile.h"

#define IAU_TILE_Q_NONE	0	/* no engine (modes 0 and 4) */
#define IAU_TILE_Q_ENG40	1	/* mode 5, the P3 40 B engine path */
#define IAU_TILE_Q_TILE	2	/* mode 5, an IAU tile group-ring member */

/* What the host registered for one I/O SQ: QCFG's credits / nsid, QCFG2's GCB, route and member gen. */
struct iau_tile_qreg {
	uint32_t credits, nsid;
	uint64_t gcb_iova;
	uint16_t route;
	uint8_t gen;
};
/* IAU_TILE_Q_* or a negative errno (see the table above); *q is filled in every case. */
int iau_tile_queue_decide(int mode, uint64_t qcfg, uint64_t qcfg2, struct iau_tile_qreg *q);

/* The lines of a GCB that one bind reads (a snapshot: nothing is re-read after the bind, V9). */
struct iau_tile_gcb_view {
	uint32_t gid, flags, n_routes;
	uint16_t nk, k;			/* the GCB's K; the route's ring (route mod K) */
	uint64_t d_iova, r_iova;	/* ring[k] */
	uint32_t d_entries, r_entries, d_stride;
	uint32_t m_credits, m_nsid, m_timeout_us, m_entries;	/* member[route] */
	uint64_t m_echo_iova;
	uint8_t m_lba_shift, m_gen;
};
/* Snapshot and check the GCB g (the binding controller's map) for the queue registered as q with SSD SQ qid of
 * sq_entries entries: magic, version, 1 <= K <= IAU_TILE_MAX_K, 1 <= n_routes <= IAU_TILE_MAX_ROUTES, route <
 * n_routes, known flags only; member[route] valid, ring == route mod K, qid, credits, nsid and gen equal QCFG /
 * QCFG2's, entries == sq_entries (<= IAU_TILE_MAX_SLOTS), echo IOVA non-zero and 64 B aligned, lba_shift 9..31;
 * ring[k] IOVAs non-zero and 64 B aligned, entries IAU_ENG6_RING_MIN..MAX, stride 64 or 48. 0 or -EINVAL. */
int iau_tile_gcb_check(const volatile struct iau_tile_gcb *g, const struct iau_tile_qreg *q, uint16_t qid, uint32_t sq_entries,
		   struct iau_tile_gcb_view *v, char *err, size_t errlen);
/* A new ring's positions are the host's zeros: ring[k].d_head == 0, db[k].d_tail == 0 and, with
 * IAU_TILE_GCB_F_RCONS, ring[k].r_consumed == 0 (k and flags from v). 0 or -EINVAL. */
int iau_tile_gcb_fresh(const volatile struct iau_tile_gcb *g, const struct iau_tile_gcb_view *v, char *err, size_t errlen);
/* Two binds describe the same ring of the same group. */
bool iau_tile_gcb_same_ring(const struct iau_tile_gcb_view *a, const struct iau_tile_gcb_view *b);
/* Ring and member configurations for the engine. The ring's pointers come from g (doorbell, d_head and, with
 * IAU_TILE_GCB_F_RCONS only, r_consumed) and the D / R maps; d_seq0 = r_seq0 = 0 (a new ring). The member's
 * SSD-side pointers and fault knob are the caller's. */
void iau_tile_ring_cfg(const struct iau_tile_gcb_view *v, volatile struct iau_tile_gcb *g, const volatile void *d,
		   volatile void *r, struct iau_eng6_ring_cfg *c);
/* The GCB words a ring points at, for iau_eng6_ring_remap() through another member's map of the same GCB. */
void iau_tile_ring_ptrs(const struct iau_tile_gcb_view *v, volatile struct iau_tile_gcb *g, const volatile uint32_t **db,
		    volatile uint64_t **d_head, const volatile uint64_t **r_consumed);
void iau_tile_member_cfg(const struct iau_tile_gcb_view *v, const struct iau_tile_qreg *q, uint16_t qid, int ctrlr,
		     struct iau_eng6_member_cfg *c);
static inline uint64_t iau_tile_d_bytes(const struct iau_tile_gcb_view *v) { return (uint64_t)v->d_entries * v->d_stride; }
static inline uint64_t iau_tile_r_bytes(const struct iau_tile_gcb_view *v) { return (uint64_t)v->r_entries * IAU_TILE_REC_BYTES; }
static inline uint64_t iau_tile_echo_bytes(const struct iau_tile_gcb_view *v) { return (uint64_t)v->m_entries * 64u; }

/* A controller incarnation's key (never 0): *slot (the controller's, zeroed at creation) keeps it until
 * iau_tile_ctrlr_key_reset(); new keys come from *next (the transport's counter). Callers hold the lock. */
uint64_t iau_tile_ctrlr_key(uint64_t *slot, uint64_t *next);
void iau_tile_ctrlr_key_reset(uint64_t *slot);

/* One route of a bound ring, for the reservation release (see above). ckey: the controller key (0 = none). */
enum { IAU_TILE_N_NONE = 0, IAU_TILE_N_LIVE = 1, IAU_TILE_N_RESV = 2 };
struct iau_tile_note {
	uint64_t ckey;
	uint16_t cqid;
	uint8_t gen;
	uint8_t state;
};
/* bind (also a rejoin): LIVE with the route's controller key, CQ and member gen */
void iau_tile_note_bind(struct iau_tile_note *n, uint64_t ckey, uint16_t cqid, uint8_t gen);
/* member detach: LIVE -> RESV (the engine keeps the need reserved); ctrlr_going (disconnect, reset, a freed
 * controller) forgets the key, so no Delete I/O CQ can name the route any more (a rejoin or the ring reset
 * releases it) */
void iau_tile_note_detach(struct iau_tile_note *n, bool ctrlr_going);
/* a host-issued Admin Delete I/O CQ (ckey, cqid) succeeded: true (with the gen recorded at bind) if this
 * route's reservation must be released now; the note is then NONE. Key 0 never matches. */
bool iau_tile_note_cq_deleted(struct iau_tile_note *n, uint64_t ckey, uint16_t cqid, uint8_t *gen);

/* The source plan (I3, see above). m[i]: alive = a member of the ring now (not dropped); ring_in = its mapping
 * of the ring memory (GCB, D, R) lies in the DMA region being removed; leaving = it detaches now. */
#define IAU_TILE_SRC_KEEP	0	/* the source stays */
#define IAU_TILE_SRC_MOVE	1	/* *to = the new source (ring_remap through its mappings) */
#define IAU_TILE_SRC_DROP	2	/* no member's mapping survives: drop the ring, every member emit=false */
struct iau_tile_rm {
	bool alive, ring_in, leaving;
};
/* KEEP if the source is alive and neither leaving nor losing its mapping; else MOVE to the first other alive
 * member whose mapping survives, one that stays preferred over one that leaves; else KEEP if the source's own
 * mapping survives and it is the last alive member (it detaches with emit and the ring unbinds); else DROP. */
int iau_tile_src_plan(const struct iau_tile_rm *m, uint32_t n, uint32_t src, uint32_t *to);
/* a member detaching from a ring that was (dropped) or was not dropped: records are written (emit) iff not */
static inline bool iau_tile_member_emit(bool dropped) { return !dropped; }

/* ticks -> microseconds: floor(ticks * mult / 2^48), mult = floor(2^48 * 1e6 / hz); at GHz rates within 1 ppb of
 * the exact value and never above it, over the whole 64-bit tick range (the product needs at most 101 bits) */
#define IAU_TILE_CLOCK_SHIFT	48
struct iau_tile_clock {
	uint64_t mult;
};
int iau_tile_clock_init(struct iau_tile_clock *c, uint64_t hz);	/* -EINVAL for hz < 1000 */
static inline uint64_t
iau_tile_clock_us(const struct iau_tile_clock *c, uint64_t ticks)
{
	return (uint64_t)(((unsigned __int128)ticks * c->mult) >> IAU_TILE_CLOCK_SHIFT);
}

/* The tile rings of one engine thread (that thread's only) and its peak slots in use. */
#define IAU_TILE_ENG_MAX_RINGS	64
struct iau_tile_eng_rings {
	uint32_t n;
	uint64_t peak_slots;		/* max over passes of the sum of iau_eng6_ring_in_use() */
	uint64_t passes;
	struct iau_eng6_ring *r[IAU_TILE_ENG_MAX_RINGS];
};
int iau_tile_eng_add(struct iau_tile_eng_rings *e, struct iau_eng6_ring *r);	/* -ENOSPC full, -EEXIST already in */
int iau_tile_eng_del(struct iau_tile_eng_rings *e, struct iau_eng6_ring *r);	/* -ENOENT not in */
/* The ring's last member leaves (P4.5 fix A, the private-ring rule), on the engine thread in ONE step:
 * iau_eng6_member_detach(r, m, emit=true) (its final D fetch makes R_DETACHED the ring's last record), then the
 * ring leaves e at once, so no poll of it can fetch a descriptor written after the detach-time d_tail. Returns the
 * detach's result (records written, or -EINVAL with nothing done); -EBUSY (the ring kept, detach done) if other
 * members remain, which the caller's bookkeeping must rule out. */
int iau_tile_eng_member_last_out(struct iau_tile_eng_rings *e, struct iau_eng6_ring *r, struct iau_eng6_member *m);
/* one pass over every ring at now_us; returns the work (iau_eng6_ring_poll() summed) */
uint32_t iau_tile_eng_poll(struct iau_tile_eng_rings *e, uint64_t now_us);
/* The same pass with another ring poll (P4.10b: iau_eng6_ring_poll_spill, installed at bind time by
 * IAU_TILE_CTX_SPILL=1). iau_tile_eng_poll() is unchanged. */
typedef uint32_t (*iau_tile_ring_poll_fn)(struct iau_eng6_ring *r, uint64_t now_us);
uint32_t iau_tile_eng_poll_fn(struct iau_tile_eng_rings *e, uint64_t now_us, iau_tile_ring_poll_fn poll);

/*
 * IAU_TILE_CTX_SPILL (P4.10b, A19; mode 5 only, default off). The software engine emulates the silicon's slot-line
 * spill into the GCB ctx region (ctx_iova / ctx_bytes, allocated by the host, P4.10c): at fetch one 64 B store of
 * the slot line into the route's block, at completion one 64 B load of it, before the record is written. Value:
 * unset / empty / 0 = off, 1 = on, anything else -EINVAL (err names it).
 */
/* The A19 route block of a route with s slots: 64 B slot lines (s of them), the PRP-list slab at the next 256 B
 * boundary (pad 0..192 B), then s lists of 256 B. It equals the RTL's block, f_csize(s) = 5 s + ((4 - s mod 4) mod 4)
 * units of 64 B (RTL_design/src/iau_mmio.v:254-260): B(s) = 320 s + 64 ((4 - s mod 4) mod 4), a multiple of 256 B. */
#define IAU_TILE_CTX_LINE		64u
#define IAU_TILE_CTX_LIST		256u
static inline uint64_t iau_tile_ctx_lines_bytes(uint32_t s) { return (uint64_t)s * IAU_TILE_CTX_LINE; }
static inline uint64_t iau_tile_ctx_list_off(uint32_t s) { return (iau_tile_ctx_lines_bytes(s) + 255u) & ~(uint64_t)255u; }
static inline uint64_t iau_tile_ctx_block_bytes(uint32_t s) { return iau_tile_ctx_list_off(s) + (uint64_t)s * IAU_TILE_CTX_LIST; }
/*
 * P4.10e: the route's slots s, as iau_mmio's BIND takes them (slots_new = SLOT_POLICY ? entries : credits,
 * iau_mmio.v:443; SLOT_POLICY 0x6118 resets to 0 = credits, :21 / :528). Credits is the tile's reset (silicon)
 * policy and the only one the testbed target runs (IAU_TILE_SLOT_POLICY_TARGET): its spill copy of the engine then hands
 * out slots 0..credits-1 only (iau_eng6_member_set_ctx()). Entries is the tile's test mode.
 */
#define IAU_TILE_SLOT_POLICY_CREDITS	0
#define IAU_TILE_SLOT_POLICY_ENTRIES	1
#define IAU_TILE_SLOT_POLICY_TARGET		IAU_TILE_SLOT_POLICY_CREDITS
static inline uint32_t
iau_tile_ctx_slots(int policy, uint32_t credits, uint32_t entries)
{
	return policy == IAU_TILE_SLOT_POLICY_ENTRIES ? entries : credits;
}
/*
 * P4 plan A21 (P4.10e): the host's ctx_bytes, the least region that holds the blocks of every route set a GCB can
 * hold under the ring invariant (credits policy, s = credits).
 *  - A ring's held blocks are those of its live and reserved routes (freed at RELEASE_ROUTE and at the ring's unbind),
 *    which iau_tile_ring_fits() bounds: sum(3 c + 3) <= L = min(d_entries - 1, r_entries). With U = floor(L / 3) and
 *    u = c + 1, B(c) = 64 (5 u - (5 - p)), p = (4 - c mod 4) mod 4, so a ring holds at most
 *    M(U) = 64 (5 U - 2 - ((U + 2) mod 4)) bytes (0 for U < 2): the least loss 5 - p over routes with sum(u) = U is
 *    2, 3, 4, 5 for U mod 4 = 2, 3, 0, 1, one route (or, U mod 4 = 1, two). Exact when one route may take c = U - 1
 *    (credits <= 4094 caps it: then an upper bound).
 *  - Shared group: the sum of M(U_k) over its K rings. Private group (one route of c credits): B(c).
 */
static inline uint64_t
iau_tile_ctx_ring_bytes(uint32_t d_entries, uint32_t r_entries)
{
	uint64_t l = d_entries == 0 ? 0 : (uint64_t)d_entries - 1, u;

	if ((uint64_t)r_entries < l) {
		l = r_entries;
	}
	u = l / 3;
	return u < 2 ? 0 : 64u * (5u * u - 2u - ((u + 2u) % 4u));
}
static inline uint64_t iau_tile_ctx_private_bytes(uint32_t credits) { return iau_tile_ctx_block_bytes(credits); }
/* The GCB header's ctx region (A19; 0 / 0 = none), one read. Not part of struct iau_tile_gcb_view, so iau_tile_gcb_check()
 * and the view keep their instructions with the knob off. */
struct iau_tile_ctx_region {
	uint64_t iova;
	uint32_t bytes;
};
void iau_tile_gcb_ctx(const volatile struct iau_tile_gcb *g, struct iau_tile_ctx_region *c);
/* The GCB's region against the RTL's first ctx check (iau_mmio.v:742): ctx_iova 0 or not 4 KiB aligned -> -EINVAL
 * (err says "publishes no ctx region" when ctx_iova and ctx_bytes are both 0). ctx_bytes is not checked here: the
 * sum check of iau_tile_ctx_alloc() refuses a region too small (-ENOSPC, as :743). v is the bind's snapshot. */
int iau_tile_ctx_region_check(const struct iau_tile_ctx_region *v, char *err, size_t errlen);
/* The route blocks of one GCB's region, as the tile holds them (one map per GCB; blk[route]). Blocks are multiples
 * of 256 B, so each starts 256 B-aligned. */
struct iau_tile_ctx_map {
	uint32_t bytes;			/* the region */
	struct {
		uint32_t off, len;	/* len 0 = free entry */
		uint16_t route;
		uint16_t slots;
	} blk[IAU_TILE_MAX_ROUTES];
};
void iau_tile_ctx_map_init(struct iau_tile_ctx_map *c, uint32_t ctx_bytes);
/* What iau_tile_ctx_alloc() replaced, for iau_tile_ctx_undo() when the same bind fails later. */
struct iau_tile_ctx_undo {
	uint32_t off, len;
	uint16_t route, slots;
	bool valid;
};
#define IAU_TILE_CTX_R_SUM		1	/* -ENOSPC, the RTL's reason CTX (iau_mmio.v:743) */
#define IAU_TILE_CTX_R_FRAG		2	/* -ENOSPC, the RTL's reason CTX_FRAG (:760, :767) */
/*
 * BIND step 7 of iau_mmio (P4.10e; :742-773, see reports/p4_10e.md section 1), on the route's block. Held = every
 * block in the map (the GCB's live and reserved routes); the route's own block is not counted:
 *  1. sum: the other held blocks plus B(s) > floor(bytes / 64) * 64 -> -ENOSPC, *reason IAU_TILE_CTX_R_SUM;
 *  2. reuse: the route holds a block of >= s slots -> it keeps the offset and its block shrinks to B(s);
 *  3. first fit: the lowest offset whose B(s) overlaps no other held block (the route's own old block is free for
 *     it: a larger rejoin), past the region -> -ENOSPC, *reason IAU_TILE_CTX_R_FRAG.
 * 0 with *off = the block's byte offset and *u = what to restore; any refusal changes nothing. -EINVAL for a route
 * out of range, s == 0 or s > IAU_TILE_MAX_SLOTS. u and reason may be NULL.
 */
int iau_tile_ctx_alloc(struct iau_tile_ctx_map *c, uint16_t route, uint32_t s, uint32_t *off, struct iau_tile_ctx_undo *u,
		   int *reason);
/* The bind that allocated u failed later (under the same lock hold): the route's block is as it was before. */
void iau_tile_ctx_undo(struct iau_tile_ctx_map *c, const struct iau_tile_ctx_undo *u);
/* The route's block ends (RELEASE_ROUTE: the RTL makes the entry DORMANT, :833). 0, or -ENOENT. */
int iau_tile_ctx_free(struct iau_tile_ctx_map *c, uint16_t route);
/* Ring k of a GCB with K = nk rings unbinds (its last member out, or the ring is dropped: the RTL's RING_RESET):
 * every block of its routes (route mod nk == k) is freed, as the RTL frees every CAM entry of the ring (:842-846).
 * Returns the number freed. */
uint32_t iau_tile_ctx_ring_free(struct iau_tile_ctx_map *c, uint16_t nk, uint16_t k);
/* The bounds check of every address the engine will touch: [off, off + 64 s) inside the region and 64 B-aligned.
 * 0 or -ERANGE. */
int iau_tile_ctx_lines_check(const struct iau_tile_ctx_map *c, uint32_t off, uint32_t s);
/*
 * The GCB ctx maps of one transport (P4.10e, review I4), under the caller's lock (vfio_user.c: eng_lock). An entry
 * lives while it has references: one per bind in flight (taken by iau_tile_ctxgcb_get() in the bind's first lock
 * section, before the block is allocated in its second) and one per bound member (the bind's reference, kept when
 * the bind succeeds). A failing bind drops only its own, so it never frees a map that a concurrent bind of the same
 * GCB still uses (875db760 counted bound rings only: a failing bind freed the map while another bind held it).
 */
struct iau_tile_ctxgcb {
	TAILQ_ENTRY(iau_tile_ctxgcb)	link;
	uint64_t			gcb_iova, ctx_iova;
	uint32_t			ctx_bytes;
	uint32_t			refs;	/* binds in flight + bound members */
	struct iau_tile_ctx_map		map;
};
TAILQ_HEAD(iau_tile_ctxgcb_list, iau_tile_ctxgcb);
/* The GCB's entry with one more reference (created with the region cr at the GCB's first bind). NULL with *rc:
 * -EINVAL the GCB is registered with another region (the RTL's ctx_conflict, :673 / :742), -ENOMEM. */
struct iau_tile_ctxgcb *iau_tile_ctxgcb_get(struct iau_tile_ctxgcb_list *l, uint64_t gcb_iova, const struct iau_tile_ctx_region *cr,
				    int *rc);
/* Drop one reference; the last one frees the entry (true). NULL: false. */
bool iau_tile_ctxgcb_put(struct iau_tile_ctxgcb_list *l, struct iau_tile_ctxgcb *cg);

/* Fault knobs (see above). status = iau_tile_status(sct, sc, dnr) when every != 0, else 0. */
#define IAU_TILE_FAULT_EVERY_MAX	1000000000u
struct iau_tile_fault {
	uint32_t every;
	uint8_t sc, sct, dnr;
	uint16_t status;
};
typedef char *(*iau_tile_getenv_fn)(const char *name);
int iau_tile_eng_fault_load(struct iau_tile_fault *f, iau_tile_getenv_fn get, char *err, size_t errlen);
int iau_tile_dev_fault_load(struct iau_tile_fault *f, int ctrlr_idx, iau_tile_getenv_fn get, char *err, size_t errlen);
/* Transport create: every IAU_DEV_FAULT_ERR_* variable of envp (NULL-terminated "NAME=value") is checked
 * (name: one of the four keys, optionally _C<k> without a leading zero; value: as iau_tile_dev_fault_load()), and the
 * knob is resolved for the global setting and for every controller a _C<k> names. *any_on: some resolution has
 * EVERY != 0. 0 or -EINVAL. */
int iau_tile_dev_fault_scan(char **envp, iau_tile_getenv_fn get, bool *any_on, char *err, size_t errlen);
/* IAU_TILE_CTX_SPILL (see above): *on; 0 or -EINVAL with err naming the value. */
int iau_tile_spill_load(bool *on, iau_tile_getenv_fn get, char *err, size_t errlen);

#endif /* SPDK_VFIO_USER_IAU_TILE_BIND_H */
