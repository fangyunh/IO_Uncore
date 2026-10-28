/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over PCIe common library
 */

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include "nvme_iau.h"
#include "spdk/trace.h"

#include "spdk_internal/trace_defs.h"

__thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr = NULL;

static struct spdk_nvme_pcie_stat g_dummy_stat = {};

/* IO-Uncore v2: env-gated, lazily-cached feature flag for the 16-byte
 * compact SQE (2 MMIO writes instead of 3). */
static int g_uncore_sqe16 = -1;
static inline int uncore_sqe16(void) {
	if (spdk_unlikely(g_uncore_sqe16 < 0)) {
		const char *e = getenv("SPDK_UNCORE_SQE16");
		g_uncore_sqe16 = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return g_uncore_sqe16;
}

/* IO-Uncore v2: env-gated, lazily-cached feature flag for multi-page
 * mailbox transfers (raises the single-page fallback ceiling to 128 KiB). */
static int g_uncore_multipage = -1;
static inline int uncore_multipage(void) {
	if (spdk_unlikely(g_uncore_multipage < 0)) {
		const char *e = getenv("SPDK_UNCORE_MULTIPAGE");
		g_uncore_multipage = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return g_uncore_multipage;
}

/* F3: SPDK_MD_TRACKERS=1 forces the tracker path on Mode D IO qpairs (A/B measurement against
 * tracker-free Mode D, the default).  Read once per process, at the first Mode D IO qpair
 * construct; only the exact value "1" forces trackers. */
static int g_md_trackers_forced = -1;
static int
nvme_pcie_md_trackers_forced(void)
{
	if (g_md_trackers_forced < 0) {
		const char *e = getenv("SPDK_MD_TRACKERS");

		g_md_trackers_forced = (e != NULL && strcmp(e, "1") == 0) ? 1 : 0;
	}
	return g_md_trackers_forced;
}

/* review fix M2: the doorbell-cost knob (nvme_pcie_internal.h nvme_db_cost()) */
uint64_t g_nvme_db_cost_ticks, g_nvme_db_cost_iau_ticks;

void __attribute__((noinline))
nvme_db_cost_spin(uint64_t ticks)
{
	uint64_t t0 = spdk_get_ticks();

	while (spdk_get_ticks() - t0 < ticks) {
	}
}

static uint64_t
db_cost_env_ticks(const char *name, long *ns_out)
{
	const char *v = getenv(name);
	long ns = v != NULL ? strtol(v, NULL, 10) : 0;

	*ns_out = ns > 0 ? ns : 0;
	return ns > 0 ? spdk_max(1ull, (uint64_t)(((__uint128_t)ns * spdk_get_ticks_hz()) / 1000000000ull)) : 0;
}

static void
db_cost_load(void)
{
	long ns, iau_ns;

	g_nvme_db_cost_ticks = db_cost_env_ticks("SPDK_DB_COST_NS", &ns);
	g_nvme_db_cost_iau_ticks = db_cost_env_ticks("SPDK_DB_COST_IAU_NS", &iau_ns);
	if (ns > 0 || iau_ns > 0) {
		fprintf(stderr, "MD_DBCOST ssd_ns=%ld iau_ns=%ld ticks=%" PRIu64 " iau_ticks=%" PRIu64 "\n",
			ns, iau_ns, g_nvme_db_cost_ticks, g_nvme_db_cost_iau_ticks);
	}
}

/* once per process, from the first IO qpair construct (the env, and so the TSC rate, is up by then; every IO path,
 * stock, S2 and the tile host, constructs there before its first doorbell). pthread_once: perf's workers create their
 * qpairs concurrently, and a second caller waits until the values are set */
void
nvme_db_cost_init(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, db_cost_load);
}

static void nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair,
		struct nvme_tracker *tr);

/* The per-IO functions (the poll and submit entry points and their stock bodies, submit_tracker,
 * complete_tracker, and the F3 tracker-free poll and submit) start on a 64 B boundary.  Without it their cache-line placement
 * follows the size of every function and object linked before them (this object's .text is
 * otherwise only 16 B aligned), so an unrelated change moves the hot code of both A/B arms. */
#define NVME_PCIE_HOT_ALIGN __attribute__((aligned(64)))

/* F3: the stock poll / submit bodies are separate functions behind one-test entry points; they
 * must not be inlined into the entry points or IPA-cloned.  With GCC >= 8 (noipa) they compile
 * exactly like the former global functions (verified by normalized objdump against pre-F3).  GCC 7
 * (noinline + noclone) and clang (noinline) still allow some IPA on internal functions (e.g. IPA
 * register allocation / pure-const propagation); for these call sites (tail calls from the entry
 * points) that does not change the callee's code. */
#if defined(__clang__)
#define NVME_PCIE_STOCK_BODY __attribute__((noinline))
#elif defined(__GNUC__) && __GNUC__ >= 8
#define NVME_PCIE_STOCK_BODY __attribute__((noipa))
#else
#define NVME_PCIE_STOCK_BODY __attribute__((noinline, noclone))
#endif

static inline void
nvme_pcie_qpair_mark_doorbell_time(struct nvme_pcie_qpair *pqpair, uint64_t t_db)
{
#ifndef SPDK_NVME_IO_CYCLE_OFF
	struct nvme_tracker *tr;

	TAILQ_FOREACH(tr, &pqpair->outstanding_tr, tq_list) {
		if (tr->req && tr->req->t_sqe_copy != 0 && tr->req->t_doorbell == 0) {
			tr->req->t_doorbell = t_db;
			tr->req->t_poll_start = pqpair->poll_cycle_accum;
		}
	}
#else
	(void)pqpair;
	(void)t_db;
#endif
}

static inline uint64_t
nvme_pcie_vtophys(struct spdk_nvme_ctrlr *ctrlr, const void *buf, uint64_t *size)
{
	if (spdk_likely(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE)) {
		return spdk_vtophys(buf, size);
	} else {
		/* vfio-user address translation with IOVA=VA mode */
		return (uint64_t)(uintptr_t)buf;
	}
}

int
nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	uint32_t i;

	/* all head/tail vals are set to 0 */
	pqpair->last_sq_tail = pqpair->sq_tail = pqpair->sq_head = pqpair->cq_head = 0;

	/* IO-Uncore v3 (Mode C) ring cursors reset alongside the classic ones so
	 * a qpair reconnect restarts the descriptor/record rings cleanly.  Harmless
	 * (fields stay 0/1, unused) when Mode C is off. */
	pqpair->sq_desc_tail = pqpair->last_sq_desc_tail = pqpair->cq_rec_head = 0;
	pqpair->cq_rec_phase = 1;

	/* The record ring restarts at slot 0 expecting phase 1, so records left from before the
	 * reset (a reconnect after a controller reset) must not look valid: clear the ring like
	 * the cpl[].status.p loop below clears the NVMe CQ.  Without this a stale Mode D record
	 * whose w0 names a request pointer that is in flight again (requests are pooled) would
	 * complete that request early.  No-op cost at construct (the ring is zeroed there).
	 * F3: the fast cid stack is NOT re-seeded here: requests still in md_inflight are
	 * completed by nvme_pcie_qpair_abort_trackers(), exactly like trackers on outstanding_tr. */
	if (pqpair->cq_rec_ring != NULL) {
		size_t rec_bytes = uncore_mode_d() ? sizeof(struct nvme_uncore_cq_cb_rec) :
				   sizeof(struct nvme_uncore_cq_rec);

		memset((void *)pqpair->cq_rec_ring, 0, (size_t)pqpair->num_entries * rec_bytes);
	}

	/*
	 * First time through the completion queue, HW will set phase
	 *  bit on completions to 1.  So set this to 1 here, indicating
	uint64_t		 poll_start;
	 *  we're looking for a 1 to know which entries have completed.
	 *  we'll toggle the bit each time when the completion queue
	 *  rolls over.
	 */
	pqpair->flags.phase = 1;
	for (i = 0; i < pqpair->num_entries; i++) {
		pqpair->cpl[i].status.p = 0;
	}

	return 0;
}

int
nvme_pcie_qpair_get_fd(struct spdk_nvme_qpair *qpair, struct spdk_event_handler_opts *opts)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_pci_device *devhandle = nvme_ctrlr_proc_get_devhandle(ctrlr);

	assert(devhandle != NULL);
	if (!ctrlr->opts.enable_interrupts) {
		return -1;
	}

	if (!opts) {
		return spdk_pci_device_get_interrupt_efd_by_index(devhandle, qpair->id);
	}

	if (!SPDK_FIELD_VALID(opts, fd_type, opts->opts_size)) {
		return -EINVAL;
	}

	spdk_fd_group_get_default_event_handler_opts(opts, opts->opts_size);
	opts->fd_type = SPDK_FD_TYPE_EVENTFD;

	return spdk_pci_device_get_interrupt_efd_by_index(devhandle, qpair->id);
}

static void
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid,
			     struct nvme_tracker_list_page *list_page, uint64_t list_page_bus_addr)
{
	tr->list_page = list_page;
	tr->prp_sgl_bus_addr = list_page_bus_addr;
	tr->cid = cid;
	tr->req = NULL;
}

static void *
nvme_pcie_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t size, uint64_t alignment,
			  uint64_t *phys_addr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	uintptr_t addr;

	if (pctrlr->cmb.mem_register_addr != NULL) {
		/* BAR is mapped for data */
		return NULL;
	}

	addr = (uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.current_offset;
	addr = (addr + (alignment - 1)) & ~(alignment - 1);

	/* CMB may only consume part of the BAR, calculate accordingly */
	if (addr + size > ((uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.size)) {
		NVME_CTRLR_ERRLOG(ctrlr, "Tried to allocate past valid CMB range!\n");
		return NULL;
	}
	*phys_addr = pctrlr->cmb.bar_pa + addr - (uintptr_t)pctrlr->cmb.bar_va;

	pctrlr->cmb.current_offset = (addr + size) - (uintptr_t)pctrlr->cmb.bar_va;

	return (void *)addr;
}

/* IAU-next P3 (testbed): construct-time-only code kept out of the layout of this object's .text.  GCC
 * emits a callee before its first caller, so a plain helper of construct would be placed in front of
 * it and move every function after it, the 64 B-aligned per-IO functions included (md_poll_asm.sh
 * addr=); a section of its own is linked right after this object's .text instead.  Not cold: a call to
 * a cold function marks the calling path unlikely, which would move construct's Mode C/D code into a
 * .cold part.  NVME_PCIE_STOCK_BODY (noipa): construct calls them as it would any external function.
 * With GCC, no_reorder emits these functions in source order and ahead of the rest (in their own
 * section, so that moves nothing else), and NVME_PCIE_CONSTRUCT_OOL_END after the last of them pads the
 * section to a multiple of 64 B, so the objects linked after this one move by whole cache lines
 * (tests/realcore/test_pcie_hot_layout.sh checks both). */
#if defined(__GNUC__) && !defined(__clang__)
#define NVME_PCIE_CONSTRUCT_OOL NVME_PCIE_STOCK_BODY \
	__attribute__((section(".text.nvme_pcie_construct_ool"), no_reorder))
#define NVME_PCIE_CONSTRUCT_OOL_END \
	__asm__(".pushsection .text.nvme_pcie_construct_ool,\"ax\",@progbits\n\t.p2align 6\n\t.popsection")
#else
#define NVME_PCIE_CONSTRUCT_OOL NVME_PCIE_STOCK_BODY __attribute__((section(".text.nvme_pcie_construct_ool")))
#define NVME_PCIE_CONSTRUCT_OOL_END SPDK_STATIC_ASSERT(1, "no construct_ool padding")
#endif

/* IAU-next P3 (testbed): N bytes inside nvme_pcie_qpair_construct() that are jumped over (never
 * executed), so that construct's size stays in the window in which the functions after it keep nx-p2's
 * offsets: (0x1cf0, 0x1d30] with the lean flags, whatever moved into the out-of-line section.
 * tests/realcore/test_pcie_hot_layout.sh enforces the window and prints the slack left; re-size N when
 * construct changes.  GCC on x86-64 only (the layout guarantees are for the lean GCC build). */
#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__)
#define NVME_PCIE_CONSTRUCT_PAD(n) __asm__ volatile("jmp 1f\n\t.skip " #n ", 0xcc\n1:")
#else
#define NVME_PCIE_CONSTRUCT_PAD(n) do { } while (0)
#endif

/* IAU-next P3: entries of each per-qid table of the IAU page (BAR0 0x5000): the ring IOVAs (0x5000,
 * 0x5400; 8 B), DESC_TAIL (0x5200) and the CQ sink (0x5300; 4 B), QCFG (0x5600; 8 B).  == IAU_MAX_QID
 * of the target (spdk/lib/nvmf/vfio_user_iau.h). */
#define NVME_PCIE_IAU_PAGE_QIDS 64u
SPDK_STATIC_ASSERT(SQ_DESC_RING_IOVA_OFFSET + 8 * NVME_PCIE_IAU_PAGE_QIDS <= IAU_DESC_TAIL_OFFSET &&
		   IAU_DESC_TAIL_OFFSET + 4 * NVME_PCIE_IAU_PAGE_QIDS <= IAU_CQDB_SINK_OFFSET &&
		   IAU_CQDB_SINK_OFFSET + 4 * NVME_PCIE_IAU_PAGE_QIDS <= CQ_REC_RING_IOVA_OFFSET &&
		   CQ_REC_RING_IOVA_OFFSET + 8 * NVME_PCIE_IAU_PAGE_QIDS <= IAU_QCFG_OFFSET &&
		   IAU_QCFG_OFFSET + 8 * NVME_PCIE_IAU_PAGE_QIDS <= FREE_SLOT_RING_IOVA_OFFSET,
		   "IAU page per-qid tables hold NVME_PCIE_IAU_PAGE_QIDS entries each");
/* IAU-next P4.5 (P4.2 notes; compile time only): host and target count the same qids in the IAU page, and no
 * host table reaches QCFG2 (0x5A00 + 8 q, spdk_internal/iau_tile_abi.h): QCFG ends at host Mode E's free-slot IOVA
 * table, which ends at QCFG2.  Writes to those tables are bounded to qid < NVME_PCIE_IAU_PAGE_QIDS by
 * nvme_pcie_qpair_iau_publish_rings() (the Mode E table is written only after it), and SPDK_IAU_TILE=1 refuses
 * the Mode C/D/E switches (nvme_iau_cfg_load()). */
SPDK_STATIC_ASSERT(IAU_TILE_QIDS == NVME_PCIE_IAU_PAGE_QIDS &&
		   IAU_QCFG_OFFSET + 8 * NVME_PCIE_IAU_PAGE_QIDS <= IAU_QCFG2_OFFSET &&
		   FREE_SLOT_RING_IOVA_OFFSET + 8 * NVME_PCIE_IAU_PAGE_QIDS <= IAU_QCFG2_OFFSET,
		   "the host's IAU page tables end below QCFG2; host and target agree on the qid count");

/* Mode C/D: publish this qpair's descriptor- and record-ring IOVAs to the IAU page ONCE (never per IO),
 * when the page is mapped.  Its tables hold NVME_PCIE_IAU_PAGE_QIDS entries, so a larger qid is refused
 * with -EINVAL before any page write (its IOVAs would land in DESC_TAIL[] / QCFG[]). */
static int NVME_PCIE_CONSTRUCT_OOL
nvme_pcie_qpair_iau_publish_rings(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (pctrlr->uncore_sq_desc_iova_reg == NULL || pctrlr->uncore_cq_rec_iova_reg == NULL) {
		return 0;
	}
	if (qpair->id >= NVME_PCIE_IAU_PAGE_QIDS) {
		NVME_QPAIR_ERRLOG(qpair, "Mode C/D: qid %u past the %u-entry IAU page tables\n",
				  (unsigned)qpair->id, NVME_PCIE_IAU_PAGE_QIDS);
		return -EINVAL;
	}
	g_thread_mmio_ctrlr = pctrlr;
	spdk_mmio_write_8(&pctrlr->uncore_sq_desc_iova_reg[qpair->id], pqpair->sq_desc_ring_bus_addr);
	spdk_mmio_write_8(&pctrlr->uncore_cq_rec_iova_reg[qpair->id], pqpair->cq_rec_ring_bus_addr);
	g_thread_mmio_ctrlr = NULL;
	return 0;
}

/* IAU-next P3 (testbed): with SPDK_IAU_ENGINE=1, register a Mode D IO qpair with the IAU engine.
 * QCFG[q] = nvme_pcie_iau_qcfg_pack(num_trackers, the active nsid) carries what the engine cannot learn
 * from the SSD's admin path: the host's in-flight cap (credits) and the namespace (the 40 B descriptor
 * has none).  The SSD's SQ/CQ are this qpair's own, created through the admin queue as usual; in silicon
 * QCFG would carry their IOVAs too (testbed shortcut).  Written once, after the ring IOVAs and before
 * Create I/O SQ.  From here on sq_tdbl is DESC_TAIL[q] and cq_hdbl the CQ sink, so the host never writes
 * the SSD doorbells of a queue the engine owns; the per-IO code is unchanged (the same stores to other
 * addresses), the P2.1 re-issue's doorbell included.
 *
 * Mode D qpairs accept READ / WRITE of at most 128 KiB only (the descriptor predicate of
 * nvme_pcie_qpair_submit_request_stock()); any other IO (other opcodes, larger transfers) is written as
 * a classic SQE that no Mode D device fetches, so it hangs until abort, in I1 (mode 4) and I1e (mode 5)
 * alike.  Under the engine the SSD's SQ is written by the engine alone, so the SQ memory becomes one
 * buffer [host-private classic SQ | SSD SQ]: pqpair->cmd is the first half (classic SQEs land there and
 * are never fetched; it is the allocation start, so nvme_pcie_qpair_destroy()'s spdk_free(pqpair->cmd)
 * frees the whole buffer) and cmd_bus_addr, the Create I/O SQ PRP1, is the page-aligned second half.
 * The MD_ENGINE line says so (io=read,write max_kib=128 classic_sq=private).
 *
 * Returns 0 when the engine is off (SPDK_IAU_ENGINE is not exactly "1") or the queue is registered.
 * Returns -EINVAL, before any write, when the engine was asked for but the queue cannot be registered:
 * IAU page not mapped, not exactly one active namespace (QCFG names one), shadow doorbells (the host
 * would skip DESC_TAIL writes), or SQ memory this hook cannot split (in the CMB or caller-provided).
 * -ENOMEM / -EFAULT if the split SQ cannot be allocated or translated.  A queue never runs unbound.  The
 * ring-IOVA publish already bounded qid to the page's NVME_PCIE_IAU_PAGE_QIDS entries. */
static int NVME_PCIE_CONSTRUCT_OOL
nvme_pcie_qpair_iau_engine_register(struct spdk_nvme_qpair *qpair, uint16_t num_trackers)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	volatile uint8_t *pg = (volatile uint8_t *)pctrlr->uncore_sq_desc_iova_reg;
	size_t page_align = sysconf(_SC_PAGESIZE);
	struct spdk_nvme_cmd *sq;
	uint32_t nsid, next_nsid;
	uint64_t qcfg, ssd_sq;
	size_t half;

	if (!uncore_iau_engine()) {
		return 0;
	}
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	next_nsid = nsid != 0 ? spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid) : 0;
	if (pg == NULL || pctrlr->uncore_cq_rec_iova_reg == NULL || nsid == 0 || next_nsid != 0 ||
	    ctrlr->shadow_doorbell != NULL || pqpair->sq_in_cmb || pqpair->sq_vaddr != NULL) {
		NVME_QPAIR_ERRLOG(qpair, "IAU engine: queue not registered (IAU page sq=%s cq=%s, nsid %u, "
				  "next nsid %u, shadow doorbells %s, SQ %s)\n", pg != NULL ? "mapped" : "unmapped",
				  pctrlr->uncore_cq_rec_iova_reg != NULL ? "mapped" : "unmapped", nsid, next_nsid,
				  ctrlr->shadow_doorbell != NULL ? "on" : "off", pqpair->sq_in_cmb ? "in CMB" :
				  pqpair->sq_vaddr != NULL ? "caller-provided" : "own");
		return -EINVAL;
	}
	assert(qpair->id < NVME_PCIE_IAU_PAGE_QIDS);
	half = spdk_max(spdk_align32pow2(pqpair->num_entries * sizeof(struct spdk_nvme_cmd)), page_align);
	sq = spdk_zmalloc(2 * half, 2 * half, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (sq == NULL) {
		NVME_QPAIR_ERRLOG(qpair, "IAU engine: alloc split SQ failed\n");
		return -ENOMEM;
	}
	ssd_sq = nvme_pcie_vtophys(ctrlr, (uint8_t *)sq + half, NULL);
	if (ssd_sq == SPDK_VTOPHYS_ERROR) {
		NVME_QPAIR_ERRLOG(qpair, "IAU engine: vtophys(split SQ) failed\n");
		spdk_free(sq);
		return -EFAULT;
	}
	spdk_free(pqpair->cmd);
	pqpair->cmd = sq;
	pqpair->cmd_bus_addr = ssd_sq;
	qcfg = nvme_pcie_iau_qcfg_pack(num_trackers, nsid);
	g_thread_mmio_ctrlr = pctrlr;
	spdk_mmio_write_8((volatile uint64_t *)(pg + (IAU_QCFG_OFFSET - SQ_DESC_RING_IOVA_OFFSET)) + qpair->id,
			  qcfg);
	g_thread_mmio_ctrlr = NULL;
	pqpair->sq_tdbl = (volatile uint32_t *)(pg + (IAU_DESC_TAIL_OFFSET - SQ_DESC_RING_IOVA_OFFSET)) +
			  qpair->id;
	pqpair->cq_hdbl = (volatile uint32_t *)(pg + (IAU_CQDB_SINK_OFFSET - SQ_DESC_RING_IOVA_OFFSET)) +
			  qpair->id;
	fprintf(stderr, "MD_ENGINE qid=%u qcfg=0x%016" PRIx64 " nsid=%u credits=%u io=read,write max_kib=128 "
		"classic_sq=private\n", (unsigned)qpair->id, qcfg, nsid, (unsigned)num_trackers);
	return 0;
}
NVME_PCIE_CONSTRUCT_OOL_END;

