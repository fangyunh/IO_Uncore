/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

#ifndef __NVME_PCIE_INTERNAL_H__
#define __NVME_PCIE_INTERNAL_H__

/*
 * Number of completion queue entries to process before ringing the
 *  completion queue doorbell.
 */
#define NVME_MIN_COMPLETIONS	(1)
#define NVME_MAX_COMPLETIONS	(128)

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(250)

#define NVME_MAX_PRP_LIST_ENTRIES	(503)

/* Minimum admin queue size */
#define NVME_PCIE_MIN_ADMIN_QUEUE_SIZE	(256)

/* PCIe transport extensions for spdk_nvme_ctrlr */
struct nvme_pcie_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers *regs;

	/** NVMe MMIO register size */
	uint64_t regs_size;

	struct {
		/* BAR mapping address which contains controller memory buffer */
		void *bar_va;

		/* BAR physical address which contains controller memory buffer */
		uint64_t bar_pa;

		/* Controller memory buffer size in Bytes */
		uint64_t size;

		/* Current offset of controller memory buffer, relative to start of BAR virt addr */
		uint64_t current_offset;

		void *mem_register_addr;
		size_t mem_register_size;
	} cmb;

	struct {
		/* BAR mapping address which contains persistent memory region */
		void *bar_va;

		/* BAR physical address which contains persistent memory region */
		uint64_t bar_pa;

		/* Persistent memory region size in Bytes */
		uint64_t size;

		void *mem_register_addr;
		size_t mem_register_size;
	} pmr;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;

	/* Opaque handle to associated PCI device. */
	struct spdk_pci_device *devhandle;

	/* Flag to indicate the MMIO register has been remapped */
	bool is_remapped;

	volatile uint32_t *doorbell_base;

	/* IO-Uncore Mode 2B (poll-lite) host hint register mapped at BAR0+0x2000.
	 * Set in nvme_pcie_ctrlr_allocate_bars() iff env SPDK_UNCORE_MODE_B=1
	 * AND the mapped BAR0 covers the offset; NULL otherwise. When non-NULL
	 * and reading 0, the polling fast path returns immediately, eliminating
	 * the DRAM CQ phase-bit scan. Reads are uncached and load-acquire-safe
	 * on x86 for aligned 4-byte access. */
	volatile uint32_t *uncore_hint_reg;

	/* IO-Uncore Mode 2B mailbox region (BAR0 + MAILBOX_BASE_OFFSET).  When
	 * non-NULL, nvme_pcie_qpair_submit_tracker emits 3 sequential 8-byte
	 * writes to uncore_mailbox_base + (qpair_id * MAILBOX_STRIDE_BYTES/8)
	 * for each submitted I/O instead of building a 64-byte SQE in DRAM
	 * and ringing the standard NVMe SQ tail doorbell.  Set in
	 * nvme_pcie_ctrlr_allocate_bars() iff env SPDK_UNCORE_MODE_B=1 AND
	 * the mapped BAR0 covers the mailbox region; NULL otherwise. */
	volatile uint64_t *uncore_mailbox_base;

	/* Mechanism #1 (HW free-CID ring) read endpoints, one uint32 per qpair.
	 * Reading uncore_free_cid_base[qid] pops the next free CID for that
	 * qpair (low 16 bits); returns 0xFFFF when the ring is empty.  Replaces
	 * SPDK's TAILQ_REMOVE(&free_tr) on the submission hot path. */
	volatile uint32_t *uncore_free_cid_base;

	/* Mechanism #2 (HW queue-depth counter) read endpoints, one uint32 per
	 * qpair.  Pure read, no side effect.  Replaces SPDK's software
	 * pqpair->qpair.queue_depth counter. */
	volatile uint32_t *uncore_qdepth_base;

	/* IO-Uncore v3 (Mode C): BAR0 IOVA-base tables for the per-qpair host-DRAM
	 * descriptor ring (0x5000) and completion record ring (0x5400).  8 bytes
	 * per qid; the host MMIO-writes each ring's IOVA to base[qid] ONCE at
	 * qpair-create.  Set in nvme_pcie_ctrlr_allocate_bars() iff env
	 * SPDK_UNCORE_MODE_C=1 AND the BAR is large enough; NULL otherwise. */
	volatile uint64_t *uncore_sq_desc_iova_reg;
	volatile uint64_t *uncore_cq_rec_iova_reg;

	/* IO-Uncore v4.2 (Mode E): BAR0 tables for the per-qpair free-slot ring.
	 * uncore_free_slot_iova_reg (0x5800, 8 B/qid) receives the ring IOVA;
	 * uncore_free_slot_seed_reg (0x5C00, 4 B/qid) receives num_trackers, whose
	 * write tells the device where its producer cursor starts (past the
	 * host-seeded region).  Set in allocate_bars() iff SPDK_UNCORE_MODE_E=1 AND
	 * the BAR is large enough; NULL otherwise. */
	volatile uint64_t *uncore_free_slot_iova_reg;
	volatile uint32_t *uncore_free_slot_seed_reg;
};

