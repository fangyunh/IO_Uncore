/*   SPDX-License-Identifier: BSD-3-Clause
 *   IAU-next host interface tile (spec §3.2; P4 plan rev 2 D5-D13, D17 as amended by A1, A9, A15): the wire
 *   formats shared by the host (spdk/lib/nvme, nvme_iau.c) and the IAU engine (spdk/lib/nvmf,
 *   vfio_user_iau_tile.c). This header is the one definition. Internal: no public SPDK API changes.
 *
 *   Pinned by tests/realcore/test_iau_tile_contract.c (authoritative for every offset below, plan A9: layout by
 *   offsetof, pack/unpack round trips, invariant, tags, PRP/SQE/replay) and by stage [5/5] of
 *   scripts/rtl/check_rtl_consistency.sh (frozen descriptor -> SQE and record bytes; the RTL leg arrives in
 *   P5). Only wire-level constants live here: the engine's evidence-line keys (IAU_TILE_STATS) are defined in
 *   spdk/lib/nvmf/vfio_user_iau_tile.h (P4.3), the host's configuration defaults in spdk/lib/nvme/nvme_iau.h.
 *
 *   Layout summary (byte offsets; little-endian):
 *     descriptor  64 B  d0 @0 d1 @8 d2 @16 d3 @24 d4 @32 d5 @40 d6 @48 d7 @56 (48 B lever: d0 d1 d2 d4 d5 d6)
 *     record      32 B  w0 cb_fn @0, w1 cb_arg @8, w2 cdw0|cdw1 @16, w3 sqhd|route|tag|phase|status @24 (LOCKED)
 *     GCB         16960 B in an IAU_TILE_GCB_BYTES (20 KiB) 4 KiB-aligned block:
 *                 header @0 (64 B), ring[4] @64, db[4] @320, member[256] @576
 *       header    magic @0 version @4 k @6 n_routes @8 gid @12 ctx_iova @16 ctx_bytes @24 flags @28 rsvd1[4] @32
 *       ring[k]   d_iova @0 r_iova @8 d_entries @16 r_entries @20 d_stride @24 rsvd0 @28 d_head @32 r_consumed @40
 *                 rsvd[2] @48
 *       db[k]     d_tail @0 rsvd[15] @4
 *       member[r] valid @0 ring @4 qid @6 credits @8 nsid @12 timeout_us @16 entries @20 echo_iova @24
 *                 lba_shift @32 gen @33 rsvd8[6] @34 rsvd[3] @40
 *     QCFG2[q]    IAU page 0x5A00 + 8 q: route [7:0] | member gen [10:8] | TILE [11] | gcb_iova [63:12]
 */

#ifndef SPDK_INTERNAL_IAU_TILE_H
#define SPDK_INTERNAL_IAU_TILE_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/nvme_spec.h"

SPDK_STATIC_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "IAU tile words are little-endian");

/*
 * Descriptor tile (D5): 64 B, one per cache line, 8 x u64.
 *   d0  [7:0] opc | [15:8] flags | [31:16] route | [47:32] nlb (0-based) | [63:48] cflags
 *   d1  IO: slba (= cdw10 | cdw11 << 32; DSM: its cdw10 | cdw11)
 *   d2  IO: buffer IOVA (the PRP1 source); REISSUE, RELEASE, ABORT_TAG: tag; ABORT_COOKIE: cookie (a cb_arg)
 *   d3  defined ignored by the engine (zeroed at ring allocation; the host never writes it)
 *   d4  [31:0] nsid: 0 = the route's nsid, any other value must equal it (the replay has no nsid, A15);
 *       [63:32] 0
 *   d5  cb_fn, d6 cb_arg: IO and RAW: the command's; ABORT_*: the ack record's w0 / w1
 *   d7  defined ignored by the engine (as d3)
 *   flags  [1:0] fuse | [2] FUA | [3] LR | [7:4] kind (enum iau_tile_kind)
 *   cflags [3:0] timeout class (IAU_TILE_CF_TMO = timed, D9) | [12:4] 0 | [15:13] member gen (H3): the route's
 *          current member gen (QCFG2 / GCB member line) on EVERY descriptor kind; the engine treats a descriptor
 *          whose gen differs from its bound member's like one for a route without a member (IO / RAW: one final
 *          ABORTED_SQ_DELETION, ABORT_*: ack 0, REISSUE / RELEASE / FLUSH ignored), so an old incarnation's
 *          unfetched descriptor can never reach a rejoined route's SSD queue
 * The host writes d0 d1 d2 d4 d5 d6 only. Stride 48 (lever I1v6n48): those six words packed in that order
 * (iau_tile_word()); RAW is refused at stride 48. Reserved fields are written 0 and ignored by the engine.
 * Kinds (D7):
 *   IO            d0 d1 d2 d4 d5 d6; the engine builds the SQE (iau_tile_sqe_from_io()).
 *   RAW           d0 (kind, route, cflags; opc/fuse/nlb ignored), d5, d6; the NEXT ring slot holds a host-built
 *                 64 B SQE whose cid the engine replaces by the tag. A RAW occupies two slots.
 *   REISSUE       d0 = iau_tile_d0_ctrl(), d2 = tag (gen checked): re-issue a parked IO-kind slot, SQE rebuilt from
 *                 the slot's replay fields (iau_tile_replay_to_sqe()).
 *   RELEASE       d0, d2 = tag (gen checked): free a parked slot; no record.
 *   ABORT_COOKIE  d0, d2 = cookie, d5/d6 = ack: mark the busy slots whose cb_arg == cookie; one ack record.
 *   ABORT_TAG     d0, d2 = tag (gen checked), d5/d6 = ack: mark that slot; one ack record.
 *   FLUSH         d0, at most one per route: from then on the route's new IO / RAW descriptors get an immediate
 *                 final ABORTED_SQ_DELETION record (no SSD command), its parked slots are disposed with a final
 *                 ABORTED_SQ_DELETION record each, and its in-flight commands complete normally (D7).
 */