int
nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
			  const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	uint16_t		i;
	uint16_t		num_trackers;
	size_t			page_align = sysconf(_SC_PAGESIZE);
	size_t			queue_align, queue_len;
	uint32_t                flags = SPDK_MALLOC_DMA;
	int32_t			numa_id;
	uint64_t		sq_paddr = 0;
	uint64_t		cq_paddr = 0;
	uint16_t		md_fast;
	bool			md_mode_d;
	int			rc;

	if (opts) {
		pqpair->sq_vaddr = opts->sq.vaddr;
		pqpair->cq_vaddr = opts->cq.vaddr;
		pqpair->flags.disable_pcie_sgl_merge = opts->disable_pcie_sgl_merge;
		sq_paddr = opts->sq.paddr;
		cq_paddr = opts->cq.paddr;
	}
	if (qpair->id != 0) {
		nvme_db_cost_init();	/* M2: before any IO doorbell */
	}

	pqpair->retry_count = ctrlr->opts.transport_retry_count;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	pqpair->poll_cycle_accum = 0;
#endif

	/*
	 * Limit the maximum number of completions to return per call to prevent wraparound,
	 * and calculate how many trackers can be submitted at once without overflowing the
	 * completion queue.
	 */
	pqpair->max_completions_cap = pqpair->num_entries / 4;
	pqpair->max_completions_cap = spdk_max(pqpair->max_completions_cap, NVME_MIN_COMPLETIONS);
	pqpair->max_completions_cap = spdk_min(pqpair->max_completions_cap, NVME_MAX_COMPLETIONS);
	num_trackers = pqpair->num_entries - pqpair->max_completions_cap;

	NVME_QPAIR_INFOLOG(qpair, "max_completions_cap = %" PRIu16 " num_trackers = %" PRIu16 "\n",
			   pqpair->max_completions_cap, num_trackers);

	assert(num_trackers != 0);

	pqpair->sq_in_cmb = false;

	if (nvme_qpair_is_admin_queue(&pqpair->qpair)) {
		flags |= SPDK_MALLOC_SHARE;
	}

	/* cmd and cpl rings must be aligned on page size boundaries. */
	if (ctrlr->opts.use_cmb_sqs) {
		pqpair->cmd = nvme_pcie_ctrlr_alloc_cmb(ctrlr, pqpair->num_entries * sizeof(struct spdk_nvme_cmd),
							page_align, &pqpair->cmd_bus_addr);
		if (pqpair->cmd != NULL) {
			pqpair->sq_in_cmb = true;
		}
	}

	if (pqpair->sq_in_cmb == false) {
		if (pqpair->sq_vaddr) {
			pqpair->cmd = pqpair->sq_vaddr;
		} else {
			/* To ensure physical address contiguity we make each ring occupy
			 * a single hugepage only. See MAX_IO_QUEUE_ENTRIES.
			 */
			queue_len = pqpair->num_entries * sizeof(struct spdk_nvme_cmd);
			queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
			pqpair->cmd = spdk_zmalloc(queue_len, queue_align, NULL, SPDK_ENV_NUMA_ID_ANY, flags);
			if (pqpair->cmd == NULL) {
				NVME_QPAIR_ERRLOG(qpair, "alloc qpair_cmd failed\n");
				return -ENOMEM;
			}
		}
		if (sq_paddr) {
			assert(pqpair->sq_vaddr != NULL);
			pqpair->cmd_bus_addr = sq_paddr;
		} else {
			pqpair->cmd_bus_addr = nvme_pcie_vtophys(ctrlr, pqpair->cmd, NULL);
			if (pqpair->cmd_bus_addr == SPDK_VTOPHYS_ERROR) {
				NVME_QPAIR_ERRLOG(qpair, "spdk_vtophys(pqpair->cmd) failed\n");
				return -EFAULT;
			}
		}
	}

	if (pqpair->cq_vaddr) {
		pqpair->cpl = pqpair->cq_vaddr;
	} else {
		queue_len = pqpair->num_entries * sizeof(struct spdk_nvme_cpl);
		queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
		numa_id = spdk_nvme_ctrlr_get_numa_id(ctrlr);
		pqpair->cpl = spdk_zmalloc(queue_len, queue_align, NULL, numa_id, flags);
		if (pqpair->cpl == NULL) {
			NVME_QPAIR_ERRLOG(qpair, "alloc qpair_cpl failed\n");
			return -ENOMEM;
		}
	}
	if (cq_paddr) {
		assert(pqpair->cq_vaddr != NULL);
		pqpair->cpl_bus_addr = cq_paddr;
	} else {
		pqpair->cpl_bus_addr =  nvme_pcie_vtophys(ctrlr, pqpair->cpl, NULL);
		if (pqpair->cpl_bus_addr == SPDK_VTOPHYS_ERROR) {
			NVME_QPAIR_ERRLOG(qpair, "spdk_vtophys(pqpair->cpl) failed\n");
			return -EFAULT;
		}
	}

	pqpair->sq_tdbl = pctrlr->doorbell_base + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
	pqpair->cq_hdbl = pctrlr->doorbell_base + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;

	/* Multi-qpair debug banner: one line per qpair construct, prints every
	 * per-qid endpoint we computed so we can prove qid > 1 is wired right.
	 * Cheap (one line per qpair create), invaluable when something misroutes. */
	NVME_QPAIR_NOTICELOG(qpair,
		"UNCORE-Q | qid=%u sq_tdbl=%p cq_hdbl=%p mailbox_slot=%p free_cid_ep=%p qdepth_ep=%p\n",
		qpair->id,
		(void *)pqpair->sq_tdbl,
		(void *)pqpair->cq_hdbl,
		pctrlr->uncore_mailbox_base ?
		    (void *)(pctrlr->uncore_mailbox_base +
		             qpair->id * (MAILBOX_STRIDE_BYTES / sizeof(uint64_t))) : NULL,
		pctrlr->uncore_free_cid_base ?
		    (void *)&pctrlr->uncore_free_cid_base[qpair->id] : NULL,
		pctrlr->uncore_qdepth_base ?
		    (void *)&pctrlr->uncore_qdepth_base[qpair->id] : NULL);

	/* IO-Uncore v3 (Mode C): allocate the per-qpair host-DRAM descriptor ring
	 * (num_entries x 24B) and completion record ring (num_entries x 16B) as
	 * DMA-capable memory, then MMIO-write each ring's IOVA to the device's
	 * BAR0 IOVA-base table ONCE here (never per IO).  Host-DRAM only — done
	 * unconditionally under the env gate regardless of the cmb/opts SQ path.
	 * The record ring is allocated but UNUSED in Phase 1 (completions still
	 * arrive via the normal CQ). */
	pqpair->sq_desc_ring = NULL;
	pqpair->cq_rec_ring = NULL;
	pqpair->sq_desc_ring_bus_addr = 0;
	pqpair->cq_rec_ring_bus_addr = 0;
	pqpair->sq_desc_tail = 0;
	pqpair->last_sq_desc_tail = 0;
	pqpair->cq_rec_head = 0;
	pqpair->cq_rec_phase = 1;
	pqpair->free_slot_ring = NULL;
	pqpair->free_slot_ring_bus_addr = 0;
	pqpair->free_slot_head = 0;
	pqpair->free_slot_phase = 1;
	if (uncore_mode_c() && !nvme_qpair_is_admin_queue(&pqpair->qpair)) {
		/* Mode D grows the descriptor to 40B and the record to 32B (carrying the
		 * completion callback); Mode C uses 24B/16B.  Mode D sets both env vars,
		 * so uncore_mode_c() is also true here — pick sizes by uncore_mode_d(). */
		size_t sq_elem = uncore_mode_d() ? sizeof(struct nvme_uncore_sq_desc_d)
						 : sizeof(struct nvme_uncore_sq_desc);
		size_t cq_elem = uncore_mode_d() ? sizeof(struct nvme_uncore_cq_cb_rec)
						 : sizeof(struct nvme_uncore_cq_rec);
		size_t sq_ring_len = pqpair->num_entries * sq_elem;
		size_t cq_ring_len = pqpair->num_entries * cq_elem;
		size_t sq_ring_align = spdk_max(spdk_align32pow2(sq_ring_len), page_align);
		size_t cq_ring_align = spdk_max(spdk_align32pow2(cq_ring_len), page_align);

		pqpair->sq_desc_ring = spdk_zmalloc(sq_ring_len, sq_ring_align, NULL,
						    SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (pqpair->sq_desc_ring == NULL) {
			NVME_QPAIR_ERRLOG(qpair, "Mode C: alloc sq_desc_ring failed\n");
			return -ENOMEM;
		}
		pqpair->sq_desc_ring_bus_addr = nvme_pcie_vtophys(ctrlr, pqpair->sq_desc_ring, NULL);
		if (pqpair->sq_desc_ring_bus_addr == SPDK_VTOPHYS_ERROR) {
			NVME_QPAIR_ERRLOG(qpair, "Mode C: vtophys(sq_desc_ring) failed\n");
			return -EFAULT;
		}

		pqpair->cq_rec_ring = spdk_zmalloc(cq_ring_len, cq_ring_align, NULL,
						   SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (pqpair->cq_rec_ring == NULL) {
			NVME_QPAIR_ERRLOG(qpair, "Mode C: alloc cq_rec_ring failed\n");
			return -ENOMEM;
		}
		pqpair->cq_rec_ring_bus_addr = nvme_pcie_vtophys(ctrlr, pqpair->cq_rec_ring, NULL);
		if (pqpair->cq_rec_ring_bus_addr == SPDK_VTOPHYS_ERROR) {
			NVME_QPAIR_ERRLOG(qpair, "Mode C: vtophys(cq_rec_ring) failed\n");
			return -EFAULT;
		}

		/* Publish the ring IOVAs to the device ONCE (not per IO); out of line, bounded to the IAU
		 * page's tables (nvme_pcie_qpair_iau_publish_rings()). */
		rc = nvme_pcie_qpair_iau_publish_rings(qpair);
		if (rc != 0) {
			return rc;
		}
		NVME_QPAIR_NOTICELOG(qpair,
			"UNCORE-C | qid=%u sq_desc_ring=%p iova=0x%" PRIx64
			" cq_rec_ring=%p iova=0x%" PRIx64 "\n",
			qpair->id, pqpair->sq_desc_ring, pqpair->sq_desc_ring_bus_addr,
			pqpair->cq_rec_ring, pqpair->cq_rec_ring_bus_addr);

		/* IO-Uncore v4.2 (Mode E): allocate the per-qpair free-slot ring
		 * (num_entries x 8B), SEED it in host DRAM with the initial free slots
		 * 0..num_trackers-1 (phase=1), publish its IOVA to the device, then tell
		 * the device (seed-count MMIO) that num_trackers slots were seeded so its
		 * producer cursor starts past them.  The host consumes one slot per
		 * submit; the device refills one per completion.  Ring size matches the
		 * record/descriptor rings (num_entries) so the device wrap (CQ depth)
		 * and the host wrap (num_entries) agree. */
		if (uncore_mode_e()) {
			size_t fs_len = pqpair->num_entries * sizeof(uint64_t);
			size_t fs_align = spdk_max(spdk_align32pow2(fs_len), page_align);
			pqpair->free_slot_ring = spdk_zmalloc(fs_len, fs_align, NULL,
							      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
			if (pqpair->free_slot_ring == NULL) {
				NVME_QPAIR_ERRLOG(qpair, "Mode E: alloc free_slot_ring failed\n");
				return -ENOMEM;
			}
			pqpair->free_slot_ring_bus_addr =
				nvme_pcie_vtophys(ctrlr, pqpair->free_slot_ring, NULL);
			if (pqpair->free_slot_ring_bus_addr == SPDK_VTOPHYS_ERROR) {
				NVME_QPAIR_ERRLOG(qpair, "Mode E: vtophys(free_slot_ring) failed\n");
				return -EFAULT;
			}
			/* Seed slots 0..num_trackers-1 (cid == tracker index).  Entry =
			 * {phase@bit48, slot[15:0]}; phase 1 matches free_slot_phase. */
			for (uint16_t s = 0; s < num_trackers; s++) {
				pqpair->free_slot_ring[s] =
					((uint64_t)1 << FREE_SLOT_PHASE_BIT) | (uint64_t)s;
			}
			pqpair->free_slot_head = 0;
			pqpair->free_slot_phase = 1;
			if (pctrlr->uncore_free_slot_iova_reg != NULL &&
			    pctrlr->uncore_free_slot_seed_reg != NULL) {
				g_thread_mmio_ctrlr = pctrlr;
				/* IOVA first, then seed-count (which sets the device producer
				 * cursor); the device does not DMA on the seed write. */
				spdk_mmio_write_8(&pctrlr->uncore_free_slot_iova_reg[qpair->id],
						  pqpair->free_slot_ring_bus_addr);
				spdk_mmio_write_4(&pctrlr->uncore_free_slot_seed_reg[qpair->id],
						  (uint32_t)num_trackers);
				g_thread_mmio_ctrlr = NULL;
			}
			NVME_QPAIR_NOTICELOG(qpair,
				"UNCORE-E | qid=%u free_slot_ring=%p iova=0x%" PRIx64
				" seeded=%u entries=%u\n",
				qpair->id, pqpair->free_slot_ring,
				pqpair->free_slot_ring_bus_addr,
				(unsigned)num_trackers, (unsigned)pqpair->num_entries);
		}
	}

	/*
	 * Reserve space for all of the trackers in a single allocation.
	 *   struct nvme_tracker is one cache line (64 B) and holds no DMA-visible list, so the
	 *   tracker array is dense and trackers in tr[] are accessed via normal array indexing.
	 */
	pqpair->tr = spdk_zmalloc(num_trackers * sizeof(*tr), sizeof(*tr), NULL,
				  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE | SPDK_MALLOC_DMA);
	if (pqpair->tr == NULL) {
		NVME_QPAIR_ERRLOG(qpair, "nvme_tr failed\n");
		return -ENOMEM;
	}

	/*
	 * Each tracker's PRP list / SGL segment (and metadata SGL) lives in its own 4 KiB
	 *   page of a separate pool: page i belongs to tr[i].  struct nvme_tracker_list_page
	 *   is exactly 4 KiB and the pool is 4 KiB-aligned, so no list spans a 4KB boundary.
	 *   Same memory flags as the tracker array (the admin qpair, and with it its pool, is
	 *   shared across processes).
	 */
	pqpair->prp_pool = spdk_zmalloc(num_trackers * sizeof(struct nvme_tracker_list_page),
					sizeof(struct nvme_tracker_list_page), NULL,
					SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE | SPDK_MALLOC_DMA);
	if (pqpair->prp_pool == NULL) {
		NVME_QPAIR_ERRLOG(qpair, "prp_pool alloc failed\n");
		return -ENOMEM;
	}

	TAILQ_INIT(&pqpair->free_tr);
	TAILQ_INIT(&pqpair->outstanding_tr);
	pqpair->qpair.queue_depth = 0;

	/*
	 * F3 tracker-free Mode D: on a Mode D IO qpair (the Mode D completion loop runs, and the
	 * submit path owns the cid: no Mode E free-slot ring, no Mech #1 hardware free-CID ring)
	 * the cid space [0, num_trackers) is split into fast cids [0, md_fast) for the tracker-free
	 * descriptor path and tracker cids [md_fast, num_trackers) for everything else.  Only the
	 * tracker cids go on free_tr, so the tracker path can never hand out a fast cid.  Every
	 * other qpair (admin, Mode 0/B/C/E, forced trackers) keeps md_fast == 0 and all trackers.
	 */
	pqpair->flags.md_trfree = 0;
	pqpair->md_inflight = NULL;
	pqpair->md_cid_stack = NULL;
	pqpair->md_cid_top = 0;
	pqpair->md_fast_cids = 0;
	md_mode_d = !nvme_qpair_is_admin_queue(qpair) && uncore_mode_d() && !uncore_mode_e() &&
		    pqpair->sq_desc_ring != NULL && pqpair->cq_rec_ring != NULL &&
		    pctrlr->uncore_free_cid_base == NULL;
	md_fast = 0;
	if (md_mode_d && !nvme_pcie_md_trackers_forced() &&
	    num_trackers > NVME_PCIE_MD_TRACKER_CIDS) {
		md_fast = num_trackers - NVME_PCIE_MD_TRACKER_CIDS;
	}

	for (i = 0; i < num_trackers; i++) {
		/* Translate every page on its own: the pool need not be IOVA-contiguous. */
		uint64_t list_page_bus_addr = nvme_pcie_vtophys(ctrlr, &pqpair->prp_pool[i], NULL);

		if (list_page_bus_addr == SPDK_VTOPHYS_ERROR) {
			NVME_QPAIR_ERRLOG(qpair, "vtophys(prp_pool[%u]) failed\n", (unsigned)i);
			return -EFAULT;
		}
		tr = &pqpair->tr[i];
		nvme_qpair_construct_tracker(tr, i, &pqpair->prp_pool[i], list_page_bus_addr);
		if (i >= md_fast) {
			/* F3: trackers below md_fast stay constructed but are never handed out. */
			TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
		}
	}

	if (md_fast != 0) {
		/* One allocation: md_fast request pointers, then the md_fast-entry cid stack. */
		pqpair->md_inflight = spdk_zmalloc(md_fast * (sizeof(struct nvme_request *) +
						   sizeof(uint16_t)), 64, NULL,
						   SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (pqpair->md_inflight == NULL) {
			NVME_QPAIR_ERRLOG(qpair, "Mode D: alloc md_inflight failed\n");
			return -ENOMEM;
		}
		pqpair->md_cid_stack = (uint16_t *)(pqpair->md_inflight + md_fast);
		/* LIFO like free_tr (the most recently freed cid is reused first); seeded so the
		 * first pops hand out 0, 1, 2, ... and the in-flight slots stay dense. */
		for (i = 0; i < md_fast; i++) {
			pqpair->md_cid_stack[i] = (uint16_t)(md_fast - 1 - i);
		}
		pqpair->md_cid_top = md_fast;
		pqpair->md_fast_cids = md_fast;
		pqpair->flags.md_trfree = 1;
		fprintf(stderr, "MD_TRFREE qid=%u fast_cids=%u\n", qpair->id, (unsigned)md_fast);
	} else if (md_mode_d) {
		if (nvme_pcie_md_trackers_forced()) {
			fprintf(stderr, "MD_TRACKERS qid=%u\n", qpair->id);
		} else {
			fprintf(stderr, "MD_TRACKERS qid=%u (queue too small: %u trackers)\n",
				qpair->id, (unsigned)num_trackers);
		}
	}

	/* IAU-next P3 (testbed): with SPDK_IAU_ENGINE=1 a Mode D IO qpair registers with the IAU engine
	 * here, before Create I/O SQ (out of line: nvme_pcie_qpair_iau_engine_register()). */
	if (md_mode_d) {
		rc = nvme_pcie_qpair_iau_engine_register(qpair, num_trackers);
		if (rc != 0) {
			return rc;
		}
	}

	NVME_PCIE_CONSTRUCT_PAD(100);
	nvme_pcie_qpair_reset(qpair);

	return 0;
}

int
nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t num_entries)
{
	struct nvme_pcie_qpair *pqpair;
	int rc;

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	if (pqpair == NULL) {
		return -ENOMEM;
	}

	pqpair->num_entries = num_entries;
	pqpair->flags.delay_cmd_submit = 0;
	pqpair->pcie_state = NVME_PCIE_QPAIR_READY;

	ctrlr->adminq = &pqpair->qpair;

	rc = nvme_qpair_init(ctrlr->adminq,
			     0, /* qpair ID */
			     ctrlr,
			     SPDK_NVME_QPRIO_URGENT,
			     num_entries,
			     false);
	if (rc != 0) {
		return rc;
	}

	pqpair->stat = spdk_zmalloc(sizeof(*pqpair->stat), 64, NULL, SPDK_ENV_NUMA_ID_ANY,
				    SPDK_MALLOC_SHARE);
	if (!pqpair->stat) {
		NVME_CTRLR_ERRLOG(ctrlr, "Failed to allocate admin qpair statistics\n");
		return -ENOMEM;
	}

	return nvme_pcie_qpair_construct(ctrlr->adminq, NULL);
}

/**
 * Note: the ctrlr_lock must be held when calling this function.
 */
void
nvme_pcie_qpair_insert_pending_admin_request(struct spdk_nvme_qpair *qpair,
		struct nvme_request *req, struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	struct nvme_request		*active_req = req;
	struct spdk_nvme_ctrlr_process	*active_proc;

	/*
	 * The admin request is from another process. Move to the per
	 *  process list for that process to handle it later.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));
	assert(active_req->pid != getpid());

	active_proc = nvme_ctrlr_get_process(ctrlr, active_req->pid);
	if (active_proc) {
		/* Save the original completion information */
		memcpy(&active_req->cpl, cpl, sizeof(*cpl));
		STAILQ_INSERT_TAIL(&active_proc->active_reqs, active_req, stailq);
	} else {
		NVME_CTRLR_ERRLOG(ctrlr, "The owning process (pid %d) is not found. Dropping the request.\n",
				  active_req->pid);
		nvme_cleanup_user_req(active_req);
		nvme_free_request(active_req);
	}
}

/**
 * Note: the ctrlr_lock must be held when calling this function.
 */
void
nvme_pcie_qpair_complete_pending_admin_request(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	struct nvme_request		*req, *tmp_req;
	pid_t				pid = getpid();
	struct spdk_nvme_ctrlr_process	*proc;

	/*
	 * Check whether there is any pending admin request from
	 * other active processes.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));

	proc = nvme_ctrlr_get_current_process(ctrlr);
	if (!proc) {
		NVME_CTRLR_ERRLOG(ctrlr, "the active process (pid %d) is not found for this controller.\n", pid);
		assert(proc);
		return;
	}

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == pid);

		nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, &req->cpl);
	}
}

int
nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	bool ien = ctrlr->opts.enable_interrupts;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_CQ;

	cmd->cdw10_bits.create_io_q.qid = io_que->id;
	cmd->cdw10_bits.create_io_q.qsize = pqpair->num_entries - 1;

	cmd->cdw11_bits.create_io_cq.pc = 1;
	if (ien) {
		cmd->cdw11_bits.create_io_cq.ien = 1;
		/* The interrupt vector offset starts from 1. We directly map the
		 * queue id to interrupt vector.
		 */
		cmd->cdw11_bits.create_io_cq.iv = io_que->id;
	}

	cmd->dptr.prp.prp1 = pqpair->cpl_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

int
nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_SQ;

	cmd->cdw10_bits.create_io_q.qid = io_que->id;
	cmd->cdw10_bits.create_io_q.qsize = pqpair->num_entries - 1;
	cmd->cdw11_bits.create_io_sq.pc = 1;
	cmd->cdw11_bits.create_io_sq.qprio = io_que->qprio;
	cmd->cdw11_bits.create_io_sq.cqid = io_que->id;
	cmd->dptr.prp.prp1 = pqpair->cmd_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

int
nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd->cdw10_bits.delete_io_q.qid = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

int
nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd->cdw10_bits.delete_io_q.qid = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static void
nvme_completion_sq_error_delete_cq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_QPAIR_ERRLOG(qpair, "delete_io_cq failed!\n");
	}

	pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
}