/* Mode 2B mailbox layout — must match SimpleSSD's MailboxBase/Stride cfg.
 * Per-qid slot = 32 bytes (4 × uint64_t).  Slot 0 (admin) is allocated
 * but unused by the submit hot path.
 * With MaxIOCQueue=64 (cqsize=65) the mailbox region spans
 *   0x3000 .. 0x3000 + 65*0x20 = 0x3820
 * which is why FREE_CID_BASE_OFFSET below is 0x4000, not 0x3400. */
#define MAILBOX_BASE_OFFSET   0x3000
#define MAILBOX_STRIDE_BYTES  0x20
/* Mechanism #1 (free-CID ring) + #2 (queue-depth counter) read endpoints.
 * Must match SimpleSSD's FreeCIDBase cfg.  Layout (per qpair):
 *   uncore_free_cid_base[qid]                         -> next free CID
 *   uncore_qdepth_base[qid]   = base + 0x400 / sizeof  -> inflight count
 * Moved 0x3400 -> 0x4000 (and 0x3800 -> 0x4400) on 2026-05-14 to give the
 * mailbox region 4 KB of headroom for up to 128 qids at stride 0x20. */
#define FREE_CID_BASE_OFFSET  0x4000
#define QDEPTH_BASE_OFFSET    0x4400

/* IO-Uncore v3 (Mode C) host-DRAM descriptor-ring submit path.
 * Two BAR0 IOVA-base tables, 8 bytes (one uint64_t IOVA) per qid.  The host
 * MMIO-writes each qpair's ring IOVA to base + 8*qid ONCE at qpair-create
 * (never per IO).  Must match SimpleSSD's SqDescRingBase/CqRecRingBase cfg;
 * the device side maps 0x5000 (submit desc ring) / 0x5400 (completion record
 * ring).  Submit-ring slot = 24 bytes (SqDescBytes), record-ring slot = 16
 * bytes (CqRecBytes). */
#define SQ_DESC_RING_IOVA_OFFSET  0x5000
#define CQ_REC_RING_IOVA_OFFSET   0x5400

/* IO-Uncore v4.2 (Mode E) device-owned tracker-slot free ring.  Two more BAR0
 * tables past the Mode-C/D ring bases:
 *   FREE_SLOT_RING_IOVA_OFFSET[qid] (8 B) -> host-DRAM free-slot ring IOVA
 *   FREE_SLOT_SEED_OFFSET[qid]      (4 B) -> num_trackers; writing it triggers
 *                                            the device to DMA-seed the ring
 *                                            with slots 0..N-1 (must be written
 *                                            AFTER the IOVA).
 * Each free-slot ring entry is one uint64_t {phase@bit48, slot[15:0]}; the ring
 * holds num_entries entries.  Must match SimpleSSD's device-side offsets. */
#define FREE_SLOT_RING_IOVA_OFFSET  0x5800
#define FREE_SLOT_SEED_OFFSET       0x5C00
#define FREE_SLOT_PHASE_BIT         48

/* IAU-next P3 (testbed): IAU-engine registration in the same BAR0 page (== spdk/lib/nvmf/vfio_user_iau.h;
 * tests/realcore/test_iau_bar0_contract.py). With SPDK_IAU_ENGINE=1 a Mode D IO qpair writes
 * QCFG[qid] = nvme_pcie_iau_qcfg_pack(num_trackers, nsid) once at construct (before Create I/O SQ), rings
 * DESC_TAIL[qid] instead of the SSD's SQ doorbell, and points cq_hdbl at a sink, so the host never writes
 * the doorbells of a queue the IAU engine owns. QCFG = valid[0] | reserved[15:1] = 0 | credits[31:16] |
 * nsid[63:32]. */