#define IAU_TILE_DESC_BYTES	64u
#define IAU_TILE_DESC48_BYTES	48u
struct iau_tile_desc {
	uint64_t d[8];
};
SPDK_STATIC_ASSERT(sizeof(struct iau_tile_desc) == IAU_TILE_DESC_BYTES, "descriptor v6 is 64 B");

#define IAU_TILE_F_FUSE_MASK	0x03u
#define IAU_TILE_F_FUA		0x04u
#define IAU_TILE_F_LR		0x08u
#define IAU_TILE_F_KIND_SHIFT	4
#define IAU_TILE_F_KIND_MASK	0xF0u
#define IAU_TILE_F_REPLAY_MASK	0x0Fu	/* the flags an IO-kind slot replays: fuse, FUA, LR */
enum iau_tile_kind {
	IAU_TILE_K_IO = 0,
	IAU_TILE_K_RAW = 1,
	IAU_TILE_K_REISSUE = 2,
	IAU_TILE_K_RELEASE = 3,
	IAU_TILE_K_ABORT_COOKIE = 4,
	IAU_TILE_K_ABORT_TAG = 5,
	IAU_TILE_K_FLUSH = 6,
};
#define IAU_TILE_CF_TMO_MASK	0x000Fu
#define IAU_TILE_CF_TMO		0x0001u
#define IAU_TILE_CF_GEN_SHIFT	13
#define IAU_TILE_CF_GEN_MASK	0xE000u	/* cflags [15:13]: the member gen (H3) */

static inline uint64_t
iau_tile_d0(uint8_t opc, uint8_t flags, uint16_t route, uint16_t nlb, uint16_t cflags)
{
	return (uint64_t)opc | ((uint64_t)flags << 8) | ((uint64_t)route << 16) | ((uint64_t)nlb << 32) |
	       ((uint64_t)cflags << 48);
}
static inline uint8_t iau_tile_d0_opc(uint64_t d0) { return (uint8_t)d0; }
static inline uint8_t iau_tile_d0_flags(uint64_t d0) { return (uint8_t)(d0 >> 8); }
static inline uint8_t iau_tile_d0_kind(uint64_t d0) { return (uint8_t)((d0 >> 12) & 0xFu); }
static inline uint16_t iau_tile_d0_route(uint64_t d0) { return (uint16_t)(d0 >> 16); }
static inline uint16_t iau_tile_d0_nlb(uint64_t d0) { return (uint16_t)(d0 >> 32); }
static inline uint16_t iau_tile_d0_cflags(uint64_t d0) { return (uint16_t)(d0 >> 48); }
/* cflags of a descriptor: timeout class (IAU_TILE_CF_TMO or 0) and the route's member gen (0..7) */
static inline uint16_t
iau_tile_cflags(uint8_t tmo_class, uint8_t gen)
{
	return (uint16_t)((tmo_class & IAU_TILE_CF_TMO_MASK) | ((uint16_t)(gen & 0x7u) << IAU_TILE_CF_GEN_SHIFT));
}
static inline uint8_t iau_tile_d0_gen(uint64_t d0) { return (uint8_t)((d0 >> (48 + IAU_TILE_CF_GEN_SHIFT)) & 0x7u); }

/* d0 of a control descriptor (REISSUE, RELEASE, ABORT_*, FLUSH): kind, route and the route's member gen */
static inline uint64_t
iau_tile_d0_ctrl(uint8_t kind, uint16_t route, uint8_t gen)
{
	return iau_tile_d0(0, (uint8_t)((kind & 0xFu) << IAU_TILE_F_KIND_SHIFT), route, 0, iau_tile_cflags(0, gen));
}

/* d0 / d1 of the IO-kind descriptor for host command c (only if iau_tile_cmd_is_io(c, ...)):
 * d0_tmpl = iau_tile_d0(0, 0, route, 0, iau_tile_cflags(tmo, member gen)); FUA (cdw12 bit 30) and LR (bit 31) land
 * in flags [2] / [3] */
static inline uint64_t
iau_tile_d0_from_cmd(const struct spdk_nvme_cmd *c, uint64_t d0_tmpl)
{
	return (uint64_t)c->opc | ((uint64_t)(c->fuse | ((c->cdw12 >> 28) & 0xCu)) << 8) | d0_tmpl |
	       ((uint64_t)(uint16_t)c->cdw12 << 32);
}
static inline uint64_t
iau_tile_d1_from_cmd(const struct spdk_nvme_cmd *c)
{
	return (uint64_t)c->cdw10 | ((uint64_t)c->cdw11 << 32);
}

/* word offset of logical word i (0..7) in a ring slot of this stride; -1 if the slot has no such word
 * (stride 48: d3, d7) or the stride is not a tile stride */
static inline int
iau_tile_word(uint32_t stride, unsigned i)
{
	static const int8_t w48[8] = { 0, 1, 2, -1, 3, 4, 5, -1 };

	if (i > 7) {
		return -1;
	}
	if (stride == IAU_TILE_DESC_BYTES) {
		return (int)i;
	}
	return stride == IAU_TILE_DESC48_BYTES ? w48[i] : -1;
}
static inline bool
iau_tile_stride_ok(uint32_t stride)
{
	return stride == IAU_TILE_DESC_BYTES || stride == IAU_TILE_DESC48_BYTES;
}