static void
nvme_completion_create_sq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	int rc;

	if (pqpair->flags.defer_destruction) {
		/* This qpair was deleted by the application while the
		 * connection was still in progress.  We had to wait
		 * to free the qpair resources until this outstanding
		 * command was completed.  Now that we have the completion
		 * free it now.
		 */
		nvme_pcie_qpair_destroy(qpair);
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_QPAIR_ERRLOG(qpair, "nvme_create_io_sq failed, deleting cq!\n");
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_sq_error_delete_cq_cb,
						      qpair);
		if (rc != 0) {
			NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_cq with rc=%d\n", rc);
			pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		}
		return;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_READY;
	if (ctrlr->shadow_doorbell) {
		pqpair->shadow_doorbell.sq_tdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 0) *
						  pctrlr->doorbell_stride_u32;
		pqpair->shadow_doorbell.cq_hdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 1) *
						  pctrlr->doorbell_stride_u32;
		pqpair->shadow_doorbell.sq_eventidx = ctrlr->eventidx + (2 * qpair->id + 0) *
						      pctrlr->doorbell_stride_u32;
		pqpair->shadow_doorbell.cq_eventidx = ctrlr->eventidx + (2 * qpair->id + 1) *
						      pctrlr->doorbell_stride_u32;
		pqpair->flags.has_shadow_doorbell = 1;
	} else {
		pqpair->flags.has_shadow_doorbell = 0;
	}
	nvme_pcie_qpair_reset(qpair);

}

static void
nvme_completion_create_cq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	int rc;

	if (pqpair->flags.defer_destruction) {
		/* This qpair was deleted by the application while the
		 * connection was still in progress.  We had to wait
		 * to free the qpair resources until this outstanding
		 * command was completed.  Now that we have the completion
		 * free it now.
		 */
		nvme_pcie_qpair_destroy(qpair);
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		NVME_QPAIR_ERRLOG(qpair, "nvme_create_io_cq failed!\n");
		return;
	}

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_create_sq_cb, qpair);

	if (rc != 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to create_io_sq, deleting cq!\n");
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_sq_error_delete_cq_cb,
						      qpair);
		if (rc != 0) {
			NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_cq with rc=%d\n", rc);
			pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		}
		return;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_WAIT_FOR_SQ;
}

static int
_nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 uint16_t qid)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	int	rc;

	/* Statistics may already be allocated in the case of controller reset */
	if (qpair->poll_group) {
		struct nvme_pcie_poll_group *group = SPDK_CONTAINEROF(qpair->poll_group,
						     struct nvme_pcie_poll_group, group);

		pqpair->stat = &group->stats;
		pqpair->shared_stats = true;
	} else {
		if (pqpair->stat == NULL) {
			pqpair->stat = calloc(1, sizeof(*pqpair->stat));
			if (!pqpair->stat) {
				NVME_QPAIR_ERRLOG(qpair, "Failed to allocate qpair statistics\n");
				nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
				return -ENOMEM;
			}
		}
	}

	rc = nvme_pcie_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_create_cq_cb, qpair);

	if (rc != 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to create_io_cq\n");
		nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
		return rc;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_WAIT_FOR_CQ;
	return 0;
}

int
nvme_pcie_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;

	if (!nvme_qpair_is_admin_queue(qpair)) {
		rc = _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qpair->id);
	} else {
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
	}

	return rc;
}

void
nvme_pcie_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	if (!nvme_qpair_is_admin_queue(qpair) || !ctrlr->is_disconnecting) {
		nvme_transport_ctrlr_disconnect_qpair_done(qpair);
	} else {
		/* If this function is called for the admin qpair via spdk_nvme_ctrlr_reset()
		 * or spdk_nvme_ctrlr_disconnect(), initiate a Controller Level Reset.
		 * Then we can abort trackers safely because the Controller Level Reset deletes
		 * all I/O SQ/CQs.
		 */
		nvme_ctrlr_disable(ctrlr);
	}
}

/* Used when dst points to MMIO (i.e. CMB) in a virtual machine - in these cases we must
 * not use wide instructions because QEMU will not emulate such instructions to MMIO space.
 * So this function ensures we only copy 8 bytes at a time.
 */
static inline void
nvme_pcie_copy_command_mmio(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	uint64_t *dst64 = (uint64_t *)dst;
	const uint64_t *src64 = (const uint64_t *)src;
	uint32_t i;

	for (i = 0; i < sizeof(*dst) / 8; i++) {
		dst64[i] = src64[i];
	}
}

static inline void
nvme_pcie_copy_command(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	/* dst and src are known to be non-overlapping and 64-byte aligned. */
#if defined(__SSE2__)
	__m128i *d128 = (__m128i *)dst;
	const __m128i *s128 = (const __m128i *)src;

	_mm_stream_si128(&d128[0], _mm_load_si128(&s128[0]));
	_mm_stream_si128(&d128[1], _mm_load_si128(&s128[1]));
	_mm_stream_si128(&d128[2], _mm_load_si128(&s128[2]));
	_mm_stream_si128(&d128[3], _mm_load_si128(&s128[3]));
#else
	*dst = *src;
#endif
}

void NVME_PCIE_HOT_ALIGN
nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr;

	req = tr->req;
	assert(req != NULL);

	spdk_trace_record(TRACE_NVME_PCIE_SUBMIT, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12,
			  pqpair->qpair.queue_depth);

	/* IO-Uncore Mode 2B: compact mailbox submission path.  Skips the
	 * 64-byte SQE copy + standard doorbell ring when:
	 *   - mailbox is mapped (SPDK_UNCORE_MODE_B=1 + BAR0 large enough)
	 *   - this is an I/O qpair (qid > 0)
	 *   - the transfer fits in a single page (payload_size <= page_size)
	 *   - command is a single-page read or write
	 * The compact 24-byte descriptor is emitted as 3 sequential 8-byte
	 * MMIO writes to the per-qid mailbox slot.  Timestamps t_sqe_copy /
	 * t_cmd_end / t_doorbell / t_fence_end / t_db_end collapse to two
	 * rdtsc samples (around the 3 MMIO writes) to reflect the absence
	 * of those stages in instrumentation. */
	pctrlr = nvme_pcie_ctrlr(ctrlr);
	if (pctrlr->uncore_mailbox_base != NULL &&
	    qpair->id != 0 &&
	    req->payload_size <= (uncore_multipage() ? 131072u : ctrlr->page_size) &&
	    (req->cmd.opc == SPDK_NVME_OPC_READ ||
	     req->cmd.opc == SPDK_NVME_OPC_WRITE)) {
		uint64_t _w0, _w1, _w2;
		volatile uint64_t *_slot;

		_slot = pctrlr->uncore_mailbox_base +
			(qpair->id * (MAILBOX_STRIDE_BYTES / sizeof(uint64_t)));

#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_sqe_copy = nvme_io_cycle_rdtsc();
#endif
		if (uncore_sqe16()) {
			/* v2 16-byte compact SQE: 2 MMIO writes.  gem5 expects
			 * nlb at w0[31:16] and control at w0[15:0]; cdw12 packs
			 * control<<16 | nlb, hence the swap below.  nsid is fixed
			 * to 1 and prp1_lo is dropped. */
			_w0 = ((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
			      ((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
			      ((uint64_t) req->cmd.cid           << 32) |
			      ((uint64_t)(req->cmd.cdw12 & 0xFFFFu) << 16) |
			      ((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu));
			_w1 = (uint64_t)req->cmd.cdw10 |
			      ((uint64_t)req->cmd.cdw11 << 32);
			g_thread_mmio_ctrlr = pctrlr;
			spdk_mmio_write_8(&_slot[0], _w0);
			spdk_mmio_write_8(&_slot[1], _w1);
			g_thread_mmio_ctrlr = NULL;
		} else {
			_w0 = ((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
			      ((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
			      ((uint64_t) req->cmd.cid           << 32) |
			      ((uint64_t) req->cmd.nsid                );
			_w1 = (uint64_t)req->cmd.cdw10 |
			      ((uint64_t)req->cmd.cdw11 << 32);  /* SLBA */
			_w2 = (((uint64_t)(uint32_t)req->cmd.dptr.prp.prp1) << 32) |
			      ((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu) <<  0) |
			      ((uint64_t)( req->cmd.cdw12        & 0xFFFFu) << 16);
			/* w2 layout: [63:32] prp1_lo32 | [31:16] nlb | [15:0] control
			 * NVMe cdw12 = [31:16] control | [15:0] nlb (0-based).  We
			 * pack them swapped so the controller-side decode matches the
			 * plan's wire format. */
			g_thread_mmio_ctrlr = pctrlr;
			spdk_mmio_write_8(&_slot[0], _w0);
			spdk_mmio_write_8(&_slot[1], _w1);
			spdk_mmio_write_8(&_slot[2], _w2);
			g_thread_mmio_ctrlr = NULL;
		}
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_cmd_end   = nvme_io_cycle_rdtsc();
		req->t_doorbell  = req->t_cmd_end;   /* no SQ doorbell */
		req->t_fence_end = req->t_cmd_end;   /* no fence (no doorbell) */
		req->t_db_end    = req->t_cmd_end;
		req->t_poll_start = pqpair->poll_cycle_accum;
#endif
		pqpair->stat->sq_mmio_doorbell_updates++;  /* keep counter for fairness */

		/* Mailbox path bypasses host SQ memory entirely; the SQE never
		 * lands there.  Advancing sq_tail here would cause a later SQ
		 * tail doorbell write (driven by delay_cmd_submit batching, or
		 * by an interleaved standard-path command) to instruct the
		 * controller to fetch SQEs from the previous sq_tail to the
		 * current one — but those slots in host SQ memory are zero/
		 * stale, yielding phantom commands with cid=0 that come back
		 * as 'cpl does not map to outstanding cmd' and pollute the
		 * Mech #1 free-CID ring with bogus recycles.  Leave sq_tail
		 * untouched; the controller has the command via mailbox MMIO. */
		return;
	}

	if (req->cmd.fuse) {
		/*
		 * Keep track of the fuse operation sequence so that we ring the doorbell only
		 * after the second fuse is submitted.
		 */
		qpair->last_fuse = req->cmd.fuse;
	}

	/* Don't use wide instructions to copy NVMe command, this is limited by QEMU
	 * virtual NVMe controller, the maximum access width is 8 Bytes for one time.
	 */
	if (spdk_unlikely((ctrlr->quirks & NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH) && pqpair->sq_in_cmb)) {
		nvme_pcie_copy_command_mmio(&pqpair->cmd[pqpair->sq_tail], &req->cmd);
	} else {
		/* Copy the command from the tracker to the submission queue. */
		nvme_pcie_copy_command(&pqpair->cmd[pqpair->sq_tail], &req->cmd);
	}

#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_sqe_copy = nvme_io_cycle_rdtsc();
	req->t_cmd_end = req->t_sqe_copy;
#endif

	if (spdk_unlikely(++pqpair->sq_tail == pqpair->num_entries)) {
		pqpair->sq_tail = 0;
	}

	if (spdk_unlikely(pqpair->sq_tail == pqpair->sq_head)) {
		NVME_QPAIR_ERRLOG(qpair, "sq_tail is passing sq_head!\n");
	}

	if (!pqpair->flags.delay_cmd_submit) {
		nvme_pcie_qpair_ring_sq_doorbell(qpair);
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_doorbell = nvme_io_cycle_rdtsc();
		req->t_poll_start = pqpair->poll_cycle_accum;
		req->t_fence_end = pqpair->last_sq_fence_ts;
		req->t_db_end = pqpair->last_sq_db_ts;
#endif
	}
}

void NVME_PCIE_HOT_ALIGN
nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_pcie_qpair		*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_request		*req;
	bool				retry, error;
	bool				print_error;

	req = tr->req;

	spdk_trace_record(TRACE_NVME_PCIE_COMPLETE, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)cpl->status_raw, pqpair->qpair.queue_depth);

	assert(req != NULL);

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < pqpair->retry_count;
	print_error = error && print_on_error && !qpair->ctrlr->opts.disable_error_logging;

	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
	}

	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		spdk_nvme_qpair_print_completion(qpair, cpl);
	}

	assert(cpl->cid == req->cmd.cid);

	if (retry) {
		req->retries++;
		nvme_pcie_qpair_submit_tracker(qpair, tr);
	} else {
		/* Mechanism #1: when the hardware free-CID ring is mapped AND this
		 * is an I/O qpair, hardware auto-recycles the CID on CQE flush and
		 * tracks the in-flight count.  We skip the symmetric TAILQ ops and
		 * the software queue_depth decrement; pqpair->tr[cid] is already
		 * free for reuse from the controller's perspective. */
		struct nvme_pcie_ctrlr *_pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
		bool hw_managed = (_pctrlr->uncore_free_cid_base != NULL &&
				   qpair->id != 0);
		if (!hw_managed) {
			TAILQ_REMOVE(&pqpair->outstanding_tr, tr, tq_list);
			pqpair->qpair.queue_depth--;
		}
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_dealloc_pre_end = nvme_io_cycle_rdtsc();

		if (req->t_completion == 0) {
			req->t_completion = req->t_cqe_seen ? req->t_cqe_seen : nvme_io_cycle_rdtsc();
		}
		/* Measure completion logic up to (but not including) callback execution. */
		req->t_end = req->t_dealloc_pre_end;
#endif
		/* Record before the callback: its resubmit reuses and resets req. */
		nvme_io_cycle_record(req);

		/* Bracket the application callback (fires after t_end at line 898)
		 * so app_callback_ns is cleanly separable from NVMe plumbing.  req
		 * memory persists after nvme_complete_request frees it to the pool,
		 * so writing t_cb_end here is safe (read later in cycle_record). */
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_cb_start = nvme_io_cycle_rdtsc();
#endif
		/* Only check admin requests from different processes. */
		if (nvme_qpair_is_admin_queue(qpair) && req->pid != getpid()) {
			nvme_pcie_qpair_insert_pending_admin_request(qpair, req, cpl);
		} else {
			nvme_complete_request(tr->cb_fn, tr->cb_arg, qpair, req, cpl);
		}
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_cb_end = nvme_io_cycle_rdtsc();
#endif

		tr->req = NULL;

		if (!hw_managed) {
			TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
		}
	}
}

void
nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
					struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
					bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.cid = tr->cid;
	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.status.dnr = dnr;
	nvme_pcie_qpair_complete_tracker(qpair, tr, &cpl, print_on_error);
}

/* F3: tracker-free requests detached from md_inflight by an abort, linked through req->stailq
 * (unused while a request is in flight in the transport). */
STAILQ_HEAD(nvme_pcie_md_reqs, nvme_request);

/* F3: move every tracker-free request in flight onto *list and clear its md_inflight slot.
 * Called before any callback runs, so this is a snapshot: IO a callback submits while the abort
 * runs is not aborted (the tracker walk gets the same effect from `last`).  The cid is NOT returned
 * here and queue_depth is not dropped: nvme_pcie_md_manual_complete() drops queue_depth (where
 * complete_tracker does) and the abort loop returns the cid only after the request's callback ran,
 * like complete_tracker returns the tracker after its callback.  So no request submitted from an
 * abort callback can be handed a cid whose aborted request is still being completed.
 * No-op on qpairs without tracker-free Mode D (md_fast_cids == 0). */
static void
nvme_pcie_md_detach_inflight(struct nvme_pcie_qpair *pqpair, struct nvme_pcie_md_reqs *list)
{
	uint16_t cid;

	STAILQ_INIT(list);
	for (cid = 0; cid < pqpair->md_fast_cids; cid++) {
		struct nvme_request *req = pqpair->md_inflight[cid];

		if (req == NULL) {
			continue;
		}
		assert(req->cmd.cid == cid);
		pqpair->md_inflight[cid] = NULL;
		STAILQ_INSERT_TAIL(list, req, stailq);
	}
}

/* F3: complete a detached tracker-free request with a host-made status: the same trace event, cpl
 * fields, error printing, queue_depth drop, per-IO cycle record and nvme_complete_request() call
 * as nvme_pcie_qpair_manual_complete_tracker() -> nvme_pcie_qpair_complete_tracker().  No retry:
 * the abort statuses used here are never retried by complete_tracker either
 * (nvme_completion_is_retry() is false for them).  The caller returns the cid afterwards. */
static void
nvme_pcie_md_manual_complete(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
			     uint32_t sct, uint32_t sc, uint32_t dnr, bool print_on_error)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_cpl	cpl;
	bool			print_error;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.cid = req->cmd.cid;
	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.status.dnr = dnr;

	spdk_trace_record(TRACE_NVME_PCIE_COMPLETE, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)cpl.status_raw, pqpair->qpair.queue_depth);
	assert(!nvme_completion_is_retry(&cpl));

	print_error = spdk_nvme_cpl_is_error(&cpl) && print_on_error &&
		      !qpair->ctrlr->opts.disable_error_logging;
	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
	}
	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		spdk_nvme_qpair_print_completion(qpair, &cpl);
	}

	assert(pqpair->qpair.queue_depth > 0);
	pqpair->qpair.queue_depth--;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_dealloc_pre_end = nvme_io_cycle_rdtsc();
	if (req->t_completion == 0) {
		req->t_completion = req->t_cqe_seen ? req->t_cqe_seen : nvme_io_cycle_rdtsc();
	}
	req->t_end = req->t_dealloc_pre_end;
