/*   SPDX-License-Identifier: BSD-3-Clause
 *   IAU-next P4.11 (plan D15 rev 2, Task P4.11 rev 2; the controller's fairness rule of 2026-10-19): the S2 software
 *   ablation, Mode 0 + SPDK_IAU_S2=1. The host builds the SQEs, keeps the cid -> cookie table, polls its own CQ,
 *   retries, times out, aborts late and tears down in software: what the IAU does in hardware for tile, with the tile host
 *   path's software optimizations (the same nvme_iau_fast_rw() tail-call hook, the run-time terms tested first, the hot
 *   functions pinned at 64 B starts, the same poll-group override, request-free completions). nvme_iau.h has the model,
 *   the rules and the evidence lines.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/barrier.h"
#include "spdk/mmio.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include "nvme_iau.h"
#include "nvme_text_pad.h"

/*
 * The hot functions start on a 64 B boundary (NVME_IAU_S2_HOT_ALIGN, as nvme_iau.c's NVME_IAU_HOT_ALIGN): the request-free
 * submit, the request submit, the qpair poll, the group poll and the out-of-line completion paths. This object's .text
 * is then 64 B aligned: it starts at phase 0 after nvme_iau.o (which ends at phase 16) and is padded to 16 modulo 64
 * (NVME_TEXT_PHASE64), so it ends at phase 16 as the 16 B-aligned stub did and every object linked after it keeps its
 * 64 B phase (tests/realcore/test_pcie_hot_layout.sh PINNED64; scripts/realcore/next/nm_phase.sh --set perf --pin0).
 */
#define NVME_IAU_S2_HOT_ALIGN	__attribute__((aligned(64)))
NVME_TEXT_PHASE64(16);

#define NVME_IAU_S2_IO_FLAGS	(SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS | SPDK_NVME_IO_FLAGS_LIMITED_RETRY)
#define NVME_IAU_S2_ST_SCT_SC	0x0FFEu		/* status word bits [11:1]: SC and SCT (bit 0 is the phase) */
#define NVME_IAU_S2_W_OWED	0x01u		/* s->work: reset leftovers to end */
#define NVME_IAU_S2_W_ABORT	0x02u		/* s->work: abort callbacks to run */
#define NVME_IAU_S2_W_ATTN	0x04u		/* s->work: the attention pass's generic call (its resubmission budget) */

SPDK_STATIC_ASSERT(SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS == 1u << 30 && SPDK_NVME_IO_FLAGS_LIMITED_RETRY == 1u << 31,
		   "FUA / LR are cdw12 bits 30 / 31: the S2 SQE takes io_flags into cdw12 unchanged");

static struct spdk_nvme_transport_ops g_s2_stock;	/* the table nvme_iau_s2_override_ops() replaced */
static uint32_t g_s2_next_pg;

/* ------------------------------------------------------------------------------------------ the S2 poll group */

/* S2's poll-group override (D12 as tile's): a PCIe poll group (first, so the PCIe code finds its stats from the tgroup)
 * with its members, the attention list, the member an attention call is in, the sweep counter */
struct nvme_iau_s2_pg {
	struct nvme_pcie_poll_group	pg;
	struct nvme_iau_s2_qlist	members, attn;
	struct nvme_iau_s2_qpair	*attn_cur;
	uint32_t			id, sweep;
	bool				in_pass, in_member_pass;
	uint64_t			polls, idle_polls, sweeps, attn_calls;
};

static inline struct nvme_iau_s2_pg *
s2_pg(struct spdk_nvme_transport_poll_group *tg)
{
	return SPDK_CONTAINEROF(tg, struct nvme_iau_s2_pg, pg.group);
}

/* a member needs its generic duties: queue it once on the attention list */
static void
s2_attn(struct nvme_iau_s2_pg *pg, struct nvme_iau_s2_qpair *s)
{
	if (!s->in_attn) {
		s->in_attn = 1;
		s->attn_list = &pg->attn;
		TAILQ_INSERT_TAIL(&pg->attn, s, attn_link);
	}
}

/* ------------------------------------------------------------------------------------------ doorbells, the SQE */

/* the SQ doorbell: the streaming SQE stores before the doorbell store (nvme_pcie_qpair_ring_sq_doorbell()'s spdk_wmb()).
 * Not needed here: its shadow-doorbell branch (refused at create), Mode C's descriptor tail (S2 is Mode 0) and the PCIe
 * hotplug SIGBUS remap (g_thread_mmio_ctrlr; vfio-user only). The fused-first hold is kept: a tracker submit of a fused
 * pair's first command (last_fuse) holds every doorbell until its second, as stock */
static inline __attribute__((always_inline)) void
s2_sq_doorbell(struct spdk_nvme_qpair *qpair, struct nvme_pcie_qpair *pq)
{
	if (spdk_unlikely(qpair->last_fuse == SPDK_NVME_IO_FLAGS_FUSE_FIRST)) {
		return;
	}
	spdk_wmb();
	spdk_mmio_write_4(pq->sq_tdbl, pq->sq_tail);
	nvme_db_cost(g_nvme_db_cost_ticks);	/* M2 */
	pq->stat->sq_mmio_doorbell_updates++;
}

static inline __attribute__((always_inline)) void
s2_cq_doorbell(struct nvme_pcie_qpair *pq)
{
	spdk_mmio_write_4(pq->cq_hdbl, pq->cq_head);
	nvme_db_cost(g_nvme_db_cost_ticks);	/* M2 */
	pq->stat->cq_mmio_doorbell_updates++;
}

/* the SQE stock builds for a contiguous READ / WRITE without metadata or PI (nvme_ns_cmd.c _nvme_ns_cmd_setup_request(),
 * nvme_pcie_common.c's PRP builder): opc, fuse 0, psdt PRP, cid; nsid; cdw2-3, mptr 0; PRP1, PRP2; cdw10-11 = slba;
 * cdw12 = nlb | FUA / LR; cdw13-15 0. Eight 8 B streaming stores (nvme_pcie_copy_command() streams the request's copy
 * in four 16 B ones; here no copy exists to load) */
static inline __attribute__((always_inline)) void
s2_sqe_put(struct spdk_nvme_cmd *dst, uint64_t w0, uint64_t prp1, uint64_t prp2, uint64_t slba, uint64_t cdw12)
{
#if defined(__x86_64__)
	long long *q = (long long *)dst;

	if (g_nvme_iau_cfg.s2_wb) {	/* review fix M3: write-back stores, as I3 writes its D slot */
		uint64_t *w = (uint64_t *)dst;

		w[0] = w0;
		w[1] = 0;
		w[2] = 0;
		w[3] = prp1;
		w[4] = prp2;
		w[5] = slba;
		w[6] = cdw12;
		w[7] = 0;
		return;
	}
	_mm_stream_si64(&q[0], (long long)w0);
	_mm_stream_si64(&q[1], 0);
	_mm_stream_si64(&q[2], 0);
	_mm_stream_si64(&q[3], (long long)prp1);
	_mm_stream_si64(&q[4], (long long)prp2);
	_mm_stream_si64(&q[5], (long long)slba);
	_mm_stream_si64(&q[6], (long long)cdw12);
	_mm_stream_si64(&q[7], 0);
#else
	uint64_t *q = (uint64_t *)dst;

	q[0] = w0;
	q[1] = 0;
	q[2] = 0;
	q[3] = prp1;
	q[4] = prp2;
	q[5] = slba;
	q[6] = cdw12;
	q[7] = 0;
#endif
}

/* PRP2 beyond two pages: iau_tile_prp(), the IAU engine's builder (the same list, the pages after the first), in the fast
 * cid's own tracker list page (fast cids' trackers are never handed out: their pages are free) */
static uint64_t __attribute__((noinline))
s2_prp_list(struct nvme_pcie_qpair *pq, uint16_t cid, uint64_t buf, uint64_t len)
{
	struct nvme_tracker *tr = &pq->tr[cid];
	uint64_t prp1, prp2;
	int rc;

	rc = iau_tile_prp(buf, len, tr->list_page->u.prp, tr->prp_sgl_bus_addr, &prp1, &prp2);
	assert(rc == 0);	/* a qualified IO: dword aligned, <= 128 KiB, a 4 KiB-aligned list page */
	(void)rc;
	return prp2;
}

/* PRP2 of lba_count blocks at buf on cid: 0 within one page, the next page for two, else the list */
static inline __attribute__((always_inline)) uint64_t
s2_prp2(struct nvme_pcie_qpair *pq, const struct nvme_iau_s2_qpair *s, uint16_t cid, uint64_t buf, uint32_t lba_count)
{
	uint64_t len = (uint64_t)lba_count << s->lba_shift, end = (buf & (IAU_TILE_PAGE - 1)) + len;

	if (spdk_likely(end <= IAU_TILE_PAGE)) {
		return 0;
	}
	if (end <= 2 * IAU_TILE_PAGE) {
		return (buf | (IAU_TILE_PAGE - 1)) + 1;
	}
	return s2_prp_list(pq, cid, buf, len);
}

