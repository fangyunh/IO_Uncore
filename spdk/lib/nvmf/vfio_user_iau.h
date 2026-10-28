/* SPDX-License-Identifier: BSD-3-Clause
 * IAU v4.1 (Mode D) engine for the SPDK vfio-user target (realcore Stage 4).
 * Ports RTL sq_fetch_engine (40B descriptor decode) and cq_callback_engine
 * (per-queue BATCH_N/BATCH_T coalescing, 32B callback records, credit return
 * per emitted record). RTL is the design standard; gem5 controller.cc is the
 * second reference. Single-thread per SQ: every function taking a qid must be
 * called only from the poll-group thread that owns that SQ.
 */
#ifndef SPDK_VFIO_USER_IAU_H
#define SPDK_VFIO_USER_IAU_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/nvme_spec.h"

#define IAU_MODE_BASELINE		0
#define IAU_MODE_D			4
#define IAU_MODE_ENGINE			5	/* IAU-next P3: SSD threads vanilla + IAU-engine threads (arm I1e) */
/* 1: the IAU-engine threads are wired into vfio_user.c (P3.9). A build with 0 refuses IAU_UNCORE_MODE=5
 * at listen and at controller create, so it cannot run mode 5 as the vanilla path; iau_ctx_init()
 * itself accepts 5 either way. */
#define IAU_ENGINE_AVAILABLE		1

/* BAR0 contract (== spdk/lib/nvme/nvme_pcie_internal.h). */
#define IAU_BAR0_UNCORE_PAGE_OFFSET	0x5000u
#define IAU_BAR0_UNCORE_PAGE_SIZE	0x1000u
#define IAU_SQ_DESC_RING_IOVA_OFFSET	0x5000u
#define IAU_CQ_REC_RING_IOVA_OFFSET	0x5400u
#define IAU_MAX_QID			64u
/* IAU-next P3 registration and doorbells in the same page (== spdk/lib/nvme/nvme_pcie_internal.h;
 * tests/realcore/test_iau_bar0_contract.py checks both headers). Per qid:
 *   IAU_DESC_TAIL_OFFSET + 4*qid  DESC_TAIL[q], the host's descriptor-tail doorbell (the engine reads it)
 *   IAU_CQDB_SINK_OFFSET + 4*qid  where the host's CQ-head doorbell pointer lands; must stay 0
 *   IAU_QCFG_OFFSET + 8*qid       QCFG[q] = valid[0] | reserved[15:1] = 0 | credits[31:16] | nsid[63:32],
 *                                 written once by the host before Create I/O SQ (the target reads it there);
 *                                 iau_qcfg_pack() / iau_qcfg_unpack() below, host nvme_pcie_iau_qcfg_pack() */
#define IAU_DESC_TAIL_OFFSET		0x5200u
#define IAU_CQDB_SINK_OFFSET		0x5300u
#define IAU_QCFG_OFFSET			0x5600u
#define IAU_QCFG_VALID			0x1ull
#define IAU_QCFG_RSVD_MASK		0xFFFEull
#define IAU_QCFG_CREDITS_SHIFT		16
#define IAU_QCFG_CREDITS_MASK		0xFFFFull
#define IAU_QCFG_NSID_SHIFT		32
#define IAU_QCFG_NSID_MASK		0xFFFFFFFFull
SPDK_STATIC_ASSERT((IAU_QCFG_VALID & IAU_QCFG_RSVD_MASK) == 0 && (IAU_QCFG_VALID | IAU_QCFG_RSVD_MASK) == 0xFFFFull &&
		   (IAU_QCFG_CREDITS_MASK << IAU_QCFG_CREDITS_SHIFT) == 0xFFFF0000ull &&
		   (IAU_QCFG_NSID_MASK << IAU_QCFG_NSID_SHIFT) == 0xFFFFFFFF00000000ull,
		   "QCFG = valid[0] | reserved[15:1] | credits[31:16] | nsid[63:32]");
SPDK_STATIC_ASSERT(IAU_SQ_DESC_RING_IOVA_OFFSET + 8 * IAU_MAX_QID <= IAU_DESC_TAIL_OFFSET &&
		   IAU_DESC_TAIL_OFFSET + 4 * IAU_MAX_QID <= IAU_CQDB_SINK_OFFSET &&
		   IAU_CQDB_SINK_OFFSET + 4 * IAU_MAX_QID <= IAU_CQ_REC_RING_IOVA_OFFSET &&
		   IAU_CQ_REC_RING_IOVA_OFFSET + 8 * IAU_MAX_QID <= IAU_QCFG_OFFSET &&
		   IAU_QCFG_OFFSET + 8 * IAU_MAX_QID <= 0x5800u, "IAU page tables overlap (0x5800: host Mode E)");