#endif
	nvme_io_cycle_record(req);
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_cb_start = nvme_io_cycle_rdtsc();
#endif
	nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, &cpl);
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_cb_end = nvme_io_cycle_rdtsc();
#endif
}

void
nvme_pcie_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *temp, *last;
	struct nvme_pcie_md_reqs md_reqs;
	struct nvme_request *req;

	last = TAILQ_LAST(&pqpair->outstanding_tr, nvme_outstanding_tr_head);
	/* F3: snapshot the tracker-free IO in flight too, before any callback can resubmit. */
	nvme_pcie_md_detach_inflight(pqpair, &md_reqs);

	/* Abort previously submitted (outstanding) trs */
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, temp) {
		if (!qpair->ctrlr->opts.disable_error_logging) {
			NVME_QPAIR_ERRLOG(qpair, "aborting outstanding command\n");
		}
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, dnr, true);

		if (tr == last) {
			break;
		}
	}

	/* F3: abort the tracker-free IO with the same status and logging, each exactly once; the
	 * cid goes back on the stack only after its request's callback ran. */
	while ((req = STAILQ_FIRST(&md_reqs)) != NULL) {
		uint16_t cid = req->cmd.cid;	/* req may be reallocated by its own callback */

		STAILQ_REMOVE_HEAD(&md_reqs, stailq);
		if (!qpair->ctrlr->opts.disable_error_logging) {
			NVME_QPAIR_ERRLOG(qpair, "aborting outstanding command\n");
		}
		nvme_pcie_md_manual_complete(qpair, req, SPDK_NVME_SCT_GENERIC,
					     SPDK_NVME_SC_ABORTED_BY_REQUEST, dnr, true);
		assert(cid < pqpair->md_fast_cids && pqpair->md_cid_top < pqpair->md_fast_cids);
		pqpair->md_cid_stack[pqpair->md_cid_top++] = cid;
	}
}

void
nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;

	tr = TAILQ_FIRST(&pqpair->outstanding_tr);
	while (tr != NULL) {
		assert(tr->req != NULL);
		if (tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			nvme_pcie_qpair_manual_complete_tracker(qpair, tr,
								SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, 0,
								false);
			tr = TAILQ_FIRST(&pqpair->outstanding_tr);
		} else {
			tr = TAILQ_NEXT(tr, tq_list);
		}
	}
}

void
nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

void
nvme_pcie_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	nvme_pcie_qpair_abort_trackers(qpair, dnr);
}

static void
nvme_pcie_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct nvme_tracker *tr, *tmp;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
		assert(tr->req != NULL);

		if (nvme_request_check_timeout(tr->req, tr->cid, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

/*
 * F3 tracker-free Mode D.  The Mode 0 / tracker code paths keep the pre-F3 instructions: the entry
 * points nvme_pcie_qpair_process_completions() and nvme_pcie_qpair_submit_request() test
 * pqpair->flags.md_trfree once and tail-call either the stock body
 * (nvme_pcie_qpair_process_completions_stock() / nvme_pcie_qpair_submit_request_stock(), the pre-F3
 * bodies verbatim) or the hot tracker-free function (nvme_pcie_md_poll() / nvme_pcie_md_submit()).
 */

/* F3: the timeout check of a tracker-free qpair, called from nvme_pcie_md_poll() where
 * nvme_pcie_qpair_process_completions_stock() calls nvme_pcie_qpair_check_timeout() (kept with a
 * single caller, so its inlining is unchanged).  Same preconditions; the tracker walk is that function's
 * loop verbatim.  Tracker-free IO is not in submit order (cids come from a LIFO stack), so every
 * in-flight fast cid is checked; the walk stops once all of them (md_fast_cids - md_cid_top) were
 * seen and re-reads each slot (a callback may abort).  The generic layer set req->submit_tick at
 * submit, as for trackers. */
static void __attribute__((noinline, cold))
nvme_pcie_md_check_timeout(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr_process *active_proc = qpair->active_proc;
	struct nvme_tracker *tr, *tmp;
	uint32_t left;
	uint64_t t02;
	uint16_t cid;

	/* Don't check timeouts during controller initialization. */
	if (qpair->ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}
	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}
	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
		assert(tr->req != NULL);

		if (nvme_request_check_timeout(tr->req, tr->cid, active_proc, t02)) {
			/* The requests are in order, so as soon as one has not timed out, stop. */
			break;
		}
	}
	left = (uint32_t)pqpair->md_fast_cids - pqpair->md_cid_top;
	for (cid = 0; cid < pqpair->md_fast_cids && left != 0; cid++) {
		struct nvme_request *req = pqpair->md_inflight[cid];

		if (req != NULL) {
			left--;
			nvme_request_check_timeout(req, cid, active_proc, t02);
		}
	}
}

/* P2.1: the poll bit and the status code and type of a Mode D record as bits of
 * (uint32_t)(cpl_hi >> 48), the NVMe status word: P [0], SC [8:1], SCT [11:9].  With that word
 * XOR the expected ring phase, these bits are all 0 <=> a new record whose status is a success
 * (spdk_nvme_cpl_is_error() false); bit 0 is set <=> no new record. */
#define NVME_PCIE_MD_REC_PHASE_SCT_SC	(0x0FFFu)

/* P2.1: re-issue an in-flight Mode D request on its own cid.  This is the stock retry
 * (nvme_pcie_qpair_complete_tracker() -> nvme_pcie_qpair_submit_tracker() on the same tracker): the
 * request stays in flight and queue_depth, md_inflight / outstanding_tr and the cid do not change.
 * It writes the descriptor the submit wrote: the tracker-free one (w2 = 0, w4 = the request) when
 * tr == NULL, else the tracker one (w2 = tr, w4 = cb_fn).  nvme_pcie_qpair_submit_tracker() itself
 * cannot be used on Mode D: it copies a classic SQE, which the Mode D device never fetches (the SQ
 * doorbell publishes sq_desc_tail), so the IO would never complete. */
static void __attribute__((noinline, cold))
nvme_pcie_md_reissue(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
		     struct nvme_tracker *tr)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_uncore_sq_desc_d *ring_d = (struct nvme_uncore_sq_desc_d *)pqpair->sq_desc_ring;
	uint32_t idx = pqpair->sq_desc_tail;

	/* the submit tracepoint the stock retry records (nvme_pcie_qpair_submit_tracker()) */
	spdk_trace_record(TRACE_NVME_PCIE_SUBMIT, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12,
			  pqpair->qpair.queue_depth);

	ring_d[idx].w0 =
		((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
		((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
		((uint64_t) req->cmd.cid           << 32) |
		((uint64_t)(req->cmd.cdw12 & 0xFFFFu) << 16) |
		((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu));
	ring_d[idx].w1 = (uint64_t)req->cmd.cdw10 | ((uint64_t)req->cmd.cdw11 << 32);
	ring_d[idx].w2 = (uint64_t)(uintptr_t)tr;
	ring_d[idx].w3 = (uint64_t)(uintptr_t)req->cb_arg;
	ring_d[idx].w4 = tr == NULL ? (uint64_t)(uintptr_t)req : (uint64_t)(uintptr_t)req->cb_fn;
	pqpair->sq_desc_tail = (idx + 1 == pqpair->num_entries) ? 0 : idx + 1;
	if (!pqpair->flags.delay_cmd_submit) {
		nvme_pcie_qpair_ring_sq_doorbell(qpair);
		pqpair->last_sq_desc_tail = pqpair->sq_desc_tail;
	}
}

/* P2.1: complete one Mode D record with the stock semantics of nvme_pcie_qpair_complete_tracker().
 * It prints as complete_tracker prints and, for a retryable error with retries left, re-issues the
 * request on its own cid (no callback).  Anything else is delivered through nvme_complete_request()
 * (completion-side error injection, request free, callback): the fast cid is returned before the
 * callback, as md_poll does, the tracker after it, as complete_tracker does.  Reached for a record
 * with an error status and for every record while an error injection is armed; the caller advanced
 * the record cursor.  A record that maps to no outstanding command is reported with md_poll's
 * messages and never completed. */
static void __attribute__((noinline, cold))
nvme_pcie_md_complete_checked(struct spdk_nvme_qpair *qpair, uint64_t rec_w0, uint16_t cid,
			      uint64_t cpl_lo, uint64_t cpl_hi)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr = NULL;
	struct nvme_request *req;
	struct spdk_nvme_cpl cpl;

	memcpy((uint8_t *)&cpl + 0, &cpl_lo, 8);
	memcpy((uint8_t *)&cpl + 8, &cpl_hi, 8);

	if (cid < pqpair->md_fast_cids) {
		req = (struct nvme_request *)(uintptr_t)rec_w0;
		if (spdk_unlikely(req == NULL || pqpair->md_inflight[cid] != req)) {
			NVME_QPAIR_ERRLOG(qpair, "mode-D trfree record cid %u does not map "
					  "to outstanding cmd\n", (unsigned)cid);
			assert(0);
			return;
		}
	} else {
		tr = &pqpair->tr[cid];
		req = tr->req;
		if (spdk_unlikely(req == NULL)) {
			NVME_QPAIR_ERRLOG(qpair,
					  "mode-D cq record cid does not map to outstanding cmd\n");
			assert(0);
			return;
		}
	}

	spdk_trace_record(TRACE_NVME_PCIE_COMPLETE, qpair->id, 0, (uintptr_t)req, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)cpl.status_raw, pqpair->qpair.queue_depth);
	assert(cpl.cid == req->cmd.cid);

	/* Every checked completion goes through the policy, as complete_tracker runs it for every
	 * CQE: with the "nvme" debug log flag a successful one prints its completion too. */
	if (nvme_iau_cpl_error_policy(qpair, &req->cmd, req->retries, pqpair->retry_count, &cpl,
				      true) == NVME_IAU_CPL_RETRY) {
		req->retries++;
		nvme_pcie_md_reissue(qpair, req, tr);
		return;
	}

	if (tr == NULL) {
		pqpair->md_inflight[cid] = NULL;
		assert(pqpair->md_cid_top < pqpair->md_fast_cids);
		pqpair->md_cid_stack[pqpair->md_cid_top++] = cid;
	} else {
		TAILQ_REMOVE(&pqpair->outstanding_tr, tr, tq_list);
	}
	assert(pqpair->qpair.queue_depth > 0);
	pqpair->qpair.queue_depth--;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_dealloc_pre_end = nvme_io_cycle_rdtsc();
	if (req->t_completion == 0) {
		req->t_completion = req->t_cqe_seen ? req->t_cqe_seen : nvme_io_cycle_rdtsc();
	}
	req->t_end = req->t_dealloc_pre_end;
#endif
	nvme_io_cycle_record(req);
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_cb_start = nvme_io_cycle_rdtsc();
#endif
	/* One call site, so nvme_complete_request() is inlined here instead of being emitted out of
	 * line in .text, where it would shift the Mode 0 functions. */
	nvme_complete_request(tr == NULL ? req->cb_fn : tr->cb_fn,
			      tr == NULL ? req->cb_arg : tr->cb_arg, qpair, req, &cpl);
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_cb_end = nvme_io_cycle_rdtsc();
#endif
	if (tr != NULL) {
		tr->req = NULL;
		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
	}
}

/* P2.1: md_poll found a new record whose status is an error (its poll bit matches, its status does
 * not read success): read the rest of the record, advance the record cursor as md_poll does, and
 * complete it with the stock semantics. */
static void __attribute__((noinline, cold))
nvme_pcie_md_error_record(struct spdk_nvme_qpair *qpair,
			  volatile struct nvme_uncore_cq_cb_rec *rec, uint64_t cpl_hi)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	uint64_t rec_w0, cpl_lo;

	nvme_iau_rec_rmb();	/* P2.2: the poll word before the payload, as in md_poll */
	rec_w0 = rec->cb_fn;
	cpl_lo = rec->cpl_lo;

	if (++pqpair->cq_rec_head == pqpair->num_entries) {
		pqpair->cq_rec_head = 0;
		pqpair->cq_rec_phase = !pqpair->cq_rec_phase;
	}
	nvme_pcie_md_complete_checked(qpair, rec_w0, (uint16_t)((cpl_hi >> 32) & 0xFFFFu), cpl_lo,
				      cpl_hi);
}

/* F3: the whole poll of a tracker-free Mode D qpair, reached by a tail call from
 * nvme_pcie_qpair_process_completions() (one test), so the stock poll keeps the pre-F3
 * instructions.  The head and the tail are those of nvme_pcie_qpair_process_completions_stock() for
 * a Mode D IO qpair; keep them in step:
 * - head: failed state, the Mode 2B hint early exit (its register is not mapped together with
 *   Mode D; kept for parity), the connect handshake, the max_completions clamp, the poll counters;
 * - loop: the Mode D record loop (same phase test, word reads, cursor advance and max_completions
 *   batching); records with cid < md_fast_cids are tracker-free (w0 = the nvme_request), the
 *   others take a copy of the tracker branch.  P2.1: the phase test also covers the status, so a
 *   record with an error status, and every record while a completion-side error injection is
 *   armed, completes through nvme_pcie_md_complete_checked() with the stock retry, print and
 *   injection semantics; the per-record success path gains no conditional branch;
 * - tail: no CQ-head doorbell (the device owns CQ-head advancement), descriptor-ring SQ doorbell
 *   batching, timeouts (trackers + fast cids), pending vtophys failures. */
static int32_t __attribute__((noinline, hot)) NVME_PCIE_HOT_ALIGN
nvme_pcie_md_poll(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	volatile struct nvme_uncore_cq_cb_rec *ring_d;
	uint16_t md_fast;
	struct nvme_tracker *tr;
	uint32_t num_completions = 0;
	int rc;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	uint64_t poll_start;
#endif

	if (spdk_unlikely(pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED)) {
		return -ENXIO;
	}

	{
		struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
		enum nvme_qpair_state _st_now = nvme_qpair_get_state(qpair);
		bool _live = (_st_now == NVME_QPAIR_CONNECTED) ||
			     (_st_now == NVME_QPAIR_ENABLED);

		if (spdk_unlikely(pctrlr->uncore_hint_reg != NULL) && spdk_likely(_live)) {
			uint32_t hint = *pctrlr->uncore_hint_reg;
			uint16_t count = (uint16_t)(hint & 0xFFFFu);
			uint16_t age   = (uint16_t)(hint >> 16);

			if (count == 0 && age < 4) {
				return 0;
			}
		}
	}

#ifndef SPDK_NVME_IO_CYCLE_OFF
	poll_start = nvme_io_cycle_rdtsc();
#endif

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		if (pqpair->pcie_state == NVME_PCIE_QPAIR_READY) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
			return -ENXIO;
		} else {
			rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
			if (rc < 0) {
				return rc;
			} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
				nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
				return -ENXIO;
			}
		}
		return 0;
	}

	if (max_completions == 0 || max_completions > pqpair->max_completions_cap) {
		max_completions = pqpair->max_completions_cap;
	}

	pqpair->stat->polls++;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	pqpair->stat->completion_calls++;