/*
 * Record (D6; LOCKED 32 B, packed by iau_pack_moded_record(), spdk/lib/nvmf/vfio_user_iau_record.h):
 *   w0  cb_fn          w1  cb_arg
 *   w2  cdw0 [31:0] | cdw1 [63:32]: rflags [47:40] (bit 40 PARKED, bit 41 ACK, bit 42 CLOSED), retries [55:48],
 *       kind [63:56] (enum iau_tile_rec_kind); [39:32] 0. cdw1 is 0 only on plain first-issue completions.
 *   w3  sqhd [15:0] (the engine's D-ring head index, informational) | route [31:16] (cpl.sqid) |
 *       tag [47:32] (cpl.cid) | phase [48] (the ring phase; written last) | status [63:49]
 * Publish and load order: the engine writes w0, w1, w2 and then w3 with release semantics
 * (iau_tile_rec_publish(); D20: a compiler barrier on x86 WB memory); the host loads w3 FIRST with acquire
 * semantics and only then w2 / w0 / w1 (iau_tile_rec_peek() for w3 + w2). Both compile to plain moves on x86.
 * TIMEOUT and DETACHED carry IAU_TILE_MARK_STATUS (SCT 7, SC 0xFE). The host's fast filter
 * (iau_tile_rec_fast()) takes a record only if its phase is the expected one, SC/SCT are 0 and cdw1 is 0:
 * MARK alone, and kind / retries / rflags alone, each send a record to the cold path.
 * Dispatch rule: the host dispatches on kind and rflags only, never on status == MARK. MARK exists only to
 * send TIMEOUT / DETACHED cold; a CPL record whose status is SCT 7 / SC 0xFE (an SSD's vendor-specific
 * status) is an error completion and is delivered to its callback.
 * The host masks cdw1 to 0 before any callback (iau_tile_rec_cpl()).
 * R_DETACHED (the route left the engine; tag NONE). With rflags CLOSED (the ring is closed: no record follows
 * on this ring): written ONLY by the detach with records of the ring's LAST member (P4.5: a private ring's only
 * member), after the engine's final fetch (P4.5 fix A): every descriptor written before the detach-time d_tail
 * got exactly one record before it, and a descriptor written after that d_tail is never fetched, so the host
 * ends every command still in flight when it reads a CLOSED R_DETACHED, and a private ring's rejoin is a ring
 * reset. Without CLOSED (the early R_DETACHED of a member leaving a ring that others keep polling, a SHARED
 * ring, P4.8; or a target without the final fetch): orphan records (tag NONE) of descriptors the host wrote
 * before it saw R_DETACHED may follow it, so the host ends only its pending entries there and the rest waits for
 * its records (or ends LOST at the drain bound); a rejoin or release there waits for queue_depth == 0,
 * ctrl_inflight == 0 and GCB d_head past the route's last descriptor (spdk/lib/nvmf/vfio_user_iau_tile.h).
 * CLOSED is never set on any other record kind.
 */
#define IAU_TILE_REC_BYTES	32u
struct iau_tile_rec {
	uint64_t w0, w1, w2, w3;
};
SPDK_STATIC_ASSERT(sizeof(struct iau_tile_rec) == IAU_TILE_REC_BYTES &&
		   offsetof(struct iau_tile_rec, w0) == 0 && offsetof(struct iau_tile_rec, w1) == 8 &&
		   offsetof(struct iau_tile_rec, w2) == 16 && offsetof(struct iau_tile_rec, w3) == 24,
		   "record v6 = the LOCKED 32 B record (0/8/16/24)");
enum iau_tile_rec_kind {
	IAU_TILE_R_CPL = 0,
	IAU_TILE_R_TIMEOUT = 1,
	IAU_TILE_R_ABORTED = 2,
	IAU_TILE_R_DETACHED = 3,
};
#define IAU_TILE_RF_PARKED	0x01u	/* an error record of a parked slot: needs REISSUE or RELEASE */
#define IAU_TILE_RF_ACK		0x02u	/* the ack of an ABORT_*: cdw0 = commands marked, tag NONE */
#define IAU_TILE_RF_CLOSED	0x04u	/* R_DETACHED only: the ring is closed, no record follows on it */
#define IAU_TILE_W2_RFLAGS_SHIFT	40
#define IAU_TILE_W2_RETRIES_SHIFT	48
#define IAU_TILE_W2_KIND_SHIFT	56
#define IAU_TILE_W2_PARKED	(1ull << 40)
#define IAU_TILE_W2_ACK		(1ull << 41)
#define IAU_TILE_W2_CLOSED	(1ull << 42)
SPDK_STATIC_ASSERT(IAU_TILE_W2_PARKED == (uint64_t)IAU_TILE_RF_PARKED << IAU_TILE_W2_RFLAGS_SHIFT &&
		   IAU_TILE_W2_ACK == (uint64_t)IAU_TILE_RF_ACK << IAU_TILE_W2_RFLAGS_SHIFT &&
		   IAU_TILE_W2_CLOSED == (uint64_t)IAU_TILE_RF_CLOSED << IAU_TILE_W2_RFLAGS_SHIFT, "rflags bits");
#define IAU_TILE_MARK_STATUS	((uint16_t)((0xFEu << 1) | (0x7u << 9)))	/* SCT 7, SC 0xFE: 0x0FFC */
#define IAU_TILE_W3_FAST_MASK	0x0FFFu	/* of (w3 >> 48): phase [0], SC [8:1], SCT [11:9]; not CRD, M, DNR */