/* the command of fast cid (its cold entry), as the SSD saw it: for the error policy's print and the retry */
static void
s2_cmd(struct nvme_pcie_qpair *pq, const struct nvme_iau_s2_qpair *s, uint16_t cid, struct spdk_nvme_cmd *cmd)
{
	const struct nvme_iau_s2_cold *c = &s->cold[cid];

	memset(cmd, 0, sizeof(*cmd));
	cmd->opc = c->opc;
	cmd->cid = cid;
	cmd->nsid = s->nsid;
	cmd->dptr.prp.prp1 = c->buf;
	cmd->dptr.prp.prp2 = s2_prp2(pq, s, cid, c->buf, (c->nlb_flags & 0xFFFFu) + 1);
	cmd->cdw10 = (uint32_t)c->slba;
	cmd->cdw11 = (uint32_t)(c->slba >> 32);
	cmd->cdw12 = c->nlb_flags;
}

/* the generic layer prints a submitted command under the "nvme" debug flag (_nvme_qpair_submit_request()): a DEBUG build
 * only */
static void __attribute__((noinline))
s2_print_cmd(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s, uint16_t cid)
{
	struct spdk_nvme_cmd cmd;

	s2_cmd(nvme_pcie_qpair(qpair), s, cid, &cmd);
	spdk_nvme_qpair_print_command(qpair, &cmd);
}

/* ------------------------------------------------------------------------------------------ the request-free submit */

/* a taken request-free IO's SQE and the rest of its submit (both submit paths): the SQE (eight streaming stores) at the
 * SQ tail, the tail advanced, the IO credit (queue_depth), the outstanding count (num_outstanding_reqs, as tile's
 * request-free IO) and the stats counted, the doorbell rung unless delay_cmd_submit (then the poll's tail rings it) */
static inline __attribute__((always_inline)) void
s2_fast_put(struct spdk_nvme_qpair *qpair, struct nvme_pcie_qpair *pqpair, struct nvme_iau_s2_qpair *s, uint64_t w0,
	    uint64_t buf, uint64_t prp2, uint64_t lba, uint32_t cdw12)
{
	uint16_t tail = pqpair->sq_tail;

	s2_sqe_put(&pqpair->cmd[tail], w0, buf, prp2, lba, cdw12);
	if (spdk_unlikely(++tail == pqpair->num_entries)) {
		tail = 0;
	}
	pqpair->sq_tail = tail;
	qpair->queue_depth++;
	qpair->num_outstanding_reqs++;
	s->st.sub_fast++;
	pqpair->stat->submitted_requests++;
	if (!pqpair->flags.delay_cmd_submit) {
		s2_sq_doorbell(qpair, pqpair);
	}
}

/* P4.11b (the P4.11 review's m1, fairness): nvme_iau_s2_fast_rw()'s continuation for a taken IO that needs a call, out
 * of line and reached by a tail call, so the hot body makes no call and keeps no stack frame (tile_fast_rw() keeps none):
 * the timeout's submit_tick (latched at connect), a PRP list (more than two pages: s2_prp_list()) or the "nvme" debug
 * print (a DEBUG build). The cookie is written; the SQE is built from the cold entry as the retry builds it. The cid is
 * the one the submit just popped, still in the stack's slot at cid_top (one argument: no extra register move there) */
static int __attribute__((noinline))
s2_fast_rw_call(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_iau_s2_qpair *s = nvme_iau_s2_qp(qpair);
	uint16_t cid = s->cid_stack[s->cid_top];
	struct nvme_iau_s2_cold *c = &s->cold[cid];

	if (s->tmo_on) {
		c->submit_tick = spdk_get_ticks();
	}
	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		s2_print_cmd(qpair, s, cid);	/* as stock (nothing without DEBUG) */
	}
	s2_fast_put(qpair, pqpair, s, (uint64_t)c->opc | (uint64_t)cid << 16 | (uint64_t)s->nsid << 32, c->buf,
		    s2_prp2(pqpair, s, cid, c->buf, (c->nlb_flags & 0xFFFFu) + 1), c->slba, c->nlb_flags);
	return 0;
}

/*
 * nvme_iau_fast_rw()'s S2 branch (a tail call with the same twelve arguments; the ns_cmd entries call nvme_iau_fast_rw()
 * on iau_fast, which is set on an S2 qpair at connect, s2_latch()). tile's qualification (D14), the run-time terms first: a
 * free fast cid and the three lists empty (one test), then md == NULL, io_flags within {FUA, LR}, 1 <= lba_count <=
 * fast_nlb, a dword-aligned buffer, the latched namespace, no stripe crossed, ENABLED, no transport failure, the controller
 * not failed. A declined IO is counted and goes to nvme_ns_cmd_rw_contig(), the stock body (a tail call): the request path,
 * whose submit op takes a tracker cid. A taken IO pops a fast cid, writes its hot cookie (cb_fn, cb_arg) and its cold
 * entry (slba, buf, then cdw12 | opc | BUSY in one 8 B store: retries and bits 0), and with PRP2 within two pages
 * (0, or the next page) builds the SQE and finishes the submit (s2_fast_put()); a latched timeout (submit_tick), a PRP
 * list or the debug flag go to s2_fast_rw_call() by a tail call (P4.11b: no call, so no stack frame, here). opc is the
 * entry's constant, used unmasked as tile uses it. nocf_check: it is reached only by the dispatcher's direct jump (the
 * dispatcher has the endbr64), as tile_fast_rw().
 */
int __attribute__((nocf_check)) NVME_IAU_S2_HOT_ALIGN
nvme_iau_s2_fast_rw(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, void *metadata, uint64_t lba,
		    uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc, uint32_t io_flags,
		    uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_iau_s2_qpair *s = nvme_iau_s2_qp(qpair);
	struct nvme_iau_s2_cold *c;
	struct nvme_iau_s2_hot *h;
	uint64_t buf = (uint64_t)(uintptr_t)buffer, w, prp2, end;
	uint32_t cdw12;
	uint16_t cid;

	if (spdk_unlikely(s->cid_top == 0 ||
			  ((uintptr_t)STAILQ_FIRST(&qpair->queued_req) | (uintptr_t)TAILQ_FIRST(&qpair->err_cmd_head) |
			   (uintptr_t)STAILQ_FIRST(&qpair->err_req_head)) != 0 ||	/* the three lists empty: one test */
			  metadata != NULL || (io_flags & ~NVME_IAU_S2_IO_FLAGS) != 0 || lba_count - 1u >= s->fast_nlb ||
			  (buf & 3u) != 0 || ns->id != s->nsid ||
			  (s->stripe != 0 && (lba & (s->stripe - 1)) + lba_count > s->stripe) ||
			  nvme_qpair_get_state(qpair) != NVME_QPAIR_ENABLED ||
			  qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE || qpair->ctrlr->is_failed)) {
		s->st.declined++;
		return nvme_ns_cmd_rw_contig(ns, qpair, buffer, metadata, lba, lba_count, cb_fn, cb_arg, opc, io_flags,
					     apptag_mask, apptag);
	}
	cid = s->cid_stack[--s->cid_top];
	h = &s->hot[cid];
	h->cb_fn = (uint64_t)(uintptr_t)cb_fn;
	h->cb_arg = (uint64_t)(uintptr_t)cb_arg;
	cdw12 = (lba_count - 1) | io_flags;
	c = &s->cold[cid];
	c->slba = lba;
	c->buf = buf;
	w = (uint64_t)cdw12 | (uint64_t)opc << 32 | (uint64_t)NVME_IAU_S2_BUSY << 40;	/* retries 0, bits 0 */
	memcpy(&c->nlb_flags, &w, sizeof(w));
	if (spdk_unlikely(s->tmo_on || SPDK_DEBUGLOG_FLAG_ENABLED("nvme"))) {
		return s2_fast_rw_call(qpair);
	}
	end = (buf & (IAU_TILE_PAGE - 1)) + ((uint64_t)lba_count << s->lba_shift);
	prp2 = 0;
	if (spdk_unlikely(end > IAU_TILE_PAGE)) {
		if (end > 2 * IAU_TILE_PAGE) {
			return s2_fast_rw_call(qpair);
		}
		prp2 = (buf | (IAU_TILE_PAGE - 1)) + 1;
	}
	s2_fast_put(qpair, pqpair, s, (uint64_t)opc | (uint64_t)cid << 16 | (uint64_t)s->nsid << 32, buf, prp2, lba, cdw12);
	return 0;
}

/* ------------------------------------------------------------------------------------------ the request submit */

static int __attribute__((noinline))
s2_submit_fail(struct spdk_nvme_qpair *qpair, int rc)
{
	struct nvme_iau_s2_qpair *s = nvme_iau_s2_qp(qpair);

	if (rc == -EAGAIN && s->pg != NULL) {
		s2_attn(s->pg, s);	/* queued by the generic layer: the attention pass's generic call resubmits it (D12) */
	}
	return rc;
}

/* the request path (a declined IO's stock body, and everything not fast): stock's PCIe submit on the tracker cids (free_tr
 * holds only them); a member whose request the generic layer queues (-EAGAIN: no free tracker) goes on its group's
 * attention list, as tile's submit_full() puts it. S2's loop records no sqhd (DV34): sq_head's only use is stock's
 * "sq_tail is passing sq_head!" check in nvme_pcie_qpair_submit_tracker(), which the cid count makes vacuous (fast +
 * tracker cids = num_trackers < num_entries), so sq_head is parked at num_entries, outside the tail's range, before
 * stock submits (connect's nvme_pcie_qpair_reset() zeroes it; a tracker's retry follows its own submit) */