#endif

	ring_d = (volatile struct nvme_uncore_cq_cb_rec *)pqpair->cq_rec_ring;
	/* P2.1: while a completion-side error injection is armed on this qpair
	 * (spdk_nvme_qpair_add_cmd_error_injection(); the stock completion applies it in
	 * nvme_complete_request()), md_fast = 0 sends every record past the tracker-free branch
	 * to nvme_pcie_md_complete_checked().  Tested once per poll, so an injection armed from
	 * inside a completion callback applies from the next poll. */
	md_fast = spdk_likely(TAILQ_EMPTY(&qpair->err_cmd_head)) ? pqpair->md_fast_cids : 0;
	while (1) {
		volatile struct nvme_uncore_cq_cb_rec *rec = &ring_d[pqpair->cq_rec_head];
		uint64_t cpl_hi = rec->cpl_hi;
#ifndef SPDK_NVME_IO_CYCLE_OFF
		uint64_t cqe_seen = nvme_io_cycle_rdtsc();
#endif

		/* Ring poll bit = cpl_hi[48] (LOCKED); toggles on wrap.  Cacheable
		 * read, NO MMIO.  P2.1: one test covers the poll bit and the status
		 * code and type (cpl_hi[59:49]): with md_d = cpl_hi[63:48] ^ the
		 * expected phase, md_d[11:0] == 0 <=> a new record with a success
		 * status.  Otherwise md_d[0] == 1: the poll bit has not flipped (no
		 * new completion); md_d[0] == 0: a new record carries an error status,
		 * which takes the stock error semantics out of line.  spdk_unlikely
		 * keeps the success path the fall-through and md_poll inside its
		 * 64 B-aligned .text.hot slot (md_submit keeps its offset). */
		uint32_t md_d = (uint32_t)(cpl_hi >> 48) ^ (uint32_t)pqpair->cq_rec_phase;

		if (spdk_unlikely((md_d & NVME_PCIE_MD_REC_PHASE_SCT_SC) != 0)) {
			if ((md_d & 0x1u) != 0) {
				break;
			}
			nvme_pcie_md_error_record(qpair, rec, cpl_hi);
			if (++num_completions == max_completions) {
				break;
			}
			continue;
		}

		/* P2.2: order the poll-word load before the payload loads on weakly
		 * ordered ISAs, as the stock CQ loop does (empty on x86). */
		nvme_iau_rec_rmb();

		/* Device published w0..w2 before w3 (phase word last), so once the
		 * poll bit matches the whole record is visible.  Read the LOCKED 32B
		 * record: w0=cb_fn, w1=cb_arg, w2=cpl_lo, w3=cpl_hi (cid at [47:32]).
		 * P2.2: w1 is read only by the tracker branch below; a tracker-free
		 * record carries the request in w0 and its callback takes req->cb_arg. */
		uint64_t rec_cb_fn  = rec->cb_fn;
		uint64_t cpl_lo     = rec->cpl_lo;
		uint16_t cid        = (uint16_t)((cpl_hi >> 32) & 0xFFFFu);

		/* Advance the record-ring cursor and toggle the expected poll phase
		 * on wrap.  Never touch cq_head / flags.phase and never ring the
		 * CQ-head doorbell — the device owns CQ-head advancement. */
		if (spdk_unlikely(++pqpair->cq_rec_head == pqpair->num_entries)) {
			pqpair->cq_rec_head = 0;
			pqpair->cq_rec_phase = !pqpair->cq_rec_phase;
		}

		if (cid < md_fast) {
			/* F3 tracker-free completion: the device echoed the request pointer
			 * (descriptor w4) in w0.  md_inflight[cid] must hold the same request
			 * (the guard mirrors the tracker branch's tr->req check: a stale or
			 * duplicate record is reported, never completed).  The slot is cleared,
			 * the cid returned and queue_depth dropped BEFORE the callback, so a
			 * resubmit from inside cb_fn can reuse this cid. */
			struct nvme_request *req = (struct nvme_request *)(uintptr_t)rec_cb_fn;

			if (spdk_likely(req != NULL && pqpair->md_inflight[cid] == req)) {
				struct spdk_nvme_cpl cpl_rec;

				memcpy((uint8_t *)&cpl_rec + 0, &cpl_lo, 8);
				memcpy((uint8_t *)&cpl_rec + 8, &cpl_hi, 8);
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cqe_seen = cqe_seen;
				req->t_completion = cqe_seen;
				req->t_cqe_valid_end = nvme_io_cycle_rdtsc();
				req->t_tr_lookup_end = req->t_cqe_valid_end;	/* no tr[cid] lookup */
				if (req->t_doorbell != 0) {
					uint64_t poll_elapsed = nvme_io_cycle_rdtsc() - poll_start;
					req->t_poll_cycles = pqpair->poll_cycle_accum +
							     poll_elapsed - req->t_poll_start;
				}
#endif
				pqpair->md_inflight[cid] = NULL;
				assert(pqpair->md_cid_top < md_fast);
				pqpair->md_cid_stack[pqpair->md_cid_top++] = cid;
				pqpair->qpair.queue_depth--;
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_dealloc_pre_end = nvme_io_cycle_rdtsc();
				req->t_end = req->t_dealloc_pre_end;
#endif
				nvme_io_cycle_record(req);
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cb_start = nvme_io_cycle_rdtsc();
#endif
				req->cb_fn(req->cb_arg, &cpl_rec);
				nvme_free_request(req);
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cb_end = nvme_io_cycle_rdtsc();
#endif
			} else {
				NVME_QPAIR_ERRLOG(qpair, "mode-D trfree record cid %u does not map "
						  "to outstanding cmd\n", (unsigned)cid);
				assert(0);
			}

			if (++num_completions == max_completions) {
				break;
			}
			continue;
		}

		/* P2.1: every record while a completion-side error injection is armed
		 * (md_fast == 0, set per poll above): the checked path, which delivers
		 * through nvme_complete_request() like the stock completion. */
		if (spdk_unlikely(md_fast == 0)) {
			nvme_pcie_md_complete_checked(qpair, rec_cb_fn, cid, cpl_lo, cpl_hi);
			if (++num_completions == max_completions) {
				break;
			}
			continue;
		}

		/* P2.2: the echoed cb_arg (w1), read only on the tracker path.  The record
		 * slot cannot be rewritten yet: this cid is still outstanding and the ring
		 * has more entries than cids. */
		uint64_t rec_cb_arg = rec->cb_arg;

		/* Recover the tracker by cid — standard SPDK indexing (same as the
		 * normal path's &pqpair->tr[cpl->cid]); needed for the recycle. */
		tr = &pqpair->tr[cid];

		if (spdk_likely(tr->req != NULL)) {
			struct nvme_request *req = tr->req;
			struct spdk_nvme_cpl cpl_rec;

			/* Reconstruct the spdk_nvme_cpl from the device-decoded record
			 * (an NVMe CQE is byte-identical to spdk_nvme_cpl).  w3[48] holds
			 * the ring phase, harmless to the app callback (it reads sc/sct). */
			memcpy((uint8_t *)&cpl_rec + 0, &cpl_lo, 8);
			memcpy((uint8_t *)&cpl_rec + 8, &cpl_hi, 8);

			/* Instrumentation: cqe_detect = the cacheable record read (NO host
			 * status decode — offloaded); tracker_lookup = the tr[cid] index;
			 * state_dealloc = the lean recycle below; app_callback = the
			 * echoed callback + req free (existing columns, apples-to-apples). */
#ifndef SPDK_NVME_IO_CYCLE_OFF
			req->t_cqe_seen = cqe_seen;
			req->t_completion = cqe_seen;
			req->t_cqe_valid_end = nvme_io_cycle_rdtsc();
			req->t_tr_lookup_end = nvme_io_cycle_rdtsc();
			if (req->t_doorbell != 0) {
				uint64_t poll_elapsed = nvme_io_cycle_rdtsc() - poll_start;
				req->t_poll_cycles = pqpair->poll_cycle_accum +
					poll_elapsed - req->t_poll_start;
			}
#endif


			/* Lean recycle (v4.1): remove from outstanding + dec queue depth.
			 * complete_tracker's status decode, retry/error branches, and cpl
			 * synthesis are all offloaded/elided (device pre-decoded cpl). */
			TAILQ_REMOVE(&pqpair->outstanding_tr, tr, tq_list);
			pqpair->qpair.queue_depth--;
#ifndef SPDK_NVME_IO_CYCLE_OFF
			req->t_dealloc_pre_end = nvme_io_cycle_rdtsc();
			req->t_end = req->t_dealloc_pre_end;
#endif
			nvme_io_cycle_record(req);

			/* Fire the ECHOED application callback directly (the only real
			 * host work) then return the request + tracker to their pools.
			 * req memory persists after free until reused, so t_cb_end below
			 * is safe (read later by nvme_io_cycle_record). */
#ifndef SPDK_NVME_IO_CYCLE_OFF
			req->t_cb_start = nvme_io_cycle_rdtsc();
#endif
			((spdk_nvme_cmd_cb)(uintptr_t)rec_cb_fn)(
				(void *)(uintptr_t)rec_cb_arg, &cpl_rec);
			nvme_free_request(req);
#ifndef SPDK_NVME_IO_CYCLE_OFF
			req->t_cb_end = nvme_io_cycle_rdtsc();
#endif

			tr->req = NULL;
			TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
		} else {
			NVME_QPAIR_ERRLOG(qpair,
				"mode-D cq record cid does not map to outstanding cmd\n");
			assert(0);
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		pqpair->stat->completions += num_completions;
		/* Mode D: the device owns CQ-head advancement, no CQ-head doorbell. */
	} else {
		pqpair->stat->idle_polls++;
	}

#ifndef SPDK_NVME_IO_CYCLE_OFF
	{
		uint32_t hist_idx = num_completions;

		if (hist_idx >= 32) {
			hist_idx = 32;
		}
		pqpair->stat->completion_calls_hist[hist_idx]++;
	}
#endif

	if (pqpair->flags.delay_cmd_submit &&
	    pqpair->last_sq_desc_tail != pqpair->sq_desc_tail) {
		nvme_pcie_qpair_ring_sq_doorbell(qpair);
		nvme_pcie_qpair_mark_doorbell_time(pqpair, nvme_io_cycle_rdtsc());
		pqpair->last_sq_desc_tail = pqpair->sq_desc_tail;
	}

	if (qpair->ctrlr->timeout_enabled) {
		nvme_pcie_md_check_timeout(qpair);
	}

#ifndef SPDK_NVME_IO_CYCLE_OFF
	pqpair->poll_cycle_accum += (nvme_io_cycle_rdtsc() - poll_start);
#endif

	if (spdk_unlikely(pqpair->flags.has_pending_vtophys_failures)) {
		struct nvme_tracker *tmp;

		TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
			if (tr->bad_vtophys) {
				tr->bad_vtophys = 0;
				nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			}
		}
		pqpair->flags.has_pending_vtophys_failures = 0;
	}

	return (int32_t)num_completions;
}

/*
 * KEEP IN STEP: nvme_pcie_md_poll() (above) copies the head and the tail of this function for a
 * tracker-free Mode D IO qpair.  A change to either must be mirrored there.  The allowed deltas of
 * the copy are:
 * - no admin lock and no admin pending-request / disconnect block (tracker-free qpairs are IO
 *   qpairs);
 * - no mode_e / mode_d / mode_c recompute (flags.md_trfree implies Mode D) and no CQ-head doorbell;
 * - no sq_tail doorbell batching branch (Mode D batches on sq_desc_tail only);
 * - no SPDK_NVME_DBG_MECH4 counters, and the hint early exit without the qid != 0 term;
 * - timeouts through nvme_pcie_md_check_timeout() (trackers + fast cids) instead of
 *   nvme_pcie_qpair_check_timeout().
 * - P2.1: the per-poll error-injection test before the record loop, and records with an error
 *   status (or all records while an injection is armed) through nvme_pcie_md_complete_checked();
 *   the stock body's own Mode C / D / E loops keep their pre-P2 behaviour.
 */
/* The stock poll: the pre-F3 body of nvme_pcie_qpair_process_completions(), verbatim.
 * NVME_PCIE_STOCK_BODY: with GCC >= 8 compiled exactly like the former global function, so its
 * instructions are those of the pre-F3 build. */
static int32_t NVME_PCIE_STOCK_BODY NVME_PCIE_HOT_ALIGN
nvme_pcie_qpair_process_completions_stock(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	struct spdk_nvme_cpl	*cpl, *next_cpl;
	uint32_t		 num_completions = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	uint16_t		 next_cq_head;
	uint8_t			 next_phase;
	bool			 next_is_valid = false;
	int			 rc;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	uint64_t		 poll_start;
#endif

	if (spdk_unlikely(pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED)) {
		return -ENXIO;
	}

	/* IO-Uncore Mode 2B (poll-lite) early exit. When the on-die uncore
	 * publishes hint=0, no I/O CQEs are queued in the lCQFIFO, so the
	 * DRAM CQ phase-bit scan would only churn cachelines for nothing.
	 * Skip on I/O qpairs (qid != 0); admin queue always polls to avoid
	 * starving controller commands. Dormant when uncore_hint_reg is NULL.
	 *
	 * Must also be gated on NVME_QPAIR_CONNECTED: during the connect
	 * handshake the qpair is in NVME_QPAIR_CONNECTING and the connect
	 * completion arrives on the adminq, which is processed by the
	 * CONNECTING branch below (lines following).  Returning 0 here while
	 * still connecting causes spdk_nvme_poll_group_all_connected() to
	 * never observe the qpair as connected, and init_ns_worker_ctx()
	 * times out after 10 s with "ERROR: init_ns_worker_ctx() failed". */
	{
		struct nvme_pcie_ctrlr *pctrlr =
			nvme_pcie_ctrlr(qpair->ctrlr);
#ifdef SPDK_NVME_DBG_MECH4
		static uint64_t _dbg_proc_calls = 0;
		static uint64_t _dbg_hint_reg_null = 0;
		static uint64_t _dbg_hint_qid0 = 0;
		static uint64_t _dbg_hint_state = 0;
		static uint64_t _dbg_hint_entered = 0;
		_dbg_proc_calls++;
#endif
		enum nvme_qpair_state _st_now = nvme_qpair_get_state(qpair);
		bool _live = (_st_now == NVME_QPAIR_CONNECTED) ||
			      (_st_now == NVME_QPAIR_ENABLED);
#ifdef SPDK_NVME_DBG_MECH4
		if (pctrlr->uncore_hint_reg == NULL) {
			_dbg_hint_reg_null++;
		} else if (qpair->id == 0) {
			_dbg_hint_qid0++;
		} else if (!_live) {
			_dbg_hint_state++;
		} else {
			_dbg_hint_entered++;
		}
		if ((_dbg_proc_calls & 0xFFFFF) == 1) {
			fprintf(stderr,
				"[DBG_MECH4] calls=%lu reg_null=%lu qid0=%lu state=%lu entered=%lu "
				"qid=%u state_now=%d hint_reg=%p\n",
				_dbg_proc_calls, _dbg_hint_reg_null,
				_dbg_hint_qid0, _dbg_hint_state,
				_dbg_hint_entered, qpair->id,
				(int)_st_now,
				(void *)pctrlr->uncore_hint_reg);
		}
#endif
		if (spdk_likely(pctrlr->uncore_hint_reg != NULL) &&
		    spdk_likely(qpair->id != 0) &&
		    spdk_likely(_live)) {
			uint32_t hint = *pctrlr->uncore_hint_reg;
			uint16_t count = (uint16_t)(hint & 0xFFFFu);
			uint16_t age   = (uint16_t)(hint >> 16);
			/* Mechanism #4: skip the CQ scan only when BOTH there is no
			 * work AND the oldest pending CQE is fresh enough that we'd
			 * rather batch.  AGE_THRESHOLD = 4 (= 4 µs @ default 1 µs/
			 * unit granularity) keeps worst-case completion latency
			 * bounded.  When age >= threshold, fall through to scan. */
			if (count == 0 && age < 4) {
				return 0;
			}
		}
	}

#ifndef SPDK_NVME_IO_CYCLE_OFF
	poll_start = nvme_io_cycle_rdtsc();
#endif

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		if (pqpair->pcie_state == NVME_PCIE_QPAIR_READY) {
			/* It is possible that another thread set the pcie_state to
			 * QPAIR_READY, if it polled the adminq and processed the SQ
			 * completion for this qpair.  So check for that condition
			 * here and then update the qpair's state to CONNECTED, since
			 * we can only set the qpair state from the qpair's thread.
			 * (Note: this fixed issue #2157.)
			 */
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
			return -ENXIO;
		} else {
			rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
			if (rc < 0) {
				return rc;
			} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
				nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
				return -ENXIO;
			}
		}
		return 0;
	}

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_ctrlr_lock(ctrlr);
	}

	if (max_completions == 0 || max_completions > pqpair->max_completions_cap) {
		/*
		 * max_completions == 0 means unlimited, but complete at most
		 * max_completions_cap batch of I/O at a time so that the completion
		 * queue doorbells don't wrap around.
		 */
		max_completions = pqpair->max_completions_cap;
	}

	pqpair->stat->polls++;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	pqpair->stat->completion_calls++;