#define IAU_DESC_TAIL_OFFSET	0x5200
#define IAU_CQDB_SINK_OFFSET	0x5300
#define IAU_QCFG_OFFSET		0x5600
#define IAU_QCFG_VALID		0x1ull
#define IAU_QCFG_RSVD_MASK	0xFFFEull
#define IAU_QCFG_CREDITS_SHIFT	16
#define IAU_QCFG_CREDITS_MASK	0xFFFFull
#define IAU_QCFG_NSID_SHIFT	32
#define IAU_QCFG_NSID_MASK	0xFFFFFFFFull
SPDK_STATIC_ASSERT((IAU_QCFG_VALID & IAU_QCFG_RSVD_MASK) == 0 && (IAU_QCFG_VALID | IAU_QCFG_RSVD_MASK) == 0xFFFFull &&
		   (IAU_QCFG_CREDITS_MASK << IAU_QCFG_CREDITS_SHIFT) == 0xFFFF0000ull &&
		   (IAU_QCFG_NSID_MASK << IAU_QCFG_NSID_SHIFT) == 0xFFFFFFFF00000000ull,
		   "QCFG = valid[0] | reserved[15:1] | credits[31:16] | nsid[63:32]");

/* QCFG[q] as the host writes it (== the target's iau_qcfg_pack(), tests/realcore/test_iau_qcfg.c):
 * credits = num_trackers (the host's in-flight cap), nsid = the first active namespace (the 40 B
 * descriptor carries none). */
static inline uint64_t
nvme_pcie_iau_qcfg_pack(uint16_t credits, uint32_t nsid)
{
	return IAU_QCFG_VALID | (((uint64_t)credits & IAU_QCFG_CREDITS_MASK) << IAU_QCFG_CREDITS_SHIFT) |
	       (((uint64_t)nsid & IAU_QCFG_NSID_MASK) << IAU_QCFG_NSID_SHIFT);
}

/* SPDK_IAU_ENGINE=1 (exactly "1"): a Mode D IO qpair registers with the IAU engine.  Read only at qpair
 * construct (nvme_pcie_qpair_iau_engine_register()), so not cached like the switches below: a cache would
 * be one more .data object in nvme_pcie_common.o, placed in front of the env caches the per-IO paths
 * read. */
static inline int
uncore_iau_engine(void)
{
	const char *e = getenv("SPDK_IAU_ENGINE");

	return (e != NULL && strcmp(e, "1") == 0) ? 1 : 0;
}

/* IO-Uncore v3 (Mode C): env-gated, lazily-cached feature flag.  When set the
 * host submits I/O by storing a 24-byte descriptor into a host-DRAM ring
 * (a plain cacheable store, NOT an MMIO write) and rings only a batched SQ
 * doorbell.  Mutually exclusive with Mode B (mailbox) at runtime; the replay
 * script clears SPDK_UNCORE_MODE_B / SQE16 / HOST_CID when Mode C is on. */