/* the 16-bit cpl status word (phase bit 0 = 0) */
static inline uint16_t
iau_tile_status(uint8_t sct, uint8_t sc, bool dnr)
{
	return (uint16_t)(((uint16_t)sc << 1) | ((uint16_t)(sct & 7u) << 9) | (dnr ? 0x8000u : 0u));
}
static inline uint64_t
iau_tile_w2(uint32_t cdw0, uint8_t kind, uint8_t retries, uint8_t rflags)
{
	return (uint64_t)cdw0 | ((uint64_t)rflags << IAU_TILE_W2_RFLAGS_SHIFT) |
	       ((uint64_t)retries << IAU_TILE_W2_RETRIES_SHIFT) | ((uint64_t)kind << IAU_TILE_W2_KIND_SHIFT);
}
static inline uint32_t iau_tile_w2_cdw0(uint64_t w2) { return (uint32_t)w2; }
static inline uint32_t iau_tile_w2_cdw1(uint64_t w2) { return (uint32_t)(w2 >> 32); }
static inline uint8_t iau_tile_w2_rflags(uint64_t w2) { return (uint8_t)(w2 >> IAU_TILE_W2_RFLAGS_SHIFT); }
static inline uint8_t iau_tile_w2_retries(uint64_t w2) { return (uint8_t)(w2 >> IAU_TILE_W2_RETRIES_SHIFT); }
static inline uint8_t iau_tile_w2_kind(uint64_t w2) { return (uint8_t)(w2 >> IAU_TILE_W2_KIND_SHIFT); }
/* w3 before the packer sets the ring phase; status's own phase bit is dropped */
static inline uint64_t
iau_tile_w3(uint16_t sqhd, uint16_t route, uint16_t tag, uint16_t status)
{
	return (uint64_t)sqhd | ((uint64_t)route << 16) | ((uint64_t)tag << 32) | ((uint64_t)(status & 0xFFFEu) << 48);
}
static inline uint16_t iau_tile_w3_sqhd(uint64_t w3) { return (uint16_t)w3; }
static inline uint16_t iau_tile_w3_route(uint64_t w3) { return (uint16_t)(w3 >> 16); }
static inline uint16_t iau_tile_w3_tag(uint64_t w3) { return (uint16_t)(w3 >> 32); }
static inline uint8_t iau_tile_w3_phase(uint64_t w3) { return (uint8_t)((w3 >> 48) & 1u); }
static inline uint16_t iau_tile_w3_status(uint64_t w3) { return (uint16_t)((w3 >> 48) & 0xFFFEu); }
/* engine: publish one record, byte-identical to iau_pack_moded_record(w0, w1, w2, w3, phase): w0, w1, w2, then
 * w3 (with the ring phase at bit 48) as a release store. w2 is a relaxed atomic store because the host's
 * iau_tile_rec_peek() reads it before it knows the slot holds a new record. */
static inline void
iau_tile_rec_publish(volatile struct iau_tile_rec *r, uint64_t w0, uint64_t w1, uint64_t w2, uint64_t w3, uint8_t phase)
{
	r->w0 = w0;
	r->w1 = w1;
	__atomic_store_n(&r->w2, w2, __ATOMIC_RELAXED);
	__atomic_store_n(&r->w3, (w3 & ~(1ull << 48)) | ((uint64_t)(phase & 1u) << 48), __ATOMIC_RELEASE);
}
/* host: the first loads of a record slot, in the contract's order: w3 (acquire), then w2. Read w0 / w1 only
 * after iau_tile_rec_ready(w3) holds. Plain loads on x86 (TSO). */
static inline uint64_t
iau_tile_rec_peek(const volatile struct iau_tile_rec *r, uint64_t *w2)
{
	uint64_t w3 = __atomic_load_n(&r->w3, __ATOMIC_ACQUIRE);

	*w2 = __atomic_load_n(&r->w2, __ATOMIC_RELAXED);
	return w3;
}
/* the cpl a record delivers to its callback: cdw0 = w2[31:0], cdw1 masked to 0, bytes 8-15 = w3 */
static inline void
iau_tile_rec_cpl(uint64_t w2, uint64_t w3, struct spdk_nvme_cpl *cpl)
{
	uint64_t lo = w2 & 0xFFFFFFFFull;

	memcpy((uint8_t *)cpl, &lo, 8);
	memcpy((uint8_t *)cpl + 8, &w3, 8);
}
/* a record is present in this slot: its phase is the one the host expects (w3 from iau_tile_rec_peek()) */
static inline bool
iau_tile_rec_ready(uint64_t w3, uint8_t phase)
{
	return (((uint32_t)(w3 >> 48) ^ (uint32_t)phase) & 1u) == 0;
}
/* the fast filter (D6): present, SC = SCT = 0, cdw1 = 0 (one xor test on w3, one test on w2 >> 32); w2 and
 * w3 are the values iau_tile_rec_peek() returned */
static inline bool
iau_tile_rec_fast(uint64_t w2, uint64_t w3, uint8_t phase)
{
	return ((((uint32_t)(w3 >> 48) ^ ((uint32_t)phase & 1u)) & IAU_TILE_W3_FAST_MASK) | (uint32_t)(w2 >> 32)) == 0;
}

/*
 * Tags (D17, A15): gen [15:12] | slot [11:0]. A slot's gen advances on every reuse (iau_tile_gen_next()), and
 * REISSUE, RELEASE and ABORT_TAG check it. The slot pool is capped at IAU_TILE_MAX_SLOTS = 4095 (slots
 * 0..4094), so no tag equals IAU_TILE_TAG_NONE = iau_tile_tag(15, 4095), the tag of acks and DETACHED records.
 */