#endif

	/* IO-Uncore v3 (Mode C) completion path.  When the on-die uncore publishes
	 * 16-byte completion records into a host-DRAM ring (cq_rec_ring), the host
	 * polls that cacheable ring INSTEAD of scanning the NVMe CQ.  Each record
	 * carries the host tracker pointer (cookie, echoed from the SQ descriptor
	 * w2), so there is no cpl->cid -> pqpair->tr[cid] lookup, and the host never
	 * rings the CQ-head doorbell (the device owns CQ-head advancement).  CID is
	 * host-managed (Decision (a)): under Mode C uncore_free_cid_base is unmapped,
	 * so complete_tracker's hw_managed gate is false and it does the standard
	 * TAILQ free-list return + app callback.  Reads NO hint register (that reg
	 * is NULL under Mode C, so the early-exit above took the reg_null path).
	 * Gated per qpair on cq_rec_ring != NULL (I/O qpairs only; admin never has
	 * a record ring).  Modes 0/1/2 and Mode B/v2 fall through to the normal scan
	 * below unchanged. */
	/* Mode D (v4.1) completion-callback ring: the device DMA-wrote a 32B record
	 * carrying the pre-decoded cpl + the host callback, so the host loop does
	 * only cb_fn(cb_arg,&cpl) + a lean tracker/req recycle — no NVMe status
	 * decode, no complete_tracker body.  Mode D sets both env vars, so exclude it
	 * from mode_c below. */
	/* Mode E (v4.2) uses the same 32B completion record as Mode D but drops even
	 * the lean recycle (the device owns the tracker-slot lifecycle via the
	 * free-slot ring).  Mode E sets all three env vars, so test it FIRST and
	 * exclude it from mode_d / mode_c. */
	bool mode_e = (uncore_mode_e() && pqpair->cq_rec_ring != NULL && qpair->id != 0);
	bool mode_d = (!mode_e && uncore_mode_d() && pqpair->cq_rec_ring != NULL && qpair->id != 0);
	bool mode_c = (!mode_d && !mode_e && uncore_mode_c() && pqpair->cq_rec_ring != NULL && qpair->id != 0);

	if (mode_e) {
		/* Mode E completion loop: read the 32B callback record, fire the ECHOED
		 * cb_fn(cb_arg,&cpl) directly, free the request — and do NOTHING to the
		 * tracker free list (the device recycles the slot into the free-slot ring
		 * that the submit path pops).  tr[cid] is recovered only to free its
		 * request; tr->req is NULLed BEFORE the callback so a callback that
		 * resubmits and re-pops this very slot cannot be clobbered afterwards. */
		volatile struct nvme_uncore_cq_cb_rec *ring_e =
			(volatile struct nvme_uncore_cq_cb_rec *)pqpair->cq_rec_ring;
		while (1) {
			volatile struct nvme_uncore_cq_cb_rec *rec = &ring_e[pqpair->cq_rec_head];
			uint64_t cpl_hi = rec->cpl_hi;
#ifndef SPDK_NVME_IO_CYCLE_OFF
			uint64_t cqe_seen = nvme_io_cycle_rdtsc();
#endif

			if (((uint8_t)((cpl_hi >> 48) & 0x1u)) != pqpair->cq_rec_phase) {
				break;
			}

			uint64_t rec_cb_fn  = rec->cb_fn;
			uint64_t rec_cb_arg = rec->cb_arg;
			uint64_t cpl_lo     = rec->cpl_lo;
			uint16_t cid        = (uint16_t)((cpl_hi >> 32) & 0xFFFFu);

			if (spdk_unlikely(++pqpair->cq_rec_head == pqpair->num_entries)) {
				pqpair->cq_rec_head = 0;
				pqpair->cq_rec_phase = !pqpair->cq_rec_phase;
			}

			/* Recover the request only to return it to the pool; the tracker slot
			 * itself is recycled by the device (free-slot ring), so there is NO
			 * TAILQ_REMOVE / TAILQ_INSERT / queue_depth here. */
			tr = &pqpair->tr[cid];

			if (spdk_likely(tr->req != NULL)) {
				struct nvme_request *req = tr->req;
				struct spdk_nvme_cpl cpl_rec;

				memcpy((uint8_t *)&cpl_rec + 0, &cpl_lo, 8);
				memcpy((uint8_t *)&cpl_rec + 8, &cpl_hi, 8);

#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cqe_seen = cqe_seen;
				req->t_completion = cqe_seen;
				req->t_cqe_valid_end = nvme_io_cycle_rdtsc();
				req->t_tr_lookup_end = nvme_io_cycle_rdtsc();
				if (req->t_doorbell != 0) {
					uint64_t poll_elapsed = nvme_io_cycle_rdtsc() - poll_start;
					req->t_poll_cycles = pqpair->poll_cycle_accum +
						poll_elapsed - req->t_poll_start;
				}
#endif


				/* state_dealloc window: Mode E does NO tracker recycle (device
				 * owns it), so this is only the NULLing of tr->req.  Done BEFORE
				 * the callback so a re-pop of this slot inside cb_fn stands. */
				tr->req = NULL;
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_dealloc_pre_end = nvme_io_cycle_rdtsc();
				req->t_end = req->t_dealloc_pre_end;
#endif
				nvme_io_cycle_record(req);

#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cb_start = nvme_io_cycle_rdtsc();
#endif
				((spdk_nvme_cmd_cb)(uintptr_t)rec_cb_fn)(
					(void *)(uintptr_t)rec_cb_arg, &cpl_rec);
				nvme_free_request(req);
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cb_end = nvme_io_cycle_rdtsc();
#endif
			} else {
				NVME_QPAIR_ERRLOG(qpair,
					"mode-E cq record cid does not map to outstanding cmd\n");
				assert(0);
			}

			if (++num_completions == max_completions) {
				break;
			}
		}
	}

	if (mode_d) {
		volatile struct nvme_uncore_cq_cb_rec *ring_d =
			(volatile struct nvme_uncore_cq_cb_rec *)pqpair->cq_rec_ring;
		while (1) {
			volatile struct nvme_uncore_cq_cb_rec *rec = &ring_d[pqpair->cq_rec_head];
			uint64_t cpl_hi = rec->cpl_hi;
#ifndef SPDK_NVME_IO_CYCLE_OFF
			uint64_t cqe_seen = nvme_io_cycle_rdtsc();
#endif

			/* Ring poll bit = cpl_hi[48] (LOCKED); toggles on wrap.  Cacheable
			 * read, NO MMIO.  Not yet flipped => no new completion. */
			if (((uint8_t)((cpl_hi >> 48) & 0x1u)) != pqpair->cq_rec_phase) {
				break;
			}

			/* Device published w0..w2 before w3 (phase word last), so once the
			 * poll bit matches the whole record is visible.  Read the LOCKED 32B
			 * record: w0=cb_fn, w1=cb_arg, w2=cpl_lo, w3=cpl_hi (cid at [47:32]). */
			uint64_t rec_cb_fn  = rec->cb_fn;
			uint64_t rec_cb_arg = rec->cb_arg;
			uint64_t cpl_lo     = rec->cpl_lo;
			uint16_t cid        = (uint16_t)((cpl_hi >> 32) & 0xFFFFu);

			/* Advance the record-ring cursor and toggle the expected poll phase
			 * on wrap.  Never touch cq_head / flags.phase and never ring the
			 * CQ-head doorbell — the device owns CQ-head advancement. */
			if (spdk_unlikely(++pqpair->cq_rec_head == pqpair->num_entries)) {
				pqpair->cq_rec_head = 0;
				pqpair->cq_rec_phase = !pqpair->cq_rec_phase;
			}

			/* Recover the tracker by cid — standard SPDK indexing (same as the
			 * normal path's &pqpair->tr[cpl->cid]); needed for the recycle. */
			tr = &pqpair->tr[cid];

			if (spdk_likely(tr->req != NULL)) {
				struct nvme_request *req = tr->req;
				struct spdk_nvme_cpl cpl_rec;

				/* Reconstruct the spdk_nvme_cpl from the device-decoded record
				 * (an NVMe CQE is byte-identical to spdk_nvme_cpl).  w3[48] holds
				 * the ring phase, harmless to the app callback (it reads sc/sct). */
				memcpy((uint8_t *)&cpl_rec + 0, &cpl_lo, 8);
				memcpy((uint8_t *)&cpl_rec + 8, &cpl_hi, 8);

				/* Instrumentation: cqe_detect = the cacheable record read (NO host
				 * status decode — offloaded); tracker_lookup = the tr[cid] index;
				 * state_dealloc = the lean recycle below; app_callback = the
				 * echoed callback + req free (existing columns, apples-to-apples). */
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cqe_seen = cqe_seen;
				req->t_completion = cqe_seen;
				req->t_cqe_valid_end = nvme_io_cycle_rdtsc();
				req->t_tr_lookup_end = nvme_io_cycle_rdtsc();
				if (req->t_doorbell != 0) {
					uint64_t poll_elapsed = nvme_io_cycle_rdtsc() - poll_start;
					req->t_poll_cycles = pqpair->poll_cycle_accum +
						poll_elapsed - req->t_poll_start;
				}
#endif


				/* Lean recycle (v4.1): remove from outstanding + dec queue depth.
				 * complete_tracker's status decode, retry/error branches, and cpl
				 * synthesis are all offloaded/elided (device pre-decoded cpl). */
				TAILQ_REMOVE(&pqpair->outstanding_tr, tr, tq_list);
				pqpair->qpair.queue_depth--;
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_dealloc_pre_end = nvme_io_cycle_rdtsc();
				req->t_end = req->t_dealloc_pre_end;
#endif
				nvme_io_cycle_record(req);

				/* Fire the ECHOED application callback directly (the only real
				 * host work) then return the request + tracker to their pools.
				 * req memory persists after free until reused, so t_cb_end below
				 * is safe (read later by nvme_io_cycle_record). */
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cb_start = nvme_io_cycle_rdtsc();
#endif
				((spdk_nvme_cmd_cb)(uintptr_t)rec_cb_fn)(
					(void *)(uintptr_t)rec_cb_arg, &cpl_rec);
				nvme_free_request(req);
#ifndef SPDK_NVME_IO_CYCLE_OFF
				req->t_cb_end = nvme_io_cycle_rdtsc();
#endif

				tr->req = NULL;
				TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
			} else {
				NVME_QPAIR_ERRLOG(qpair,
					"mode-D cq record cid does not map to outstanding cmd\n");
				assert(0);
			}

			if (++num_completions == max_completions) {
				break;
			}
		}
	}

	if (mode_c) {
		while (1) {
			volatile struct nvme_uncore_cq_rec *rec =
				&pqpair->cq_rec_ring[pqpair->cq_rec_head];
			uint64_t w1 = rec->w1;
#ifndef SPDK_NVME_IO_CYCLE_OFF
			uint64_t cqe_seen = nvme_io_cycle_rdtsc();
#endif

			/* Poll bit (locked record layout w1[48]) gates validity; it toggles
			 * on every ring wrap and matches pqpair->cq_rec_phase when ready.
			 * Cacheable read, NO MMIO. */
			if (((uint8_t)((w1 >> 48) & 0x1u)) != pqpair->cq_rec_phase) {
				break;
			}

			/* Device publishes w0 (cookie) BEFORE w1 (phase-bit last), so once
			 * the poll bit matches, w0 is visible.  Decode the locked 16B record:
			 * w1[15:0]=status (native phase bit forced 0 by producer),
			 * w1[47:32]=cid, w0=cookie=host tracker pointer. */
			uint64_t cookie = rec->w0;
			uint16_t status = (uint16_t)(w1 & 0xFFFFu);
			uint16_t cid    = (uint16_t)((w1 >> 32) & 0xFFFFu);

			/* Cookie IS the host tracker pointer — DIRECT cast, no tr[cid] map. */
			tr = (struct nvme_tracker *)(uintptr_t)cookie;

			/* Advance the record-ring cursor and toggle the expected poll phase
			 * on wrap.  Never touch cq_head / flags.phase and never ring the
			 * CQ-head doorbell — the device owns CQ-head advancement. */
			if (spdk_unlikely(++pqpair->cq_rec_head == pqpair->num_entries)) {
				pqpair->cq_rec_head = 0;
				pqpair->cq_rec_phase = !pqpair->cq_rec_phase;
			}

			if (spdk_likely(tr != NULL && tr->req != NULL)) {
				struct spdk_nvme_cpl cpl_rec;

				/* Synthesize a local cpl from the record for complete_tracker. */
				memset(&cpl_rec, 0, sizeof(cpl_rec));
				cpl_rec.cid = cid;
				cpl_rec.sqid = qpair->id;
				cpl_rec.status_raw = status;

				/* Completion instrumentation: cqe_detect ~ the cacheable poll-bit
				 * read; tracker_lookup = 0 (cookie is the tr, no lookup);
				 * state_dealloc is stamped inside complete_tracker (t_dealloc_pre_end,
				 * which also sets t_end so nvme_io_cycle_record's gate passes). */
#ifndef SPDK_NVME_IO_CYCLE_OFF
				tr->req->t_cqe_seen = cqe_seen;
				tr->req->t_completion = cqe_seen;
				tr->req->t_cqe_valid_end = nvme_io_cycle_rdtsc();
				tr->req->t_tr_lookup_end = tr->req->t_cqe_valid_end;
				if (tr->req->t_doorbell != 0) {
					uint64_t poll_elapsed = nvme_io_cycle_rdtsc() - poll_start;
					tr->req->t_poll_cycles = pqpair->poll_cycle_accum +
						poll_elapsed - tr->req->t_poll_start;
				}
#endif

				__builtin_prefetch(&tr->req->stailq);
				/* Reuse complete_tracker unchanged: hw_managed==false gives the
				 * cheap TAILQ free-list return + app callback + t_end stamp, and
				 * it never touches cq_head/phase (that is scan-only). */
				nvme_pcie_qpair_complete_tracker(qpair, tr, &cpl_rec, true);
			} else {
				NVME_QPAIR_ERRLOG(qpair,
					"mode-C cq record cookie does not map to outstanding cmd\n");
				assert(0);
			}

			if (++num_completions == max_completions) {
				break;
			}
		}
	}

	while (!mode_c && !mode_d && !mode_e) {
		cpl = &pqpair->cpl[pqpair->cq_head];

		if (!next_is_valid && cpl->status.p != pqpair->flags.phase) {
			break;
		}

		if (spdk_likely(pqpair->cq_head + 1 != pqpair->num_entries)) {
			next_cq_head = pqpair->cq_head + 1;
			next_phase = pqpair->flags.phase;
		} else {
			next_cq_head = 0;
			next_phase = !pqpair->flags.phase;
		}
		next_cpl = &pqpair->cpl[next_cq_head];
		next_is_valid = (next_cpl->status.p == next_phase);
		if (next_is_valid) {
			__builtin_prefetch(&pqpair->tr[next_cpl->cid]);
		}

#if defined(__PPC64__) || defined(__riscv) || defined(__loongarch__)
		/*
		 * This memory barrier prevents reordering of:
		 * - load after store from/to tr
		 * - load after load cpl phase and cpl cid
		 */
		spdk_mb();
#elif defined(__aarch64__)
		__asm volatile("dmb oshld" ::: "memory");
#endif

		if (spdk_unlikely(++pqpair->cq_head == pqpair->num_entries)) {
			pqpair->cq_head = 0;
			pqpair->flags.phase = !pqpair->flags.phase;
		}

#ifndef SPDK_NVME_IO_CYCLE_OFF
		uint64_t cqe_seen = nvme_io_cycle_rdtsc();
#endif
		tr = &pqpair->tr[cpl->cid];
		pqpair->sq_head = cpl->sqhd;
#ifndef SPDK_NVME_IO_CYCLE_OFF
		if (tr->req) {
			tr->req->t_cqe_seen = cqe_seen;
			tr->req->t_completion = cqe_seen;
			tr->req->t_cqe_valid_end = nvme_io_cycle_rdtsc();
			tr->req->t_tr_lookup_end = nvme_io_cycle_rdtsc();
			if (tr->req->t_doorbell != 0) {
				uint64_t poll_elapsed = nvme_io_cycle_rdtsc() - poll_start;
				tr->req->t_poll_cycles = pqpair->poll_cycle_accum + poll_elapsed - tr->req->t_poll_start;
			}
		}
#endif

		if (tr->req) {
			/* Prefetch the req's STAILQ_ENTRY since we'll need to access it
			 * as part of putting the req back on the qpair's free list.
			 */
			__builtin_prefetch(&tr->req->stailq);
			nvme_pcie_qpair_complete_tracker(qpair, tr, cpl, true);
		} else {
			NVME_QPAIR_ERRLOG(qpair, "cpl does not map to outstanding cmd\n");
			spdk_nvme_qpair_print_completion(qpair, cpl);
			assert(0);
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		pqpair->stat->completions += num_completions;
		/* Mode C/D/E: the device owns CQ-head advancement (host consumed the
		 * record ring, not the NVMe CQ), so do NOT ring the CQ-head doorbell. */
		if (!mode_c && !mode_d && !mode_e) {
			nvme_pcie_qpair_ring_cq_doorbell(qpair);
		}
	} else {
		pqpair->stat->idle_polls++;
	}

#ifndef SPDK_NVME_IO_CYCLE_OFF
	/* Track histogram of completions per call (last bucket is 32+) */
	{
		uint32_t hist_idx = num_completions;
		if (hist_idx >= 32) {
			hist_idx = 32;
		}
		pqpair->stat->completion_calls_hist[hist_idx]++;
	}
#endif

	if (pqpair->flags.delay_cmd_submit) {
		if (pqpair->sq_desc_ring != NULL) {
			/* IO-Uncore v3 (Mode C): batch on the descriptor-ring cursor.
			 * ring_sq_doorbell publishes sq_desc_tail (not sq_tail).  Per-IO
			 * t_doorbell is already stamped at submit, so mark_doorbell_time
			 * is a no-op here — the batched doorbell is not attributed per IO. */
			if (pqpair->last_sq_desc_tail != pqpair->sq_desc_tail) {
				nvme_pcie_qpair_ring_sq_doorbell(qpair);
				nvme_pcie_qpair_mark_doorbell_time(pqpair, nvme_io_cycle_rdtsc());
				pqpair->last_sq_desc_tail = pqpair->sq_desc_tail;
			}
		} else if (pqpair->last_sq_tail != pqpair->sq_tail) {
			nvme_pcie_qpair_ring_sq_doorbell(qpair);
			nvme_pcie_qpair_mark_doorbell_time(pqpair, nvme_io_cycle_rdtsc());
			pqpair->last_sq_tail = pqpair->sq_tail;
		}
	}

	if (ctrlr->timeout_enabled) {
		/*
		 * User registered for timeout callback
		 */
		nvme_pcie_qpair_check_timeout(qpair);
	}

#ifndef SPDK_NVME_IO_CYCLE_OFF
	pqpair->poll_cycle_accum += (nvme_io_cycle_rdtsc() - poll_start);
#endif

	/* Before returning, complete any pending admin request or
	 * process the admin qpair disconnection.
	 */
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_pcie_qpair_complete_pending_admin_request(qpair);

		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
			rc = nvme_ctrlr_disable_poll(qpair->ctrlr);
			if (rc != -EAGAIN) {
				nvme_transport_ctrlr_disconnect_qpair_done(qpair);
			}
		}

		nvme_ctrlr_unlock(ctrlr);
	}

	if (spdk_unlikely(pqpair->flags.has_pending_vtophys_failures)) {
		struct nvme_tracker *tr, *tmp;

		TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
			if (tr->bad_vtophys) {
				tr->bad_vtophys = 0;
				nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			}
		}
		pqpair->flags.has_pending_vtophys_failures = 0;
	}

	return num_completions;
}

int32_t NVME_PCIE_HOT_ALIGN
nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	/* F3: one test; a tracker-free Mode D qpair polls out of line. */
	if (spdk_unlikely(nvme_pcie_qpair(qpair)->flags.md_trfree)) {
		return nvme_pcie_md_poll(qpair, max_completions);
	}
	return nvme_pcie_qpair_process_completions_stock(qpair, max_completions);
}

int
nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_pcie_admin_qpair_destroy(qpair);
	}
	/*
	 * We check sq_vaddr and cq_vaddr to see if the user specified the memory
	 * buffers when creating the I/O queue.
	 * If the user specified them, we cannot free that memory.
	 * Nor do we free it if it's in the CMB.
	 */
	if (!pqpair->sq_vaddr && pqpair->cmd && !pqpair->sq_in_cmb) {
		spdk_free(pqpair->cmd);
	}
	if (!pqpair->cq_vaddr && pqpair->cpl) {
		spdk_free(pqpair->cpl);
	}
	if (pqpair->tr) {
		spdk_free(pqpair->tr);
	}
	if (pqpair->prp_pool) {
		spdk_free(pqpair->prp_pool);
	}
	/* IO-Uncore rings (Mode C/D descriptor + record rings, Mode E free-slot ring) are
	 * spdk_zmalloc'ed in nvme_pcie_qpair_construct(); they were never freed before F3. */
	if (pqpair->sq_desc_ring) {
		spdk_free(pqpair->sq_desc_ring);
		pqpair->sq_desc_ring = NULL;
	}
	if (pqpair->cq_rec_ring) {
		spdk_free(pqpair->cq_rec_ring);
		pqpair->cq_rec_ring = NULL;
	}
	if (pqpair->free_slot_ring) {
		spdk_free(pqpair->free_slot_ring);
		pqpair->free_slot_ring = NULL;
	}
	/* F3: md_inflight owns the cid stack too (one allocation). */
	if (pqpair->md_inflight) {
		spdk_free(pqpair->md_inflight);
		pqpair->md_inflight = NULL;
		pqpair->md_cid_stack = NULL;
	}
	pqpair->flags.md_trfree = 0;
	pqpair->md_fast_cids = 0;
	pqpair->md_cid_top = 0;

	nvme_qpair_deinit(qpair);

	if (!pqpair->shared_stats && (!qpair->active_proc ||
				      qpair->active_proc == nvme_ctrlr_get_current_process(qpair->ctrlr))) {
		if (qpair->id) {
			free(pqpair->stat);
		} else {
			/* statistics of admin qpair are allocates from huge pages because
			 * admin qpair is shared for multi-process */
			spdk_free(pqpair->stat);
		}

	}

	spdk_free(pqpair);

	return 0;
}

struct spdk_nvme_qpair *
nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	struct nvme_pcie_qpair *pqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	assert(ctrlr != NULL);

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	if (pqpair == NULL) {
		return NULL;
	}

	pqpair->num_entries = opts->io_queue_size;
	pqpair->flags.delay_cmd_submit = opts->delay_cmd_submit;

	qpair = &pqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, opts->qprio, opts->io_queue_requests, opts->async_mode);
	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	rc = nvme_pcie_qpair_construct(qpair, opts);

	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

int
nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_completion_poll_status *status;
	int rc;

	assert(ctrlr != NULL);

	if (ctrlr->is_removed) {
		goto free;
	}

	if (ctrlr->prepare_for_reset) {
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
			pqpair->flags.defer_destruction = true;
		}
		goto clear_shadow_doorbells;
	}

	/* If attempting to delete a qpair that's still being connected, we have to wait until it's
	 * finished, so that we don't free it while it's waiting for the create cq/sq callbacks.
	 */
	while (pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ ||
	       pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ) {
		rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		if (rc < 0) {
			break;
		}
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to allocate status tracker\n");
		goto free;
	}

	/* Delete the I/O submission queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_sq with rc=%d\n", rc);
		free(status);
		goto free;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, false);
	if (rc) {
		if (!status->timed_out) {
			free(status);
		}

		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_pcie_ctrlr_cmd_delete_io_sq failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		goto free;
	}

	/* Now that the submission queue is deleted, the device is supposed to have
	 * completed any outstanding I/O. Try to complete them. If they don't complete,
	 * they'll be marked as aborted and completed below. */
	if (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr)) {
		nvme_pcie_qpair_process_completions(qpair, 0);
	}

	memset(status, 0, sizeof(*status));
	/* Delete the completion queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_cq with rc=%d\n", rc);
		free(status);
		goto free;
	}

	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_pcie_ctrlr_cmd_delete_io_cq failed: rc=%s\n",
				  spdk_strerror(abs(rc)));
		goto free;
	}

clear_shadow_doorbells:
	if (pqpair->flags.has_shadow_doorbell && ctrlr->shadow_doorbell) {
		*pqpair->shadow_doorbell.sq_tdbl = 0;
		*pqpair->shadow_doorbell.cq_hdbl = 0;
		*pqpair->shadow_doorbell.sq_eventidx = 0;
		*pqpair->shadow_doorbell.cq_eventidx = 0;
	}
free:
	if (qpair->no_deletion_notification_needed == 0) {
		/* Abort the rest of the I/O */
		nvme_pcie_qpair_abort_trackers(qpair, 1);
	}

	if (!pqpair->flags.defer_destruction) {
		nvme_pcie_qpair_destroy(qpair);
	}
	return 0;
}

static void
nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	if (!qpair->in_completion_context) {
		struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

		tr->bad_vtophys = 1;
		pqpair->flags.has_pending_vtophys_failures = 1;
		return;
	}

	/*
	 * Bad vtophys translation, so abort this request and return
	 *  immediately.
	 */
	NVME_QPAIR_ERRLOG(qpair, "vtophys or other payload buffer related error\n");
	nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						SPDK_NVME_SC_INVALID_FIELD,
						1 /* do not retry */, true);
}