static inline int
uncore_mode_c(void)
{
	static int g_uncore_mode_c = -1;
	if (g_uncore_mode_c < 0) {
		const char *e = getenv("SPDK_UNCORE_MODE_C");
		g_uncore_mode_c = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return g_uncore_mode_c;
}

/* IO-Uncore v4.1 (Mode D): completion-callback ring.  A strict superset of the
 * Mode-C ring transport (Mode D ⊃ Mode C): the replay script sets BOTH
 * SPDK_UNCORE_MODE_C=1 (ring IOVA mapping + descriptor submit plumbing) AND
 * SPDK_UNCORE_MODE_D=1.  Mode D grows the submit descriptor to 40 B (carries the
 * completion callback) and replaces the 16 B cookie record with a 32 B
 * completion-callback record; the host completion loop then calls cb_fn(cb_arg,
 * &cpl) directly plus a minimal cache-hot tracker recycle, skipping the
 * complete_tracker body.  Divergent submit/completion branches MUST test
 * uncore_mode_d() BEFORE uncore_mode_c().
 * Mode D qpairs accept READ / WRITE of at most 128 KiB only: any other IO (other opcodes, larger
 * transfers) becomes a classic SQE that no Mode D device fetches, so it hangs until abort (I1 = mode 4
 * and I1e = mode 5; nvme_pcie_qpair_iau_engine_register()). */
static inline int
uncore_mode_d(void)
{
	static int g_uncore_mode_d = -1;
	if (g_uncore_mode_d < 0) {
		const char *e = getenv("SPDK_UNCORE_MODE_D");
		g_uncore_mode_d = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return g_uncore_mode_d;
}

/* IO-Uncore v4.2 (Mode E): device-owned tracker-slot free ring.  A strict
 * superset of Mode D (Mode E ⊃ Mode D ⊃ Mode C): the replay script sets
 * SPDK_UNCORE_MODE_C=1 + SPDK_UNCORE_MODE_D=1 + SPDK_UNCORE_MODE_E=1, so the
 * 40 B descriptor / 32 B completion-callback record transport is reused
 * verbatim.  What Mode E adds: on submit the host POPS a pre-allocated slot
 * index from a device-fed free-slot ring (a cacheable DRAM read) instead of
 * walking the SPDK tracker free list (removes Tracker_Alloc); on completion the
 * device recycles the slot internally and refills the ring by DMA, so the host
 * completion loop drops the Mode-D lean recycle (TAILQ churn) and does only
 * cb_fn + nvme_free_request.  Divergent branches MUST test uncore_mode_e()
 * BEFORE uncore_mode_d(). */
static inline int
uncore_mode_e(void)
{
	static int g_uncore_mode_e = -1;
	if (g_uncore_mode_e < 0) {
		const char *e = getenv("SPDK_UNCORE_MODE_E");
		g_uncore_mode_e = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return g_uncore_mode_e;
}

/* IO-Uncore v3 (Mode C) 24-byte host->device SQ descriptor (little-endian,
 * 3 x uint64_t).  w0/w1 packing is byte-identical to the v2 16B compact SQE;
 * w2 carries the opaque host tracker pointer (dormant in Phase 1, echoed to
 * the CQ record only in Phase 2). */
struct nvme_uncore_sq_desc {
	uint64_t w0;
	uint64_t w1;
	uint64_t w2;
};

/* IO-Uncore v3 (Mode C) 16-byte completion record (Phase 2).  Allocated in
 * Phase 1 but UNUSED — completions still arrive via the normal CQ. */
struct nvme_uncore_cq_rec {
	uint64_t w0;
	uint64_t w1;
};

/* IO-Uncore v4.1 (Mode D) LOCKED 40-byte host->device SQ descriptor (little-
 * endian, 5 x uint64_t).  w0/w1/w2 are byte-identical to the Mode-C 24 B
 * descriptor (w0={opcode,flags,cid,nlb,control}, w1=slba, w2=cookie=tr); w3/w4
 * carry the SPDK completion callback so the device can echo it into the record. */
struct nvme_uncore_sq_desc_d {
	uint64_t w0;      /* {opcode[63:56],flags[55:48],cid[47:32],nlb[31:16],control[15:0]} */
	uint64_t w1;      /* slba */
	uint64_t w2;      /* cookie = host tracker pointer (tr) */
	uint64_t w3;      /* cb_arg  (SPDK req->cb_arg, 64-bit host pointer) */
	uint64_t w4;      /* cb_fn   (SPDK req->cb_fn,  64-bit host func pointer) */
};

/* IO-Uncore v4.1 (Mode D) LOCKED 32-byte completion-callback record (little-
 * endian, 4 x uint64_t).  Device decodes the CQE and writes this; the host does
 * only cb_fn(cb_arg,&cpl) + minimal recycle.  Phase bit is w3 (cpl_hi) bit 48
 * — the authoritative ring poll bit — and is published LAST by the device so the
 * host never sees a valid phase over a half-written record.  cid is packed at
 * cpl_hi[47:32]; native NVMe phase (cpl_hi bit 0) forced 0. */
struct nvme_uncore_cq_cb_rec {
	uint64_t cb_fn;   /* w0 */
	uint64_t cb_arg;  /* w1 */
	uint64_t cpl_lo;  /* w2 = NVMe cpl dwords 0-1 (cdw0 + rsvd) */
	uint64_t cpl_hi;  /* w3 = {phase@48, cid[47:32], sqid/sqhd low32, status16[15:1], phase0=0} */
};

extern __thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr;

/*
 * Per-tracker DMA-visible list page.  The PRP list / SGL segment of a tracker (and its metadata
 * SGL descriptor) live in a separate per-qpair pool (nvme_pcie_qpair::prp_pool), one 4 KiB page
 * per tracker, instead of inside struct nvme_tracker.  Keeping them out of the tracker makes the
 * tracker array dense (64 B stride): with the lists embedded, every tracker was 4 KiB and all
 * tracker headers mapped to the same L1 set / L2 alias class.
 *
 * The page is 4096 B and the pool is 4096-aligned, so the list (page offset 0, at most 4024 B for
 * PRP or 4000 B for SGL) and the metadata SGL descriptor (page offset 4080) never cross a 4 KiB
 * boundary.  NVME_MAX_PRP_LIST_ENTRIES therefore stays 503.
 */
#define NVME_TRACKER_LIST_PAGE_SIZE	(0x1000)

struct nvme_tracker_list_page {
	union {
		uint64_t			prp[NVME_MAX_PRP_LIST_ENTRIES];
		struct spdk_nvme_sgl_descriptor	sgl[NVME_MAX_SGL_DESCRIPTORS];
		uint8_t				raw[NVME_TRACKER_LIST_PAGE_SIZE -
						    sizeof(struct spdk_nvme_sgl_descriptor)];
	} u;

	/* Metadata SGL descriptor (MPTR target when PSDT = SGL_MPTR_SGL), at the page tail. */
	struct spdk_nvme_sgl_descriptor		meta_sgl;
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker_list_page) == NVME_TRACKER_LIST_PAGE_SIZE,
		   "nvme_tracker_list_page is not 4K (list overruns meta_sgl or crosses a page)");
SPDK_STATIC_ASSERT(offsetof(struct nvme_tracker_list_page, u) == 0, "list must start the page");
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker_list_page, u.sgl) & 7) == 0,
		   "SGL must be Qword aligned");
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker_list_page, meta_sgl) & 7) == 0,
		   "SGL must be Qword aligned");