#define IAU_TILE_TAG_SLOT_BITS	12
#define IAU_TILE_TAG_SLOT_MASK	0x0FFFu
#define IAU_TILE_TAG_GEN_MASK	0xFu
#define IAU_TILE_TAG_NONE		0xFFFFu
#define IAU_TILE_MAX_SLOTS	4095u
SPDK_STATIC_ASSERT(IAU_TILE_TAG_NONE == ((IAU_TILE_TAG_GEN_MASK << IAU_TILE_TAG_SLOT_BITS) | IAU_TILE_TAG_SLOT_MASK) &&
		   IAU_TILE_MAX_SLOTS == IAU_TILE_TAG_SLOT_MASK, "slots 0..4094: slot 0xFFF (in TAG_NONE) never used");
static inline uint16_t
iau_tile_tag(uint8_t gen, uint16_t slot)
{
	return (uint16_t)(((gen & IAU_TILE_TAG_GEN_MASK) << IAU_TILE_TAG_SLOT_BITS) | (slot & IAU_TILE_TAG_SLOT_MASK));
}
static inline uint16_t iau_tile_tag_slot(uint16_t t) { return t & IAU_TILE_TAG_SLOT_MASK; }
static inline uint8_t iau_tile_tag_gen(uint16_t t) { return (uint8_t)(t >> IAU_TILE_TAG_SLOT_BITS); }
static inline uint8_t iau_tile_gen_next(uint8_t gen) { return (uint8_t)((gen + 1u) & IAU_TILE_TAG_GEN_MASK); }

/*
 * Group control block (D3, D4, D17): host memory, IAU_TILE_GCB_ALIGN-aligned, IAU_TILE_GCB_BYTES, zeroed at
 * create; read by the engine when a member binds (the testbed stand-in for the tile's iau_mmio registers).
 *   header  magic "IAU6", version, k (rings, 1..IAU_TILE_MAX_K), n_routes (routes [0, n_routes) may be valid),
 *           gid (group id; ring k is served by engine (gid + k) mod n_engines), ctx_iova / ctx_bytes: an
 *           optional HMB-style IAU context region (0 = none) where silicon may spill slot records at high
 *           device latency (the testbed engine ignores it), flags: IAU_TILE_GCB_F_* (bit 0 RCONS; every other bit
 *           must be 0: the engine refuses a GCB with an unknown flag bit, iau_tile_gcb_flags_ok()).
 *   ring[k] the D and R rings of pair k (64 B-aligned IOVAs, entries per iau_tile_ring_fits(), d_stride 64 or
 *           48). d_head is engine-written: the monotonic count of D slots the engine has consumed (A15 debug
 *           word; the host asserts its written-slot count - d_head < d_entries). r_consumed is host-written
 *           (optional debug tripwire): the monotonic count of records the host has consumed from R_k, stored
 *           before its next d_tail store; the engine only counts r_overflow from it, and only when the GCB
 *           header flag IAU_TILE_GCB_F_RCONS is set. Nothing else in the line changes after create.
 *   db[k]   d_tail: ring k's descriptor-tail doorbell (the slot index after the last published descriptor),
 *           written by the host at most once per poll. One line per ring.
 *   member[r] one line per route: valid (IAU_TILE_MEMBER_VALID), ring (k = r mod K), qid (the SSD queue),
 *           credits (IO credits), nsid, timeout_us (0 = none, D9), entries (the route's slots = SSD SQ entries,
 *           <= IAU_TILE_MAX_SLOTS), echo_iova (entries x 64 B: the engine writes a parked IO slot's rebuilt SQE
 *           at [tag slot], D8), lba_shift, gen (the member gen, IAU_TILE_MEMBER_GEN_MASK bits; == QCFG2's).
 */
#define IAU_TILE_MAX_K		4u
#define IAU_TILE_MAX_ROUTES	256u
#define IAU_TILE_GCB_ALIGN	4096u
#define IAU_TILE_GCB_MAGIC	0x36554149u	/* "IAU6" */
#define IAU_TILE_GCB_VERSION	1u
#define IAU_TILE_MEMBER_VALID	1u
#define IAU_TILE_GCB_F_RCONS	0x1u	/* the host maintains ring[k].r_consumed (the engine's r_overflow tripwire) */
#define IAU_TILE_GCB_F_KNOWN	IAU_TILE_GCB_F_RCONS
static inline bool iau_tile_gcb_flags_ok(uint32_t flags) { return (flags & ~IAU_TILE_GCB_F_KNOWN) == 0; }
struct iau_tile_gcb_ring {
	uint64_t d_iova, r_iova;
	uint32_t d_entries, r_entries;
	uint32_t d_stride, rsvd0;
	uint64_t d_head;
	uint64_t r_consumed;
	uint64_t rsvd[2];
};
struct iau_tile_gcb_db {
	uint32_t d_tail;
	uint32_t rsvd[15];
};
struct iau_tile_gcb_member {
	uint32_t valid;
	uint16_t ring, qid;
	uint32_t credits, nsid;
	uint32_t timeout_us, entries;
	uint64_t echo_iova;
	uint8_t lba_shift, gen;
	uint8_t rsvd8[6];
	uint64_t rsvd[3];
};
struct iau_tile_gcb {
	uint32_t magic;
	uint16_t version, k;
	uint32_t n_routes, gid;
	uint64_t ctx_iova;
	uint32_t ctx_bytes, flags;
	uint64_t rsvd1[4];
	struct iau_tile_gcb_ring ring[IAU_TILE_MAX_K];
	struct iau_tile_gcb_db db[IAU_TILE_MAX_K];
	struct iau_tile_gcb_member member[IAU_TILE_MAX_ROUTES];
};
SPDK_STATIC_ASSERT(sizeof(struct iau_tile_gcb_ring) == 64 && sizeof(struct iau_tile_gcb_db) == 64 &&
		   sizeof(struct iau_tile_gcb_member) == 64, "GCB entries are one line each");