static int NVME_IAU_S2_HOT_ALIGN
nvme_iau_s2_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_pcie_qpair *pq;
	int rc;

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		return nvme_pcie_qpair_submit_request(qpair, req);
	}
	pq = nvme_pcie_qpair(qpair);
	pq->sq_head = pq->num_entries;
	rc = nvme_pcie_qpair_submit_request(qpair, req);
	if (spdk_unlikely(rc != 0)) {
		return s2_submit_fail(qpair, rc);
	}
	nvme_iau_s2_qp(qpair)->st.sub_tr++;
	return 0;
}

/* ------------------------------------------------------------------------------------------ completions */

/* a fast cid back on its stack with its IO credit and outstanding count (before the callback, as tile's request-free IO);
 * P4.11b (the P4.11 review's I1): its hot cb_fn becomes the idle sentinel (NVME_IAU_S2_IDLE), before the callback, which
 * may take the cid again */
static inline __attribute__((always_inline)) void
s2_free_cid(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s, uint16_t cid)
{
	s->hot[cid].cb_fn = NVME_IAU_S2_IDLE;
	s->cid_stack[s->cid_top++] = cid;
	qpair->queue_depth--;
	qpair->num_outstanding_reqs--;
}

/* the completion of a late-aborted command: the IAU engine's record status (ST_ABRQ: ABORTED_BY_REQUEST, dnr; cdw0 0) */
static void
s2_abrq(struct spdk_nvme_cpl *out, const struct spdk_nvme_cpl *cpl)
{
	*out = *cpl;
	out->cdw0 = 0;
	out->cdw1 = 0;
	out->status.sct = SPDK_NVME_SCT_GENERIC;
	out->status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	out->status.crd = 0;
	out->status.m = 0;
	out->status.dnr = 1;
}

/* the retry (stock's complete_tracker() -> submit_tracker() on the same tracker): the same cid, the SQE rebuilt from the
 * cold entry by the submit's builder (byte-equal), rung unless delay_cmd_submit (the poll's tail rings it then) */
static void
s2_resubmit(struct spdk_nvme_qpair *qpair, struct nvme_pcie_qpair *pq, struct nvme_iau_s2_qpair *s, uint16_t cid)
{
	const struct nvme_iau_s2_cold *c = &s->cold[cid];
	uint16_t tail = pq->sq_tail;

	s2_sqe_put(&pq->cmd[tail], (uint64_t)c->opc | (uint64_t)cid << 16 | (uint64_t)s->nsid << 32, c->buf,
		   s2_prp2(pq, s, cid, c->buf, (c->nlb_flags & 0xFFFFu) + 1), c->slba, c->nlb_flags);
	if (++tail == pq->num_entries) {
		tail = 0;
	}
	pq->sq_tail = tail;
	if (!pq->flags.delay_cmd_submit) {
		s2_sq_doorbell(qpair, pq);
	}
}

/* P4.11b (the P4.11 review's I1): a CQE for a fast cid that is not in flight (its hot cb_fn is the idle sentinel): a
 * duplicate or replayed CQE, or a device fault. Stock's "does not map" (logged, the CQE printed, nothing called back:
 * nvme_pcie_qpair_process_completions_stock()) and the IAU's stray record (counted): no cid push, no credit, no callback,
 * counted in stray. Stock also asserts in a debug build; S2 counts, as the IAU does */
static void __attribute__((noinline))
s2_cpl_stray(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s, struct spdk_nvme_cpl *cpl)
{
	s->st.stray++;
	NVME_QPAIR_ERRLOG(qpair, "cpl does not map to outstanding cmd\n");
	spdk_nvme_qpair_print_completion(qpair, cpl);
}

/*
 * A fast cid's CQE off the fast path (an error status, a late-abort mark, the "nvme" debug flag, or a hot cb_fn of NULL
 * or the idle sentinel): a cid not in flight is a stray (s2_cpl_stray()); else stock's completion semantics
 * (nvme_pcie_qpair_complete_tracker()) on the cookie. A marked command is delivered ABORTED_BY_REQUEST (dnr) and printed
 * as a final error; otherwise the error policy (print as stock; retry while retries < retry_count for a retryable error:
 * the same cid rebuilt, no callback); else the cid, credit and outstanding count back, then the callback (none for a
 * NULL cb_fn).
 */
static void __attribute__((noinline)) NVME_IAU_S2_HOT_ALIGN
s2_cpl_cold(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s, struct spdk_nvme_cpl *cpl, uint16_t cid)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	struct nvme_iau_s2_cold *c = &s->cold[cid];
	spdk_nvme_cmd_cb cb_fn = (spdk_nvme_cmd_cb)(uintptr_t)s->hot[cid].cb_fn;
	void *cb_arg = (void *)(uintptr_t)s->hot[cid].cb_arg;
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl ab;

	if (spdk_unlikely(s->hot[cid].cb_fn == NVME_IAU_S2_IDLE)) {
		s2_cpl_stray(qpair, s, cpl);
		return;
	}
	s2_cmd(pq, s, cid, &cmd);
	if (c->bits & NVME_IAU_S2_B_ABORT) {
		/* late abort (D7, DV5, as the IAU's marked slot): the command ran; its CQE is ABORTED_BY_REQUEST */
		c->bits &= (uint8_t)~NVME_IAU_S2_B_ABORT;
		s->special--;
		s->st.aborted++;
		s2_abrq(&ab, cpl);
		cpl = &ab;
		(void)nvme_iau_cpl_error_policy(qpair, &cmd, UINT8_MAX, 0, cpl, true);
	} else if (nvme_iau_cpl_error_policy(qpair, &cmd, c->retries, pq->retry_count, cpl, true) == NVME_IAU_CPL_RETRY) {
		c->retries++;
		s->st.retries++;
		s2_resubmit(qpair, pq, s, cid);
		return;
	} else if (spdk_nvme_cpl_is_error(cpl)) {
		s->st.err++;
	}
	s2_free_cid(qpair, s, cid);
	if (cb_fn != NULL) {
		cb_fn(cb_arg, cpl);	/* a NULL cb_fn is skipped, as stock */
	}
}

/* a tracker cid's CQE: stock's tracker completion (with a late-abort mark: ABORTED_BY_REQUEST); a cid without a request is
 * stock's "does not map" */
static void __attribute__((noinline)) NVME_IAU_S2_HOT_ALIGN
s2_cpl_tracker(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s, struct spdk_nvme_cpl *cpl, uint16_t cid)
{
	struct nvme_tracker *tr = &nvme_pcie_qpair(qpair)->tr[cid];
	uint32_t t = (uint32_t)cid - s->fast_cids;
	struct spdk_nvme_cpl ab;

	if (spdk_unlikely(tr->req == NULL)) {
		NVME_QPAIR_ERRLOG(qpair, "cpl does not map to outstanding cmd\n");
		spdk_nvme_qpair_print_completion(qpair, cpl);
		assert(0);
		return;
	}
	__builtin_prefetch(&tr->req->stailq);
	if (spdk_unlikely(s->special != 0) && t < NVME_IAU_S2_TRACKER_CIDS && s->trk_mark[t]) {
		s->trk_mark[t] = 0;
		s->special--;
		s->st.aborted++;
		s2_abrq(&ab, cpl);
		nvme_pcie_qpair_complete_tracker(qpair, tr, &ab, true);
		return;
	}
	nvme_pcie_qpair_complete_tracker(qpair, tr, cpl, true);
}

static uint32_t s2_work(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s);
static void s2_check_timeout(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s);
static void s2_vtophys_failures(struct spdk_nvme_qpair *qpair);

/*
 * The S2 Mode 0 CQ loop of one IO qpair (an ENABLED member's direct poll in the group pass; inlined twice): the cold work
 * first (reset leftovers, abort callbacks: the qpair's completion context); then, per CQE, its dword 3 once (cid,
 * phase, status), the phase test, the cursor advance (the stock order: the head before the callback), and for a fast cid
 * with a success status and no mark the cookie's callback after its cid, credit and outstanding count are back (no
 * request); anything else s2_cpl_cold() / s2_cpl_tracker(). The SQ head is not tracked: fast and tracker cids together
 * are fewer than the SQ's entries, so the SQ cannot overflow (stock only logs it). Tail as stock's: the CQ-head doorbell
 * if anything completed, the SQ doorbell batching (delay_cmd_submit), the timeouts, pending vtophys failures.
 */