struct nvme_tracker {
	TAILQ_ENTRY(nvme_tracker)       tq_list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint16_t			bad_vtophys : 1;
	uint16_t			rsvd0 : 15;
	uint32_t			rsvd1;

	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;

	/* Bus address of list_page (the PRP list / SGL segment start, 4 KiB-aligned). */
	uint64_t			prp_sgl_bus_addr;

	/* This tracker's page in pqpair->prp_pool, fixed from construct to destroy. */
	struct nvme_tracker_list_page	*list_page;
};
/*
 * struct nvme_tracker is one cache line and the tracker array is dense.  The PRP/SGL lists are in
 * struct nvme_tracker_list_page (see above), so the tracker no longer has to be 4K.
 */
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) <= 128, "nvme_tracker grew past 128 B");
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) % 64 == 0,
		   "nvme_tracker is not a cache-line multiple");
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) == 64, "nvme_tracker is not one cache line");

struct nvme_pcie_poll_group {
	struct spdk_nvme_transport_poll_group group;
	struct spdk_nvme_pcie_stat stats;
};

enum nvme_pcie_qpair_state {
	NVME_PCIE_QPAIR_WAIT_FOR_CQ = 1,
	NVME_PCIE_QPAIR_WAIT_FOR_SQ,
	NVME_PCIE_QPAIR_READY,
	NVME_PCIE_QPAIR_FAILED,
};

/* PCIe transport extensions for spdk_nvme_qpair */
struct nvme_pcie_qpair {
	/* Submission queue tail doorbell */
	volatile uint32_t *sq_tdbl;

	/* Completion queue head doorbell */
	volatile uint32_t *cq_hdbl;

	/* Submission queue */
	struct spdk_nvme_cmd *cmd;

	/* Completion queue */
	struct spdk_nvme_cpl *cpl;

	TAILQ_HEAD(, nvme_tracker) free_tr;
	TAILQ_HEAD(nvme_outstanding_tr_head, nvme_tracker) outstanding_tr;