SPDK_STATIC_ASSERT(offsetof(struct iau_tile_gcb, ring) == 64 && offsetof(struct iau_tile_gcb, db) == 320 &&
		   offsetof(struct iau_tile_gcb, member) == 576, "GCB header 64 B, then ring[], db[], member[]");
SPDK_STATIC_ASSERT(offsetof(struct iau_tile_gcb, ctx_iova) == 16 && offsetof(struct iau_tile_gcb_ring, d_head) == 32 &&
		   offsetof(struct iau_tile_gcb_member, gen) == 33 && offsetof(struct iau_tile_gcb_ring, r_consumed) == 40 &&
		   offsetof(struct iau_tile_gcb, flags) == 28, "GCB fields (A9, A15, r_consumed, flags)");
#define IAU_TILE_GCB_BYTES	((uint32_t)(((sizeof(struct iau_tile_gcb) + IAU_TILE_GCB_ALIGN - 1) / IAU_TILE_GCB_ALIGN) * \
					    IAU_TILE_GCB_ALIGN))

/*
 * QCFG2[q] (IAU page 0x5A00 + 8 q, q < IAU_TILE_QIDS; written by the host before Create I/O SQ q, read by the
 * target there): route [7:0] | member gen [10:8] | TILE [11] | gcb_iova [63:12] (IAU_TILE_GCB_ALIGN-aligned).
 * 0 = not a tile queue (the P3 40 B path). The member gen equals the GCB member line's gen; the host bumps it
 * on every rejoin of a route (iau_tile_member_gen_next()), and the engine keeps the last gen per (GCB, route)
 * (A15). No reserved bits remain.
 */
#define IAU_QCFG2_OFFSET	0x5A00u
#define IAU_TILE_QIDS		64u	/* == IAU_MAX_QID (target), NVME_PCIE_IAU_PAGE_QIDS (host) */
#define IAU_QCFG2_ROUTE_MASK	0xFFull
#define IAU_QCFG2_GEN_SHIFT	8
#define IAU_QCFG2_GEN_MASK	0x7ull
#define IAU_QCFG2_TILE		0x800ull
#define IAU_QCFG2_GCB_MASK	0xFFFFFFFFFFFFF000ull
#define IAU_QCFG2_INVALID	0x800ull	/* == IAU_QCFG2_TILE alone: what iau_tile_qcfg2_pack() returns for a misfit */
#define IAU_TILE_MEMBER_GEN_MASK	0x7u
SPDK_STATIC_ASSERT(IAU_QCFG2_ROUTE_MASK + 1 >= IAU_TILE_MAX_ROUTES && IAU_QCFG2_GEN_MASK == IAU_TILE_MEMBER_GEN_MASK &&
		   (IAU_QCFG2_ROUTE_MASK | (IAU_QCFG2_GEN_MASK << IAU_QCFG2_GEN_SHIFT) | IAU_QCFG2_TILE |
		    IAU_QCFG2_GCB_MASK) == UINT64_MAX &&
		   (IAU_QCFG2_ROUTE_MASK ^ (IAU_QCFG2_GEN_MASK << IAU_QCFG2_GEN_SHIFT) ^ IAU_QCFG2_TILE ^
		    IAU_QCFG2_GCB_MASK) == UINT64_MAX && ~IAU_QCFG2_GCB_MASK + 1 == IAU_TILE_GCB_ALIGN,
		   "QCFG2 = route[7:0] | gen[10:8] | V6[11] | gcb_iova[63:12]");
SPDK_STATIC_ASSERT(0x5800u + 8 * IAU_TILE_QIDS <= IAU_QCFG2_OFFSET && IAU_QCFG2_OFFSET + 8 * IAU_TILE_QIDS <= 0x5C00u,
		   "QCFG2 sits between host Mode E's free-slot IOVA (0x5800 + 8 q) and seed (0x5C00 + 4 q) tables");
static inline uint8_t iau_tile_member_gen_next(uint8_t gen) { return (uint8_t)((gen + 1u) & IAU_TILE_MEMBER_GEN_MASK); }
SPDK_STATIC_ASSERT((IAU_TILE_CF_GEN_MASK >> IAU_TILE_CF_GEN_SHIFT) == IAU_TILE_MEMBER_GEN_MASK && (IAU_TILE_CF_GEN_MASK &
		   IAU_TILE_CF_TMO_MASK) == 0, "descriptor cflags [15:13] carry the 3-bit member gen (H3)");
/* IAU_QCFG2_INVALID (TILE set, GCB 0) for a gcb_iova that is 0 or not IAU_TILE_GCB_ALIGN-aligned, a route >
 * IAU_QCFG2_ROUTE_MASK or a gen > IAU_QCFG2_GEN_MASK: the host refuses the queue on it, and a host that wrote it
 * anyway gets its Create I/O SQ refused (unpack -EINVAL), never a silent P3 40 B binding (QCFG2 0). */
static inline uint64_t
iau_tile_qcfg2_pack(uint64_t gcb_iova, uint16_t route, uint8_t gen)
{
	if (gcb_iova == 0 || (gcb_iova & ~IAU_QCFG2_GCB_MASK) != 0 || route > IAU_QCFG2_ROUTE_MASK ||
	    gen > IAU_QCFG2_GEN_MASK) {
		return IAU_QCFG2_INVALID;
	}
	return gcb_iova | IAU_QCFG2_TILE | ((uint64_t)gen << IAU_QCFG2_GEN_SHIFT) | (uint64_t)route;
}
/* 0 for a tile queue; -ENOENT for 0 (not a tile queue); -EINVAL if TILE is clear or gcb_iova is 0. The fields are
 * decoded in every case. */