static inline __attribute__((always_inline)) int32_t
s2_poll(struct spdk_nvme_qpair *qpair, struct nvme_pcie_qpair *pqpair, uint32_t max_completions)
{
	struct nvme_iau_s2_qpair *s = nvme_iau_s2_qp(qpair);
	uint32_t n = 0, extra = 0;

	if (spdk_unlikely(s->work != 0)) {
		extra = s2_work(qpair, s);
	}
	if (max_completions == 0 || max_completions > pqpair->max_completions_cap) {
		max_completions = pqpair->max_completions_cap;
	}
	pqpair->stat->polls++;
	for (;;) {
		struct spdk_nvme_cpl *cpl = &pqpair->cpl[pqpair->cq_head];
		uint32_t dw3 = ((volatile uint32_t *)cpl)[3];
		uint32_t x = (dw3 >> 16) ^ pqpair->flags.phase;
		uint16_t cid = (uint16_t)dw3;

		if (x & 1u) {
			break;	/* no new CQE */
		}
		nvme_iau_rec_rmb();	/* the phase before the other words on weak ISAs, as stock (empty on x86) */
		if (spdk_unlikely(++pqpair->cq_head == pqpair->num_entries)) {
			pqpair->cq_head = 0;
			pqpair->flags.phase = !pqpair->flags.phase;
		}
		if (spdk_likely(cid < s->fast_cids)) {
			if (spdk_unlikely(((x & NVME_IAU_S2_ST_SCT_SC) | s->special) != 0 || SPDK_DEBUGLOG_FLAG_ENABLED("nvme"))) {
				s2_cpl_cold(qpair, s, cpl, cid);
			} else {
				const struct nvme_iau_s2_hot *h = &s->hot[cid];
				spdk_nvme_cmd_cb cb_fn = (spdk_nvme_cmd_cb)(uintptr_t)h->cb_fn;
				void *cb_arg = (void *)(uintptr_t)h->cb_arg;

				/* P4.11b (I1): one unsigned compare tests both the idle sentinel (a stray CQE) and a NULL
				 * cb_fn (stock's skip): both go cold */
				if (spdk_unlikely((uintptr_t)cb_fn <= NVME_IAU_S2_IDLE)) {
					s2_cpl_cold(qpair, s, cpl, cid);
				} else {
					s2_free_cid(qpair, s, cid);
					cb_fn(cb_arg, cpl);
				}
			}
		} else {
			s2_cpl_tracker(qpair, s, cpl, cid);
		}
		if (++n == max_completions) {
			break;
		}
	}
	if (n != 0) {
		pqpair->stat->completions += n;
		s->st.cpl += n;
		s2_cq_doorbell(pqpair);
	} else {
		pqpair->stat->idle_polls++;
	}
	if (pqpair->flags.delay_cmd_submit && pqpair->last_sq_tail != pqpair->sq_tail) {
		s2_sq_doorbell(qpair, pqpair);
		pqpair->last_sq_tail = pqpair->sq_tail;
	}
	if (spdk_unlikely(qpair->ctrlr->timeout_enabled)) {
		s2_check_timeout(qpair, s);
	}
	if (spdk_unlikely(pqpair->flags.has_pending_vtophys_failures)) {
		s2_vtophys_failures(qpair);
	}
	return (int32_t)(n + extra);
}

/* the stock connect state machine of a CONNECTING IO qpair (nvme_pcie_qpair_process_completions_stock()) */
static int32_t __attribute__((noinline))
s2_connecting(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	int rc;

	if (pq->pcie_state == NVME_PCIE_QPAIR_READY) {
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
	} else if (pq->pcie_state == NVME_PCIE_QPAIR_FAILED) {
		nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
		return -ENXIO;
	} else {
		rc = spdk_nvme_qpair_process_completions(qpair->ctrlr->adminq, 0);
		if (rc < 0) {
			return rc;
		} else if (pq->pcie_state == NVME_PCIE_QPAIR_FAILED) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
			return -ENXIO;
		}
	}
	return 0;
}

/* the transport poll (qpair_process_completions): the admin queue is stock's */
static int32_t NVME_IAU_S2_HOT_ALIGN
nvme_iau_s2_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		return g_s2_stock.qpair_process_completions(qpair, max_completions);
	}
	if (spdk_unlikely(pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED)) {
		return -ENXIO;
	}
	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		return s2_connecting(qpair);
	}
	return s2_poll(qpair, pqpair, max_completions);
}

/* ------------------------------------------------------------------------------------------ cold paths */

/* the busy fast cids, a bitmap: every cid not on the free stack (the authority; no per-completion store keeps a state) */
#define S2_MAP_WORDS	((UINT16_MAX + 1) / 64)
static void
s2_busy(const struct nvme_iau_s2_qpair *s, uint64_t *map)
{
	uint32_t i;

	memset(map, 0xFF, ((size_t)s->fast_cids + 63) / 64 * sizeof(uint64_t));
	for (i = 0; i < s->cid_top; i++) {
		map[s->cid_stack[i] >> 6] &= ~(1ull << (s->cid_stack[i] & 63));
	}
}

static inline bool
s2_is_busy(const uint64_t *map, uint16_t cid)
{
	return (map[cid >> 6] >> (cid & 63)) & 1u;
}

/* fast cid ends without its CQE (a teardown, a reset, a removed controller): stock's abort_trackers() status and print
 * (ABORTED_BY_REQUEST, dnr as given; "aborting outstanding command" and the command / completion printed), once */
static void
s2_end_cookie(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s, uint16_t cid, uint32_t dnr)
{
	struct nvme_iau_s2_cold *c = &s->cold[cid];
	spdk_nvme_cmd_cb cb_fn = (spdk_nvme_cmd_cb)(uintptr_t)s->hot[cid].cb_fn;
	void *cb_arg = (void *)(uintptr_t)s->hot[cid].cb_arg;
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;

	if (c->bits & NVME_IAU_S2_B_ABORT) {
		s->special--;
	}
	c->bits = 0;
	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.cid = cid;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	cpl.status.dnr = dnr;
	if (!qpair->ctrlr->opts.disable_error_logging) {
		NVME_QPAIR_ERRLOG(qpair, "aborting outstanding command\n");
	}
	s2_cmd(nvme_pcie_qpair(qpair), s, cid, &cmd);
	(void)nvme_iau_cpl_error_policy(qpair, &cmd, UINT8_MAX, 0, &cpl, true);
	s->st.leftovers++;
	s2_free_cid(qpair, s, cid);
	if (cb_fn != NULL) {
		cb_fn(cb_arg, &cpl);
	}
}

/* the tracker marks go with the trackers stock's abort walk ends */
static void
s2_trk_unmark_all(struct nvme_iau_s2_qpair *s)
{
	uint32_t t;

	for (t = 0; t < NVME_IAU_S2_TRACKER_CIDS; t++) {
		if (s->trk_mark[t]) {
			s->trk_mark[t] = 0;
			s->special--;
		}
	}
}

/* every command in flight ends once (a delete, a removed controller): the trackers by stock's abort walk
 * (nvme_pcie_qpair_abort_trackers(), F3's order: trackers, then the request-free IO), then every busy fast cid (a
 * snapshot taken before any callback); stats count them */
static uint32_t
s2_end_all(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s, uint32_t dnr)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr;
	uint64_t busy[S2_MAP_WORDS];
	uint32_t n = 0, ntr = 0;
	uint16_t cid;

	s2_busy(s, busy);
	TAILQ_FOREACH(tr, &pq->outstanding_tr, tq_list) {
		ntr++;
	}
	s2_trk_unmark_all(s);
	s->owed = 0;
	s->owed_last = NULL;
	if (ntr != 0) {
		nvme_pcie_qpair_abort_trackers(qpair, dnr);
		s->st.leftovers += ntr;
	}
	for (cid = 0; cid < s->fast_cids; cid++) {
		if (s2_is_busy(busy, cid)) {
			s2_end_cookie(qpair, s, cid, dnr);
			n++;
		}
	}
	return n + ntr;
}

/* the reset leftovers marked at a reconnect (s2_owe()) end now, in a completion context (the qpair's own set, as tile's
 * route_lost_end() guard: a callback's free of the qpair is deferred): the trackers in flight then (stock's abort walk up
 * to the snapshot; the ones submitted since are behind it), then the fast cids marked OWED; stock PCIe's reset status
 * (nvme_qpair_check_enabled() -> qpair_abort_reqs(abort_dnr): ABORTED_BY_REQUEST, abort_dnr) */
static void
s2_owed_end(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *tmp, *last = s->owed_last;
	uint8_t icc = qpair->in_completion_context;
	uint16_t cid;

	s->owed_last = NULL;
	qpair->in_completion_context = 1;
	if (last != NULL) {
		TAILQ_FOREACH_SAFE(tr, &pq->outstanding_tr, tq_list, tmp) {
			uint32_t t = (uint32_t)tr->cid - s->fast_cids;

			if (t < NVME_IAU_S2_TRACKER_CIDS && s->trk_mark[t]) {
				s->trk_mark[t] = 0;
				s->special--;
			}
			if (!qpair->ctrlr->opts.disable_error_logging) {
				NVME_QPAIR_ERRLOG(qpair, "aborting outstanding command\n");
			}
			s->st.leftovers++;
			nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_BY_REQUEST,
								qpair->abort_dnr, true);
			if (tr == last) {
				break;
			}
		}
	}
	for (cid = 0; cid < s->fast_cids && s->owed != 0; cid++) {
		if (s->cold[cid].bits & NVME_IAU_S2_B_OWED) {
			s->owed--;
			s2_end_cookie(qpair, s, cid, qpair->abort_dnr);
		}
	}
	qpair->in_completion_context = icc;
}

/* an abort's callback: cdw0 bit0 = 1 if nothing was marked (DV6), as stock's abort_ext parent (status 0) */
static void
s2_abort_cb(struct nvme_iau_s2_abort *a)
{
	struct spdk_nvme_cpl cpl;

	memset(&cpl, 0, sizeof(cpl));
	cpl.cdw0 = a->marked != 0 ? 0 : 1;
	if (a->cb_fn != NULL) {
		a->cb_fn(a->cb_arg, &cpl);
	}
	free(a);
}

static void
s2_abort_done_all(struct nvme_iau_s2_qpair *s)
{
	struct nvme_iau_s2_abort *a;

	while ((a = TAILQ_FIRST(&s->abort_done)) != NULL) {
		TAILQ_REMOVE(&s->abort_done, a, link);
		s2_abort_cb(a);
	}
}

/* the free trackers: the resubmission budget of a member's attention call while it has queued requests (as tile's
 * shared_poll() returns its free credits there) */