	/* Array of trackers indexed by command ID. */
	struct nvme_tracker *tr;

	struct spdk_nvme_pcie_stat *stat;

	uint16_t num_entries;

	uint8_t pcie_state;

	uint8_t retry_count;

	uint16_t max_completions_cap;

	uint16_t last_sq_tail;
	uint16_t sq_tail;
	uint16_t cq_head;
	uint16_t sq_head;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	uint64_t poll_cycle_accum;
	uint64_t last_sq_fence_ts;
	uint64_t last_sq_db_ts;
#endif

	/* IO-Uncore v3 (Mode C) hot cursors.  sq_desc_tail is the producer index
	 * into sq_desc_ring (advanced per IO via a plain cacheable store, NOT an
	 * sq_tail NVMe-ring advance).  last_sq_desc_tail is the batching cursor
	 * used by the deferred delay_cmd_submit doorbell flush so Mode C rings one
	 * coalesced SQ doorbell per poll instead of one MMIO per IO.  cq_rec_head /
	 * cq_rec_phase are the Phase-2 record-ring cursors (unused in Phase 1). */
	uint32_t sq_desc_tail;
	uint32_t last_sq_desc_tail;
	uint32_t cq_rec_head;
	uint8_t  cq_rec_phase;

	struct {
		uint8_t phase			: 1;
		uint8_t delay_cmd_submit	: 1;
		uint8_t has_shadow_doorbell	: 1;
		uint8_t has_pending_vtophys_failures : 1;
		uint8_t defer_destruction	: 1;

		/* Disable merging of physically contiguous SGL entries */
		uint8_t disable_pcie_sgl_merge	: 1;

		/* F3 tracker-free Mode D on this IO qpair (md_* fields at the struct tail are
		 * valid).  A spare bit of this existing byte, so no field offset moves and the
		 * per-submit / per-poll test reads a line the hot path already touches. */
		uint8_t md_trfree		: 1;
	} flags;

	/*
	 * Base qpair structure.
	 * This is located after the hot data in this structure so that the important parts of
	 * nvme_pcie_qpair are in the same cache line.
	 */
	struct spdk_nvme_qpair qpair;

	struct {
		/* Submission queue shadow tail doorbell */
		volatile uint32_t *sq_tdbl;

		/* Completion queue shadow head doorbell */
		volatile uint32_t *cq_hdbl;

		/* Submission queue event index */
		volatile uint32_t *sq_eventidx;

		/* Completion queue event index */
		volatile uint32_t *cq_eventidx;
	} shadow_doorbell;

	/*
	 * Fields below this point should not be touched on the normal I/O path.
	 */

	bool sq_in_cmb;
	bool shared_stats;

	uint64_t cmd_bus_addr;
	uint64_t cpl_bus_addr;

	struct spdk_nvme_cmd *sq_vaddr;
	struct spdk_nvme_cpl *cq_vaddr;

	/* IO-Uncore v3 (Mode C) host-DRAM rings + their IOVAs.  Allocated once in
	 * nvme_pcie_qpair_construct() when uncore_mode_c().  sq_desc_ring holds
	 * num_entries 24-byte descriptors; cq_rec_ring holds num_entries 16-byte
	 * records (allocated in Phase 1 but UNUSED until Phase 2).  NULL when Mode
	 * C is off — presence of sq_desc_ring is the per-qpair Mode-C gate on the
	 * submit + doorbell paths. */
	struct nvme_uncore_sq_desc *sq_desc_ring;
	struct nvme_uncore_cq_rec  *cq_rec_ring;
	uint64_t sq_desc_ring_bus_addr;
	uint64_t cq_rec_ring_bus_addr;

	/* IO-Uncore v4.2 (Mode E) device-owned tracker-slot free ring.  Allocated
	 * once in nvme_pcie_qpair_construct() when uncore_mode_e(); num_entries
	 * uint64_t entries {phase@bit48, slot[15:0]}.  The device seeds it with
	 * slots 0..num_trackers-1 and refills one entry per completion.  The host
	 * POPS one slot per submit (free_slot_head / free_slot_phase consumer
	 * cursor).  NULL when Mode E is off — presence gates the Mode-E submit
	 * pop + the drop of the completion recycle. */
	uint64_t *free_slot_ring;
	uint64_t  free_slot_ring_bus_addr;
	uint32_t  free_slot_head;
	uint8_t   free_slot_phase;