/*
 * Append PRP list entries to describe a virtually contiguous buffer starting at virt_addr of len bytes.
 *
 * *prp_index will be updated to account for the number of PRP entries used.
 */
static inline int
nvme_pcie_prp_list_append(struct spdk_nvme_ctrlr *ctrlr, struct nvme_tracker *tr,
			  uint32_t *prp_index, void *virt_addr, size_t len,
			  uint32_t page_size)
{
	struct spdk_nvme_cmd *cmd = &tr->req->cmd;
	uintptr_t page_mask = page_size - 1;
	uint64_t phys_addr;
	uint32_t i;

	NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp_index:%u virt_addr:%p len:%u\n", *prp_index, virt_addr,
			    (uint32_t)len);

	if (spdk_unlikely(((uintptr_t)virt_addr & 3) != 0)) {
		NVME_QPAIR_ERRLOG(tr->req->qpair, "virt_addr %p not dword aligned\n", virt_addr);
		return -EFAULT;
	}

	i = *prp_index;
	while (len) {
		uint32_t seg_len;

		/*
		 * prp_index 0 is stored in prp1, and the rest are stored in the prp[] array,
		 * so prp_index == count is valid.
		 */
		if (spdk_unlikely(i > SPDK_COUNTOF(tr->list_page->u.prp))) {
			NVME_QPAIR_ERRLOG(tr->req->qpair, "out of PRP entries\n");
			return -EFAULT;
		}

		phys_addr = nvme_pcie_vtophys(ctrlr, virt_addr, NULL);
		if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR)) {
			NVME_QPAIR_ERRLOG(tr->req->qpair, "vtophys(%p) failed\n", virt_addr);
			return -EFAULT;
		}

		if (i == 0) {
			NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp1 = %p\n", (void *)phys_addr);
			cmd->dptr.prp.prp1 = phys_addr;
			seg_len = page_size - ((uintptr_t)virt_addr & page_mask);
		} else {
			if ((phys_addr & page_mask) != 0) {
				NVME_QPAIR_ERRLOG(tr->req->qpair, "PRP %u not page aligned (%p)\n", i, virt_addr);
				return -EFAULT;
			}

			NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp[%u] = %p\n", i - 1, (void *)phys_addr);
			tr->list_page->u.prp[i - 1] = phys_addr;
			seg_len = page_size;
		}

		seg_len = spdk_min(seg_len, len);
		virt_addr = (uint8_t *)virt_addr + seg_len;
		len -= seg_len;
		i++;
	}

	cmd->psdt = SPDK_NVME_PSDT_PRP;
	if (i <= 1) {
		cmd->dptr.prp.prp2 = 0;
	} else if (i == 2) {
		cmd->dptr.prp.prp2 = tr->list_page->u.prp[0];
		NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp2 = %p\n", (void *)cmd->dptr.prp.prp2);
	} else {
		cmd->dptr.prp.prp2 = tr->prp_sgl_bus_addr;
		NVME_QPAIR_DEBUGLOG(tr->req->qpair, "prp2 = %p (PRP list)\n", (void *)cmd->dptr.prp.prp2);
	}

	*prp_index = i;
	return 0;
}

static int
nvme_pcie_qpair_build_request_invalid(struct spdk_nvme_qpair *qpair,
				      struct nvme_request *req, struct nvme_tracker *tr, bool dword_aligned)
{
	assert(0);
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EINVAL;
}

/**
 * Build PRP list describing physically contiguous payload buffer.
 */
static int
nvme_pcie_qpair_build_contig_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr, bool dword_aligned)
{
	uint32_t prp_index = 0;
	int rc;

	rc = nvme_pcie_prp_list_append(qpair->ctrlr, tr, &prp_index,
				       (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,
				       req->payload_size, qpair->ctrlr->page_size);
	if (rc) {
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	} else {
		NVME_QPAIR_DEBUGLOG(qpair, "Number of PRP entries: %" PRIu32 "\n", prp_index);
	}

	return rc;
}

/**
 * Build an SGL describing a physically contiguous payload buffer.
 *
 * This is more efficient than using PRP because large buffers can be
 * described this way.
 */
static int
nvme_pcie_qpair_build_contig_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
		struct nvme_tracker *tr, bool dword_aligned)
{
	uint8_t *virt_addr;
	uint64_t phys_addr, mapping_length;
	uint32_t length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	sgl = tr->list_page->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	length = req->payload_size;
	/* ubsan complains about applying zero offset to null pointer if contig_or_cb_arg is NULL,
	 * so just double cast it to make it go away */
	virt_addr = (uint8_t *)((uintptr_t)req->payload.contig_or_cb_arg + req->payload_offset);

	while (length > 0) {
		if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
			NVME_QPAIR_ERRLOG(qpair, "virt_addr %p not dword aligned\n", virt_addr);
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		mapping_length = length;
		phys_addr = nvme_pcie_vtophys(qpair->ctrlr, virt_addr, &mapping_length);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		mapping_length = spdk_min(length, mapping_length);

		if (nseg == 0 && mapping_length == length) {
			/*
			 * One mapping covers the whole payload: describe it in SGL1 directly
			 *  (the spec's Data Block special case, see nseg == 1 below) without
			 *  staging it in the tracker's list page.  The command bytes are
			 *  identical to the nseg == 1 copy; the list page (4 KiB-strided across
			 *  trackers) is simply not touched.
			 */
			req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			req->cmd.dptr.sgl1.address = phys_addr;
			req->cmd.dptr.sgl1.unkeyed.length = mapping_length;
			NVME_QPAIR_DEBUGLOG(qpair, "Number of SGL descriptors: 1\n");
			return 0;
		}

		length -= mapping_length;
		virt_addr += mapping_length;

		sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl->unkeyed.length = mapping_length;
		sgl->address = phys_addr;
		sgl->unkeyed.subtype = 0;

		sgl++;
		nseg++;
	}

	if (nseg == 1) {
		/*
		 * The whole transfer can be described by a single SGL descriptor.
		 *  Use the special case described by the spec where SGL1's type is Data Block.
		 *  This means the SGL in the tracker is not used at all, so copy the first (and only)
		 *  SGL element into SGL1.  (Unreachable for contig payloads since the early return
		 *  above; kept so the two SGL builders stay alike.)
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.address = tr->list_page->u.sgl[0].address;
		req->cmd.dptr.sgl1.unkeyed.length = tr->list_page->u.sgl[0].unkeyed.length;
	} else {
		/* SPDK NVMe driver supports only 1 SGL segment for now, it is enough because
		 *  NVME_MAX_SGL_DESCRIPTORS * 16 is less than one page.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	NVME_QPAIR_DEBUGLOG(qpair, "Number of SGL descriptors: %" PRIu32 "\n", nseg);
	return 0;
}

/**
 * Build SGL list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr, bool dword_aligned)
{
	int rc;
	void *virt_addr;
	uint64_t phys_addr, mapping_length;
	uint32_t remaining_transfer_len, remaining_user_sge_len, length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	sgl = tr->list_page->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	remaining_transfer_len = req->payload_size;

	while (remaining_transfer_len > 0) {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg,
					      &virt_addr, &remaining_user_sge_len);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		/* Bit Bucket SGL descriptor */
		if ((uint64_t)virt_addr == UINT64_MAX) {
			/* TODO: enable WRITE and COMPARE when necessary */
			if (req->cmd.opc != SPDK_NVME_OPC_READ) {
				NVME_QPAIR_ERRLOG(qpair, "Only READ command can be supported\n");
				goto exit;
			}
			if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
				NVME_QPAIR_ERRLOG(qpair, "Too many SGL entries\n");
				goto exit;
			}

			sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_BIT_BUCKET;
			/* If the SGL describes a destination data buffer, the length of data
			 * buffer shall be discarded by controller, and the length is included
			 * in Number of Logical Blocks (NLB) parameter. Otherwise, the length
			 * is not included in the NLB parameter.
			 */
			remaining_user_sge_len = spdk_min(remaining_user_sge_len, remaining_transfer_len);
			remaining_transfer_len -= remaining_user_sge_len;

			sgl->unkeyed.length = remaining_user_sge_len;
			sgl->address = 0;
			sgl->unkeyed.subtype = 0;

			sgl++;
			nseg++;

			continue;
		}

		remaining_user_sge_len = spdk_min(remaining_user_sge_len, remaining_transfer_len);
		remaining_transfer_len -= remaining_user_sge_len;
		while (remaining_user_sge_len > 0) {
			if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
				NVME_QPAIR_ERRLOG(qpair, "Too many SGL entries\n");
				goto exit;
			}

			if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
				NVME_QPAIR_ERRLOG(qpair, "virt_addr %p not dword aligned\n", virt_addr);
				goto exit;
			}

			mapping_length = remaining_user_sge_len;
			phys_addr = nvme_pcie_vtophys(qpair->ctrlr, virt_addr, &mapping_length);
			if (phys_addr == SPDK_VTOPHYS_ERROR) {
				goto exit;
			}

			length = spdk_min(remaining_user_sge_len, mapping_length);
			remaining_user_sge_len -= length;
			virt_addr = (uint8_t *)virt_addr + length;

			if (!pqpair->flags.disable_pcie_sgl_merge && nseg > 0 &&
			    phys_addr == (*(sgl - 1)).address + (*(sgl - 1)).unkeyed.length) {
				/* extend previous entry */
				(*(sgl - 1)).unkeyed.length += length;
				continue;
			}

			sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			sgl->unkeyed.length = length;
			sgl->address = phys_addr;
			sgl->unkeyed.subtype = 0;

			sgl++;
			nseg++;
		}
	}

	if (nseg == 1) {
		/*
		 * The whole transfer can be described by a single SGL descriptor.
		 *  Use the special case described by the spec where SGL1's type is Data Block.
		 *  This means the SGL in the tracker is not used at all, so copy the first (and only)
		 *  SGL element into SGL1.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.address = tr->list_page->u.sgl[0].address;
		req->cmd.dptr.sgl1.unkeyed.length = tr->list_page->u.sgl[0].unkeyed.length;
	} else {
		/* SPDK NVMe driver supports only 1 SGL segment for now, it is enough because
		 *  NVME_MAX_SGL_DESCRIPTORS * 16 is less than one page.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	NVME_QPAIR_DEBUGLOG(qpair, "Number of SGL descriptors: %" PRIu32 "\n", nseg);
	return 0;

exit:
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EFAULT;
}

/**
 * Build PRP list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_prps_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				       struct nvme_tracker *tr, bool dword_aligned)
{
	int rc;
	void *virt_addr;
	uint32_t remaining_transfer_len, length;
	uint32_t prp_index = 0;
	uint32_t page_size = qpair->ctrlr->page_size;

	/*
	 * Build scattered payloads.
	 */
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	remaining_transfer_len = req->payload_size;
	while (remaining_transfer_len > 0) {
		assert(req->payload.next_sge_fn != NULL);
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		length = spdk_min(remaining_transfer_len, length);

		/*
		 * Any incompatible sges should have been handled up in the splitting routine,
		 *  but assert here as an additional check.
		 *
		 * All SGEs except last must end on a page boundary.
		 */
		assert((length == remaining_transfer_len) ||
		       _is_page_aligned((uintptr_t)virt_addr + length, page_size));

		rc = nvme_pcie_prp_list_append(qpair->ctrlr, tr, &prp_index, virt_addr, length, page_size);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return rc;
		}

		remaining_transfer_len -= length;
	}

	NVME_QPAIR_DEBUGLOG(qpair, "Number of PRP entries: %" PRIu32 "\n", prp_index);
	return 0;
}

typedef int(*build_req_fn)(struct spdk_nvme_qpair *, struct nvme_request *, struct nvme_tracker *,
			   bool);

static build_req_fn const g_nvme_pcie_build_req_table[][2] = {
	[NVME_PAYLOAD_TYPE_INVALID] = {
		nvme_pcie_qpair_build_request_invalid,			/* PRP */
		nvme_pcie_qpair_build_request_invalid			/* SGL */
	},
	[NVME_PAYLOAD_TYPE_CONTIG] = {
		nvme_pcie_qpair_build_contig_request,			/* PRP */
		nvme_pcie_qpair_build_contig_hw_sgl_request		/* SGL */
	},
	[NVME_PAYLOAD_TYPE_SGL] = {
		nvme_pcie_qpair_build_prps_sgl_request,			/* PRP */
		nvme_pcie_qpair_build_hw_sgl_request			/* SGL */
	}
};