#define IAU_DESC_D_BYTES		40u
#define IAU_REC_BYTES			32u
#define IAU_CQ_BATCH_N_DEFAULT		1u	/* F4 (G-05): N=8 / 4 us caused the low-depth loss and never helped a CPU-bound cell */
#define IAU_CQ_BATCH_T_NS_DEFAULT	4000u

/* LOCKED 40B Mode-D descriptor (== host struct nvme_uncore_sq_desc_d). */
struct iau_desc_d {
	uint64_t w0;	/* opcode[63:56] flags[55:48] cid[47:32] nlb[31:16] control[15:0] */
	uint64_t w1;	/* slba */
	uint64_t w2;	/* cookie (host tracker pointer); not echoed in Mode D */
	uint64_t w3;	/* cb_arg */
	uint64_t w4;	/* cb_fn */
};
SPDK_STATIC_ASSERT(sizeof(struct iau_desc_d) == IAU_DESC_D_BYTES, "Mode-D descriptor must be 40 B");

struct iau_staged {
	uint64_t cb_fn;
	uint64_t cb_arg;
	uint64_t cpl_lo;	/* NVMe cpl bytes [0:8) */
	uint64_t cpl_hi_raw;	/* NVMe cpl bytes [8:16), cid at [47:32] */
};

#define IAU_SQ_ALIGN 128u	/* two 64 B lines: per-SQ state is private to one device thread */

struct __attribute__((aligned(IAU_SQ_ALIGN))) iau_sq {
	bool				active;
	uint32_t			entries;	/* SQ size == descriptor ring == record ring */
	const volatile struct iau_desc_d	*desc_ring;
	volatile uint64_t		*rec_ring;	/* entries * 4 words */
	uint32_t			dev_tail;	/* next descriptor to fetch */
	uint32_t			rec_tail;	/* next record slot */
	uint8_t				rec_phase;	/* starts 1 (host cq_rec_phase = 1), flips on wrap */
	struct iau_staged		*staging;	/* 'entries' slots (QD-bounded staging SRAM) */
	uint32_t			staged;
	uint64_t			oldest_tsc;
	uint32_t			credits_in_use;	/* RTL credit_manager: slots held until emitted */
	uint64_t			fetched;
	uint64_t			emitted;
	uint64_t			flush_by_n;
	uint64_t			flush_by_t;
};
SPDK_STATIC_ASSERT(sizeof(struct iau_sq) % IAU_SQ_ALIGN == 0, "iau_sq must be a multiple of IAU_SQ_ALIGN");

struct iau_ctx {
	int				mode;
	uint32_t			batch_n;
	uint64_t			batch_t_ticks;
	const volatile uint64_t		*uncore_page;	/* target view of BAR0 page 0x5000 */
	struct iau_sq			*sq;	/* IAU_MAX_QID entries, IAU_SQ_ALIGN-aligned heap block */
};

/* Reads IAU_UNCORE_MODE (0|4|5, default 0), IAU_CQ_BATCH_N (default 1),
 * IAU_CQ_BATCH_T_NS (default 4000). Returns 0 or -EINVAL. */
int iau_ctx_init(struct iau_ctx *ctx, const volatile void *uncore_page, uint64_t ticks_hz);
void iau_ctx_fini(struct iau_ctx *ctx);

static inline bool
iau_mode_d(const struct iau_ctx *ctx)
{
	return ctx != NULL && ctx->mode == IAU_MODE_D;
}

static inline bool
iau_mode_engine(const struct iau_ctx *ctx)
{
	return ctx != NULL && ctx->mode == IAU_MODE_ENGINE;
}

/* QCFG[q] as the host writes it (== host nvme_pcie_iau_qcfg_pack()): valid, reserved bits zero,
 * credits = the host's in-flight cap, nsid. */
static inline uint64_t
iau_qcfg_pack(uint16_t credits, uint32_t nsid)
{
	return IAU_QCFG_VALID | (((uint64_t)credits & IAU_QCFG_CREDITS_MASK) << IAU_QCFG_CREDITS_SHIFT) |
	       (((uint64_t)nsid & IAU_QCFG_NSID_MASK) << IAU_QCFG_NSID_SHIFT);
}

/* Decode QCFG[q]: 0 for a registered queue (valid set, reserved bits zero), -EINVAL if a reserved bit
 * is set (valid or not), -ENOENT if the valid bit is clear. *credits and *nsid are written in every case. */
static inline int
iau_qcfg_unpack(uint64_t qcfg, uint32_t *credits, uint32_t *nsid)
{
	*credits = (uint32_t)((qcfg >> IAU_QCFG_CREDITS_SHIFT) & IAU_QCFG_CREDITS_MASK);
	*nsid = (uint32_t)((qcfg >> IAU_QCFG_NSID_SHIFT) & IAU_QCFG_NSID_MASK);
	if ((qcfg & IAU_QCFG_RSVD_MASK) != 0) {
		return -EINVAL;
	}
	return (qcfg & IAU_QCFG_VALID) != 0 ? 0 : -ENOENT;
}