static inline int
iau_tile_qcfg2_unpack(uint64_t q, uint64_t *gcb_iova, uint16_t *route, uint8_t *gen)
{
	*gcb_iova = q & IAU_QCFG2_GCB_MASK;
	*route = (uint16_t)(q & IAU_QCFG2_ROUTE_MASK);
	*gen = (uint8_t)((q >> IAU_QCFG2_GEN_SHIFT) & IAU_QCFG2_GEN_MASK);
	if (q == 0) {
		return -ENOENT;
	}
	if ((q & IAU_QCFG2_TILE) == 0 || *gcb_iova == 0) {
		return -EINVAL;
	}
	return 0;
}

/*
 * Ring invariant (D13 as amended by A1; checked at registration, the engine refuses a violation). Engine
 * slots <= credits at every fetch. Per route with c IO credits:
 *   route_need_d(c) = 3c + 1 + CTRL: per credit up to 3 unfetched D slots (RELEASE + a new two-slot RAW),
 *                     per route one FLUSH and CTRL control descriptors (an unfetched RELEASE can follow a
 *                     returned credit);
 *   route_need_r(c) = 3c + 1 + CTRL: per credit up to 3 unconsumed records (a TIMEOUT notice, a PARKED error
 *                     record and a FLUSH disposal record), per route one DETACHED and CTRL acks.
 * A group fits iff sum(need_d) + 1 <= d_entries (the D ring keeps one slot empty) and sum(need_r) <= r_entries.
 * A private group of one (D4) has K = 1 and d_entries = r_entries = iau_tile_private_entries(c) = round64(3c + 4).
 */
#define IAU_TILE_CTRL_CREDITS	2u
static inline uint64_t iau_tile_route_need_d(uint32_t c) { return 3ull * c + 1 + IAU_TILE_CTRL_CREDITS; }
static inline uint64_t iau_tile_route_need_r(uint32_t c) { return 3ull * c + 1 + IAU_TILE_CTRL_CREDITS; }
static inline bool
iau_tile_ring_fits(uint64_t need_d, uint64_t need_r, uint32_t d_entries, uint32_t r_entries)
{
	return need_d < d_entries && need_r <= r_entries;
}
/* 0 (no ring fits) for c > IAU_TILE_MAX_SLOTS: a route has at most that many slots */
static inline uint32_t
iau_tile_private_entries(uint32_t c)
{
	return c > IAU_TILE_MAX_SLOTS ? 0 : (uint32_t)((3ull * c + 4 + 63) & ~63ull);
}

/*
 * PRP and SQE building (D7). The engine builds an IO kind's SQE from d0/d1/d2/d4 with PRP1 = d2 (dword
 * aligned, as stock nvme_pcie_prp_list_append() requires), PRP2 = the second page or, beyond two pages, its
 * own PRP list (at most IAU_TILE_PRP_MAX entries at a non-zero, 8 B-aligned list_iova, within one page: no
 * chaining); at most IAU_TILE_IO_MAX_BYTES. A REISSUE and the echo slab rebuild the same SQE from the slot's
 * replay fields with the same builder (D17).
 */
#define IAU_TILE_PAGE		4096u
#define IAU_TILE_PRP_MAX		32u	/* any buffer of <= 128 KiB, whatever its offset */
#define IAU_TILE_IO_MAX_BYTES	(IAU_TILE_PAGE * IAU_TILE_PRP_MAX)	/* an IO kind moves at most 128 KiB */
#define IAU_TILE_CDW12_IO_RSVD	0x3FFF0000u	/* cdw12 [29:16] (PRINFO, DTYPE, STC, DEAC, ...): RAW if set */

/* bytes an IO kind transfers; -ENOTSUP if the opcode / form is not an IO kind (the host uses RAW);
 * -EINVAL for an lba_shift > 31 */
static inline int64_t
iau_tile_io_len(uint8_t opc, uint16_t nlb, uint64_t d1, uint8_t lba_shift)
{
	switch (opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_COMPARE:
		if (lba_shift > 31) {
			return -EINVAL;
		}
		return (int64_t)(((uint64_t)nlb + 1) << lba_shift);
	case SPDK_NVME_OPC_FLUSH:
	case SPDK_NVME_OPC_WRITE_ZEROES:
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
	case SPDK_NVME_OPC_VERIFY:
		return 0;
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:	/* cdw10 [7:0] = ranges - 1, 16 B each; cdw12 reserved */
		return nlb == 0 ? (int64_t)(((d1 & 0xFFu) + 1) * 16u) : -ENOTSUP;
	default:
		return -ENOTSUP;
	}
}
/* PRP list entries a buffer of len bytes at buf needs (0 for at most two pages; may exceed IAU_TILE_PRP_MAX) */
static inline uint32_t
iau_tile_prp_entries(uint64_t buf, uint64_t len)
{
	uint64_t first = IAU_TILE_PAGE - (buf & (IAU_TILE_PAGE - 1)), n;

	if (len <= first + IAU_TILE_PAGE) {
		return 0;
	}
	n = (len - first + IAU_TILE_PAGE - 1) / IAU_TILE_PAGE;
	return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}
/* PRP1/PRP2 for len bytes at buf (no data: both 0, buf ignored); beyond two pages, list[0..n) = the pages after
 * the first and PRP2 = list_iova. 0; -EINVAL (PRP1 = PRP2 = 0, list untouched) if buf is not dword aligned, len
 * > IAU_TILE_IO_MAX_BYTES, or a needed list's list_iova is 0, not 8 B aligned or would cross a page. */