static int
nvme_pcie_qpair_build_metadata(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
			       bool sgl_supported, bool mptr_sgl_supported, bool dword_aligned)
{
	void *md_payload;
	struct nvme_request *req = tr->req;
	uint64_t mapping_length;

	if (req->payload.md) {
		md_payload = (uint8_t *)req->payload.md + req->md_offset;
		if (dword_aligned && ((uintptr_t)md_payload & 3)) {
			NVME_QPAIR_ERRLOG(qpair, "virt_addr %p not dword aligned\n", md_payload);
			goto exit;
		}

		mapping_length = req->md_size;
		if (sgl_supported && mptr_sgl_supported && dword_aligned) {
			/* The metadata SGL descriptor is at the tail of the list page. */
			struct spdk_nvme_sgl_descriptor *meta_sgl = &tr->list_page->meta_sgl;

			assert(req->cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
			req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;

			meta_sgl->address = nvme_pcie_vtophys(qpair->ctrlr, md_payload,
							      &mapping_length);
			if (meta_sgl->address == SPDK_VTOPHYS_ERROR ||
			    mapping_length != req->md_size) {
				goto exit;
			}
			meta_sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			meta_sgl->unkeyed.length = req->md_size;
			meta_sgl->unkeyed.subtype = 0;
			req->cmd.mptr = tr->prp_sgl_bus_addr +
					offsetof(struct nvme_tracker_list_page, meta_sgl);
		} else {
			req->cmd.mptr = nvme_pcie_vtophys(qpair->ctrlr, md_payload, &mapping_length);
			if (req->cmd.mptr == SPDK_VTOPHYS_ERROR || mapping_length != req->md_size) {
				goto exit;
			}
		}
	}

	return 0;

exit:
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EINVAL;
}

/*
 * F3: the requests the tracker-free Mode D path takes.  The first two terms are exactly the
 * existing Mode D descriptor predicate in nvme_pcie_qpair_submit_request_stock() (read/write, at most
 * 128 KiB; its qpair terms - Mode D, descriptor ring, IO qpair - are implied by
 * flags.md_trfree).  The last two keep the request a single contiguous buffer with no separate
 * metadata buffer, i.e. one that never needs a tracker-owned list page (SGL segment, metadata
 * SGL); such requests still reach the device through the Mode D descriptor, with a tracker cid.
 */
static inline bool
nvme_pcie_md_trfree_eligible(const struct nvme_request *req)
{
	return (req->cmd.opc == SPDK_NVME_OPC_READ || req->cmd.opc == SPDK_NVME_OPC_WRITE) &&
	       req->payload_size <= 131072u &&
	       nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG &&
	       req->payload.md == NULL;
}

/*
 * F3 tracker-free Mode D submit: a fast cid from the LIFO stack, the request recorded in
 * md_inflight[cid] (for abort / timeout / iterate), and the 40 B descriptor with w4 = the request
 * pointer (the device echoes it in record w0) and w3 = cb_arg.  No tracker, no free_tr /
 * outstanding_tr list operation.  w0/w1 and the doorbell handling are those of the tracker Mode D
 * branch; w2 (the tracker cookie, which the device ignores in Mode D) is 0.  req->cmd's
 * dptr/psdt stay as allocated (zero): the descriptor carries neither.  The caller checked
 * flags.md_trfree, eligibility and a non-empty stack.  Expanded into nvme_pcie_md_submit().
 */
static inline __attribute__((always_inline)) int
nvme_pcie_md_trfree_submit(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_uncore_sq_desc_d *ring_d = (struct nvme_uncore_sq_desc_d *)pqpair->sq_desc_ring;
	uint32_t idx = pqpair->sq_desc_tail;
	uint16_t cid;

	assert(pqpair->md_cid_top != 0);
	cid = pqpair->md_cid_stack[--pqpair->md_cid_top];
	assert(cid < pqpair->md_fast_cids);
	assert(pqpair->md_inflight[cid] == NULL);
	pqpair->md_inflight[cid] = req;
	pqpair->stat->submitted_requests++;
	pqpair->qpair.queue_depth++;
	req->cmd.cid = cid;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_tr_alloc_end = nvme_io_cycle_rdtsc();
	req->t_xlate_end = req->t_tr_alloc_end;  /* addr_xlate elided */
	req->t_sqe_copy = nvme_io_cycle_rdtsc();
#endif
	ring_d[idx].w0 =
		((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
		((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
		((uint64_t) req->cmd.cid           << 32) |
		((uint64_t)(req->cmd.cdw12 & 0xFFFFu) << 16) |
		((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu));
	ring_d[idx].w1 = (uint64_t)req->cmd.cdw10 |
			 ((uint64_t)req->cmd.cdw11 << 32);
	ring_d[idx].w2 = 0;                                   /* no tracker cookie */
	ring_d[idx].w3 = (uint64_t)(uintptr_t)req->cb_arg;     /* cb_arg, echoed in w1 */
	ring_d[idx].w4 = (uint64_t)(uintptr_t)req;             /* request, echoed in w0 */
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_cmd_end   = nvme_io_cycle_rdtsc();
	req->t_doorbell  = req->t_cmd_end;   /* doorbell batched, not per IO */
	req->t_fence_end = req->t_cmd_end;
	req->t_db_end    = req->t_cmd_end;
	req->t_poll_start = pqpair->poll_cycle_accum;
#endif

	pqpair->sq_desc_tail = (idx + 1 == pqpair->num_entries) ? 0 : idx + 1;
	if (!pqpair->flags.delay_cmd_submit) {
		nvme_pcie_qpair_ring_sq_doorbell(qpair);
		pqpair->last_sq_desc_tail = pqpair->sq_desc_tail;
	}
	return 0;
}

/* The stock submit: the pre-F3 body of nvme_pcie_qpair_submit_request(), verbatim.
 * NVME_PCIE_STOCK_BODY: with GCC >= 8 compiled exactly like the former global function, so its
 * instructions are those of the pre-F3 build.  Also the tracker path of a tracker-free qpair
 * (ineligible IO, empty fast cid stack). */
static int NVME_PCIE_STOCK_BODY NVME_PCIE_HOT_ALIGN
nvme_pcie_qpair_submit_request_stock(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_tracker	*tr;
	int			rc = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	enum nvme_payload_type	payload_type;
	bool			sgl_supported;
	bool			mptr_sgl_supported;
	bool			dword_aligned = true;

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_ctrlr_lock(ctrlr);
	}

	{
		struct nvme_pcie_ctrlr *_pctrlr_alloc = nvme_pcie_ctrlr(ctrlr);
		if (_pctrlr_alloc->uncore_free_cid_base != NULL && qpair->id != 0) {
			/* Mechanism #1: hardware free-CID ring path.  One MMIO read
			 * replaces TAILQ_FIRST + TAILQ_REMOVE + TAILQ_INSERT_TAIL +
			 * the software queue_depth increment.  pqpair->tr[hw_cid] is
			 * the per-tracker slot the hardware reserves for this CID. */
			uint32_t hw_cid = _pctrlr_alloc->uncore_free_cid_base[qpair->id];
			if (hw_cid == 0xFFFFu) {
				pqpair->stat->queued_requests++;
				rc = -EAGAIN;
				goto exit;
			}
			tr = &pqpair->tr[hw_cid];
			tr->req = req;
			tr->cb_fn = req->cb_fn;
			tr->cb_arg = req->cb_arg;
			req->cmd.cid = (uint16_t)hw_cid;
			pqpair->stat->submitted_requests++;
			/* Skip TAILQ ops + queue_depth++; hw owns CID lifecycle.
			 * Mechanism #2: read the on-die hardware in-flight counter
			 * and refresh the software shadow. This is the structurally-
			 * required complement to Mechanism #1 (per RTL spec §13.3) —
			 * once HW owns CID allocation, pqpair->qpair.queue_depth would
			 * otherwise drift stale (the inc/dec was skipped above). One
			 * MMIO read replaces ~4 software ops on every submit. */
			if (_pctrlr_alloc->uncore_qdepth_base != NULL) {
				pqpair->qpair.queue_depth =
					_pctrlr_alloc->uncore_qdepth_base[qpair->id];
			}
		} else if (uncore_mode_e() && pqpair->free_slot_ring != NULL &&
			   qpair->id != 0) {
			/* IO-Uncore v4.2 (Mode E): POP a device-fed free tracker slot from
			 * the host-DRAM free-slot ring instead of walking the tracker free
			 * list.  One cacheable DRAM read (the slot line the device DMA-wrote)
			 * + a cursor advance replaces TAILQ_FIRST + TAILQ_REMOVE +
			 * TAILQ_INSERT_TAIL + queue_depth++.  No outstanding_tr list and no
			 * queue_depth are maintained (Mode E does not support abort/reset —
			 * it is a steady-state perf mode); the free-slot ring itself provides
			 * backpressure (phase mismatch => -EAGAIN, requeued like a full
			 * free-list). */
			uint64_t slot_e = pqpair->free_slot_ring[pqpair->free_slot_head];
			if (((slot_e >> FREE_SLOT_PHASE_BIT) & 0x1u) != pqpair->free_slot_phase) {
				/* Device has not published the next free slot yet. */
				pqpair->stat->queued_requests++;
				rc = -EAGAIN;
				goto exit;
			}
			uint16_t slot_idx = (uint16_t)(slot_e & 0xFFFFu);
			if (spdk_unlikely(++pqpair->free_slot_head == pqpair->num_entries)) {
				pqpair->free_slot_head = 0;
				pqpair->free_slot_phase ^= 1;
			}
			pqpair->stat->submitted_requests++;
			tr = &pqpair->tr[slot_idx];
			tr->req = req;
			tr->cb_fn = req->cb_fn;
			tr->cb_arg = req->cb_arg;
			req->cmd.cid = slot_idx;
		} else {
			/* Standard path (Mode 0/1 or Mech #1 not mapped). */
			tr = TAILQ_FIRST(&pqpair->free_tr);
			if (tr == NULL) {
				pqpair->stat->queued_requests++;
				rc = -EAGAIN;
				goto exit;
			}
			pqpair->stat->submitted_requests++;
			TAILQ_REMOVE(&pqpair->free_tr, tr, tq_list);
			TAILQ_INSERT_TAIL(&pqpair->outstanding_tr, tr, tq_list);
			pqpair->qpair.queue_depth++;
			tr->req = req;
			tr->cb_fn = req->cb_fn;
			tr->cb_arg = req->cb_arg;
			req->cmd.cid = tr->cid;
		}
	}
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_tr_alloc_end = nvme_io_cycle_rdtsc();
#endif
	/* Use PRP by default. This bit will be overridden below if needed. */
	req->cmd.psdt = SPDK_NVME_PSDT_PRP;

	/* IO-Uncore v3 (Mode C) host-DRAM descriptor-ring submit path.  Like the
	 * Mode-B mailbox path it bypasses PRP-list construction, the 64-byte SQE
	 * copy and the per-IO doorbell — but instead of MMIO-writing the compact
	 * descriptor it does a PLAIN CACHEABLE STORE into a host-DRAM ring slot and
	 * rings only a batched SQ doorbell (via the delay_cmd_submit deferral, or
	 * immediately when batching is disabled).  CID is host-managed: this branch
	 * runs only when uncore_free_cid_base is unmapped, so the CID-alloc gate
	 * above took the standard TAILQ path and tr / req->cmd.cid are already set.
	 * Phase 1 leaves the completion path completely unchanged (normal CQ scan).
	 *
	 * Timestamps: t_xlate_end := t_tr_alloc_end (addr_xlate elided);
	 * t_sqe_copy just before the ring store; t_cmd_end just after it;
	 * t_doorbell/t_fence_end/t_db_end := t_cmd_end (the doorbell is batched and
	 * not attributed per IO). */
	/* IO-Uncore v4.1 (Mode D) submit: write a LOCKED 40B descriptor — the Mode-C
	 * 24B layout (w0/w1/w2) plus w3=cb_arg, w4=cb_fn — so the device can echo the
	 * host completion callback into the 32B completion-callback record.  Checked
	 * BEFORE the Mode-C branch because Mode D sets both env vars (uncore_mode_c()
	 * is also true).  Plain cacheable ring stores, NO MMIO. */
	if (uncore_mode_d() &&
	    pqpair->sq_desc_ring != NULL &&
	    qpair->id != 0 &&
	    req->payload_size <= 131072u &&
	    (req->cmd.opc == SPDK_NVME_OPC_READ ||
	     req->cmd.opc == SPDK_NVME_OPC_WRITE)) {
		struct nvme_uncore_sq_desc_d *ring_d =
			(struct nvme_uncore_sq_desc_d *)pqpair->sq_desc_ring;
		uint32_t _idx = pqpair->sq_desc_tail;

		req->cmd.dptr.prp.prp1 = 0;
		req->cmd.dptr.prp.prp2 = 0;
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_xlate_end = req->t_tr_alloc_end;  /* addr_xlate elided */
#endif

#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_sqe_copy = nvme_io_cycle_rdtsc();
#endif
		ring_d[_idx].w0 =
			((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
			((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
			((uint64_t) req->cmd.cid           << 32) |
			((uint64_t)(req->cmd.cdw12 & 0xFFFFu) << 16) |
			((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu));
		ring_d[_idx].w1 = (uint64_t)req->cmd.cdw10 |
				  ((uint64_t)req->cmd.cdw11 << 32);
		ring_d[_idx].w2 = (uint64_t)(uintptr_t)tr;              /* cookie = tr */
		ring_d[_idx].w3 = (uint64_t)(uintptr_t)req->cb_arg;     /* cb_arg */
		ring_d[_idx].w4 = (uint64_t)(uintptr_t)req->cb_fn;      /* cb_fn  */
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_cmd_end   = nvme_io_cycle_rdtsc();
		req->t_doorbell  = req->t_cmd_end;   /* doorbell batched, not per IO */
		req->t_fence_end = req->t_cmd_end;
		req->t_db_end    = req->t_cmd_end;
		req->t_poll_start = pqpair->poll_cycle_accum;
#endif

		pqpair->sq_desc_tail = (_idx + 1 == pqpair->num_entries) ? 0 : _idx + 1;
		if (!pqpair->flags.delay_cmd_submit) {
			nvme_pcie_qpair_ring_sq_doorbell(qpair);
			pqpair->last_sq_desc_tail = pqpair->sq_desc_tail;
		}
		rc = 0;
		goto exit;
	}

	if (uncore_mode_c() &&
	    pqpair->sq_desc_ring != NULL &&
	    qpair->id != 0 &&
	    req->payload_size <= 131072u &&
	    (req->cmd.opc == SPDK_NVME_OPC_READ ||
	     req->cmd.opc == SPDK_NVME_OPC_WRITE)) {
		uint64_t _w0, _w1, _w2;
		uint32_t _idx = pqpair->sq_desc_tail;

		/* Path-E fast path never dereferences PRP1/PRP2 (no data DMA); the
		 * on-die SQ Engine would resolve VA -> IOVA in real HW. */
		req->cmd.dptr.prp.prp1 = 0;
		req->cmd.dptr.prp.prp2 = 0;
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_xlate_end = req->t_tr_alloc_end;  /* addr_xlate elided */
#endif

		/* w0/w1 packing byte-identical to the v2 16B compact SQE; w2 carries
		 * the opaque host tracker pointer (device stores it keyed by
		 * {qid,cid}; dormant in Phase 1). */
		_w0 = ((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
		      ((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
		      ((uint64_t) req->cmd.cid           << 32) |
		      ((uint64_t)(req->cmd.cdw12 & 0xFFFFu) << 16) |
		      ((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu));
		_w1 = (uint64_t)req->cmd.cdw10 |
		      ((uint64_t)req->cmd.cdw11 << 32);
		_w2 = (uint64_t)(uintptr_t)tr;

#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_sqe_copy = nvme_io_cycle_rdtsc();
#endif
		/* PLAIN CACHEABLE STORES into the host-DRAM ring — NOT MMIO. */
		pqpair->sq_desc_ring[_idx].w0 = _w0;
		pqpair->sq_desc_ring[_idx].w1 = _w1;
		pqpair->sq_desc_ring[_idx].w2 = _w2;
#ifndef SPDK_NVME_IO_CYCLE_OFF
		req->t_cmd_end   = nvme_io_cycle_rdtsc();
		req->t_doorbell  = req->t_cmd_end;   /* doorbell batched, not per IO */
		req->t_fence_end = req->t_cmd_end;
		req->t_db_end    = req->t_cmd_end;
		req->t_poll_start = pqpair->poll_cycle_accum;
#endif

		/* Advance the descriptor-ring producer index (NOT the NVMe sq_tail).
		 * Ring the SQ doorbell through the BATCHED delay_cmd_submit path: when
		 * batching is on, the deferred flush in process_completions coalesces
		 * one doorbell per poll (~0.13 doorbells/IO); when off, ring now. */
		pqpair->sq_desc_tail = (uint32_t)((_idx + 1) % pqpair->num_entries);
		if (!pqpair->flags.delay_cmd_submit) {
			nvme_pcie_qpair_ring_sq_doorbell(qpair);
			pqpair->last_sq_desc_tail = pqpair->sq_desc_tail;
		}
		rc = 0;
		goto exit;
	}

	/* IO-Uncore Mode 2B deep-offload entry point.  This is the EARLY branch
	 * that bypasses the per-IO PRP-list construction (~337 ns/IO at QD=128)
	 * AND the 64-byte SQE copy (~51 ns/IO) AND the doorbell ring (~3 ns/IO).
	 * The compact 24-byte descriptor is emitted as 3 sequential 8-byte MMIO
	 * writes to the per-qid mailbox slot; the controller's SQ Engine
	 * synthesizes the full SQE on-die (modeled in SimpleSSD with a fixed
	 * cycle budget; see MailboxLatch/MailboxDecode/MailboxInject cfg).
	 *
	 * Falls back to the standard path for:
	 *   - admin qpair (qid == 0): admin commands always use full path
	 *   - multi-page transfers: needs PRP list (v1 limitation)
	 *   - non-read/write opcodes: needs full command construction
	 *   - mailbox not mapped (SPDK_UNCORE_MODE_B != 1)
	 *
	 * Timestamps are collapsed so the cycle_breakdown CSV reflects which
	 * stages the hardware absorbed:
	 *   t_xlate_end := t_tr_alloc_end   (addr_xlate_ns == 0)
	 *   t_sqe_copy/t_cmd_end := after the 3 MMIO writes
	 *   t_fence_end/t_db_end/t_doorbell := t_cmd_end (fence_ns, doorbell_ns == 0)
	 */
	{
		struct nvme_pcie_ctrlr *_pctrlr = nvme_pcie_ctrlr(ctrlr);
		if (_pctrlr->uncore_mailbox_base != NULL &&
		    qpair->id != 0 &&
		    req->payload_size <= (uncore_multipage() ? 131072u : ctrlr->page_size) &&
		    (req->cmd.opc == SPDK_NVME_OPC_READ ||
		     req->cmd.opc == SPDK_NVME_OPC_WRITE)) {
			uint64_t _w0, _w1, _w2;
			volatile uint64_t *_slot;
			/* PRP1 is conventionally the IOVA of the host payload buffer.
			 * In the simulator's Path-E fast path the controller never
			 * dereferences it (no actual data DMA), so we can pass 0 here
			 * — real hardware would have the on-die SQ Engine resolve
			 * VA -> IOVA via the IOMMU page-table cache. */
			req->cmd.dptr.prp.prp1 = 0;
			req->cmd.dptr.prp.prp2 = 0;
#ifndef SPDK_NVME_IO_CYCLE_OFF
			req->t_xlate_end = req->t_tr_alloc_end;  /* addr_xlate elided */
#endif
			_slot = _pctrlr->uncore_mailbox_base +
			        (qpair->id * (MAILBOX_STRIDE_BYTES / sizeof(uint64_t)));
#ifndef SPDK_NVME_IO_CYCLE_OFF
			req->t_sqe_copy = nvme_io_cycle_rdtsc();
#endif
			if (uncore_sqe16()) {
				/* v2 16-byte compact SQE: 2 MMIO writes.  gem5 expects
				 * nlb at w0[31:16] and control at w0[15:0]; cdw12 packs
				 * control<<16 | nlb, hence the swap below.  nsid is fixed
				 * to 1 and prp1_lo is dropped. */
				_w0 = ((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
				      ((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
				      ((uint64_t) req->cmd.cid           << 32) |
				      ((uint64_t)(req->cmd.cdw12 & 0xFFFFu) << 16) |
				      ((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu));
				_w1 = (uint64_t)req->cmd.cdw10 |
				      ((uint64_t)req->cmd.cdw11 << 32);
				g_thread_mmio_ctrlr = _pctrlr;
				spdk_mmio_write_8(&_slot[0], _w0);
				spdk_mmio_write_8(&_slot[1], _w1);
				g_thread_mmio_ctrlr = NULL;
			} else {
				_w0 = ((uint64_t)(req->cmd.opc  & 0xFFu) << 56) |
				      ((uint64_t)(req->cmd.fuse & 0xFFu) << 48) |
				      ((uint64_t) req->cmd.cid           << 32) |
				      ((uint64_t) req->cmd.nsid                );
				_w1 = (uint64_t)req->cmd.cdw10 |
				      ((uint64_t)req->cmd.cdw11 << 32);  /* SLBA */
				_w2 = ((uint64_t)((req->cmd.cdw12 >> 16) & 0xFFFFu) <<  0) |
				      ((uint64_t)( req->cmd.cdw12        & 0xFFFFu) << 16);
				g_thread_mmio_ctrlr = _pctrlr;
				spdk_mmio_write_8(&_slot[0], _w0);
				spdk_mmio_write_8(&_slot[1], _w1);
				spdk_mmio_write_8(&_slot[2], _w2);
				g_thread_mmio_ctrlr = NULL;
			}
#ifndef SPDK_NVME_IO_CYCLE_OFF
			req->t_cmd_end   = nvme_io_cycle_rdtsc();
			req->t_doorbell  = req->t_cmd_end;
			req->t_fence_end = req->t_cmd_end;
			req->t_db_end    = req->t_cmd_end;
#endif
			pqpair->stat->sq_mmio_doorbell_updates++;
			/* Do NOT advance sq_tail — mailbox writes do not touch
			 * host SQ memory.  See the longer rationale in the
			 * sibling mailbox path inside nvme_pcie_qpair_submit_tracker
			 * (search for "phantom commands with cid=0"). */
			rc = 0;
			goto exit;
		}
	}

	if (req->payload_size != 0) {
		payload_type = nvme_payload_type(&req->payload);
		/* According to the specification, PRPs shall be used for all
		 *  Admin commands for NVMe over PCIe implementations.
		 */
		sgl_supported = (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) != 0 &&
				!nvme_qpair_is_admin_queue(qpair);
		mptr_sgl_supported = (ctrlr->flags & SPDK_NVME_CTRLR_MPTR_SGL_SUPPORTED) != 0 &&
				     !nvme_qpair_is_admin_queue(qpair);

		if (sgl_supported) {
			/* Don't use SGL for DSM command */
			if (spdk_unlikely((ctrlr->quirks & NVME_QUIRK_NO_SGL_FOR_DSM) &&
					  (req->cmd.opc == SPDK_NVME_OPC_DATASET_MANAGEMENT))) {
				sgl_supported = false;
			}
		}

		if (sgl_supported && !(ctrlr->flags & SPDK_NVME_CTRLR_SGL_REQUIRES_DWORD_ALIGNMENT)) {
			dword_aligned = false;
		}

		/* If we fail to build the request or the metadata, do not return the -EFAULT back up
		 * the stack.  This ensures that we always fail these types of requests via a
		 * completion callback, and never in the context of the submission.
		 */
		rc = g_nvme_pcie_build_req_table[payload_type][sgl_supported](qpair, req, tr, dword_aligned);
		if (rc < 0) {
			assert(rc == -EFAULT);
			rc = 0;
			goto exit;
		}

		rc = nvme_pcie_qpair_build_metadata(qpair, tr, sgl_supported, mptr_sgl_supported, dword_aligned);
		if (rc < 0) {
			assert(rc == -EFAULT);
			rc = 0;
			goto exit;
		}
	}
#ifndef SPDK_NVME_IO_CYCLE_OFF
	req->t_xlate_end = nvme_io_cycle_rdtsc();
#endif

	nvme_pcie_qpair_submit_tracker(qpair, tr);

exit:
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_ctrlr_unlock(ctrlr);
	}

	return rc;
}

/* F3: submit on a tracker-free Mode D qpair (reached by a tail call from
 * nvme_pcie_qpair_submit_request()).  Small on purpose: the fast path is inlined here; ineligible
 * IO and IO that finds the fast cid stack empty tail-call the stock path, whose tracker free list
 * holds only tracker cids on these qpairs. */
static int __attribute__((noinline, hot)) NVME_PCIE_HOT_ALIGN
nvme_pcie_md_submit(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (spdk_likely(pqpair->md_cid_top != 0 && nvme_pcie_md_trfree_eligible(req))) {
		return nvme_pcie_md_trfree_submit(qpair, req);
	}
	return nvme_pcie_qpair_submit_request_stock(qpair, req);
}

int NVME_PCIE_HOT_ALIGN
nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	/* F3: one test; a tracker-free Mode D qpair submits out of line. */
	if (spdk_unlikely(nvme_pcie_qpair(qpair)->flags.md_trfree)) {
		return nvme_pcie_md_submit(qpair, req);
	}
	return nvme_pcie_qpair_submit_request_stock(qpair, req);
}

struct spdk_nvme_transport_poll_group *
nvme_pcie_poll_group_create(void)
{
	struct nvme_pcie_poll_group *group = calloc(1, sizeof(*group));

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	return &group->group;
}

int
nvme_pcie_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_pcie_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_pcie_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			 struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_pcie_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			    struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->stat = &g_dummy_stat;
	return 0;
}

int64_t
nvme_pcie_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	int32_t local_completions = 0;
	int64_t total_completions = 0;

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		disconnected_qpair_cb(qpair, tgroup->group->ctx);
	}

	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
		local_completions = spdk_nvme_qpair_process_completions(qpair, completions_per_qpair);
		if (spdk_unlikely(local_completions < 0)) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
			total_completions = -ENXIO;
		} else if (spdk_likely(total_completions >= 0)) {
			total_completions += local_completions;
		}
	}

	return total_completions;
}

void
nvme_pcie_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair, *tmp_qpair;

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		disconnected_qpair_cb(qpair, tgroup->group->ctx);
	}
}

int
nvme_pcie_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	free(tgroup);

	return 0;
}

int
nvme_pcie_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_pcie_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_PCIE;
	group = SPDK_CONTAINEROF(tgroup, struct nvme_pcie_poll_group, group);
	memcpy(&stats->pcie, &group->stats, sizeof(group->stats));

	*_stats = stats;

	return 0;
}

void
nvme_pcie_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_transport_poll_group_stat *stats)
{
	free(stats);
}

static void
nvme_pcie_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"NVME_PCIE_SUBMIT", TRACE_NVME_PCIE_SUBMIT,
			OWNER_TYPE_NVME_PCIE_QP, OBJECT_NVME_PCIE_REQ, 1,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "opc", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "dw10", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw11", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw12", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
		{
			"NVME_PCIE_COMPLETE", TRACE_NVME_PCIE_COMPLETE,
			OWNER_TYPE_NVME_PCIE_QP, OBJECT_NVME_PCIE_REQ, 0,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "cpl", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
	};

	spdk_trace_register_object(OBJECT_NVME_PCIE_REQ, 'p');
	spdk_trace_register_owner_type(OWNER_TYPE_NVME_PCIE_QP, 'q');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
SPDK_TRACE_REGISTER_FN(nvme_pcie_trace, "nvme_pcie", TRACE_GROUP_NVME_PCIE)