static uint32_t
s2_free_trackers(struct nvme_pcie_qpair *pq)
{
	struct nvme_tracker *tr;
	uint32_t n = 0;

	TAILQ_FOREACH(tr, &pq->free_tr, tq_list) {
		n++;
	}
	return n;
}

/* a poll's cold work (s->work): the reset leftovers, the abort callbacks (callbacks may queue more: run until none), and
 * an attention call's resubmission budget */
static uint32_t __attribute__((noinline))
s2_work(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s)
{
	uint8_t w = s->work;

	s->work = 0;
	if (s->owed != 0 || s->owed_last != NULL) {
		s2_owed_end(qpair, s);
	}
	s2_abort_done_all(s);
	if ((w & NVME_IAU_S2_W_ATTN) && !STAILQ_EMPTY(&qpair->queued_req)) {
		return s2_free_trackers(nvme_pcie_qpair(qpair));
	}
	return 0;
}

/*
 * The timeouts (ctrlr->timeout_enabled, at the poll's tail): not while the controller initializes, only with the
 * process's timeout callback; the trackers by stock's walk (nvme_pcie_qpair_check_timeout(): submit order, the first not
 * expired stops it), then, with the timeout latched at connect (D9 / DV7 as tile), every busy fast cid whose submit_tick is
 * older than the latched timeout and whose notice was not given (B_TMO): one timeout_cb per command lifetime, cid = the
 * fast cid (a retry keeps submit_tick and the bit). The busy set is a snapshot (a notice's callback may abort, not free)
 */
static void __attribute__((noinline))
s2_check_timeout(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr_process *p = qpair->active_proc;
	struct nvme_tracker *tr, *tmp;
	uint64_t busy[S2_MAP_WORDS];
	uint64_t now;
	uint16_t cid;

	if (qpair->ctrlr->state != NVME_CTRLR_STATE_READY || p == NULL || p->timeout_cb_fn == NULL) {
		return;
	}
	now = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tr, &pq->outstanding_tr, tq_list, tmp) {
		assert(tr->req != NULL);
		if (nvme_request_check_timeout(tr->req, tr->cid, p, now)) {
			break;
		}
	}
	if (!s->tmo_on || s->cid_top == s->fast_cids) {
		return;
	}
	s2_busy(s, busy);
	for (cid = 0; cid < s->fast_cids; cid++) {
		struct nvme_iau_s2_cold *c = &s->cold[cid];

		if (!s2_is_busy(busy, cid) || (c->bits & NVME_IAU_S2_B_TMO) != 0 || c->submit_tick + s->tmo_ticks > now) {
			continue;
		}
		c->bits |= NVME_IAU_S2_B_TMO;
		s->st.timeouts++;
		p->timeout_cb_fn(p->timeout_cb_arg, qpair->ctrlr, qpair, cid);
	}
}

/* nvme_pcie_fail_request_bad_vtophys() in a completion context, for the trackers stock's submit deferred */
static void __attribute__((noinline))
s2_vtophys_failures(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *tmp;

	TAILQ_FOREACH_SAFE(tr, &pq->outstanding_tr, tq_list, tmp) {
		if (tr->bad_vtophys) {
			tr->bad_vtophys = 0;
			NVME_QPAIR_ERRLOG(qpair, "vtophys or other payload buffer related error\n");
			nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD, 1,
								true);
		}
	}
	pq->flags.has_pending_vtophys_failures = 0;
}

/* ------------------------------------------------------------------------------------------ qpair ops */

/* the timeout (D9 / DV7, as tile's latch_timeout()) and the request-free path (D14, as tile's latch_fast(): a namespace
 * without metadata, PI or extended LBA, page size 4096, fast cids), latched at every connect */
static void
s2_latch(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *p = qpair->active_proc;
	struct spdk_nvme_ns *ns = s->nsid != 0 ? spdk_nvme_ctrlr_get_ns(ctrlr, s->nsid) : NULL;
	uint64_t us;

	s->tmo_on = 0;
	s->tmo_ticks = 0;
	s->tmo_us = 0;
	if (ctrlr->timeout_enabled && p != NULL && p->timeout_cb_fn != NULL && p->timeout_io_ticks != 0) {
		s->tmo_on = 1;
		s->tmo_ticks = p->timeout_io_ticks;
		us = p->timeout_io_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
		s->tmo_us = us == 0 ? 1 : us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
	}
	qpair->iau_fast = 0;
	s->fast_nlb = 0;
	s->stripe = 0;
	if (s->fast_cids == 0 || ns == NULL || ns->md_size != 0 || (ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) != 0 ||
	    ns->extended_lba_size != ns->sector_size || ctrlr->page_size != IAU_TILE_PAGE ||
	    nvme_iau_nlb_lim(s->nsid, s->lba_shift) == 0) {
		return;
	}
	s->fast_nlb = spdk_min(nvme_iau_nlb_lim(s->nsid, s->lba_shift), ns->sectors_per_max_io);
	s->stripe = ns->sectors_per_stripe;
	qpair->iau_fast = s->fast_nlb != 0;
}

static void
s2_print_qpair(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s)
{
	fprintf(stderr, "IAU_S2_QPAIR qid=%u fast_cids=%u trackers=%u nsid=%u fast=%u fast_nlb=%u tmo_us=%u pg=%d sqe=%s\n",
		qpair->id, s->fast_cids, (uint32_t)(nvme_pcie_qpair(qpair)->num_entries -
				nvme_pcie_qpair(qpair)->max_completions_cap - s->fast_cids), s->nsid, qpair->iau_fast,
		s->fast_nlb, s->tmo_us, s->pg != NULL ? (int)s->pg->id : -1, g_nvme_iau_cfg.s2_wb ? "wb" : "nt");
}

static void
s2_free_tables(struct nvme_iau_s2_qpair *s)
{
	free(s->hot);	/* one allocation: hot, cold, the cid stack */
	s->hot = NULL;
	s->cold = NULL;
	s->cid_stack = NULL;
	s->fast_cids = 0;
	s->cid_top = 0;
}

/* stock's create (nvme_pcie_ctrlr_create_io_qpair()) with room for the S2 state, then the cid split (D15): fast cids
 * [0, F) leave free_tr, their cookies and stack in one 64 B-aligned allocation; F = num_trackers - 16 (0 if smaller) */
static struct spdk_nvme_qpair *
s2_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid, const struct spdk_nvme_io_qpair_opts *opts)
{
	struct nvme_iau_s2_pqpair *x;
	struct nvme_pcie_qpair *pq;
	struct spdk_nvme_qpair *qpair;
	struct nvme_iau_s2_qpair *s;
	struct spdk_nvme_ns *ns;
	uint32_t ntr, f, i, nsid;
	size_t hb, cb;
	const char *why = NULL;
	void *mem;
	int rc;

	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_VFIOUSER) {
		why = "not a vfio-user controller (the testbed transport; A5 and R4 rely on its check_enabled())";
	} else if (ctrlr->shadow_doorbell != NULL) {
		why = "shadow doorbells (S2 rings the doorbells itself)";
	}
	if (why != NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "IAU_S2_REFUSED qid=%u: %s\n", qid, why);
		errno = ENOTSUP;
		return NULL;
	}
	x = spdk_zmalloc(sizeof(*x), 64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	if (x == NULL) {
		return NULL;
	}
	pq = &x->pq;
	pq->num_entries = opts->io_queue_size;
	pq->flags.delay_cmd_submit = opts->delay_cmd_submit;
	qpair = &pq->qpair;
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
	s = &x->s2;
	s->qpair = qpair;
	TAILQ_INIT(&s->abort_done);
	ntr = pq->num_entries - pq->max_completions_cap;
	f = ntr > NVME_IAU_S2_TRACKER_CIDS ? ntr - NVME_IAU_S2_TRACKER_CIDS : 0;
	if (f != 0) {
		hb = ((size_t)f * sizeof(struct nvme_iau_s2_hot) + 63) & ~(size_t)63;
		cb = ((size_t)f * sizeof(struct nvme_iau_s2_cold) + 63) & ~(size_t)63;
		if (posix_memalign(&mem, 64, hb + cb + (size_t)f * sizeof(uint16_t)) != 0) {
			nvme_pcie_qpair_destroy(qpair);
			return NULL;
		}
		memset(mem, 0, hb + cb + (size_t)f * sizeof(uint16_t));
		s->hot = mem;
		s->cold = (struct nvme_iau_s2_cold *)((uint8_t *)mem + hb);
		s->cid_stack = (uint16_t *)((uint8_t *)mem + hb + cb);
		for (i = 0; i < f; i++) {
			s->cid_stack[i] = (uint16_t)(f - 1 - i);	/* the first pops hand out 0, 1, 2, ... */
			s->hot[i].cb_fn = NVME_IAU_S2_IDLE;	/* P4.11b (I1): free */
			TAILQ_REMOVE(&pq->free_tr, &pq->tr[i], tq_list);	/* free_tr holds only the tracker cids */
		}
	}
	s->fast_cids = (uint16_t)f;
	s->cid_top = (uint16_t)f;
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	ns = nsid != 0 ? spdk_nvme_ctrlr_get_ns(ctrlr, nsid) : NULL;
	if (ns != NULL && ns->sector_size >= 512 && (ns->sector_size & (ns->sector_size - 1)) == 0) {
		s->nsid = nsid;
		s->lba_shift = (uint8_t)__builtin_ctz(ns->sector_size);
	}
	qpair->iau_kind = NVME_IAU_KIND_S2;
	return qpair;
}