	/* Per-tracker PRP list / SGL segment pages: tr[i].list_page == &prp_pool[i].  num_trackers
	 * pages, 4 KiB-aligned, DMA + shared (like tr).  Kept at the struct end so existing field
	 * offsets do not move. */
	struct nvme_tracker_list_page *prp_pool;

	/*
	 * F3 tracker-free Mode D (IAU v4.1).  Valid only when flags.md_trfree is set (Mode D IO
	 * qpair, SPDK_MD_TRACKERS not "1").  The cid space [0, num_trackers) is partitioned:
	 *   fast cids    [0, md_fast_cids)            no tracker; the nvme_request pointer rides in
	 *                                             descriptor w4 and comes back in record w0
	 *   tracker cids [md_fast_cids, num_trackers) tr[cid] on free_tr / outstanding_tr as before
	 *                                             (ineligible IO, or the fast stack is empty)
	 * md_fast_cids = num_trackers - NVME_PCIE_MD_TRACKER_CIDS.  md_inflight[cid] holds the
	 * request of every fast cid in flight (NULL = free), so abort, timeout, teardown and
	 * iterate_requests see tracker-free IO.  md_cid_stack is the LIFO of free fast cids
	 * (md_cid_top entries).  Both arrays live in one allocation owned by md_inflight.
	 * Kept at the struct end so existing field offsets do not move.
	 */
	struct nvme_request **md_inflight;
	uint16_t *md_cid_stack;
	uint16_t md_cid_top;
	uint16_t md_fast_cids;
};

/* F3: tracker cids reserved on a tracker-free Mode D qpair for IO the descriptor fast path does
 * not take (non-read/write, > 128 KiB, SGL payload, separate metadata buffer) and for eligible
 * IO that finds the fast cid stack empty. */
#define NVME_PCIE_MD_TRACKER_CIDS	(16)

static inline struct nvme_pcie_qpair *
nvme_pcie_qpair(struct spdk_nvme_qpair *qpair)
{
	return SPDK_CONTAINEROF(qpair, struct nvme_pcie_qpair, qpair);
}

static inline struct nvme_pcie_ctrlr *
nvme_pcie_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	return SPDK_CONTAINEROF(ctrlr, struct nvme_pcie_ctrlr, ctrlr);
}

static inline int
nvme_pcie_qpair_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old)
{
	return (uint16_t)(new_idx - event_idx) <= (uint16_t)(new_idx - old);
}

static inline bool
nvme_pcie_qpair_update_mmio_required(uint16_t value,
				     volatile uint32_t *shadow_db,
				     volatile uint32_t *eventidx)
{
	uint16_t old;

	spdk_wmb();

	old = *shadow_db;
	*shadow_db = value;

	/*
	 * Ensure that the doorbell is updated before reading the EventIdx from
	 * memory
	 */
	spdk_mb();

	if (!nvme_pcie_qpair_need_event(*eventidx, value, old)) {
		return false;
	}

	return true;
}

/* review fix M2: SPDK_DB_COST_NS (SSD doorbells) and SPDK_DB_COST_IAU_NS (the IAU D-tail doorbell) spin this many TSC
 * ticks after each IO doorbell store, as an uncached MMIO write would stall; 0 (default) = off. Admin doorbells are
 * not charged */
extern uint64_t g_nvme_db_cost_ticks, g_nvme_db_cost_iau_ticks;
void nvme_db_cost_spin(uint64_t ticks);
void nvme_db_cost_init(void);

static inline __attribute__((always_inline)) void
nvme_db_cost(uint64_t ticks)
{
	if (spdk_unlikely(ticks != 0)) {
		nvme_db_cost_spin(ticks);
	}
}