static inline int
iau_tile_prp(uint64_t buf, uint64_t len, uint64_t *list, uint64_t list_iova, uint64_t *prp1, uint64_t *prp2)
{
	uint64_t first = IAU_TILE_PAGE - (buf & (IAU_TILE_PAGE - 1)), p;
	uint32_t n, i;

	*prp1 = 0;
	*prp2 = 0;
	if (len == 0) {
		return 0;
	}
	if ((buf & 3u) != 0 || len > IAU_TILE_IO_MAX_BYTES) {
		return -EINVAL;
	}
	if (len <= first) {
		*prp1 = buf;
		return 0;
	}
	if (len <= first + IAU_TILE_PAGE) {
		*prp1 = buf;
		*prp2 = buf + first;
		return 0;
	}
	n = iau_tile_prp_entries(buf, len);
	if (n > IAU_TILE_PRP_MAX || list_iova == 0 || (list_iova & 7u) != 0 ||
	    (list_iova & (IAU_TILE_PAGE - 1)) + 8ull * n > IAU_TILE_PAGE) {
		return -EINVAL;
	}
	for (i = 0, p = buf + first; i < n; i++, p += IAU_TILE_PAGE) {
		list[i] = p;
	}
	*prp1 = buf;
	*prp2 = list_iova;
	return 0;
}
/* The SSD-facing SQE of an IO-kind descriptor d (route nsid route_nsid, cid = tag). 0; -ENOTSUP if d is not
 * an IO kind, its opcode / form has no IO kind, or d4 names a foreign nsid; -EINVAL if iau_tile_prp() refuses
 * the buffer or the list (or lba_shift > 31). cmd is written only on success. */
static inline int
iau_tile_sqe_from_io(const uint64_t d[8], uint16_t tag, uint32_t route_nsid, uint8_t lba_shift, uint64_t *list,
		   uint64_t list_iova, struct spdk_nvme_cmd *cmd)
{
	uint8_t opc = iau_tile_d0_opc(d[0]), fl = iau_tile_d0_flags(d[0]);
	uint16_t nlb = iau_tile_d0_nlb(d[0]);
	uint32_t nsid = (uint32_t)d[4];
	uint64_t prp1, prp2;
	int64_t len;

	if (iau_tile_d0_kind(d[0]) != IAU_TILE_K_IO || (nsid != 0 && nsid != route_nsid)) {
		return -ENOTSUP;
	}
	len = iau_tile_io_len(opc, nlb, d[1], lba_shift);
	if (len < 0) {
		return (int)len;
	}
	if (iau_tile_prp(d[2], (uint64_t)len, list, list_iova, &prp1, &prp2) != 0) {
		return -EINVAL;
	}
	memset(cmd, 0, sizeof(*cmd));
	cmd->opc = opc;
	cmd->fuse = fl & IAU_TILE_F_FUSE_MASK;
	cmd->cid = tag;
	cmd->nsid = route_nsid;
	cmd->cdw10 = (uint32_t)d[1];
	cmd->cdw11 = (uint32_t)(d[1] >> 32);
	cmd->cdw12 = (uint32_t)nlb | ((fl & IAU_TILE_F_FUA) ? (1u << 30) : 0u) | ((fl & IAU_TILE_F_LR) ? (1u << 31) : 0u);
	cmd->dptr.prp.prp1 = prp1;
	cmd->dptr.prp.prp2 = prp2;
	return 0;
}
/* True if host command c with its IOVA-contiguous payload at buf (no metadata) is expressible as an IO kind on
 * a route with this lba_shift and nsid: then iau_tile_sqe_from_io() of {iau_tile_d0_from_cmd(), iau_tile_d1_from_cmd(),
 * buf, 0, c->nsid} succeeds (with a valid engine list) and is c with cid = tag and PRPs built from buf.
 * - nsid must be the route's, and the route must have one (A15; a command with nsid 0 also goes RAW, so the
 *   SSD sees exactly the stock SQE);
 * - a payload must be dword aligned and at most IAU_TILE_IO_MAX_BYTES: otherwise RAW, whose stock PRP builder
 *   refuses an unaligned buffer exactly as stock does. buf is ignored for commands without data. */
static inline bool
iau_tile_cmd_is_io(const struct spdk_nvme_cmd *c, uint64_t buf, uint8_t lba_shift, uint32_t route_nsid)
{
	int64_t len = iau_tile_io_len(c->opc, (uint16_t)c->cdw12, iau_tile_d1_from_cmd(c), lba_shift);

	return len >= 0 && (uint64_t)len <= IAU_TILE_IO_MAX_BYTES && (len == 0 || (buf & 3u) == 0) && route_nsid != 0 &&
	       c->nsid == route_nsid && c->psdt == 0 && c->rsvd1 == 0 && c->rsvd2 == 0 && c->rsvd3 == 0 &&
	       c->mptr == 0 && (c->cdw12 & IAU_TILE_CDW12_IO_RSVD) == 0 && c->cdw13 == 0 && c->cdw14 == 0 &&
	       c->cdw15 == 0;
}
/* Replay (D17): rebuild an IO-kind slot's SQE from its replay fields {opc, flags, nlb, slba (d1), buf}, with
 * the route's nsid (a slot keeps no nsid) and the same builder; only flags & IAU_TILE_F_REPLAY_MASK count. */
static inline int
iau_tile_replay_to_sqe(uint8_t opc, uint8_t flags, uint16_t nlb, uint64_t slba, uint64_t buf, uint32_t nsid,
		     uint16_t tag, uint8_t lba_shift, uint64_t *list, uint64_t list_iova, struct spdk_nvme_cmd *cmd)
{
	const uint64_t d[8] = { iau_tile_d0(opc, (uint8_t)(flags & IAU_TILE_F_REPLAY_MASK), 0, nlb, 0), slba, buf,
				0, 0, 0, 0, 0
			      };

	return iau_tile_sqe_from_io(d, tag, nsid, lba_shift, list, list_iova, cmd);
}

#endif /* SPDK_INTERNAL_IAU_TILE_H */