/* the commands in flight on an S2 IO qpair (fast cids busy + trackers outstanding) */
static uint32_t
s2_inflight(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s)
{
	struct nvme_tracker *tr;
	uint32_t n = (uint32_t)s->fast_cids - s->cid_top;

	TAILQ_FOREACH(tr, &nvme_pcie_qpair(qpair)->outstanding_tr, tq_list) {
		n++;
	}
	return n;
}

/* a reset's reconnect: what is in flight was dropped by the controller's disable; it ends once in the next completion
 * context (s2_owed_end()), never here (no callback inside a connect op, as stock); a second reconnect before that adds
 * what was submitted since */
static void
s2_owe(struct spdk_nvme_qpair *qpair, struct nvme_iau_s2_qpair *s)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	uint64_t busy[S2_MAP_WORDS];
	uint16_t cid;

	s2_busy(s, busy);
	for (cid = 0; cid < s->fast_cids; cid++) {
		if (s2_is_busy(busy, cid) && (s->cold[cid].bits & NVME_IAU_S2_B_OWED) == 0) {
			s->cold[cid].bits |= NVME_IAU_S2_B_OWED;
			s->owed++;
		}
	}
	if (!TAILQ_EMPTY(&pq->outstanding_tr)) {
		s->owed_last = TAILQ_LAST(&pq->outstanding_tr, nvme_outstanding_tr_head);
	}
	s->work |= NVME_IAU_S2_W_OWED;
}

/*
 * connect: the first one latches the timeout and the request-free path; a reconnect (a reset's
 * nvme_ctrlr_reinitialize_io_qpair(), or check_enabled()'s for FAILURE_RESET) with commands in flight is allowed only for a
 * controller reset whose disable is done (is_resetting with is_disconnecting clear, or the FAILURE_RESET reinitialize):
 * those commands are owed (s2_owe()); otherwise the device may still hold them (DV10: stock vfio-user keeps the SSD queues
 * across a disconnect) and the reconnect is refused (IAU_S2_RESET_REFUSED, -EBUSY), as tile refuses a live route's. A poll
 * group member's reconnect inside a reset is deferred to its own poll (n4, as tile's shared member): FAILURE_RESET,
 * CONNECTED, rc 0, nothing else touched on the resetting thread. A member's successful connect goes on the attention list
 * (its generic calls drive it to ENABLED, D12)
 */
static int
s2_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_iau_s2_qpair *s;
	uint32_t inflight;
	int rc;

	if (nvme_qpair_is_admin_queue(qpair)) {
		return g_s2_stock.ctrlr_connect_qpair(ctrlr, qpair);
	}
	s = nvme_iau_s2_qp(qpair);
	if (s->connected) {
		if (s->pg != NULL && ctrlr->is_resetting) {
			fprintf(stderr, "IAU_S2_RESET_DEFERRED qid=%u pg=%u\n", qpair->id, s->pg->id);
			s->reset_deferred = 1;
			qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_RESET;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
			return 0;
		}
		inflight = s2_inflight(qpair, s);
		if (inflight != 0) {
			if (!(ctrlr->is_resetting && !ctrlr->is_disconnecting) &&
			    qpair->last_transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_RESET) {
				NVME_QPAIR_ERRLOG(qpair, "IAU_S2_RESET_REFUSED qid=%u inflight=%u: a reconnect outside a controller "
						  "reset with commands in flight (the device may still hold them)\n", qpair->id, inflight);
				return -EBUSY;
			}
			s2_owe(qpair, s);
		}
		s->reset_deferred = 0;
		s->st.rejoins++;
	}
	s2_latch(qpair, s);
	s2_print_qpair(qpair, s);
	s->connected = 1;
	rc = g_s2_stock.ctrlr_connect_qpair(ctrlr, qpair);
	if (rc == 0 && s->pg != NULL && !ctrlr->is_resetting) {
		s2_attn(s->pg, s);
	}
	return rc;
}

/* disconnect: stock's (the SSD queues persist, F6; in-flight IO ends at the delete); a disconnected qpair takes no
 * request-free IO (iau_fast 0, as tile's M8; the connect re-latches it) */
static void
s2_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	if (!nvme_qpair_is_admin_queue(qpair)) {
		qpair->iau_fast = 0;
	}
	g_s2_stock.ctrlr_disconnect_qpair(ctrlr, qpair);
}

/* a removed controller (qpair_abort_reqs): every command in flight ends once, stock's status, dnr as given; the abort
 * callbacks run */
static void
s2_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_iau_s2_qpair *s;

	if (nvme_qpair_is_admin_queue(qpair)) {
		g_s2_stock.qpair_abort_reqs(qpair, dnr);
		return;
	}
	s = nvme_iau_s2_qp(qpair);
	(void)s2_end_all(qpair, s, dnr);
	s2_abort_done_all(s);
}

#define S2_QSTAT_FMT(n) " " #n "=%" PRIu64
#define S2_QSTAT_ARG(n) , s->st.n

/* a member leaves its poll group (remove, or its delete) */
static void
s2_leave_pg(struct nvme_iau_s2_qpair *s)
{
	struct nvme_iau_s2_pg *pg = s->pg;

	if (pg == NULL) {
		return;
	}
	TAILQ_REMOVE(&pg->members, s, link);
	if (s->in_attn) {
		TAILQ_REMOVE(s->attn_list, s, attn_link);	/* pg->attn or an attention pass's local list (A3) */
		s->in_attn = 0;
	}
	if (pg->attn_cur == s) {
		pg->attn_cur = NULL;	/* left inside its attention call: the pass must not touch it (A2) */
	}
	fprintf(stderr, "IAU_S2_PG_END pg=%u qid=%u polls=%" PRIu64 " idle_polls=%" PRIu64 " sweeps=%" PRIu64
		" attn_calls=%" PRIu64 "\n", pg->id, s->qpair->id, pg->polls, pg->idle_polls, pg->sweeps, pg->attn_calls);
	s->pg = NULL;
}

/*
 * P4.11b (the P4.11 review's m7, as tile's reset_disable(), P4.10d / DV5): a delete during a controller reset with commands
 * in flight waits for the reset's disable before they end, so no buffer goes back while the SSD may still DMA into it.
 * Done: is_disconnecting clear (nvme_ctrlr_disconnect_done() ran) or the admin queue DISCONNECTED (the disable finished,
 * CSTS.RDY = 0 seen). Before that (is_disconnecting, the admin queue DISCONNECTING) this delete polls the admin queue,
 * which runs the disable (nvme_ctrlr_disable_poll()) as the resetting context would, under the controller lock the
 * delete holds, until DISCONNECTED or the drain bound (g_nvme_iau_drain_us, 1 s). One IAU_S2_RESET_DISABLE line per
 * wait (done=0|1, polls). Not done (the bound, or is_disconnecting with the admin queue in neither state, which stock
 * never has): the commands end anyway, as stock ends them (S2 holds every cookie, so nothing is withheld or lost; tile
 * withholds a fetched request-free IO there because its cookie is in the IAU's slot: DV35). Cold: the delete only
 */
static bool
s2_reset_disable(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_qpair *aq = ctrlr->adminq;
	uint64_t deadline;
	uint32_t polls = 0;
	bool done;

	if (!ctrlr->is_disconnecting || nvme_qpair_get_state(aq) == NVME_QPAIR_DISCONNECTED) {
		return true;	/* the disable is done */
	}
	if (nvme_qpair_get_state(aq) != NVME_QPAIR_DISCONNECTING) {
		fprintf(stderr, "IAU_S2_RESET_DISABLE qid=%u done=0 polls=0\n", qpair->id);
		return false;
	}
	deadline = spdk_get_ticks() + spdk_get_ticks_hz() * g_nvme_iau_drain_us / SPDK_SEC_TO_USEC;
	while (nvme_qpair_get_state(aq) == NVME_QPAIR_DISCONNECTING && spdk_get_ticks() <= deadline) {
		(void)spdk_nvme_qpair_process_completions(aq, 0);
		polls++;
		if (nvme_qpair_get_state(aq) == NVME_QPAIR_DISCONNECTING) {
			spdk_delay_us(1);
		}
	}
	done = nvme_qpair_get_state(aq) == NVME_QPAIR_DISCONNECTED;
	fprintf(stderr, "IAU_S2_RESET_DISABLE qid=%u done=%u polls=%u\n", qpair->id, done, polls);
	return done;
}

/*
 * delete (D11-style, stock's sequence with the S2 poll): a removed controller or one being reset sends no admin command
 * (P4.11b: inside a reset, with commands in flight, it first waits for the reset's disable, s2_reset_disable());
 * else a connect in progress finishes, Delete I/O SQ (wait: the target stops the SQ and drops what it had scheduled, F5),
 * the S2 poll consumes the CQEs it posted (fast cids by cookie: no "does not map"; under the qpair's own completion
 * context, so a callback's free of it is deferred and never recurses), Delete I/O CQ (wait); then every command still in
 * flight ends once (s2_end_all(), stock's status) unless the owner died (no_deletion_notification_needed, as stock), the
 * abort callbacks run, IAU_S2_STATS, the poll group left, the destroy
 */