static inline void
nvme_pcie_qpair_ring_sq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	bool need_mmio = true;
	/* IO-Uncore v3 (Mode C) publishes the descriptor-ring producer index, not
	 * the classic NVMe SQ tail (the device fetches SQEs from the host-DRAM
	 * descriptor ring).  Same sq_tdbl register, different value.  In every
	 * non-Mode-C path sq_desc_ring is NULL and this is exactly sq_tail. */
	uint32_t db_val = (pqpair->sq_desc_ring != NULL) ?
			  pqpair->sq_desc_tail : pqpair->sq_tail;

	if (qpair->last_fuse == SPDK_NVME_IO_FLAGS_FUSE_FIRST) {
		/* This is first cmd of two fused commands - don't ring doorbell */
		return;
	}

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		pqpair->stat->sq_shadow_doorbell_updates++;
		need_mmio = nvme_pcie_qpair_update_mmio_required(
				    db_val,
				    pqpair->shadow_doorbell.sq_tdbl,
				    pqpair->shadow_doorbell.sq_eventidx);
	}

	if (spdk_likely(need_mmio)) {
		spdk_wmb();
#ifndef SPDK_NVME_IO_CYCLE_OFF
		pqpair->last_sq_fence_ts = nvme_io_cycle_rdtsc();
#endif
		pqpair->stat->sq_mmio_doorbell_updates++;
		g_thread_mmio_ctrlr = pctrlr;
		spdk_mmio_write_4(pqpair->sq_tdbl, db_val);
		g_thread_mmio_ctrlr = NULL;
		if (spdk_unlikely(g_nvme_db_cost_ticks != 0) && qpair->id != 0) {	/* M2: IO doorbells only */
			nvme_db_cost_spin(g_nvme_db_cost_ticks);
		}
#ifndef SPDK_NVME_IO_CYCLE_OFF
		pqpair->last_sq_db_ts = nvme_io_cycle_rdtsc();
#endif
	}
}

static inline void
nvme_pcie_qpair_ring_cq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	bool need_mmio = true;

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		pqpair->stat->cq_shadow_doorbell_updates++;
		need_mmio = nvme_pcie_qpair_update_mmio_required(
				    pqpair->cq_head,
				    pqpair->shadow_doorbell.cq_hdbl,
				    pqpair->shadow_doorbell.cq_eventidx);
	}

	if (spdk_likely(need_mmio)) {
		pqpair->stat->cq_mmio_doorbell_updates++;
		g_thread_mmio_ctrlr = pctrlr;
		spdk_mmio_write_4(pqpair->cq_hdbl, pqpair->cq_head);
		g_thread_mmio_ctrlr = NULL;
		if (spdk_unlikely(g_nvme_db_cost_ticks != 0) && qpair->id != 0) {	/* M2: IO doorbells only */
			nvme_db_cost_spin(g_nvme_db_cost_ticks);
		}
	}
}

int nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair);
int nvme_pcie_qpair_get_fd(struct spdk_nvme_qpair *qpair, struct spdk_event_handler_opts *opts);
int nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
			      const struct spdk_nvme_io_qpair_opts *opts);
int nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t num_entries);
void nvme_pcie_qpair_insert_pending_admin_request(struct spdk_nvme_qpair *qpair,
		struct nvme_request *req, struct spdk_nvme_cpl *cpl);
void nvme_pcie_qpair_complete_pending_admin_request(struct spdk_nvme_qpair *qpair);
int nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				     void *cb_arg);
int nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_pcie_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
void nvme_pcie_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
void nvme_pcie_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr);
void nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
		struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
		bool print_on_error);
void nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				      struct spdk_nvme_cpl *cpl, bool print_on_error);
void nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr);
void nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair);
void nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair);
void nvme_pcie_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);
int32_t nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);
int nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair);
struct spdk_nvme_qpair *nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
		const struct spdk_nvme_io_qpair_opts *opts);
int nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);
int nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req);
int nvme_pcie_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
				   struct spdk_nvme_transport_poll_group_stat **_stats);
void nvme_pcie_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				     struct spdk_nvme_transport_poll_group_stat *stats);

struct spdk_nvme_transport_poll_group *nvme_pcie_poll_group_create(void);
int nvme_pcie_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			     struct spdk_nvme_qpair *qpair);
int nvme_pcie_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_qpair *qpair);
int64_t nvme_pcie_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
void nvme_pcie_poll_group_check_disconnected_qpairs(
	struct spdk_nvme_transport_poll_group *tgroup,
	spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
int nvme_pcie_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup);

#endif