/* IAU-next P3.9 (D5): what the target does with QCFG[q] when the host creates I/O SQ q, given the
 * IAU_UNCORE_MODE the target runs. IAU_QCFG_BIND: mode 5 and the queue is registered (*credits, *nsid
 * decoded; their ranges are iau_eng_q_init()'s to refuse). IAU_QCFG_NONE: mode 0 or 4 and QCFG 0, so
 * no engine. Negative = refuse the Create I/O SQ (INVALID_FIELD): -ENOENT mode 5 and the queue is not
 * registered; -EEXIST mode 0/4 but the host wrote QCFG (SPDK_IAU_ENGINE=1 against a target without
 * engines: nobody would read its DESC_TAIL); -EINVAL a reserved QCFG bit (any mode). */
#define IAU_QCFG_NONE	0
#define IAU_QCFG_BIND	1
static inline int
iau_qcfg_decide(int mode, uint64_t qcfg, uint32_t *credits, uint32_t *nsid)
{
	int rc = iau_qcfg_unpack(qcfg, credits, nsid);

	if (mode == IAU_MODE_ENGINE) {
		return rc == 0 ? IAU_QCFG_BIND : rc;
	}
	if (qcfg == 0) {
		return IAU_QCFG_NONE;
	}
	return rc == -EINVAL ? -EINVAL : -EEXIST;
}

/* Ring-base IOVAs the host wrote into the shared BAR0 page (0 if unset). */
uint64_t iau_desc_ring_iova(const struct iau_ctx *ctx, uint16_t qid);
uint64_t iau_rec_ring_iova(const struct iau_ctx *ctx, uint16_t qid);

int iau_sq_attach(struct iau_ctx *ctx, uint16_t qid, uint32_t entries,
		  const volatile void *desc_ring, volatile void *rec_ring);
void iau_sq_detach(struct iau_ctx *ctx, uint16_t qid);

/* Decode one LOCKED descriptor. PRP1/PRP2 = 0 (spec decision D1(a)). nsid = 1. */
void iau_decode_desc(const struct iau_desc_d *d, struct spdk_nvme_cmd *cmd,
		     uint64_t *cb_fn, uint64_t *cb_arg);
/* Decode desc_ring[dev_tail] and advance dev_tail. The caller must issue
 * spdk_rmb() after reading the doorbell and before the first fetch of a batch. */
void iau_fetch_one(struct iau_ctx *ctx, uint16_t qid, struct spdk_nvme_cmd *cmd,
		   uint64_t *cb_fn, uint64_t *cb_arg);
/* Stage one completion (takes a credit); flushes when staged >= batch_n. */
void iau_complete(struct iau_ctx *ctx, uint16_t qid, uint64_t cb_fn, uint64_t cb_arg,
		  const struct spdk_nvme_cpl *cpl, uint64_t now_tsc);
/* Same, from the cpl's two raw words: bytes [0:8) and [8:16) as little-endian u64. */
void iau_complete_words(struct iau_ctx *ctx, uint16_t qid, uint64_t cb_fn, uint64_t cb_arg,
			uint64_t cpl_lo, uint64_t cpl_hi_raw, uint64_t now_tsc);

/* Bytes [8:16) of a zeroed struct spdk_nvme_cpl with sqhd, sqid, cid, status.sct and
 * status.sc set (phase 0), built in registers. P1.2: the device path used to build the
 * struct with 16-bit stores and read it back as a u64 (a store-forwarding stall). */
static inline uint64_t
iau_cpl_hi_word(uint16_t sqhd, uint16_t sqid, uint16_t cid, uint8_t sct, uint8_t sc)
{
	struct spdk_nvme_status st;
	uint16_t raw;

	SPDK_STATIC_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "cpl words are little-endian");
	memset(&st, 0, sizeof(st));
	st.sct = sct;
	st.sc = sc;
	memcpy(&raw, &st, sizeof(raw));
	return (uint64_t)sqhd | ((uint64_t)sqid << 16) | ((uint64_t)cid << 32) | ((uint64_t)raw << 48);
}
/* Flush this SQ if its oldest staged completion is >= batch_t old. */
void iau_sq_poll_timer(struct iau_ctx *ctx, uint16_t qid, uint64_t now_tsc);
/* True while this SQ holds staged completions: the only case in which
 * iau_sq_poll_timer() reads now_tsc, so a poller may skip the tick read otherwise. */
static inline bool
iau_sq_has_staged(const struct iau_ctx *ctx, uint16_t qid)
{
	return ctx->sq[qid].staged != 0;
}
/* Publish every staged completion as a 32B record (w0..w2, wmb, w3). */
void iau_flush(struct iau_ctx *ctx, uint16_t qid);

#endif