static int
s2_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	struct nvme_iau_s2_qpair *s = nvme_iau_s2_qp(qpair);
	struct nvme_completion_poll_status *status;
	uint32_t ended = 0, lost = 0, i;
	uint8_t icc;
	int rc;

	if (ctrlr->is_removed) {
		goto free;
	}
	if (ctrlr->prepare_for_reset) {
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
			pq->flags.defer_destruction = true;
		}
		if (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr) && s2_inflight(qpair, s) != 0) {
			(void)s2_reset_disable(qpair);	/* P4.11b (m7): the commands end after the disable */
		}
		goto free;
	}
	while (pq->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ || pq->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ) {
		if (spdk_nvme_qpair_process_completions(ctrlr->adminq, 0) < 0) {
			break;
		}
	}
	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to allocate status tracker\n");
		goto free;
	}
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_sq with rc=%d\n", rc);
		free(status);
		goto free;
	}
	rc = nvme_wait_for_adminq_completion(ctrlr, status, false);
	if (rc != 0) {
		if (!status->timed_out) {
			free(status);
		}
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_pcie_ctrlr_cmd_delete_io_sq failed: rc=%d\n", rc);
		goto free;
	}
	if (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr) && pq->pcie_state != NVME_PCIE_QPAIR_FAILED) {
		icc = qpair->in_completion_context;
		qpair->in_completion_context = 1;
		for (i = 0; i <= pq->num_entries &&
		     nvme_iau_s2_process_completions(qpair, 0) >= (int32_t)pq->max_completions_cap; i++) {
		}
		qpair->in_completion_context = icc;
	}
	memset(status, 0, sizeof(*status));
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to send request to delete_io_cq with rc=%d\n", rc);
		free(status);
		goto free;
	}
	rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "wait for nvme_pcie_ctrlr_cmd_delete_io_cq failed: rc=%d\n", rc);
	}
free:
	icc = qpair->in_completion_context;
	qpair->in_completion_context = 1;
	if (qpair->no_deletion_notification_needed == 0) {
		ended = s2_end_all(qpair, s, 1);
	} else {
		lost = s2_inflight(qpair, s);	/* the owner died: no callback, as stock */
	}
	s2_abort_done_all(s);
	qpair->in_completion_context = icc;
	fprintf(stderr, "IAU_S2_STATS qid=%u fast_cids=%u" NVME_IAU_S2_QSTATS(S2_QSTAT_FMT) " ended=%u lost=%u special=%u\n",
		qpair->id, s->fast_cids NVME_IAU_S2_QSTATS(S2_QSTAT_ARG), ended, lost, s->special);
	s2_leave_pg(s);
	s2_free_tables(s);
	if (!pq->flags.defer_destruction) {
		nvme_pcie_qpair_destroy(qpair);
	}
	return 0;
}

/* stock's PCIe iterate (nvme_pcie.c nvme_pcie_qpair_iterate_requests(); stock vfio-user registers none): every
 * outstanding tracker's request; request-free IO has none */
static int
s2_iterate_requests(struct spdk_nvme_qpair *qpair, int (*iter_fn)(struct nvme_request *req, void *arg), void *arg)
{
	struct nvme_tracker *tr, *tmp;
	int rc;

	TAILQ_FOREACH_SAFE(tr, &nvme_pcie_qpair(qpair)->outstanding_tr, tq_list, tmp) {
		assert(tr->req != NULL);
		rc = iter_fn(tr->req, arg);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------ late abort (D7, D10) */

static bool
s2_live(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	enum nvme_qpair_state st = nvme_qpair_get_state(qpair);

	return !ctrlr->is_removed && (st == NVME_QPAIR_CONNECTING || st == NVME_QPAIR_CONNECTED ||
				       st == NVME_QPAIR_ENABLING || st == NVME_QPAIR_ENABLED);
}

/* the abort's callback, owed to the qpair's next poll (s2_work()), as tile's ack record comes from a later poll */
static int
s2_abort_queue(struct nvme_iau_s2_qpair *s, uint32_t marked, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_iau_s2_abort *a = calloc(1, sizeof(*a));

	if (a == NULL) {
		return -ENOMEM;
	}
	a->cb_fn = cb_fn;
	a->cb_arg = cb_arg;
	a->marked = marked;
	TAILQ_INSERT_TAIL(&s->abort_done, a, link);
	s->work |= NVME_IAU_S2_W_ABORT;
	s->st.aborts++;
	return 0;
}

static void
s2_mark_cookie(struct nvme_iau_s2_qpair *s, uint16_t cid)
{
	s->cold[cid].bits |= NVME_IAU_S2_B_ABORT;
	s->special++;
	s->st.marked++;
}

static void
s2_mark_tracker(struct nvme_iau_s2_qpair *s, uint32_t t)
{
	s->trk_mark[t] = 1;
	s->special++;
	s->st.marked++;
}

/*
 * spdk_nvme_ctrlr_cmd_abort_ext() on an S2 qpair (cmd_cb_arg != NULL): the queued requests matching cmd_cb_arg are aborted
 * in software as stock does (counted as marked; a member goes on its attention list for their completion), then every busy
 * fast cid whose cookie's cb_arg is cmd_cb_arg and every tracker request in flight that nvme_request_abort_match() takes is
 * marked (not twice): their CQEs are delivered ABORTED_BY_REQUEST. rc 0 also when nothing matches (DV6); the callback
 * comes from the qpair's next poll with cdw0 bit0 = 1 if nothing was marked. A qpair that takes no command (or a removed
 * controller): stock's result, as tile's (the callback at once if queued requests matched, else -ENOENT)
 */
int
nvme_iau_s2_cmd_abort_ext(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, void *cmd_cb_arg,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_iau_s2_qpair *s = nvme_iau_s2_qp(qpair);
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	uint64_t busy[S2_MAP_WORDS];
	struct nvme_tracker *tr;
	struct spdk_nvme_cpl cpl;
	uint32_t marked, t;
	uint16_t cid;

	if (!s2_live(ctrlr, qpair)) {
		if (nvme_qpair_abort_queued_reqs_with_cbarg(qpair, cmd_cb_arg) == 0) {
			return -ENOENT;
		}
		memset(&cpl, 0, sizeof(cpl));
		if (cb_fn != NULL) {
			cb_fn(cb_arg, &cpl);
		}
		return 0;
	}
	marked = nvme_qpair_abort_queued_reqs_with_cbarg(qpair, cmd_cb_arg);
	if (marked != 0 && s->pg != NULL) {
		s2_attn(s->pg, s);	/* the attention pass's generic call completes them (D12) */
	}
	s2_busy(s, busy);
	for (cid = 0; cid < s->fast_cids; cid++) {
		if (s2_is_busy(busy, cid) && (s->cold[cid].bits & NVME_IAU_S2_B_ABORT) == 0 &&
		    s->hot[cid].cb_arg == (uint64_t)(uintptr_t)cmd_cb_arg) {
			s2_mark_cookie(s, cid);
			marked++;
		}
	}
	TAILQ_FOREACH(tr, &pq->outstanding_tr, tq_list) {
		t = (uint32_t)tr->cid - s->fast_cids;
		if (t < NVME_IAU_S2_TRACKER_CIDS && !s->trk_mark[t] && nvme_request_abort_match(tr->req, cmd_cb_arg)) {
			s2_mark_tracker(s, t);
			marked++;
		}
	}
	return s2_abort_queue(s, marked, cb_fn, cb_arg);
}

/* spdk_nvme_ctrlr_cmd_abort() on an S2 qpair: cid a fast cid (a timeout notice's) or a tracker cid: the command in flight
 * on it is marked (as the IAU's ABORT_TAG, without its gen: stock's cid has none); rc 0 and the callback from the next
 * poll. A qpair that takes no command (or a removed controller): -ENXIO, no NVMe Abort goes to the SSD (DV5) */
int
nvme_iau_s2_cmd_abort(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, uint16_t cid, spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_iau_s2_qpair *s = nvme_iau_s2_qp(qpair);
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qpair);
	uint32_t marked = 0, t = (uint32_t)cid - s->fast_cids, i;
	bool busy = cid < s->fast_cids;

	if (!s2_live(ctrlr, qpair)) {
		return -ENXIO;
	}
	if (cid < s->fast_cids) {
		for (i = 0; i < s->cid_top && busy; i++) {
			busy = s->cid_stack[i] != cid;
		}
		if (busy && (s->cold[cid].bits & NVME_IAU_S2_B_ABORT) == 0) {
			s2_mark_cookie(s, cid);
			marked = 1;
		}
	} else if (t < NVME_IAU_S2_TRACKER_CIDS && cid < pq->num_entries - pq->max_completions_cap &&
		   pq->tr[cid].req != NULL && !s->trk_mark[t]) {
		s2_mark_tracker(s, t);
		marked = 1;
	}
	return s2_abort_queue(s, marked, cb_fn, cb_arg);
}

/* ------------------------------------------------------------------------------------------ poll-group ops */

static struct spdk_nvme_transport_poll_group *
s2_pg_create(void)
{
	struct nvme_iau_s2_pg *pg = calloc(1, sizeof(*pg));

	if (pg == NULL) {
		return NULL;
	}
	TAILQ_INIT(&pg->members);
	TAILQ_INIT(&pg->attn);
	pg->id = __atomic_fetch_add(&g_s2_next_pg, 1, __ATOMIC_RELAXED);
	return &pg->pg.group;
}

static int
s2_pg_add(struct spdk_nvme_transport_poll_group *tg, struct spdk_nvme_qpair *qpair)
{
	struct nvme_iau_s2_pg *pg = s2_pg(tg);
	struct nvme_iau_s2_qpair *s;

	if (qpair->iau_kind != NVME_IAU_KIND_S2) {
		NVME_QPAIR_ERRLOG(qpair, "IAU_S2_JOIN_REFUSED qid=%u reason=not_s2\n", qpair->id);
		return -EINVAL;	/* the group pass polls S2 members only (as tile's m7) */
	}
	s = nvme_iau_s2_qp(qpair);
	s->pg = pg;
	TAILQ_INSERT_TAIL(&pg->members, s, link);
	return g_s2_stock.poll_group_add(tg, qpair);
}

static int
s2_pg_remove(struct spdk_nvme_transport_poll_group *tg, struct spdk_nvme_qpair *qpair)
{
	s2_leave_pg(nvme_iau_s2_qp(qpair));
	return g_s2_stock.poll_group_remove(tg, qpair);
}

/* D12's predicate (tile's needs_attn()): the generic duties a member's own spdk_nvme_qpair_process_completions() does that
 * no direct poll does */
static bool
s2_needs_attn(struct spdk_nvme_qpair *qp)
{
	return nvme_qpair_get_state(qp) != NVME_QPAIR_ENABLED ||
	       qp->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE || qp->ctrlr->is_failed ||
	       !STAILQ_EMPTY(&qp->queued_req) || !STAILQ_EMPTY(&qp->aborting_queued_req) ||
	       !STAILQ_EMPTY(&qp->err_req_head) || qp->delete_after_completion_context ||
	       nvme_pcie_qpair(qp)->pcie_state == NVME_PCIE_QPAIR_FAILED;
}

/* every 64 group polls (D12) */
static void
s2_sweep(struct nvme_iau_s2_pg *pg)
{
	struct nvme_iau_s2_qpair *s;

	pg->sweeps++;
	TAILQ_FOREACH(s, &pg->members, link) {
		if (s2_needs_attn(s->qpair)) {
			s2_attn(pg, s);
		}
	}
}

/*
 * The attention pass (tile's attn_pass(), D12, A2, A3): the list moves to a local list whose entries point at it, so a
 * delete may unlink any member from whichever list holds it; per member from the head: a free a callback asked for is
 * carried out; a failure the pass saw is reported once (disconnected_qpair_cb, if still on the connected list); members
 * not CONNECTING / CONNECTED / ENABLING / ENABLED are skipped (stock's disconnected loop serves them); the others get one
 * generic call (check_enabled, the connect state machine, err_req expiry, the queued requests' resubmission with the free
 * trackers as budget: S2_W_ATTN), and nothing of a member is touched after its generic call deleted it. -ENXIO if a member
 * failed (R4), else 0
 */
static int
s2_attn_pass(struct nvme_iau_s2_pg *pg, spdk_nvme_disconnected_qpair_cb cb, void *ctx)
{
	struct nvme_iau_s2_qlist work = TAILQ_HEAD_INITIALIZER(work);
	struct nvme_iau_s2_qpair *s;
	int rc = 0;

	TAILQ_CONCAT(&work, &pg->attn, attn_link);
	TAILQ_FOREACH(s, &work, attn_link) {
		s->attn_list = &work;
	}
	pg->in_member_pass = true;
	while ((s = TAILQ_FIRST(&work)) != NULL) {
		struct spdk_nvme_qpair *qp = s->qpair;
		enum nvme_qpair_state st = nvme_qpair_get_state(qp);
		int32_t r;

		TAILQ_REMOVE(&work, s, attn_link);
		s->in_attn = 0;
		s->attn_list = NULL;
		if (qp->delete_after_completion_context) {
			pg->attn_calls++;
			spdk_nvme_ctrlr_free_io_qpair(qp);
			continue;
		}
		if (s->fail_report) {
			s->fail_report = 0;
			if (qp->poll_group_tailq_head == &pg->pg.group.connected_qpairs) {
				rc = -ENXIO;
				cb(qp, ctx);
			}
			continue;
		}
		if (st != NVME_QPAIR_CONNECTING && st != NVME_QPAIR_CONNECTED && st != NVME_QPAIR_ENABLING &&
		    st != NVME_QPAIR_ENABLED) {
			continue;
		}
		pg->attn_calls++;
		pg->attn_cur = s;
		s->work |= NVME_IAU_S2_W_ATTN;
		r = spdk_nvme_qpair_process_completions(qp, 0);
		if (pg->attn_cur != s) {
			continue;	/* deleted inside its own generic call */
		}
		pg->attn_cur = NULL;
		s->work &= (uint8_t)~NVME_IAU_S2_W_ATTN;
		if (r < 0) {
			if (qp->poll_group != &pg->pg.group) {
				continue;	/* removed from the poll group: not the group's to report */
			}
			rc = -ENXIO;
			cb(qp, ctx);	/* once per failure; the callback may free qp: nothing after it */
		} else if (!STAILQ_EMPTY(&qp->queued_req) || nvme_qpair_get_state(qp) != NVME_QPAIR_ENABLED) {
			s2_attn(pg, s);	/* still queued requests, or still connecting: the next pass again */
		}
	}
	pg->in_member_pass = false;
	return rc;
}

/*
 * The group poll (S2's override of the PCIe poll group's, D12 as tile's): stock's disconnected loop; then every connected
 * member that is ENABLED polled directly by the S2 loop (no generic wrapper) with the tgroup's in_completion_context set
 * (a free a callback asks for is deferred); a member with queued requests or a deferred free after its poll, or whose
 * poll failed, goes on the attention list; every 64 polls the sweep; the attention pass when its list is not empty.
 * Returns the deliveries, or -ENXIO if a member failed (R4; disconnected_qpair_cb ran for it). Nested inside a callback of
 * its own pass: 0
 */
static int64_t NVME_IAU_S2_HOT_ALIGN
nvme_iau_s2_pg_process_completions(struct spdk_nvme_transport_poll_group *tg, uint32_t cpq,
				   spdk_nvme_disconnected_qpair_cb cb)
{
	struct nvme_iau_s2_pg *pg = s2_pg(tg);
	struct spdk_nvme_qpair *qp, *tmp;
	int64_t total = 0;
	int32_t n;
	int rc = 0;

	STAILQ_FOREACH_SAFE(qp, &tg->disconnected_qpairs, poll_group_stailq, tmp) {
		cb(qp, tg->group->ctx);
	}
	if (spdk_unlikely(pg->in_pass || pg->in_member_pass)) {
		return 0;
	}
	pg->in_pass = true;
	tg->in_completion_context = true;
	STAILQ_FOREACH_SAFE(qp, &tg->connected_qpairs, poll_group_stailq, tmp) {
		struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qp);

		if (spdk_unlikely(nvme_qpair_get_state(qp) != NVME_QPAIR_ENABLED)) {
			continue;	/* the sweep and the attention pass serve it */
		}
		if (spdk_unlikely(pq->pcie_state == NVME_PCIE_QPAIR_FAILED)) {
			n = -ENXIO;
		} else {
			n = s2_poll(qp, pq, cpq);
		}
		if (spdk_likely(n >= 0)) {
			total += n;
		} else {
			nvme_iau_s2_qp(qp)->fail_report = 1;
			s2_attn(pg, nvme_iau_s2_qp(qp));
		}
		if (spdk_unlikely(((uintptr_t)STAILQ_FIRST(&qp->queued_req) | qp->delete_after_completion_context) != 0)) {
			s2_attn(pg, nvme_iau_s2_qp(qp));
		}
	}
	tg->in_completion_context = false;
	pg->in_pass = false;
	if (spdk_unlikely(++pg->sweep == 64)) {
		pg->sweep = 0;
		s2_sweep(pg);
	}
	if (spdk_unlikely(!TAILQ_EMPTY(&pg->attn))) {
		rc = s2_attn_pass(pg, cb, tg->group->ctx);
	}
	pg->polls++;
	if (total == 0) {
		pg->idle_polls++;
	}
	return rc != 0 ? rc : total;
}

/* -EBUSY while a member is left or inside its own pass; else stock's (which frees the group) */
static int
s2_pg_destroy(struct spdk_nvme_transport_poll_group *tg)
{
	struct nvme_iau_s2_pg *pg = s2_pg(tg);

	if (pg->in_pass || pg->in_member_pass || !TAILQ_EMPTY(&pg->members)) {
		return -EBUSY;
	}
	return g_s2_stock.poll_group_destroy(tg);
}

/* ------------------------------------------------------------------------------------------ the override */

int
nvme_iau_s2_override_ops(struct spdk_nvme_transport_ops *ops)
{
	g_s2_stock = *ops;
	ops->ctrlr_create_io_qpair = s2_create_io_qpair;
	ops->ctrlr_delete_io_qpair = s2_delete_io_qpair;
	ops->ctrlr_connect_qpair = s2_connect_qpair;
	ops->ctrlr_disconnect_qpair = s2_disconnect_qpair;
	ops->qpair_abort_reqs = s2_abort_reqs;
	ops->qpair_submit_request = nvme_iau_s2_qpair_submit_request;
	ops->qpair_process_completions = nvme_iau_s2_process_completions;
	ops->qpair_iterate_requests = s2_iterate_requests;
	ops->poll_group_create = s2_pg_create;
	ops->poll_group_add = s2_pg_add;
	ops->poll_group_remove = s2_pg_remove;
	ops->poll_group_process_completions = nvme_iau_s2_pg_process_completions;
	ops->poll_group_destroy = s2_pg_destroy;
	fprintf(stderr, "IAU_S2_OPS s2=1 tracker_cids=%u pg=1\n", NVME_IAU_S2_TRACKER_CIDS);
	return 0;
}
