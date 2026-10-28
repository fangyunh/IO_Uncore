/*   SPDX-License-Identifier: BSD-3-Clause
 *   IAU-next P4.5: the IAU host interface tile on per-qpair (private) rings: the transport ops override, the
 *   request path, the record loop (fast filter + cold kinds), park / retry / release with the pending list, the
 *   quiesced delete and the reconnect guard; P4.8 group rings, P4.9 the request-free path, P4.10a abort, the CLOSED
 *   acks, the reset deferral and the lost-IO rules.  nvme_iau.h has the model and the rules; the wire formats are
 *   spdk/include/spdk_internal/iau_tile_abi.h.
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
 * P4.10c (the P4.10d controller decision; P4.10a/b review m8): the tile hot functions start on a 64 B boundary
 * (NVME_IAU_HOT_ALIGN, as NVME_PCIE_HOT_ALIGN pins the md_poll / stock per-IO functions in nvme_pcie_common.c), so
 * their cache-line placement no longer follows the size of the cold code before them: the request submit (and its
 * 48 B lever, its full-credit and non-fast exits), the request-free submit, the private poll, the group poll and the
 * group's direct poll (the record loop, consume(), is inlined into the three polls), and the out-of-line record paths
 * (slow_record(), the request completion nvme_iau_req_done()). The attribute changes no instruction.
 * The object's .text is then 64 B aligned, so the linker starts it on a 64 B boundary. In both linked binaries
 * (spdk_nvme_perf and nvmf_tgt: libspdk_nvme whole-archive, nvme_pcie_common.o's 64 B-aligned .text ahead of it, and
 * every object between them keeping its footprint modulo 64, tests/realcore/test_pcie_hot_layout.sh) the objects
 * before it end at phase 11 and, 16 B aligned, it started at phase 16 (nx-p4a4: pend_add, this object's offset 0, at
 * 0x8e9d0 in perf and at 0x175150 in nvmf_tgt). 64 B aligned it starts at phase 0 (53 B of fill, not 5), so its .text
 * is padded to 16 modulo 64 (NVME_TEXT_PHASE64): it ends at phase 16 as before, and every object linked after it
 * keeps its 64 B phase (the guard computes the phase and checks the size; nm_phase.sh checks the linked binaries).
 * scripts/realcore/next/nm_phase.sh --set perf checks the pinned starts (phase 0) on the linked perf.
 */
#define NVME_IAU_HOT_ALIGN	__attribute__((aligned(64)))
NVME_TEXT_PHASE64(16);	/* the .text size is 16 mod 64, the phase this object starts at 16 B aligned: see above */

struct nvme_iau_cfg g_nvme_iau_cfg;
uint32_t g_nvme_iau_drain_us = NVME_IAU_DRAIN_US;	/* the drain bound; only the tile harness changes it */
static struct spdk_nvme_transport_ops g_stock;	/* the table nvme_iau_override_ops() replaced: forwarding */
static uint32_t g_next_gid;
static struct spdk_nvme_pcie_stat g_iau_stat_none;	/* a group's stat until its qpair has one */

/* nvme_pcie_vtophys() (static in nvme_pcie_common.c): IOVA = VA on vfio-user, the only tile transport */
static uint64_t
iau_iova(struct spdk_nvme_ctrlr *ctrlr, const void *buf)
{
	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		return spdk_vtophys(buf, NULL);
	}
	return (uint64_t)(uintptr_t)buf;
}

/* ------------------------------------------------------------------------------------------ configuration */

static bool
env_is_1(const char *name)
{
	const char *v = getenv(name);

	return v != NULL && strcmp(v, "1") == 0;
}

/* as the readers of the old modes test their switches (the first character) */
static bool
env_first_1(const char *name)
{
	const char *v = getenv(name);

	return v != NULL && v[0] == '1';
}

static int
env_u32(const char *name, uint32_t lo, uint32_t hi, uint32_t def, uint32_t *out, char *err, size_t errlen)
{
	const char *v = getenv(name);
	unsigned long x;
	char *end = NULL;

	if (v == NULL || v[0] == '\0') {
		*out = def;
		return 0;
	}
	errno = 0;
	x = strtoul(v, &end, 10);
	if (v[0] < '0' || v[0] > '9' || errno != 0 || end == v || *end != '\0' || x < lo || x > hi) {
		snprintf(err, errlen, "%s=%s is not a decimal number in %u..%u", name, v, lo, hi);
		return -EINVAL;
	}
	*out = (uint32_t)x;
	return 0;
}

int
nvme_iau_cfg_load(char *err, size_t errlen)
{
	struct nvme_iau_cfg c;

	memset(&g_nvme_iau_cfg, 0, sizeof(g_nvme_iau_cfg));
	memset(&c, 0, sizeof(c));
	c.tile = env_is_1("SPDK_IAU_TILE");
	c.s2 = env_is_1("SPDK_IAU_S2");
	c.s2_wb = env_is_1("SPDK_IAU_S2_WB");
	c.group = env_is_1("SPDK_IAU_GROUP");
	c.fast = env_is_1("SPDK_IAU_FAST");
	c.rcons = env_is_1("SPDK_IAU_RCONS");
	if (env_u32("SPDK_IAU_RINGS", 1, IAU_TILE_MAX_K, 1, &c.rings, err, errlen) != 0 ||
	    env_u32("SPDK_IAU_RING_ENTRIES", 64, 65535, NVME_IAU_RING_ENTRIES_DEFAULT, &c.ring_entries, err,
		    errlen) != 0 ||
	    env_u32("SPDK_IAU_DESC_STRIDE", IAU_TILE_DESC48_BYTES, IAU_TILE_DESC_BYTES, IAU_TILE_DESC_BYTES, &c.stride, err,
		    errlen) != 0 ||
	    env_u32("SPDK_IAU_DB_BATCH", 0, 65535, NVME_IAU_DB_BATCH_DEFAULT, &c.db_batch, err, errlen) != 0) {
		return -EINVAL;
	}
	if (!iau_tile_stride_ok(c.stride)) {
		snprintf(err, errlen, "SPDK_IAU_DESC_STRIDE=%u is not 64 or 48", c.stride);
		return -EINVAL;
	}
	if (c.tile && c.s2) {
		snprintf(err, errlen, "SPDK_IAU_TILE=1 and SPDK_IAU_S2=1 exclude each other");
		return -EINVAL;
	}
	if (c.s2_wb && !c.s2) {
		snprintf(err, errlen, "SPDK_IAU_S2_WB=1 needs SPDK_IAU_S2=1 (the S2 SQE store type)");
		return -EINVAL;
	}
	if (c.stride == IAU_TILE_DESC48_BYTES && (c.fast || c.group)) {
		snprintf(err, errlen, "SPDK_IAU_DESC_STRIDE=48 is the per-qpair request-path lever: not with "
			 "SPDK_IAU_FAST=1 or SPDK_IAU_GROUP=1");
		return -EINVAL;
	}
	if (c.group && !c.tile) {
		snprintf(err, errlen, "SPDK_IAU_GROUP=1 needs SPDK_IAU_TILE=1 (group rings are a v6 mode)");
		return -EINVAL;
	}
	if (!(c.tile && c.group)) {
		/* a group-pass knob: 0 (off) is a no-op anywhere (an env column of 0 cannot abort a private arm),
		 * a non-zero value needs the group pass */
		if (getenv("SPDK_IAU_DB_BATCH") != NULL && getenv("SPDK_IAU_DB_BATCH")[0] != '\0' && c.db_batch != 0) {
			snprintf(err, errlen, "SPDK_IAU_DB_BATCH=%u needs SPDK_IAU_TILE=1 and SPDK_IAU_GROUP=1 (a group-pass "
				 "knob; 0 is accepted anywhere)", c.db_batch);
			return -EINVAL;
		}
		c.db_batch = 0;	/* no group pass: IAU_TILE_OPS says db_batch=0 */
	}
	if (c.fast && !c.tile) {
		snprintf(err, errlen, "SPDK_IAU_FAST=1 needs SPDK_IAU_TILE=1 (the request-free path is a v6 mode)");
		return -EINVAL;
	}
	if (c.s2 && (c.stride != IAU_TILE_DESC_BYTES || c.rcons || env_first_1("SPDK_UNCORE_MODE_C") ||
		     env_first_1("SPDK_UNCORE_MODE_D") || env_first_1("SPDK_UNCORE_MODE_E") || env_is_1("SPDK_IAU_ENGINE"))) {
		/* P4.11: S2 is Mode 0 (no IAU, no host Mode C/D/E, no engine); the stride and RCONS are tile knobs */
		snprintf(err, errlen, "SPDK_IAU_S2=1 runs alone on Mode 0: unset SPDK_UNCORE_MODE_C/_D/_E, SPDK_IAU_ENGINE, "
			 "SPDK_IAU_RCONS and SPDK_IAU_DESC_STRIDE (v6 knobs)");
		return -EINVAL;
	}
	if (c.tile && (env_first_1("SPDK_UNCORE_MODE_C") || env_first_1("SPDK_UNCORE_MODE_D") ||
		     env_first_1("SPDK_UNCORE_MODE_E") || env_is_1("SPDK_IAU_ENGINE"))) {
		snprintf(err, errlen, "SPDK_IAU_TILE=1 runs alone: unset SPDK_UNCORE_MODE_C/_D/_E and SPDK_IAU_ENGINE "
			 "(D1; host Mode E's free-slot table at 0x5800 + 8 q borders QCFG2)");
		return -EINVAL;
	}
	g_nvme_iau_cfg = c;
	return 0;
}

/* P4.11: the x86-64 fast-path dispatcher (nvme_iau_fast_rw(), below) tests NVME_IAU_KIND_S2_BIT at byte
 * NVME_IAU_KIND_BYTE of the qpair: true if that is where this compiler put iau_kind's S2 bit (and neither tile's kind nor
 * iau_fast sets it) */
#define NVME_IAU_KIND_BYTE	12
#define NVME_IAU_KIND_S2_BIT	0x40
static bool
iau_kind_bits_ok(void)
{
	struct spdk_nvme_qpair q;
	uint8_t s2, tile;

	memset(&q, 0, sizeof(q));
	q.iau_kind = NVME_IAU_KIND_S2;
	s2 = ((const uint8_t *)&q)[NVME_IAU_KIND_BYTE];
	memset(&q, 0, sizeof(q));
	q.iau_kind = NVME_IAU_KIND_TILE;
	q.iau_fast = 1;
	tile = ((const uint8_t *)&q)[NVME_IAU_KIND_BYTE];
	return s2 == NVME_IAU_KIND_S2_BIT && (tile & NVME_IAU_KIND_S2_BIT) == 0;
}

void
nvme_iau_select_ops(struct spdk_nvme_transport_ops *ops)
{
	char err[256];

	if (nvme_iau_cfg_load(err, sizeof(err)) != 0) {
		fprintf(stderr, "IAU_TILE_CONFIG_ERROR %s\n", err);
		abort();
	}
	if ((g_nvme_iau_cfg.tile || g_nvme_iau_cfg.s2) && !iau_kind_bits_ok()) {
		fprintf(stderr, "IAU_TILE_CONFIG_ERROR iau_kind's S2 bit is not 0x40 of qpair byte 12 (nvme_iau_fast_rw())\n");
		abort();
	}
	if (g_nvme_iau_cfg.s2 && nvme_iau_s2_override_ops(ops) != 0) {
		fprintf(stderr, "IAU_TILE_CONFIG_ERROR SPDK_IAU_S2=1: the S2 override failed\n");
		abort();
	}
	if (g_nvme_iau_cfg.tile) {
		nvme_iau_override_ops(ops);
		fprintf(stderr, "IAU_TILE_OPS tile=1 stride=%u rcons=%u rings=%u ring_entries=%u group=%u db_batch=%u\n",
			g_nvme_iau_cfg.stride, g_nvme_iau_cfg.rcons, g_nvme_iau_cfg.rings, g_nvme_iau_cfg.ring_entries,
			g_nvme_iau_cfg.group, g_nvme_iau_cfg.db_batch);
	}
}

/* ------------------------------------------------------------------------------------------ rings */

static inline void
r_advance(struct nvme_iau_ring *r)
{
	if (spdk_unlikely(++r->r_head == r->r_entries)) {
		r->r_head = 0;
		r->r_phase ^= 1;
	}
}

static inline void
d_advance(struct nvme_iau_ring *r)
{
	if (spdk_unlikely(++r->d_tail == r->d_entries)) {
		r->d_tail = 0;
	}
	r->d_written++;
	/* A15: the host's written-slot count minus the engine's monotonic d_head stays below d_entries */
	assert(r->d_written - *r->d_head < r->d_entries);
}

/* the D-tail doorbell (at most once per poll with delay_cmd_submit): descriptors (and, with RCONS, the
 * consumed-record count) before the d_tail store; stat is the caller's qpair's (a poll group's while it is a
 * member: nvme_pcie_poll_group_remove() repoints it, so it is never cached) */
static inline void
doorbell(struct nvme_iau_group *g, struct nvme_iau_ring *r, struct spdk_nvme_pcie_stat *stat)
{
	if (r->d_tail != r->d_rung) {
		if (spdk_unlikely(r->r_consumed != NULL)) {
			*r->r_consumed = r->r_seq;
		}
		spdk_wmb();
		*r->db = r->d_tail;
		nvme_db_cost(g_nvme_db_cost_iau_ticks);	/* M2: the IAU doorbell's own knob */
		r->d_rung = r->d_tail;
		stat->sq_mmio_doorbell_updates++;
		g->doorbells++;
	}
}

/* the stat of iq's qpair (none before its first connect allocated one) */
static struct spdk_nvme_pcie_stat *
iq_stat(struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_pcie_stat *st = nvme_pcie_qpair(iq->qpair)->stat;

	return st != NULL ? st : &g_iau_stat_none;
}

/* one descriptor of any stride: logical words d0 d1 d2 d4 d5 d6 (d3, d7 are never written, D5) */
static void
d_store(struct nvme_iau_ring *r, const uint64_t d[8])
{
	uint64_t *s = (uint64_t *)(r->d + (size_t)r->d_tail * r->stride);
	unsigned i;

	for (i = 0; i < 8; i++) {
		int w = iau_tile_word(r->stride, i);

		if (w >= 0 && i != 3 && i != 7) {
			s[w] = d[i];
		}
	}
	d_advance(r);
}

/* a control descriptor (REISSUE, RELEASE, FLUSH; ABORT_* in P4.10) with the route's member gen (H3) */
static void
ctrl(struct nvme_iau_qpair *iq, uint8_t kind, uint64_t d2, uint64_t d5, uint64_t d6)
{
	uint64_t d[8] = { iau_tile_d0_ctrl(kind, iq->route, iq->gen), 0, d2, 0, 0, d5, d6, 0 };

	d_store(iq->ring, d);
	if (!nvme_pcie_qpair(iq->qpair)->flags.delay_cmd_submit) {
		doorbell(iq->g, iq->ring, iq_stat(iq));
	}
}

/* FLUSH, at most once per incarnation (D7, D11) */
static void
flush_once(struct nvme_iau_qpair *iq)
{
	if (iq->flushed) {
		return;
	}
	iq->flushed = 1;
	ctrl(iq, IAU_TILE_K_FLUSH, 0, 0, 0);
	doorbell(iq->g, iq->ring, iq_stat(iq));
	if (iq->state == NVME_IAU_RT_ACTIVE) {
		iq->state = NVME_IAU_RT_FLUSHING;
	}
	iq->qpair->iau_fast = 0;	/* P4.10a (P4.9 review M8): a FLUSHING route takes no IO; the entries' stock body runs */
}

/* ------------------------------------------------------------------------------------------ pending list */

static void
pend_add(struct nvme_iau_qpair *iq, uint16_t tag, uint64_t w0, uint64_t w1)
{
	uint16_t i;

	for (i = 0; i < iq->n_pend; i++) {
		if (iq->pend[i].tag == tag) {
			iq->pend[i].w0 = w0;
			iq->pend[i].w1 = w1;
			return;
		}
	}
	assert(iq->n_pend < iq->credits);
	iq->pend[iq->n_pend].tag = tag;
	iq->pend[iq->n_pend].w0 = w0;
	iq->pend[iq->n_pend].w1 = w1;
	iq->n_pend++;
}

static void
pend_clear(struct nvme_iau_qpair *iq, uint16_t tag)
{
	uint16_t i;

	if (tag == IAU_TILE_TAG_NONE) {
		return;
	}
	for (i = 0; i < iq->n_pend; i++) {
		if (iq->pend[i].tag == tag) {
			iq->pend[i] = iq->pend[--iq->n_pend];
			return;
		}
	}
}

/* ------------------------------------------------------------------------------------------ records */

static inline bool
is_req(uint64_t w0)
{
	return w0 == (uint64_t)(uintptr_t)nvme_iau_req_done;
}

void NVME_IAU_HOT_ALIGN
nvme_iau_req_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request *req = arg;
	struct spdk_nvme_qpair *qp = req->qpair;
	struct nvme_iau_qpair *iq = nvme_iau_qp(qp);
	struct spdk_nvme_cpl c = *cpl;
	uint16_t idx = req->cmd.cid;

	assert(idx < iq->credits && iq->inflight[idx] == req);
	iq->inflight[idx] = NULL;
	iq->idx_stack[iq->idx_top++] = idx;
	/* completion-side error injection applies here, as in the stock completion */
	nvme_complete_request(req->cb_fn, req->cb_arg, qp, req, &c);
}

/* print as the stock completion does (nvme_iau_cpl_error_policy()'s rule; nothing is retried here) */
static void
print_final(struct nvme_iau_qpair *iq, uint64_t w0, uint64_t w1, struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_cmd cmd;

	if (is_req(w0)) {
		cmd = ((struct nvme_request *)(uintptr_t)w1)->cmd;
	} else {
		memset(&cmd, 0, sizeof(cmd));
	}
	(void)nvme_iau_cpl_error_policy(iq->qpair, &cmd, UINT8_MAX, 0, cpl, true);
}

/* for_iq of a shared group's group poll (P4.8): every delivery counts (the poll's total) */
#define NVME_IAU_FOR_ALL	((struct nvme_iau_qpair *)(uintptr_t)1)

static inline bool
counts_for(const struct nvme_iau_qpair *for_iq, const struct nvme_iau_qpair *iq)
{
	return for_iq == iq || for_iq == NVME_IAU_FOR_ALL;
}

/* P4.8: a shared group's member needs its generic duties (D12): queue it once on the attention list */
static void
attn(struct nvme_iau_group *g, struct nvme_iau_qpair *iq)
{
	if (!iq->in_attn) {
		iq->in_attn = 1;
		iq->attn_list = &g->attn;
		TAILQ_INSERT_TAIL(&g->attn, iq, attn_link);
	}
}

/* a callback on a cold path (deliver(), error_record(), an ack). A shared group's member removed from its poll group
 * (qpair->poll_group NULL: nvme_ctrlr.c's group-pass test cannot see it) runs it in its own completion context, so a
 * free it asks for is deferred like a member's (I1); a free asked for here is carried out by the attention pass.
 * Everything is read before the callback (m6) */
static void
cold_cb(struct nvme_iau_qpair *iq, uint64_t w0, uint64_t w1, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct nvme_iau_group *g = iq->g;
	bool shared = g != NULL && !g->is_private;
	bool own = shared && qp->poll_group == NULL && !qp->in_completion_context;

	if (own) {
		qp->in_completion_context = 1;
	}
	if (spdk_likely(w0 != 0)) {
		((spdk_nvme_cmd_cb)(uintptr_t)w0)((void *)(uintptr_t)w1, cpl);	/* a NULL cb_fn is skipped, as stock */
	}
	if (own) {
		qp->in_completion_context = 0;
	}
	if (shared && spdk_unlikely(qp->delete_after_completion_context)) {
		attn(g, iq);
	}
}

/* one final delivery: the credit returns, then the callback */
static void
deliver(struct nvme_iau_qpair *iq, uint64_t w0, uint64_t w1, struct spdk_nvme_cpl *cpl, struct nvme_iau_qpair *for_iq,
	uint32_t *n_for)
{
	struct spdk_nvme_qpair *qp = iq->qpair;

	print_final(iq, w0, w1, cpl);
	qp->queue_depth--;
	if (!is_req(w0)) {
		qp->num_outstanding_reqs--;
	}
	if (counts_for(for_iq, iq)) {
		(*n_for)++;
	}
	cold_cb(iq, w0, w1, cpl);
}

static void
synth_cpl(struct nvme_iau_qpair *iq, uint16_t tag, uint16_t sc, struct spdk_nvme_cpl *cpl)
{
	memset(cpl, 0, sizeof(*cpl));
	cpl->sqid = iq->route;
	cpl->cid = tag;
	cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	cpl->status.sc = sc;
	cpl->status.dnr = 1;
}

static int nvme_iau_submit_raw(struct nvme_iau_qpair *iq, struct nvme_request *req, uint16_t idx);

/* CPL + PARKED (D8): an error of a parked slot awaiting the host's disposition */
static void
error_record(struct nvme_iau_qpair *iq, uint64_t w0, uint64_t w1, struct spdk_nvme_cpl *cpl, uint16_t tag,
	     uint8_t rec_retries, struct nvme_iau_qpair *for_iq, uint32_t *n_for)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct nvme_request *req = is_req(w0) ? (struct nvme_request *)(uintptr_t)w1 : NULL;
	struct spdk_nvme_cmd cmd;
	uint64_t buf;
	bool raw;

	iq->st.err++;
	if (iq->flushed) {
		/* after the host's FLUSH: FLUSH's own final record for this tag (or R_DETACHED) delivers it */
		pend_add(iq, tag, w0, w1);
		return;
	}
	if (req != NULL) {
		cmd = req->cmd;
		raw = !nvme_iau_req_is_io(iq, req, &buf);
	} else {
		/* request-free IO (P4.9): the SQE the engine rebuilt into the echo slab before the record */
		memcpy(&cmd, (const void *)&iq->echo[iau_tile_tag_slot(tag)], sizeof(cmd));
		raw = false;
	}
	if (nvme_iau_cpl_error_policy(qp, &cmd, req != NULL ? req->retries : rec_retries,
				      nvme_pcie_qpair(qp)->retry_count, cpl, true) == NVME_IAU_CPL_RETRY) {
		iq->st.retries++;
		if (req != NULL) {
			req->retries++;
		}
		if (!raw) {
			ctrl(iq, IAU_TILE_K_REISSUE, tag, 0, 0);	/* the same tag, the SQE rebuilt from the replay */
			iq->st.reissues++;
			pend_add(iq, tag, w0, w1);
			return;
		}
		/* RAW is never replayed by the engine: free the slot, then the same command as a new RAW (DV2; stock re-submits
		 * the tracker, nvme_pcie_qpair_complete_tracker()); P4.10c: a payload that no longer builds (an SGL iterator
		 * that fails the second walk) is handed to the IAU to refuse (raw_refuse(): INVALID_FIELD, one callback) */
		ctrl(iq, IAU_TILE_K_RELEASE, tag, 0, 0);
		iq->st.releases++;
		if (nvme_iau_submit_raw(iq, req, req->cmd.cid) == 0) {
			if (!nvme_pcie_qpair(qp)->flags.delay_cmd_submit) {
				doorbell(iq->g, iq->ring, iq_stat(iq));
			}
			return;
		}
		/* cannot fail (stride 64: the first submit was a RAW): deliver the error */
	} else {
		ctrl(iq, IAU_TILE_K_RELEASE, tag, 0, 0);	/* before the callback (D8) */
		iq->st.releases++;
	}
	pend_clear(iq, tag);
	qp->queue_depth--;
	if (req == NULL) {
		qp->num_outstanding_reqs--;
	}
	if (counts_for(for_iq, iq)) {
		(*n_for)++;
	}
	cold_cb(iq, w0, w1, cpl);	/* the policy printed already */
}

/* the request-backed IO still in flight and the pending entries end now with ABORTED_SQ_DELETION: only when
 * no record can come for them any more (a CLOSED R_DETACHED, a removed controller, or a ring that is never
 * consumed again); returns the request-free credits that cannot be returned (DV19, P4.9). It walks inflight[]
 * (request-backed IO) and the pending list only: at a CLOSED R_DETACHED end_unfetched() first ends the request-free
 * IO whose descriptors were written but never fetched (P4.9), and so does route_lost() at a drain bound (P4.10d); on
 * a removed controller those stay in queue_depth (lost, DV19 case 1). */
static uint32_t
abort_leftovers(struct nvme_iau_qpair *iq, uint32_t dnr, struct nvme_iau_qpair *for_iq, uint32_t *n_for)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct spdk_nvme_cpl cpl;
	uint32_t i;

	while (iq->n_pend != 0) {
		struct nvme_iau_pend p = iq->pend[--iq->n_pend];

		if (is_req(p.w0)) {
			continue;	/* in inflight[] too: completed below */
		}
		synth_cpl(iq, p.tag, SPDK_NVME_SC_ABORTED_SQ_DELETION, &cpl);
		cpl.status.dnr = dnr & 1u;
		deliver(iq, p.w0, p.w1, &cpl, for_iq, n_for);
	}
	for (i = 0; i < iq->credits; i++) {
		struct nvme_request *req = iq->inflight[i];

		if (req == NULL) {
			continue;
		}
		synth_cpl(iq, IAU_TILE_TAG_NONE, SPDK_NVME_SC_ABORTED_SQ_DELETION, &cpl);
		cpl.status.dnr = dnr & 1u;
		deliver(iq, (uint64_t)(uintptr_t)nvme_iau_req_done, (uint64_t)(uintptr_t)req, &cpl, for_iq, n_for);
	}
	return qp->queue_depth;
}

/* P4.9: at a CLOSED R_DETACHED of ring k (its engine ring is unbound: nothing of it is fetched again) the request-free
 * IO whose descriptors sit between the engine's last fetch and the host's last write end now, as their records
 * would: the slots from the GCB d_head (the engine's monotonic fetch count, published before the CLOSED record, A15)
 * up to d_written. Per slot: a RAW header's SQE slot is skipped (it is not a descriptor); an IO-kind descriptor whose
 * w0 (d5) is not nvme_iau_req_done (request-backed IO and RAW end through inflight[], abort_leftovers()) and whose
 * d0 route is a member of ring k with the d0 member gen gets ABORTED_SQ_DELETION (dnr, tag NONE), its credit back
 * (deliver()); P4.10a (R04): an ABORT_* control gets the ack the engine gives a fetched one at its final fetch (0
 * marked, tag NONE, its control credit back) through its own w0 / w1 (nvme_iau_abort_ack(), its abort context), so
 * every control's credit is back at the CLOSED and its context counts it. The other controls end elsewhere (REISSUE:
 * the pending list; RELEASE, FLUSH: nothing). A route without a member, or a LOST one (it ended at its drain bound:
 * DV19), is skipped. d_written and last_w are not touched; the walk ends at the d_written it starts with (the callers
 * put every route of the ring GONE first, so a callback writes nothing on this ring).
 * P4.10d (DV19 case 2): only != NULL walks for one route at its drain bound (route_lost(), the route LOST already): its
 * request-free IO-kind descriptors with its gen only, each ending once as above; its controls are skipped
 * (abort_q_complete() ends their contexts; a late fetch of one is acked as a stray).
 * P4.10d (P4.10a review I3): the range is checked in every build. A GCB d_head past d_written, or d_written - d_head
 * >= d_entries (a stale or corrupt d_head: the slots would be read from an older lap, whose d5 / d6 may name freed abort
 * contexts or IO already called back), ends nothing: one IAU_TILE_WALK_CLAMP line, and the routes keep their credits
 * (undrained: their drains end LOST at the bound). Returns false then, else true */
static bool
end_unfetched(struct nvme_iau_group *g, uint32_t k, struct nvme_iau_qpair *only, struct nvme_iau_qpair *for_iq,
	      uint32_t *n_for)
{
	struct nvme_iau_ring *r = &g->ring[k];
	uint64_t p = __atomic_load_n(r->d_head, __ATOMIC_ACQUIRE), end = r->d_written;
	int w0i = iau_tile_word(r->stride, 0), w5 = iau_tile_word(r->stride, 5), w6 = iau_tile_word(r->stride, 6);
	struct spdk_nvme_cpl cpl;

	if (spdk_unlikely(p > end || end - p >= r->d_entries)) {
		fprintf(stderr, "IAU_TILE_WALK_CLAMP gid=%u ring=%u d_head=%" PRIu64 " d_written=%" PRIu64 " entries=%u\n",
			g->gid, k, p, end, r->d_entries);
		return false;
	}
	for (; p < end; p++) {
		const uint64_t *sl = (const uint64_t *)(r->d + (size_t)(p % r->d_entries) * r->stride);
		uint64_t d0 = sl[w0i], w0, w1;
		uint8_t kind = iau_tile_d0_kind(d0);
		struct nvme_iau_qpair *iq;
		uint16_t route;

		if (kind == IAU_TILE_K_RAW) {
			p++;	/* its SQE slot */
			continue;
		}
		if (kind != IAU_TILE_K_IO && kind != IAU_TILE_K_ABORT_COOKIE && kind != IAU_TILE_K_ABORT_TAG) {
			continue;
		}
		w0 = sl[w5];
		w1 = sl[w6];
		if (kind == IAU_TILE_K_IO && is_req(w0)) {
			continue;
		}
		route = iau_tile_d0_route(d0);
		iq = route < IAU_TILE_MAX_ROUTES ? g->route[route] : NULL;
		if (iq == NULL || iq->ring != r || iau_tile_d0_gen(d0) != iq->gen ||
		    (only != NULL ? iq != only : iq->state == NVME_IAU_RT_LOST)) {
			continue;
		}
		if (kind == IAU_TILE_K_IO) {
			synth_cpl(iq, IAU_TILE_TAG_NONE, SPDK_NVME_SC_ABORTED_SQ_DELETION, &cpl);
			deliver(iq, w0, w1, &cpl, for_iq, n_for);
			continue;
		}
		if (only != NULL) {
			continue;	/* a control of the LOST route: its context ends in abort_q_complete() */
		}
		/* R04: an ABORT_* never fetched: ack(0), as slow_record() takes an ack record */
		if (iq->ctrl_inflight == 0) {
			g->stray++;	/* no control outstanding on the route: nothing to ack (never dereferenced) */
			continue;
		}
		memset(&cpl, 0, sizeof(cpl));
		cpl.sqid = iq->route;
		cpl.cid = IAU_TILE_TAG_NONE;
		iq->ctrl_inflight--;
		iq->st.ctrl++;
		cold_cb(iq, w0, w1, &cpl);
	}
	return true;
}

/* the route left the engine (R_DETACHED consumed, or its ring closed under it): GONE; a live route's qpair FAILED */
static void
route_gone(struct nvme_iau_qpair *iq)
{
	if (iq->state == NVME_IAU_RT_ACTIVE) {
		/* a target-initiated detach of a live route (controller disable / reset, region removal) */
		nvme_pcie_qpair(iq->qpair)->pcie_state = NVME_PCIE_QPAIR_FAILED;
		if (iq->qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_RESET) {
			/* as stock (m2); P4.10a (n4): a reconnect deferred to the member's own poll keeps its RESET */
			iq->qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;
		}
		iq->qpair->iau_fast = 0;
		if (!iq->g->is_private) {
			attn(iq->g, iq);	/* R4: its generic poll reports -ENXIO (D11) */
		}
	}
	if (iq->state != NVME_IAU_RT_LOST) {
		iq->state = NVME_IAU_RT_GONE;	/* first: a callback's resubmit gets -ENXIO */
	}
	/* no descriptor of this route follows (submits get -ENXIO from here): its last D slot is below this count,
	 * the shared-ring release precondition (route_drained()) */
	iq->last_w = iq->ring->d_written;
}

/* P4.8: a shared ring's CLOSED R_DETACHED (its last member's, or one whose member already left, m3): every route
 * still on ring k ends its leftovers (detached() below). The ring is closed first (m2: a callback in the loop sees
 * it closed), and it is bound to no engine ring any more, so a taint (a LOST member that might still be bound)
 * ends here and the next member resets it (group_ring_prep()). P4.9: every route of the ring goes GONE before any
 * callback runs (a callback's submit or disconnect then writes nothing on the closed ring), then the request-free IO
 * that was never fetched ends (end_unfetched()), then each route's pending entries and request-backed IO */
static void
close_ring(struct nvme_iau_group *g, uint32_t k, struct nvme_iau_qpair *for_iq, uint32_t *n_for)
{
	struct nvme_iau_pend p;
	struct spdk_nvme_cpl cpl;
	uint32_t rt, j;
	bool walked;

	g->gr[k].closed = 1;
	g->gr[k].tainted = 0;
	g->keep = false;
	for (j = 0; j < g->k; j++) {
		g->keep = g->keep || g->gr[j].tainted;
	}
	for (rt = k; rt < IAU_TILE_MAX_ROUTES; rt += g->k) {
		struct nvme_iau_qpair *m = g->route[rt];

		if (g->route_held[rt]) {
			/* the unbound ring took the target's reservations with it: a held route is free again (R21) */
			g->ring[k].need_d -= iau_tile_route_need_d(g->route_resv[rt]);
			g->ring[k].need_r -= iau_tile_route_need_r(g->route_resv[rt]);
			g->route_held[rt] = 0;
			g->route_resv[rt] = 0;
		}
		if (m != NULL && (m->state == NVME_IAU_RT_ACTIVE || m->state == NVME_IAU_RT_FLUSHING)) {
			route_gone(m);	/* every route of the ring GONE before any callback (P4.9: nothing more is written) */
		}
	}
	walked = end_unfetched(g, k, NULL, for_iq, n_for);	/* P4.9: the request-free IO never fetched */
	for (rt = k; rt < IAU_TILE_MAX_ROUTES; rt += g->k) {
		struct nvme_iau_qpair *m = g->route[rt];

		if (m == NULL) {
			continue;
		}
		while (m->n_pend != 0) {
			p = m->pend[--m->n_pend];
			synth_cpl(m, p.tag, SPDK_NVME_SC_ABORTED_SQ_DELETION, &cpl);
			deliver(m, p.w0, p.w1, &cpl, for_iq, n_for);
		}
		(void)abort_leftovers(m, 1, for_iq, n_for);
		/* P4.10a (R04): every control's ack came before the CLOSED (a fetched one's, the final fetch's orphan ack) or
		 * from the walk above (an unfetched one's): no control credit is outstanding (a stale one keeps the route
		 * undrained: LOST, never a silent reset of the count; P4.10d: so does a clamped walk, I3) */
		assert(!walked || m->ctrl_inflight == 0);
	}
	(void)walked;
}

/* R_DETACHED: the target detached the route; the leftover pending entries end now (D8, D11), and with CLOSED
 * (the ring is closed) the rest of the in-flight commands too */
static void
detached(struct nvme_iau_qpair *iq, bool closed, struct nvme_iau_qpair *for_iq, uint32_t *n_for)
{
	struct nvme_iau_group *g = iq->g;
	struct nvme_iau_pend p;
	struct spdk_nvme_cpl cpl;

	iq->st.detached++;
	route_gone(iq);
	while (iq->n_pend != 0) {
		p = iq->pend[--iq->n_pend];
		synth_cpl(iq, p.tag, SPDK_NVME_SC_ABORTED_SQ_DELETION, &cpl);
		deliver(iq, p.w0, p.w1, &cpl, for_iq, n_for);
	}
	if (!closed) {
		return;
	}
	/* The private-ring rule (P4.5 fix A, iau_tile_abi.h), keyed on the record's rflags CLOSED only: every descriptor
	 * written before the detach-time d_tail got its one record before this R_DETACHED, the ring's last record. A
	 * command still in flight now was written after that d_tail (a resubmit from a callback of a record consumed
	 * while ACTIVE, a RELEASE + new RAW of a RAW retry) and never gets a record: it ends here, exactly once, with
	 * ABORTED_SQ_DELETION (dnr), credits and in-flight indices returned as by a record. Its controls written then
	 * get their ack(0) from the walk (end_unfetched(), R04). Without CLOSED (a SHARED ring's early R_DETACHED,
	 * a target without the final fetch, a last member detached the shared way) orphan records of the route's
	 * later descriptors still come (the ring keeps being polled): the rest waits for them or ends LOST at the
	 * drain bound, never twice.
	 * P4.8, a SHARED ring: CLOSED comes on the last member's route only, and the descriptors every OTHER route
	 * of the ring wrote after that same final d_tail never get records either, so every route still on ring k
	 * ends its leftovers here (each already saw its own R_DETACHED: every member's precedes the last one's in R;
	 * a route the engine dropped without records (emit=false) goes GONE here). The ring is closed: the next
	 * member on it starts a reset ring (group_ring_prep()). */
	if (g->is_private) {
		/* P4.9: the request-free IO never fetched; P4.10a: the controls */
		bool walked = end_unfetched(g, 0, NULL, for_iq, n_for);

		(void)abort_leftovers(iq, 1, for_iq, n_for);
		assert(!walked || iq->ctrl_inflight == 0);	/* P4.10a (R04), as close_ring() */
		(void)walked;
		return;
	}
	close_ring(g, iq->route % g->k, for_iq, n_for);
}

static void abort_q_issue(struct nvme_iau_qpair *iq);

/* everything the fast filter does not take (D6): dispatch on kind and rflags only */
static void __attribute__((noinline)) NVME_IAU_HOT_ALIGN
slow_record(struct nvme_iau_group *g, uint64_t w0, uint64_t w1, uint64_t w2, uint64_t w3,
	    struct nvme_iau_qpair *for_iq, uint32_t *n_for)
{
	uint16_t route = iau_tile_w3_route(w3), tag = iau_tile_w3_tag(w3);
	struct nvme_iau_qpair *iq = route < IAU_TILE_MAX_ROUTES ? g->route[route] : NULL;
	uint8_t kind = iau_tile_w2_kind(w2), rflags = iau_tile_w2_rflags(w2);
	struct spdk_nvme_cpl cpl;

	if (iq == NULL || spdk_unlikely(iq->state == NVME_IAU_RT_LOST)) {
		/* P4.10d (DV19 case 2): a LOST route ended at its drain bound (route_lost(): its requests, its abort contexts and
		 * its unfetched request-free IO got their callbacks then), so a record of it is a stray too, a late fetch of a
		 * descriptor that walk ended included: never a second callback. A shared ring's CLOSED still closes the ring */
		if (kind == IAU_TILE_R_DETACHED && (rflags & IAU_TILE_RF_CLOSED) && !g->is_private &&
		    route < IAU_TILE_MAX_ROUTES) {
			close_ring(g, route % g->k, for_iq, n_for);	/* its member already left (m3), or it is LOST */
			return;
		}
		g->stray++;	/* no member (route >= 256 never aliases a table entry, R30) */
		return;
	}
	iau_tile_rec_cpl(w2, w3, &cpl);	/* cdw1 (kind, retries, rflags) is the IAU's: masked */
	if (rflags & IAU_TILE_RF_ACK) {
		if (kind != IAU_TILE_R_CPL || iq->ctrl_inflight == 0) {
			g->stray++;
			return;
		}
		iq->ctrl_inflight--;	/* a control credit, not an IO credit (D13) */
		iq->st.ctrl++;
		abort_q_issue(iq);	/* P4.10a (D10): the credit goes to the next queued control first */
		cold_cb(iq, w0, w1, &cpl);	/* nvme_iau_abort_ack() */
		return;
	}
	switch (kind) {
	case IAU_TILE_R_TIMEOUT: {
		struct spdk_nvme_qpair *qp = iq->qpair;
		struct spdk_nvme_ctrlr_process *p = qp->active_proc;

		iq->st.timeouts++;
		if (qp->ctrlr->state == NVME_CTRLR_STATE_READY && p != NULL && p->timeout_cb_fn != NULL) {
			struct nvme_iau_group *tg = iq->g;
			bool shared = !tg->is_private, own = shared && qp->poll_group == NULL && !qp->in_completion_context;

			if (own) {
				qp->in_completion_context = 1;	/* as cold_cb() (I1) */
			}
			p->timeout_cb_fn(p->timeout_cb_arg, qp->ctrlr, qp, tag);
			if (own) {
				qp->in_completion_context = 0;
			}
			if (shared && spdk_unlikely(qp->delete_after_completion_context)) {
				attn(tg, iq);
			}
		}
		return;
	}
	case IAU_TILE_R_DETACHED:
		detached(iq, (rflags & IAU_TILE_RF_CLOSED) != 0, for_iq, n_for);
		return;
	case IAU_TILE_R_ABORTED:
		iq->st.aborted++;
		pend_clear(iq, tag);
		deliver(iq, w0, w1, &cpl, for_iq, n_for);
		return;
	case IAU_TILE_R_CPL:
		if (rflags & IAU_TILE_RF_PARKED) {
			error_record(iq, w0, w1, &cpl, tag, iau_tile_w2_retries(w2), for_iq, n_for);
			return;
		}
		/* a final record: a re-issued command's completion (retries > 0), a final error (route not ACTIVE;
		 * fused, DV21), an INVALID_FIELD refusal, or a CPL whose status is an SSD's SCT 7 / SC 0xFE */
		pend_clear(iq, tag);
		deliver(iq, w0, w1, &cpl, for_iq, n_for);
		return;
	default:
		g->stray++;
		return;
	}
}

/* the inline compare's value while an injection is armed: no record's w0 is 1 (w0 is nvme_iau_req_done, a
 * request-free IO's cb_fn, or 0 for a NULL cb_fn, which the generic path skips), so every record then takes the
 * generic path (P4.7e review m1: 0 would send a NULL-cb_fn request-free record inline as a request) */
#define NVME_IAU_W0_NEVER	1u

/* a completion-side error injection (spdk_nvme_qpair_add_cmd_error_injection()) armed on the route of a private
 * group (its one route; a shared group tests err_cmd_head per record, P4.8) */
static bool
group_inject_armed(const struct nvme_iau_group *g)
{
	const struct spdk_nvme_qpair *qp = g->route_qp[0];

	return qp != NULL && !TAILQ_EMPTY(&qp->err_cmd_head);
}

/* how a consume pass counts the completions it delivers for for_iq's qpair (the poll's return value): a
 * compile-time argument, so each inlined copy keeps only its own rule */
enum {
	CNT_NONE,	/* a private group's drain(): nothing counted */
	CNT_PRIVATE,	/* a group of one route: every delivery is for route_qp[0], counted once per pass */
	CNT_ROUTE,	/* a shared group's direct poll of one member (R3): per record, by the record's qpair */
	CNT_ALL,	/* a shared group's group poll: every delivery, counted once per pass (for_iq NVME_IAU_FOR_ALL) */
	CNT_GNONE,	/* a shared group's drain: nothing counted */
};
/* a shared group's pass (P4.8): per record it also tests the record's qpair's err_cmd_head (the per-pass test
 * over every route would be O(members)) and, after the callback, delete_after_completion_context (a free the
 * callback asked for, deferred by the poll group's in_completion_context: the attention pass does it) */
#define CNT_SHARED(c)	((c) >= CNT_ROUTE)

/*
 * One pass over ring r (D6, D12): w3 then w2 (iau_tile_rec_peek()), the fast filter (phase, SC = SCT = 0,
 * cdw1 = 0), the route lookup and the completion; anything else is cold.
 * P4.7e: a request-backed record (w0 = nvme_iau_req_done) completes INLINE, nvme_pcie_md_poll()'s per-record
 * sequence: the cpl built with two 8 B stores (w2, whose cdw1 the fast filter found 0, and w3), so the callee's
 * loads of its fields forward;
 * the in-flight index, the IO credit (queue_depth) and the request back BEFORE the callback, so a resubmit from
 * inside it reuses them. The order is the stock one of this SPDK (nvme_complete_request(): _nvme_free_request(),
 * then cb_fn), not md_poll's (cb_fn, then nvme_free_request()): md_poll's order lets a callback see
 * spdk_nvme_qpair_get_num_outstanding_reqs() one higher than stock and keeps the request from a resubmit made
 * inside the callback; the free-first order keeps both stock and costs no extra instruction. What
 * nvme_complete_request() checks per record is handled as md_poll does:
 * - err_cmd_head (completion-side error injection) is tested ONCE per pass on a private group (its one route):
 *   while it is armed every request-backed record takes nvme_iau_req_done() and so nvme_complete_request(),
 *   which injects exactly as stock. An injection armed from inside a callback applies from the next pass
 *   (md_poll's P2.1 rule; stock applies it from the next record). A shared group (P4.8) tests the record's own
 *   qpair per record instead (one pass serves many qpairs): an injection applies from the next record, as stock;
 * - req->accel_sequence is not tested: it is NULL on every request here. nvme_allocate_request() clears it, and
 *   only _nvme_ns_cmd_rw() sets it, after _is_accel_sequence_valid() accepted it, which needs the controller
 *   flag SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED that only the TCP and RDMA transports set (never PCIe or
 *   vfio-user): the stock test is always false here (asserted);
 * - a NULL cb_fn is skipped, as stock does (md_poll calls cb_fn unconditionally).
 * The print rule needs nothing: these records are successes (stock prints them only under the "nvme" debug
 * flag). Everything else (RAW and request-free records, error and cold records, an armed injection) keeps
 * the generic path: the record's own callback, nvme_iau_req_done() for request-backed IO.
 */
static inline __attribute__((always_inline)) uint32_t
consume(struct nvme_iau_group *g, struct nvme_iau_ring *r, uint32_t budget, struct nvme_iau_qpair *for_iq,
	uint32_t *n_for, const int cnt)
{
	const struct spdk_nvme_qpair *for_qp = (cnt == CNT_PRIVATE || cnt == CNT_ROUTE) && for_iq != NULL ?
					       for_iq->qpair : NULL;
	const uint64_t inl = CNT_SHARED(cnt) || spdk_likely(!group_inject_armed(g)) ?
			     (uint64_t)(uintptr_t)nvme_iau_req_done : NVME_IAU_W0_NEVER;
	uint32_t n = 0, nf = 0, n_cold = 0;

	while (n < budget) {
		uint32_t h = r->r_head;
		uint8_t ph = r->r_phase;
		volatile struct iau_tile_rec *rec = &r->r[h];
		uint64_t w2, w3 = iau_tile_rec_peek(rec, &w2), w0, w1;
		struct spdk_nvme_qpair *qp;
		struct spdk_nvme_cpl cpl;
		uint16_t route;

		if (ph > 1) {
			__builtin_unreachable();	/* r_phase is 0 or 1 */
		}
		if (spdk_unlikely(!iau_tile_rec_fast(w2, w3, ph))) {
			if (!iau_tile_rec_ready(w3, ph)) {
				break;	/* no new record */
			}
			w0 = rec->w0;
			w1 = rec->w1;
			r_advance(r);
			n++;
cold:
			n_cold++;
			slow_record(g, w0, w1, w2, w3, for_iq, n_for);
			continue;
		}
		w0 = rec->w0;
		w1 = rec->w1;
		r->r_head = h + 1;	/* r_advance() on the head read above */
		if (spdk_unlikely(h + 1 == r->r_entries)) {
			r->r_head = 0;
			r->r_phase ^= 1;
		}
		n++;
		route = iau_tile_w3_route(w3);
		qp = spdk_likely(route < IAU_TILE_MAX_ROUTES) ? g->route_qp[route] : NULL;
		if (spdk_unlikely(qp == NULL)) {
			/* no member (a stray), or a member removed from its poll group (route_qp cleared at its
			 * poll_group_remove, I1): the cold path above, where slow_record() delivers the latter's records
			 * (one call site, so the hot loop's code is unchanged) */
			goto cold;
		}
		/* iau_tile_rec_cpl() without its cdw1 mask: the fast filter took this record only with w2[63:32]
		 * (cdw1) == 0, so w2 is already cdw0 | 0 << 32 */
		memcpy((uint8_t *)&cpl, &w2, 8);
		memcpy((uint8_t *)&cpl + 8, &w3, 8);
		if (SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
			spdk_nvme_qpair_print_completion(qp, &cpl);	/* as stock; false (nothing) without DEBUG (m7) */
		}
		qp->queue_depth--;
		if (cnt == CNT_ROUTE) {
			nf += qp == for_qp;
		}
		if (spdk_likely(w0 == inl && (!CNT_SHARED(cnt) || TAILQ_EMPTY(&qp->err_cmd_head)))) {
			/* inline request completion (P4.7e, see above) */
			struct nvme_request *req = (struct nvme_request *)(uintptr_t)w1;
			struct nvme_iau_qpair *iq = nvme_iau_qp(qp);
			spdk_nvme_cmd_cb cb_fn = req->cb_fn;
			void *cb_arg = req->cb_arg;
			uint16_t idx = req->cmd.cid;

			assert(idx < iq->credits && iq->inflight[idx] == req && req->qpair == qp);
			assert(req->accel_sequence == NULL);
			iq->inflight[idx] = NULL;
			iq->idx_stack[iq->idx_top++] = idx;
			_nvme_free_request(req, qp);
			if (spdk_likely(cb_fn != NULL)) {
				cb_fn(cb_arg, &cpl);
			}
			if (CNT_SHARED(cnt) && spdk_unlikely(qp->delete_after_completion_context)) {
				attn(g, iq);
			}
			continue;
		}
		if (!is_req(w0)) {
			qp->num_outstanding_reqs--;
		}
		if (spdk_likely(w0 != 0)) {
			((spdk_nvme_cmd_cb)(uintptr_t)w0)((void *)(uintptr_t)w1, &cpl);	/* NULL: skipped, as stock */
		}
		if (CNT_SHARED(cnt) && spdk_unlikely(qp->delete_after_completion_context)) {
			attn(g, nvme_iau_qp(qp));
		}
	}
	if (cnt == CNT_PRIVATE && for_qp != NULL && for_qp == g->route_qp[0]) {
		*n_for += n - n_cold;	/* the fast deliveries; the cold ones counted themselves (deliver()) */
	} else if (cnt == CNT_ROUTE) {
		*n_for += nf;
	} else if (cnt == CNT_ALL) {
		*n_for += n - n_cold;	/* the cold deliveries counted themselves (for_iq NVME_IAU_FOR_ALL) */
	}
	g->records += n;
	r->r_seq += n;
	return n;
}

/* every ring of a private group once (budget per ring), then the doorbells */
static uint32_t
group_pass(struct nvme_iau_group *g, uint32_t max, struct nvme_iau_qpair *for_iq, uint32_t *n_for,
	   struct spdk_nvme_pcie_stat *stat)
{
	uint32_t k, n = 0, b;

	assert(g->is_private && g->n_routes == 1);	/* a shared group polls through shared_pass() (P4.8) */
	g->in_ring_poll = true;
	for (k = 0; k < g->k; k++) {
		struct nvme_iau_ring *r = &g->ring[k];

		b = (max == 0 || max > r->budget) ? r->budget : max;
		n += consume(g, r, b, for_iq, n_for, CNT_PRIVATE);
	}
	g->in_ring_poll = false;
	for (k = 0; k < g->k; k++) {
		doorbell(g, &g->ring[k], stat);
	}
	g->polls++;
	return n;
}

/* ------------------------------------------------------------------------------------------ shared groups */

#define NVME_IAU_EMPTY_RING_BUDGET	64u	/* a shared ring without members: its pass budget */

/* {is_private = 1, in_ring_poll = 0} read as one uint16_t: a private group outside its own pass (the poll's one test,
 * P4.8) */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define NVME_IAU_PRIVATE_IDLE	0x0001u
#else
#define NVME_IAU_PRIVATE_IDLE	0x0100u
#endif
SPDK_STATIC_ASSERT(offsetof(struct nvme_iau_group, in_ring_poll) == offsetof(struct nvme_iau_group, is_private) + 1 &&
		   sizeof(bool) == 1, "is_private and in_ring_poll are adjacent bytes");

static int nvme_iau_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair);

/* a delete op a ring pass deferred (DV23), under its controller's lock as stock's spdk_nvme_ctrlr_free_io_qpair()
 * holds it around the transport delete (nvme_ctrlr.c:657; the mutex is recursive) (n1) */
static void
run_deferred_one(struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct spdk_nvme_ctrlr *ctrlr = qp->ctrlr;

	iq->delete_deferred = 0;
	nvme_ctrlr_lock(ctrlr);
	(void)nvme_iau_ctrlr_delete_io_qpair(ctrlr, qp);
	nvme_ctrlr_unlock(ctrlr);
}

/* a controller lock taken without waiting (the robust-mutex rule of nvme_robust_mutex_lock()) */
static bool
ctrlr_trylock(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc = pthread_mutex_trylock(&ctrlr->ctrlr_lock);

#ifndef __FreeBSD__
	if (rc == EOWNERDEAD) {
		/* the owner died holding it: this thread owns it now, whatever pthread_mutex_consistent() says */
		(void)pthread_mutex_consistent(&ctrlr->ctrlr_lock);
		rc = 0;
	}
#endif
	if (rc != 0) {
		return false;
	}
	ctrlr->lock_depth++;
	return true;
}

/* n3: the frees a pass of g deferred, carried out as soon as no pass of g is active: at the end of a top-level
 * shared delete or drain (held: its controller, whose lock the caller holds; skip: the member whose own drain or
 * delete is running), so a free nested in a top-level drain completes before that call returns. Both kinds: a
 * delete op that ran nested (delete_deferred), and a free deferred by a flag (delete_after_completion_context: a
 * removed member's own cold-path callback, or a poll-group member freed under the tgroup's in_completion_context),
 * which never reached the delete op. Another controller's member needs its lock without waiting (no two controller
 * locks are ever nested in a blocking order: ABBA); if it is contended, the next attention pass frees it (DV23) */
static void
run_deferred(struct nvme_iau_group *g, struct spdk_nvme_ctrlr *held, struct nvme_iau_qpair *skip)
{
	struct nvme_iau_qpair *tried[IAU_TILE_MAX_ROUTES], *iq;
	uint32_t n_tried = 0, i;
	bool again = true;

	while (again && !g->in_member_pass && !g->in_ring_poll && n_tried < IAU_TILE_MAX_ROUTES) {
		again = false;
		TAILQ_FOREACH(iq, &g->members, link) {
			struct spdk_nvme_qpair *qp = iq->qpair;
			struct spdk_nvme_ctrlr *ctrlr = qp->ctrlr;

			if (iq == skip || qp->in_completion_context ||
			    (!iq->delete_deferred && !qp->delete_after_completion_context)) {
				continue;	/* not deferred, or inside its own completion context */
			}
			for (i = 0; i < n_tried && tried[i] != iq; i++) {
			}
			if (i < n_tried) {
				continue;	/* once per call */
			}
			tried[n_tried++] = iq;
			if (ctrlr != held && !ctrlr_trylock(ctrlr)) {
				continue;	/* contended: left to the attention pass (DV23) */
			}
			if (ctrlr == held) {
				nvme_ctrlr_lock(ctrlr);	/* recursive: the caller holds it */
			}
			if (iq->delete_deferred) {
				iq->delete_deferred = 0;
				(void)nvme_iau_ctrlr_delete_io_qpair(ctrlr, qp);
			} else {
				spdk_nvme_ctrlr_free_io_qpair(qp);	/* its own in_completion_context is 0 now */
			}
			nvme_ctrlr_unlock(ctrlr);
			again = true;	/* iq may be gone: the walk restarts */
			break;
		}
	}
}

static uint32_t route_lost_end(struct nvme_iau_qpair *iq, const char *reason);	/* P4.10f: with route_lost() */

/* D12: the generic duties a member's own spdk_nvme_qpair_process_completions() does that no group pass does */
static bool
needs_attn(struct spdk_nvme_qpair *qp)
{
	return nvme_qpair_get_state(qp) != NVME_QPAIR_ENABLED ||
	       qp->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE || qp->ctrlr->is_failed ||
	       !STAILQ_EMPTY(&qp->queued_req) || !STAILQ_EMPTY(&qp->aborting_queued_req) ||
	       !STAILQ_EMPTY(&qp->err_req_head) || qp->delete_after_completion_context ||
	       nvme_pcie_qpair(qp)->pcie_state == NVME_PCIE_QPAIR_FAILED;
}

/* every 64 group polls (D12): the generic-only state changes no transport op sees (err_req_head expiry, a reset's
 * failure reason, a controller failure, a free deferred from another member's callback) */
static void
sweep(struct nvme_iau_group *g)
{
	struct nvme_iau_qpair *iq;

	g->sweeps++;
	TAILQ_FOREACH(iq, &g->members, link) {
		if (needs_attn(iq->qpair)) {
			attn(g, iq);
		}
	}
}

/* the destroys a delete inside the member pass deferred (A2), once the pass is over */
static void
reap_dead(struct nvme_iau_group *g)
{
	struct nvme_iau_qpair *iq;

	while ((iq = TAILQ_FIRST(&g->dead)) != NULL) {
		TAILQ_REMOVE(&g->dead, iq, attn_link);
		nvme_pcie_qpair_destroy(iq->qpair);
	}
}

/*
 * The attention pass (D12, rev 2 I6, A2, A3): every member on g->attn gets its generic
 * spdk_nvme_qpair_process_completions(), which runs what the group pass does not (check_enabled, err_req, the
 * resubmission of queued requests, a deferred delete, the abort-queued completions). The list moves to a LOCAL
 * list first, and every entry's attn_list points at it, so a callback or a delete may unlink any member from
 * whichever list holds it (the delete op unlinks from *attn_list) and a member marked during the pass goes on
 * g->attn for the next pass (never lost). A member not connecting / connected / enabled is skipped: the stock
 * disconnected loop owns it (C4), and its -ENXIO never fails the group. skip (a direct poll's own member) is
 * skipped too: the caller's generic call does its duties (A2). After a generic call nothing of the member is
 * touched unless g->attn_cur still names it (the delete op clears it); its own generic call's return value is
 * the resubmission budget (nvme_iau_qpair_process_completions()). A delete inside the pass defers the qpair's
 * destroy to its end (A2). cb == NULL (a direct poll): a failed member goes back on the list for the next group
 * poll, which reports it. Returns -ENXIO if a member ENABLED before its call failed (R4), else 0.
 */
static int
attn_pass(struct nvme_iau_group *g, struct nvme_iau_qpair *skip, spdk_nvme_disconnected_qpair_cb cb, void *ctx)
{
	struct nvme_iau_qlist work = TAILQ_HEAD_INITIALIZER(work);
	struct nvme_iau_qpair *iq;
	int rc = 0;

	TAILQ_CONCAT(&work, &g->attn, attn_link);
	TAILQ_FOREACH(iq, &work, attn_link) {
		iq->attn_list = &work;
	}
	g->in_member_pass = true;
	while ((iq = TAILQ_FIRST(&work)) != NULL) {
		struct spdk_nvme_qpair *qp = iq->qpair;
		enum nvme_qpair_state st = nvme_qpair_get_state(qp);
		int32_t r;

		TAILQ_REMOVE(&work, iq, attn_link);
		iq->in_attn = 0;
		iq->attn_list = NULL;
		if (iq == skip) {
			continue;	/* the polled member: its own generic call is the outer one */
		}
		if (iq->delete_deferred) {
			/* a delete op that ran nested in a ring pass (I1): its drain, Delete I/O SQ / CQ and destroy now,
			 * outside the pass (the destroy itself at this pass's end, A2) */
			g->attn_calls++;
			run_deferred_one(iq);
			continue;
		}
		if (iq->lost_owed) {
			/* P4.10f (the P4.10c/d/e review's I1): a rejoin that ended LOST in its connect op owes its end; it
			 * runs here, outside any connect op and ring pass, under the completion-context guard
			 * (route_lost_end()); a free one of its callbacks asked for is carried out next (below, or the
			 * next pass if another member's callback asked for it) */
			g->attn_calls++;
			(void)route_lost_end(iq, "rejoin");
			if (qp->delete_after_completion_context) {
				spdk_nvme_ctrlr_free_io_qpair(qp);
			}
			continue;
		}
		if (qp->delete_after_completion_context) {
			/* a free a callback asked for inside a pass (deferred by the poll group's in_completion_context):
			 * carried out here for every state (the generic wrapper's own deferred free is unreachable for a
			 * DISCONNECTED member or a failed controller: it returns -ENXIO first); the destroy waits for the
			 * pass end (A2) */
			g->attn_calls++;
			spdk_nvme_ctrlr_free_io_qpair(qp);
			continue;
		}
		if (iq->fail_report) {
			/* a failure a direct poll's pass saw (it had no callback to report to) */
			if (cb == NULL) {
				attn(g, iq);	/* for the next group pass */
				continue;
			}
			iq->fail_report = 0;
			if (qp->poll_group_tailq_head == &g->tg->connected_qpairs) {
				/* a failed asynchronous connect: DISCONNECTED on the connected list, unseen by stock's
				 * disconnected loop. Anything else is on the disconnected list (check_enabled() moved a
				 * failed non-PCIe qpair there), whose stock loop reports it: no second call (I2) */
				rc = -ENXIO;
				cb(qp, ctx);
			}
			continue;
		}
		if (st != NVME_QPAIR_CONNECTING && st != NVME_QPAIR_CONNECTED && st != NVME_QPAIR_ENABLING &&
		    st != NVME_QPAIR_ENABLED) {
			continue;	/* C4: stock's disconnected loop serves it; it never fails the group */
		}
		g->attn_calls++;
		g->attn_cur = iq;
		r = spdk_nvme_qpair_process_completions(qp, 0);
		if (g->attn_cur != iq) {
			continue;	/* deleted inside its own generic call: not touched again (A2) */
		}
		g->attn_cur = NULL;
		if (r < 0) {
			/* R4: a called member's failure (an ENABLED member whose transport failed, or a CONNECTING one
			 * whose connect failed: DISCONNECTED on the connected list, which stock's disconnected loop never
			 * walks; stock's connected loop reports every rc < 0). A member removed from the poll group is not
			 * the group's to report: its own polls see -ENXIO */
			if (qp->poll_group != g->tg) {
				continue;
			}
			if (cb == NULL) {
				iq->fail_report = 1;
				attn(g, iq);
				continue;
			}
			rc = -ENXIO;
			cb(qp, ctx);	/* once per failure; the callback may free qp: nothing after it */
		} else if (!STAILQ_EMPTY(&qp->queued_req) || nvme_qpair_get_state(qp) != NVME_QPAIR_ENABLED) {
			attn(g, iq);	/* still queued requests, or still connecting: the next pass again */
		}
	}
	g->in_member_pass = false;
	reap_dead(g);
	return rc;
}

/* a pass over the K rings of a shared group (D12); a free a callback asks for is deferred to the attention pass.
 * P4.8e, the doorbell policy: the pass runs in rounds; a round consumes at most db_batch records (consume()'s own
 * budget bound, so the record loop is unchanged) from every ring that has budget left, in ring order. After a round
 * in which some ring returned a full chunk, the dirty doorbells are rung (the caller's stats) and the next round
 * starts; the pass ends after a round with no full chunk, or when every budget is spent. So the descriptors the
 * callbacks write reach the device during a long pass (the convoy), a ready record waits at most (K - 1) x db_batch
 * records of the other rings, and a descriptor waits at most one round (< K x db_batch records) for its doorbell.
 * db_batch 0 is UINT32_MAX here: one round, every ring up to its budget, the doorbells at the pass end only.
 * (P4.8e fix round I1: rounds, not each ring to its budget in turn, so that no ring waits for another's budget) */
static inline __attribute__((always_inline)) uint32_t
shared_pass(struct nvme_iau_group *g, uint32_t max, struct nvme_iau_qpair *for_iq, uint32_t *n_for, const int cnt,
	    struct spdk_nvme_pcie_stat *stat)
{
	uint32_t left[IAU_TILE_MAX_K], k, j, n = 0, c, m, l, b = g->db_batch;
	bool full, first = true;

	g->in_ring_poll = true;
	g->tg->in_completion_context = true;
	for (;;) {
		full = false;
		for (k = 0; k < g->k; k++) {
			struct nvme_iau_ring *r = &g->ring[k];

			/* the ring's budget left in this pass (round 1: its pass budget, as before P4.8e) */
			l = first ? ((max == 0 || max > r->budget) ? r->budget : max) : left[k];
			c = l < b ? l : b;	/* 0 once the budget is spent: consume() returns 0 */
			m = consume(g, r, c, for_iq, n_for, cnt);
			n += m;
			left[k] = l - m;
			full |= m == b;
		}
		if (spdk_likely(!full)) {
			break;	/* no ring returned a full chunk: each is empty or out of budget */
		}
		first = false;
		for (j = 0; j < g->k; j++) {
			doorbell(g, &g->ring[j], stat);	/* the caller's stat, as at the pass end */
		}
	}
	g->tg->in_completion_context = false;
	g->in_ring_poll = false;
	return n;
}

/* R3: a direct poll of a shared group's member consumes the group's rings (every member's records), runs the
 * attention pass (deferred deletes; not nested in another member pass) and returns the member's completions.
 * The attention pass's own generic call of a member does not consume (the group pass just did): it returns the
 * IO credits free now if requests are queued, the generic wrapper's resubmission budget (D12). */
static int32_t __attribute__((noinline)) NVME_IAU_HOT_ALIGN
shared_poll(struct nvme_iau_qpair *iq, uint32_t max, struct spdk_nvme_pcie_stat *stat)
{
	struct nvme_iau_group *g = iq->g;
	struct spdk_nvme_qpair *qp = iq->qpair;
	uint32_t n_for = 0, n, k;

	if (g->in_member_pass && g->attn_cur == iq) {
		return !STAILQ_EMPTY(&qp->queued_req) ? (int32_t)(iq->credits - qp->queue_depth) : 0;
	}
	n = shared_pass(g, max, iq, &n_for, CNT_ROUTE, stat);
	if (spdk_unlikely(++g->sweep == 64)) {
		g->sweep = 0;	/* every 64 polls of the group, group or direct (m1) */
		sweep(g);
	}
	if (!g->in_member_pass && spdk_unlikely(!TAILQ_EMPTY(&g->attn))) {
		(void)attn_pass(g, iq, NULL, NULL);
	}
	for (k = 0; k < g->k; k++) {
		doorbell(g, &g->ring[k], stat);
	}
	g->polls++;
	stat->polls++;
	if (n != 0) {
		stat->completions += n_for;
	} else {
		stat->idle_polls++;
		g->idle_polls++;
	}
	return (int32_t)n_for;
}

/* the route of iq drained (A4): GONE (R_DETACHED consumed) with no IO or control credit outstanding. On a shared
 * ring (P4.8; iau_tile_abi.h, vfio_user_iau_tile.h) also every D slot the route wrote fetched: the GCB d_head (the
 * engine's monotonic fetch count, A15) at or past the ring's d_written when the route went GONE (last_w, which
 * covers the route's last slot; RELEASE / REISSUE / FLUSH get no record, so queue_depth alone does not cover
 * them), or the ring closed (nothing of it is fetched again). Only then may the route's number and member gen
 * be released or rejoined: no slot of the old incarnation can be fetched under a later one (no gen ABA). */
static bool
route_drained(struct nvme_iau_qpair *iq)
{
	struct nvme_iau_group *g = iq->g;

	return iq->state == NVME_IAU_RT_GONE && iq->qpair->queue_depth == 0 && iq->ctrl_inflight == 0 &&
	       (g->is_private || g->gr[iq->route % g->k].closed ||
		__atomic_load_n(iq->ring->d_head, __ATOMIC_ACQUIRE) >= iq->last_w);
}

/* consume until drained or g_nvme_iau_drain_us passed (D11: 1 s); callbacks run as in a completion context.
 * It busy-waits (spdk_delay_us()): on a reconnect inside a controller reset that is up to 1 s under the
 * controller lock (nvme_ctrlr_reinitialize_io_qpair(), m6). */
static bool
drain(struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	uint64_t deadline = spdk_get_ticks() + spdk_get_ticks_hz() * g_nvme_iau_drain_us / SPDK_SEC_TO_USEC;
	uint8_t icc = qp->in_completion_context;
	uint32_t n_for = 0, k, n;

	if (iq->g->in_ring_poll) {
		return route_drained(iq);	/* nested in this group's own pass */
	}
	qp->in_completion_context = 1;
	while (!route_drained(iq)) {
		struct nvme_iau_group *g = iq->g;

		g->in_ring_poll = true;
		if (g->is_private) {
			for (k = 0, n = 0; k < g->k; k++) {
				n += consume(g, &g->ring[k], g->ring[k].r_entries, NULL, &n_for, CNT_NONE);
			}
		} else {
			/* a shared drain delivers every member's records; a free a callback asks for is deferred (the
			 * poll group's in_completion_context) to the attention pass */
			bool tic = g->tg->in_completion_context;

			g->tg->in_completion_context = true;
			for (k = 0, n = 0; k < g->k; k++) {
				n += consume(g, &g->ring[k], g->ring[k].r_entries, NULL, &n_for, CNT_GNONE);
			}
			g->tg->in_completion_context = tic;
		}
		iq->g->in_ring_poll = false;
		for (k = 0; k < iq->g->k; k++) {
			doorbell(iq->g, &iq->g->ring[k], iq_stat(iq));
		}
		if (n == 0) {
			if (spdk_get_ticks() > deadline) {
				break;
			}
			spdk_delay_us(1);
		}
	}
	qp->in_completion_context = icc;
	if (!iq->g->is_private) {
		run_deferred(iq->g, qp->ctrlr, iq);	/* a free nested in this top-level drain completes now (n3) */
	}
	return route_drained(iq);
}

static void abort_q_complete(struct nvme_iau_qpair *iq);	/* P4.10a: with the abort API, after the qpair ops */
static uint32_t route_lost(struct nvme_iau_qpair *iq, const char *reason);	/* P4.10d: with the delete op */
static void route_lost_mark(struct nvme_iau_qpair *iq);			/* P4.10f: with route_lost() */
static uint32_t leftovers_end(struct nvme_iau_qpair *iq, uint32_t dnr);	/* P4.10f: with route_lost() */

/* ------------------------------------------------------------------------------------------ the request path */

/*
 * P4.10c (plan cases 53-55), P4.10f (the P4.10c/d/e review's I4: the same API as stock): a RAW command's payload is
 * built exactly as stock builds it. The RAW SQE is opaque to the IAU (the engine copies it and writes only the cid, as
 * the tile's iau_sqe_builder takes in_raw_sqe), so whatever stock would hand the SSD passes through unchanged, PRP list
 * or hardware SGL. Each function below is the stock one (nvme_pcie_common.c, cited by line) with only these changes:
 * the SQE is built in a copy of req->cmd (the RAW slot; req->cmd stays as the application set it, so a retry rebuilds
 * the same SQE, DV2), the tracker's list page and its bus address are those of the command's in-flight index
 * (pq->tr[idx]: trackers are never used on a tile qpair; the index, so the page, is held until the command's final
 * record), and a failure returns non-zero where stock calls nvme_pcie_fail_request_bad_vtophys() (the caller hands
 * the command to the IAU to refuse: raw_refuse(), DV29). Stock's choice (nvme_pcie_qpair_submit_request_stock(),
 * :3571-3572, :3787-3829): psdt PRP by default; a payload_size of 0 builds nothing more; else
 * g_nvme_pcie_build_req_table[payload type][sgl_supported]
 * (:3333-3346) with sgl_supported = the controller's SGL_SUPPORTED flag (an IO queue; a DSM under
 * NVME_QUIRK_NO_SGL_FOR_DSM takes PRP) and dword_aligned false when SGL is supported without
 * SGL_REQUIRES_DWORD_ALIGNMENT; then nvme_pcie_qpair_build_metadata() (:3349-3395). On vfio-user the target reports
 * sgls.supported = 1 (lib/nvmf/ctrlr.c:391), so stock (and RAW) build a hardware SGL with dword_aligned false: a
 * contiguous payload of any alignment or size is one Data Block descriptor (:3048-3139, IOVA = VA: one mapping), an
 * SGL payload one descriptor per element run (empty elements skipped, physically adjacent ones merged unless
 * disable_pcie_sgl_merge, at most NVME_MAX_SGL_DESCRIPTORS = 250, a bit bucket for a READ; :3145-3275), MPTR the
 * metadata address (an SGL descriptor only with MPTR SGL support and dword alignment). P4.10f drops P4.10c's two
 * refusals stock does not make: an empty element is skipped on both paths (the PRP walk appends nothing for it,
 * :2938-3003 / :3296-3320) and a middle element that does not end on a page boundary is legal for the hardware SGL
 * and only asserted on the PRP path (:3305-3309), as stock.
 */

/* nvme_pcie_vtophys() (nvme_pcie_common.c:104-112): spdk_vtophys() on PCIe; on vfio-user IOVA = VA and *size as given */
static uint64_t
raw_vtophys(struct spdk_nvme_ctrlr *ctrlr, const void *buf, uint64_t *size)
{
	if (ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		return spdk_vtophys(buf, size);
	}
	return (uint64_t)(uintptr_t)buf;
}

/* stock's tracker, as far as its builders use it: the SQE they build, the list page and its bus address */
struct raw_tr {
	struct spdk_nvme_qpair		*qp;
	struct nvme_request		*req;
	struct spdk_nvme_cmd		*cmd;	/* stock: tr->req->cmd */
	struct nvme_tracker_list_page	*lp;	/* stock: tr->list_page */
	uint64_t			bus;	/* stock: tr->prp_sgl_bus_addr */
};

/* nvme_pcie_prp_list_append() (:2938-3003) */
static int
raw_prp_list_append(struct raw_tr *t, uint32_t *prp_index, void *virt_addr, size_t len, uint32_t page_size)
{
	struct spdk_nvme_cmd *cmd = t->cmd;
	uintptr_t page_mask = page_size - 1;
	uint64_t phys_addr;
	uint32_t i;

	if (spdk_unlikely(((uintptr_t)virt_addr & 3) != 0)) {
		NVME_QPAIR_ERRLOG(t->qp, "virt_addr %p not dword aligned\n", virt_addr);
		return -EFAULT;
	}
	i = *prp_index;
	while (len) {
		uint32_t seg_len;

		if (spdk_unlikely(i > SPDK_COUNTOF(t->lp->u.prp))) {
			NVME_QPAIR_ERRLOG(t->qp, "out of PRP entries\n");
			return -EFAULT;
		}
		phys_addr = raw_vtophys(t->qp->ctrlr, virt_addr, NULL);
		if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR)) {
			NVME_QPAIR_ERRLOG(t->qp, "vtophys(%p) failed\n", virt_addr);
			return -EFAULT;
		}
		if (i == 0) {
			cmd->dptr.prp.prp1 = phys_addr;
			seg_len = page_size - ((uintptr_t)virt_addr & page_mask);
		} else {
			if ((phys_addr & page_mask) != 0) {
				NVME_QPAIR_ERRLOG(t->qp, "PRP %u not page aligned (%p)\n", i, virt_addr);
				return -EFAULT;
			}
			t->lp->u.prp[i - 1] = phys_addr;
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
		cmd->dptr.prp.prp2 = t->lp->u.prp[0];
	} else {
		cmd->dptr.prp.prp2 = t->bus;
	}
	*prp_index = i;
	return 0;
}

/* nvme_pcie_qpair_build_request_invalid() (:3011-3017) */
static int
raw_build_invalid(struct raw_tr *t, bool dword_aligned)
{
	assert(0);
	return -EINVAL;
}

/* nvme_pcie_qpair_build_contig_request() (:3023-3039) */
static int
raw_build_contig(struct raw_tr *t, bool dword_aligned)
{
	uint32_t prp_index = 0;

	return raw_prp_list_append(t, &prp_index, (uint8_t *)t->req->payload.contig_or_cb_arg + t->req->payload_offset,
				   t->req->payload_size, t->qp->ctrlr->page_size);
}

/* nvme_pcie_qpair_build_contig_hw_sgl_request() (:3048-3139, this tree's: one mapping covering the payload goes into
 * SGL1 without staging it in the list page) */
static int
raw_build_contig_hw_sgl(struct raw_tr *t, bool dword_aligned)
{
	struct nvme_request *req = t->req;
	struct spdk_nvme_cmd *cmd = t->cmd;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint64_t phys_addr, mapping_length;
	uint32_t length, nseg = 0;
	uint8_t *virt_addr;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);
	sgl = t->lp->u.sgl;
	cmd->psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	cmd->dptr.sgl1.unkeyed.subtype = 0;
	length = req->payload_size;
	virt_addr = (uint8_t *)((uintptr_t)req->payload.contig_or_cb_arg + req->payload_offset);
	while (length > 0) {
		if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
			return -EFAULT;
		}
		if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
			NVME_QPAIR_ERRLOG(t->qp, "virt_addr %p not dword aligned\n", virt_addr);
			return -EFAULT;
		}
		mapping_length = length;
		phys_addr = raw_vtophys(t->qp->ctrlr, virt_addr, &mapping_length);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			return -EFAULT;
		}
		mapping_length = spdk_min(length, mapping_length);
		if (nseg == 0 && mapping_length == length) {
			cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			cmd->dptr.sgl1.address = phys_addr;
			cmd->dptr.sgl1.unkeyed.length = mapping_length;
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
		cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		cmd->dptr.sgl1.address = t->lp->u.sgl[0].address;
		cmd->dptr.sgl1.unkeyed.length = t->lp->u.sgl[0].unkeyed.length;
	} else {
		cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		cmd->dptr.sgl1.address = t->bus;
		cmd->dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}
	return 0;
}

/* nvme_pcie_qpair_build_hw_sgl_request() (:3145-3275) */
static int
raw_build_hw_sgl(struct raw_tr *t, bool dword_aligned)
{
	struct nvme_request *req = t->req;
	struct spdk_nvme_cmd *cmd = t->cmd;
	uint32_t remaining_transfer_len, remaining_user_sge_len, length, nseg = 0;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint64_t phys_addr, mapping_length;
	void *virt_addr;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);
	sgl = t->lp->u.sgl;
	cmd->psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	cmd->dptr.sgl1.unkeyed.subtype = 0;
	remaining_transfer_len = req->payload_size;
	while (remaining_transfer_len > 0) {
		if (req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &remaining_user_sge_len) != 0) {
			return -EFAULT;
		}
		/* Bit Bucket SGL descriptor */
		if ((uint64_t)virt_addr == UINT64_MAX) {
			if (cmd->opc != SPDK_NVME_OPC_READ) {
				NVME_QPAIR_ERRLOG(t->qp, "Only READ command can be supported\n");
				return -EFAULT;
			}
			if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
				NVME_QPAIR_ERRLOG(t->qp, "Too many SGL entries\n");
				return -EFAULT;
			}
			sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_BIT_BUCKET;
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
				NVME_QPAIR_ERRLOG(t->qp, "Too many SGL entries\n");
				return -EFAULT;
			}
			if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
				NVME_QPAIR_ERRLOG(t->qp, "virt_addr %p not dword aligned\n", virt_addr);
				return -EFAULT;
			}
			mapping_length = remaining_user_sge_len;
			phys_addr = raw_vtophys(t->qp->ctrlr, virt_addr, &mapping_length);
			if (phys_addr == SPDK_VTOPHYS_ERROR) {
				return -EFAULT;
			}
			length = spdk_min(remaining_user_sge_len, mapping_length);
			remaining_user_sge_len -= length;
			virt_addr = (uint8_t *)virt_addr + length;
			if (!nvme_pcie_qpair(t->qp)->flags.disable_pcie_sgl_merge && nseg > 0 &&
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
		cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		cmd->dptr.sgl1.address = t->lp->u.sgl[0].address;
		cmd->dptr.sgl1.unkeyed.length = t->lp->u.sgl[0].unkeyed.length;
	} else {
		cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		cmd->dptr.sgl1.address = t->bus;
		cmd->dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}
	return 0;
}

/* nvme_pcie_qpair_build_prps_sgl_request() (:3281-3328) */
static int
raw_build_prps_sgl(struct raw_tr *t, bool dword_aligned)
{
	struct nvme_request *req = t->req;
	uint32_t remaining_transfer_len, length, prp_index = 0, page_size = t->qp->ctrlr->page_size;
	void *virt_addr;
	int rc;

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);
	remaining_transfer_len = req->payload_size;
	while (remaining_transfer_len > 0) {
		assert(req->payload.next_sge_fn != NULL);
		if (req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &length) != 0) {
			return -EFAULT;
		}
		length = spdk_min(remaining_transfer_len, length);
		/* All SGEs except last must end on a page boundary (stock asserts it, :3305-3309) */
		assert((length == remaining_transfer_len) ||
		       ((((uintptr_t)virt_addr + length) & (page_size - 1)) == 0));
		rc = raw_prp_list_append(t, &prp_index, virt_addr, length, page_size);
		if (rc) {
			return rc;
		}
		remaining_transfer_len -= length;
	}
	return 0;
}

/* g_nvme_pcie_build_req_table (:3333-3346) */
static int (*const g_raw_build_table[][2])(struct raw_tr *t, bool dword_aligned) = {
	[NVME_PAYLOAD_TYPE_INVALID] = { raw_build_invalid, raw_build_invalid },
	[NVME_PAYLOAD_TYPE_CONTIG] = { raw_build_contig, raw_build_contig_hw_sgl },
	[NVME_PAYLOAD_TYPE_SGL] = { raw_build_prps_sgl, raw_build_hw_sgl },
};

/* nvme_pcie_qpair_build_metadata() (:3349-3395) */
static int
raw_build_metadata(struct raw_tr *t, bool sgl_supported, bool mptr_sgl_supported, bool dword_aligned)
{
	struct nvme_request *req = t->req;
	struct spdk_nvme_cmd *cmd = t->cmd;
	uint64_t mapping_length;
	void *md_payload;

	if (req->payload.md) {
		md_payload = (uint8_t *)req->payload.md + req->md_offset;
		if (dword_aligned && ((uintptr_t)md_payload & 3)) {
			NVME_QPAIR_ERRLOG(t->qp, "virt_addr %p not dword aligned\n", md_payload);
			return -EINVAL;
		}
		mapping_length = req->md_size;
		if (sgl_supported && mptr_sgl_supported && dword_aligned) {
			/* The metadata SGL descriptor is at the tail of the list page. */
			struct spdk_nvme_sgl_descriptor *meta_sgl = &t->lp->meta_sgl;

			assert(cmd->psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
			cmd->psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
			meta_sgl->address = raw_vtophys(t->qp->ctrlr, md_payload, &mapping_length);
			if (meta_sgl->address == SPDK_VTOPHYS_ERROR || mapping_length != req->md_size) {
				return -EINVAL;
			}
			meta_sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			meta_sgl->unkeyed.length = req->md_size;
			meta_sgl->unkeyed.subtype = 0;
			cmd->mptr = t->bus + offsetof(struct nvme_tracker_list_page, meta_sgl);
		} else {
			cmd->mptr = raw_vtophys(t->qp->ctrlr, md_payload, &mapping_length);
			if (cmd->mptr == SPDK_VTOPHYS_ERROR || mapping_length != req->md_size) {
				return -EINVAL;
			}
		}
	}
	return 0;
}

/* the RAW SQE of request req at in-flight index idx: req->cmd (cid 0: the engine writes its tag) with stock's payload
 * (nvme_pcie_qpair_submit_request_stock(), :3787-3829); non-zero: the command cannot be described (stock:
 * nvme_pcie_fail_request_bad_vtophys()) */
static int
raw_build(struct nvme_iau_qpair *iq, struct nvme_request *req, uint16_t idx, struct spdk_nvme_cmd *sqe)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct spdk_nvme_ctrlr *ctrlr = qp->ctrlr;
	struct nvme_pcie_qpair *pq = nvme_pcie_qpair(qp);
	struct raw_tr t = { qp, req, sqe, pq->tr[idx].list_page, pq->tr[idx].prp_sgl_bus_addr };
	bool sgl_supported, mptr_sgl_supported, dword_aligned = true;
	enum nvme_payload_type payload_type;

	*sqe = req->cmd;
	sqe->cid = 0;			/* the engine writes its tag */
	sqe->psdt = SPDK_NVME_PSDT_PRP;	/* stock's default for every command, payload or not (:3571-3572) */
	if (req->payload_size == 0) {
		return 0;
	}
	payload_type = nvme_payload_type(&req->payload);
	sgl_supported = (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) != 0 && !nvme_qpair_is_admin_queue(qp);
	mptr_sgl_supported = (ctrlr->flags & SPDK_NVME_CTRLR_MPTR_SGL_SUPPORTED) != 0 && !nvme_qpair_is_admin_queue(qp);
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
	if (g_raw_build_table[payload_type][sgl_supported](&t, dword_aligned) < 0) {
		return -EFAULT;
	}
	return raw_build_metadata(&t, sgl_supported, mptr_sgl_supported, dword_aligned) < 0 ? -EFAULT : 0;
}

/* the opcode and nsid of a descriptor the IAU must refuse: 0xFF has no IO kind (iau_tile_io_len()) and the broadcast
 * nsid is no route's, so iau_tile_sqe_from_io() returns -ENOTSUP (as the RTL's iau_sqe_builder: NOTSUP) */
#define NVME_IAU_REFUSED_OPC	0xFFu
#define NVME_IAU_REFUSED_NSID	0xFFFFFFFFu

/*
 * P4.10c (plan case 55), P4.10f (DV29 reworded): a RAW payload stock's builders cannot describe fails as stock fails it,
 * nvme_pcie_fail_request_bad_vtophys() (nvme_pcie_common.c:2912): INVALID_FIELD with dnr, printed, through the
 * completion callback: the submit returns 0 (:3815-3829 "never in the context of the submission") and the command stays
 * in flight until then (its index and credit held, iterate_requests visits it, a teardown ends it as any request). Stock
 * marks the tracker and fails it at the end of the qpair's next poll (has_pending_vtophys_failures, :2682-2691), or at
 * once when the submit runs in the qpair's completion context (:2914). tile adds no test to its polls: it hands the request
 * to the IAU in an IO-kind descriptor the IAU refuses (NVME_IAU_REFUSED_OPC / _NSID). That refusal is an item of the
 * IAU contract: an IO-kind descriptor the SQE builder cannot build (its opcode has no IO kind, or its nsid is neither 0
 * nor the route's) completes R_CPL INVALID_FIELD dnr, tag NONE, w0 / w1 as written (nvme_iau_req_done / the request),
 * without a slot or an SSD command (the C engine, vfio_user_iau_tile.c; the consistency gate's C-side vectors pin it,
 * scripts/rtl/iau/vectors_fetch.txt; the RTL fetch path's vector is P5.7 / P5.9's). Its delivery waits for the IAU's fetch of
 * that descriptor, so it is device-dependent (a later poll, also from a completion context; ABORTED_SQ_DELETION if the
 * route ends first: a FLUSH before its fetch by the engine's FLUSHED rule, a CLOSED or LOST route through inflight[],
 * abort_leftovers()); a later poll delivers it as any final record (deliver(): printed, the index and credit back, then
 * the callback). The engine counts it (rec_inval).
 */
static void
raw_refuse(struct nvme_iau_qpair *iq, struct nvme_request *req)
{
	uint64_t d[8] = { 0 };

	NVME_QPAIR_ERRLOG(iq->qpair, "vtophys or other payload buffer related error\n");
	d[0] = iau_tile_d0(NVME_IAU_REFUSED_OPC, 0, iq->route, 0, iau_tile_cflags(0, iq->gen));
	d[4] = NVME_IAU_REFUSED_NSID;
	d[5] = (uint64_t)(uintptr_t)nvme_iau_req_done;
	d[6] = (uint64_t)(uintptr_t)req;
	d_store(iq->ring, d);
}

/* the RAW of request req at in-flight index idx: the header descriptor (kind RAW, d5 / d6 the request), then the SQE
 * (raw_build()) in the next slot; a payload that cannot be built is handed to the IAU to refuse (raw_refuse()). 0 (a
 * descriptor written), or -ENOTSUP at stride 48 (D5: nothing written). Out of line (P4.10f: its callers' streams,
 * submit_other() among the pinned ones, never take in the builders) */
static int __attribute__((noinline))
nvme_iau_submit_raw(struct nvme_iau_qpair *iq, struct nvme_request *req, uint16_t idx)
{
	struct nvme_iau_ring *r = iq->ring;
	struct spdk_nvme_cmd sqe;
	uint64_t d[8] = { 0 }, *s;

	if (r->stride != IAU_TILE_DESC_BYTES) {
		return -ENOTSUP;	/* RAW is refused at stride 48 (D5) */
	}
	if (raw_build(iq, req, idx, &sqe) != 0) {
		raw_refuse(iq, req);
		return 0;
	}
	d[0] = iau_tile_d0(0, (uint8_t)(IAU_TILE_K_RAW << IAU_TILE_F_KIND_SHIFT), iq->route, 0,
			 iau_tile_cflags(iq->tmo_us != 0 ? IAU_TILE_CF_TMO : 0, iq->gen));
	d[5] = (uint64_t)(uintptr_t)nvme_iau_req_done;
	d[6] = (uint64_t)(uintptr_t)req;
	d_store(r, d);
	s = (uint64_t *)(r->d + (size_t)r->d_tail * IAU_TILE_DESC_BYTES);	/* the next slot: the SQE itself */
	memcpy(s, &sqe, sizeof(sqe));
	d_advance(r);
	iq->st.sub_raw++;
	return 0;
}

/* the IO-kind descriptor in the next D slot: d0, then d1, d2 (buf), d4 (nsid), d5 / d6 (request-backed) */
static inline __attribute__((always_inline)) void
d_store_io(struct nvme_iau_qpair *iq, struct nvme_request *req, uint64_t d0, uint64_t buf)
{
	struct nvme_iau_ring *r = iq->ring;
	uint64_t *d = (uint64_t *)(r->d + (size_t)r->d_tail * IAU_TILE_DESC_BYTES);

	d[0] = d0;
	d[1] = iau_tile_d1_from_cmd(&req->cmd);
	d[2] = buf;
	d[4] = req->cmd.nsid;
	d[5] = (uint64_t)(uintptr_t)nvme_iau_req_done;
	d[6] = (uint64_t)(uintptr_t)req;
	d_advance(r);
	iq->st.sub_req++;
}

/* the submit's common end: the IO credit, the stat, the doorbell unless delay_cmd_submit */
static inline __attribute__((always_inline)) int
submit_end(struct spdk_nvme_qpair *qpair, struct nvme_pcie_qpair *pqpair, struct nvme_iau_qpair *iq)
{
	qpair->queue_depth++;
	pqpair->stat->submitted_requests++;
	if (!pqpair->flags.delay_cmd_submit) {
		doorbell(iq->g, iq->ring, pqpair->stat);
	}
	return 0;
}

/* no IO credit: the generic layer queues the request (-EAGAIN); a shared group's member goes on the attention
 * list, whose generic call resubmits it (D12: the group poll calls no member's generic poll otherwise) */
static int __attribute__((noinline)) NVME_IAU_HOT_ALIGN
submit_full(struct nvme_pcie_qpair *pqpair, struct nvme_iau_qpair *iq)
{
	pqpair->stat->queued_requests++;
	if (!iq->g->is_private) {
		attn(iq->g, iq);
	}
	return -EAGAIN;
}

/* everything nvme_iau_cmd_rw_fast() does not take (P4.7e): the full IO test (nvme_iau_req_is_io()), then the
 * generic d0, or RAW. Out of line, so the fast submit is one straight path */
static int __attribute__((noinline)) NVME_IAU_HOT_ALIGN
submit_other(struct spdk_nvme_qpair *qpair, struct nvme_request *req, uint16_t idx)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_iau_qpair *iq = nvme_iau_qp(qpair);
	uint64_t buf;
	int rc;

	if (nvme_iau_req_is_io(iq, req, &buf)) {
		d_store_io(iq, req, iau_tile_d0_from_cmd(&req->cmd, iq->d0_tmpl), buf);
	} else {
		rc = nvme_iau_submit_raw(iq, req, idx);
		if (rc != 0) {
			iq->inflight[idx] = NULL;
			iq->idx_stack[iq->idx_top++] = idx;
			return rc;
		}
	}
	return submit_end(qpair, pqpair, iq);
}

static int NVME_IAU_HOT_ALIGN
nvme_iau_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_pcie_qpair *pqpair;
	struct nvme_iau_qpair *iq;
	uint64_t buf;
	uint16_t idx;

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		return g_stock.qpair_submit_request(qpair, req);
	}
	pqpair = nvme_pcie_qpair(qpair);
	iq = nvme_iau_qp(qpair);
	if (spdk_unlikely(iq->state != NVME_IAU_RT_ACTIVE)) {
		return -ENXIO;
	}
	if (spdk_unlikely(qpair->queue_depth >= iq->credits)) {
		return submit_full(pqpair, iq);
	}
	/* a free index: every index in use belongs to a command counted in queue_depth (request-free IO, P4.9,
	 * holds a credit without one), and indices and the IO credit return together before a callback runs, so
	 * queue_depth < credits implies idx_top > 0 (P4.7e: no second test) */
	assert(iq->idx_top != 0);
	idx = iq->idx_stack[--iq->idx_top];
	iq->inflight[idx] = req;
	req->cmd.cid = idx;
	/* P4.7e: the fast IO test (nvme_iau_cmd_rw_fast(): an unfused READ / WRITE) and its d0 builder
	 * (nvme_iau_d0_rw_fast()); whatever the test does not take goes to the full test (submit_other()): the same
	 * classification, the same d0 */
	buf = (uint64_t)(uintptr_t)req->payload.contig_or_cb_arg + req->payload_offset;
	if (spdk_unlikely(req->payload_size == 0 || nvme_payload_type(&req->payload) != NVME_PAYLOAD_TYPE_CONTIG ||
			  req->payload.md != NULL || !nvme_iau_cmd_rw_fast(iq, &req->cmd, buf))) {
		return submit_other(qpair, req, idx);
	}
	d_store_io(iq, req, nvme_iau_d0_rw_fast(&req->cmd, iq->d0_tmpl), buf);
	return submit_end(qpair, pqpair, iq);
}

/* the 48 B lever (I1v6n48): the same, the six words packed (d_store()) */
static int NVME_IAU_HOT_ALIGN
nvme_iau_qpair_submit_request48(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_pcie_qpair *pqpair;
	struct nvme_iau_qpair *iq;
	uint64_t d[8] = { 0 }, buf;
	uint16_t idx;

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		return g_stock.qpair_submit_request(qpair, req);
	}
	pqpair = nvme_pcie_qpair(qpair);
	iq = nvme_iau_qp(qpair);
	if (spdk_unlikely(iq->state != NVME_IAU_RT_ACTIVE)) {
		return -ENXIO;
	}
	if (spdk_unlikely(!nvme_iau_req_is_io(iq, req, &buf))) {
		return -ENOTSUP;	/* RAW is refused at 48 B (D5) */
	}
	if (spdk_unlikely(qpair->queue_depth >= iq->credits || iq->idx_top == 0)) {
		pqpair->stat->queued_requests++;
		return -EAGAIN;
	}
	idx = iq->idx_stack[--iq->idx_top];
	iq->inflight[idx] = req;
	req->cmd.cid = idx;
	d[0] = iau_tile_d0_from_cmd(&req->cmd, iq->d0_tmpl);
	d[1] = iau_tile_d1_from_cmd(&req->cmd);
	d[2] = buf;
	d[4] = req->cmd.nsid;
	d[5] = (uint64_t)(uintptr_t)nvme_iau_req_done;
	d[6] = (uint64_t)(uintptr_t)req;
	d_store(iq->ring, d);
	iq->st.sub_req++;
	qpair->queue_depth++;
	pqpair->stat->submitted_requests++;
	if (!pqpair->flags.delay_cmd_submit) {
		doorbell(iq->g, iq->ring, pqpair->stat);
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------ the request-free path */

#define NVME_IAU_FAST_IO_FLAGS	(SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS | SPDK_NVME_IO_FLAGS_LIMITED_RETRY)
SPDK_STATIC_ASSERT(SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS == 1u << 30 && SPDK_NVME_IO_FLAGS_LIMITED_RETRY == 1u << 31 &&
		   ((SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS >> 28) & 0xCu) << 8 == (uint64_t)IAU_TILE_F_FUA << 8 &&
		   ((SPDK_NVME_IO_FLAGS_LIMITED_RETRY >> 28) & 0xCu) << 8 == (uint64_t)IAU_TILE_F_LR << 8,
		   "FUA / LR land in d0 flags [2] / [3] as nvme_iau_d0_rw_fast() puts cdw12 bits 30 / 31");

/* the generic layer prints a submitted command under the "nvme" debug flag (_nvme_qpair_submit_request()); the
 * request-free one is printed as the SQE the request path would build (a DEBUG build only) */
static void __attribute__((noinline))
fast_print_cmd(struct spdk_nvme_qpair *qpair, const uint64_t *d)
{
	struct spdk_nvme_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = iau_tile_d0_opc(d[0]);
	cmd.nsid = (uint32_t)d[4];
	cmd.cdw10 = (uint32_t)d[1];
	cmd.cdw11 = (uint32_t)(d[1] >> 32);
	cmd.cdw12 = (uint32_t)iau_tile_d0_nlb(d[0]) | ((iau_tile_d0_flags(d[0]) & IAU_TILE_F_FUA) ? 1u << 30 : 0u) |
		    ((iau_tile_d0_flags(d[0]) & IAU_TILE_F_LR) ? 1u << 31 : 0u);
	cmd.dptr.prp.prp1 = d[2];
	spdk_nvme_qpair_print_command(qpair, &cmd);
}

/*
 * P4.9 (D14) / P4.9b (P4.9 review I2, M6): a contiguous READ / WRITE without an nvme_request, called by
 * spdk_nvme_ns_cmd_{read,write}{,_with_md} (nvme_ns_cmd.c) on a qpair with iau_fast set (a tile qpair: latch_fast()) with
 * nvme_ns_cmd_rw_contig()'s twelve arguments. One test of every qualification term (nvme_iau.h), the run-time terms
 * first (a free IO credit, the three lists empty: the ones that decline on a running qpair, so such a decline is the
 * cheapest); a declined IO writes nothing, is counted and goes to nvme_ns_cmd_rw_contig(), the entries' unchanged stock
 * body, with the same arguments: the same argument list and stack-argument area, so GCC emits a jump (a sibcall; the
 * ns_layout guard checks it). A taken IO writes the IO-kind descriptor the request path writes for the same command
 * (nvme_iau_d0_rw_fast() of its SQE: opc, FUA / LR in flags [2] / [3], the route's d0 template, nlb = lba_count - 1;
 * d1 = lba, d2 = the buffer (IOVA = VA on vfio-user), d4 = nsid) with d5 / d6 = the user's cb_fn / cb_arg; the IO
 * credit (queue_depth), num_outstanding_reqs (the outstanding count stock keeps through the request), the stats and,
 * unless delay_cmd_submit, the doorbell, as submit_end() does. opc is the entry's constant (SPDK_NVME_OPC_READ /
 * WRITE), so it is used as d0's opcode byte without a mask (a mask would add an instruction to every taken IO).
 * P4.11: this body is tile_fast_rw(), reached from nvme_iau_fast_rw() below (noipa and used: its assembler name is
 * referenced by the x86-64 dispatcher's jump; nocf_check: reached only by that direct jump, so no endbr64).
 */
static int __attribute__((noipa, used, nocf_check)) NVME_IAU_HOT_ALIGN
tile_fast_rw(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, void *metadata, uint64_t lba,
	   uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc, uint32_t io_flags,
	   uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_iau_qpair *iq = nvme_iau_qp(qpair);
	struct nvme_iau_ring *r;
	uint64_t *d;

	if (spdk_unlikely(qpair->queue_depth >= iq->credits ||
			  ((uintptr_t)STAILQ_FIRST(&qpair->queued_req) | (uintptr_t)TAILQ_FIRST(&qpair->err_cmd_head) |
			   (uintptr_t)STAILQ_FIRST(&qpair->err_req_head)) != 0 ||	/* the three lists empty: one test */
			  metadata != NULL || (io_flags & ~NVME_IAU_FAST_IO_FLAGS) != 0 || lba_count - 1u >= iq->fast_nlb ||
			  ((uintptr_t)buffer & 3u) != 0 || ns->id != iq->nsid ||
			  (iq->stripe != 0 && (lba & (iq->stripe - 1)) + lba_count > iq->stripe) ||
			  nvme_qpair_get_state(qpair) != NVME_QPAIR_ENABLED ||
			  qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE ||
			  iq->state != NVME_IAU_RT_ACTIVE || qpair->ctrlr->is_failed)) {
		iq->st.declined++;
		return nvme_ns_cmd_rw_contig(ns, qpair, buffer, metadata, lba, lba_count, cb_fn, cb_arg, opc, io_flags,
					     apptag_mask, apptag);
	}
	r = iq->ring;
	d = (uint64_t *)(r->d + (size_t)r->d_tail * IAU_TILE_DESC_BYTES);
	d[0] = (uint64_t)opc | ((uint64_t)((io_flags >> 28) & 0xCu) << 8) | iq->d0_tmpl | ((uint64_t)(lba_count - 1) << 32);
	d[1] = lba;
	d[2] = (uint64_t)(uintptr_t)buffer;
	d[4] = iq->nsid;
	d[5] = (uint64_t)(uintptr_t)cb_fn;
	d[6] = (uint64_t)(uintptr_t)cb_arg;
	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		fast_print_cmd(qpair, d);	/* as stock (nothing without DEBUG) */
	}
	d_advance(r);
	qpair->queue_depth++;
	qpair->num_outstanding_reqs++;
	iq->st.sub_fast++;
	pqpair->stat->submitted_requests++;
	if (!pqpair->flags.delay_cmd_submit) {
		doorbell(iq->g, r, pqpair->stat);
	}
	return 0;
}

/*
 * P4.11 (the S2 ablation; the controller's fairness rule: S2 dispatches through the same hook): the four contiguous
 * entries call nvme_iau_fast_rw() on iau_fast, which is set only on tile and S2 qpairs, so the kind's S2 bit alone tells
 * them apart. One test, then a tail jump with the twelve arguments untouched: an S2 qpair to nvme_iau_s2_fast_rw()
 * (nvme_iau_s2.c), a tile one to tile_fast_rw() above (the P4.9b body; static, so without the endbr64 of its old global
 * entry). On x86-64 the dispatcher is three instructions (endbr64 with CET, testb, jne, jmp) at this object's .text start,
 * 64 B aligned: written in C, GCC re-extends the two uint16_t stack arguments before each sibcall (four instructions per
 * IO on both paths), and a test at the top of the tile body is not shrink-wrapped (its prologue and six stack-argument
 * loads come first, so S2 would pay them and their epilogue on every IO). NVME_IAU_KIND_BYTE / _S2_BIT name where
 * iau_kind's S2 bit lies in struct spdk_nvme_qpair; nvme_iau_select_ops() checks them at registration (iau_kind_bits_ok()),
 * the bits line (SIZE 304 QD 14) pins the struct.
 */
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
#define NVME_IAU_STR_(x)	#x
#define NVME_IAU_STR(x)		NVME_IAU_STR_(x)
#ifdef __CET__
#define NVME_IAU_ENDBR		"\tendbr64\n"
#else
#define NVME_IAU_ENDBR		""
#endif
__asm__(".pushsection .text,\"ax\",@progbits\n\t.p2align 6\n\t.globl nvme_iau_fast_rw\n"
	"\t.type nvme_iau_fast_rw,@function\nnvme_iau_fast_rw:\n" NVME_IAU_ENDBR
	"\ttestb $" NVME_IAU_STR(NVME_IAU_KIND_S2_BIT) ", " NVME_IAU_STR(NVME_IAU_KIND_BYTE) "(%rsi)\n"
	"\tjne nvme_iau_s2_fast_rw@PLT\n\tjmp tile_fast_rw\n\t.size nvme_iau_fast_rw, .-nvme_iau_fast_rw\n\t.popsection");
#else
int NVME_IAU_HOT_ALIGN
nvme_iau_fast_rw(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer, void *metadata, uint64_t lba,
		 uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc, uint32_t io_flags,
		 uint16_t apptag_mask, uint16_t apptag)
{
	if (spdk_unlikely((qpair->iau_kind & NVME_IAU_KIND_S2) != 0)) {
		return nvme_iau_s2_fast_rw(ns, qpair, buffer, metadata, lba, lba_count, cb_fn, cb_arg, opc, io_flags,
					   apptag_mask, apptag);
	}
	return tile_fast_rw(ns, qpair, buffer, metadata, lba, lba_count, cb_fn, cb_arg, opc, io_flags, apptag_mask, apptag);
}
#endif

/* ------------------------------------------------------------------------------------------ completions */

static int32_t NVME_IAU_HOT_ALIGN
nvme_iau_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_iau_qpair *iq;
	uint32_t n_for = 0, n;
	uint16_t priv_poll;
	int rc;

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		return g_stock.qpair_process_completions(qpair, max_completions);
	}
	if (spdk_unlikely(pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED)) {
		return -ENXIO;
	}
	iq = nvme_iau_qp(qpair);
	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		/* the stock connect state machine (nvme_pcie_md_poll()); the route is bound once Create I/O SQ
		 * succeeded */
		if (pqpair->pcie_state == NVME_PCIE_QPAIR_READY) {
			if (iq->state == NVME_IAU_RT_JOINED) {
				iq->state = NVME_IAU_RT_ACTIVE;
			}
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
	if (spdk_unlikely(iq->state < NVME_IAU_RT_ACTIVE || iq->state > NVME_IAU_RT_FLUSHING)) {
		return -ENXIO;	/* never bound, GONE or LOST */
	}
	memcpy(&priv_poll, &iq->g->is_private, sizeof(priv_poll));
	if (spdk_unlikely(priv_poll != NVME_IAU_PRIVATE_IDLE)) {
		if (iq->g->in_ring_poll) {
			return 0;	/* nested inside this group's own pass */
		}
		return shared_poll(iq, max_completions, pqpair->stat);	/* a shared group's member (P4.8, R3) */
	}
	n = group_pass(iq->g, max_completions, iq, &n_for, pqpair->stat);
	pqpair->stat->polls++;
	if (n != 0) {
		pqpair->stat->completions += n_for;
	} else {
		pqpair->stat->idle_polls++;
	}
	return (int32_t)n_for;
}

/* ------------------------------------------------------------------------------------------ registration */

static volatile uint8_t *
iau_page(struct spdk_nvme_ctrlr *ctrlr)
{
	return (volatile uint8_t *)nvme_pcie_ctrlr(ctrlr)->uncore_sq_desc_iova_reg;
}

static volatile uint64_t *
page_u64(struct spdk_nvme_ctrlr *ctrlr, uint32_t off, uint16_t qid)
{
	return (volatile uint64_t *)(iau_page(ctrlr) + (off - SQ_DESC_RING_IOVA_OFFSET)) + qid;
}

static volatile uint32_t *
page_u32(struct spdk_nvme_ctrlr *ctrlr, uint32_t off, uint16_t qid)
{
	return (volatile uint32_t *)(iau_page(ctrlr) + (off - SQ_DESC_RING_IOVA_OFFSET)) + qid;
}

static void
page_write(struct spdk_nvme_ctrlr *ctrlr, volatile uint64_t *reg, uint64_t v)
{
	g_thread_mmio_ctrlr = nvme_pcie_ctrlr(ctrlr);
	spdk_mmio_write_8(reg, v);
	g_thread_mmio_ctrlr = NULL;
}

/* the route's registration (before Create I/O CQ / SQ): the GCB member line (valid last), QCFG, QCFG2 */
static int
publish(struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct nvme_iau_group *g = iq->g;
	volatile struct iau_tile_gcb_member *m = &g->gcb->member[iq->route];
	uint64_t qcfg2 = iau_tile_qcfg2_pack(g->gcb_iova, iq->route, iq->gen);

	if (qcfg2 == IAU_QCFG2_INVALID) {
		NVME_QPAIR_ERRLOG(qp, "IAU_TILE_REFUSED qid=%u: QCFG2 cannot name GCB %#" PRIx64 " route %u gen %u\n",
				  qp->id, g->gcb_iova, iq->route, iq->gen);
		return -EINVAL;
	}
	m->valid = 0;
	spdk_wmb();
	m->ring = (uint16_t)(iq->route % g->k);
	m->qid = qp->id;
	m->credits = iq->credits;
	m->nsid = iq->nsid;
	m->timeout_us = iq->tmo_us;
	m->entries = iq->entries;
	m->echo_iova = iq->echo_iova;
	m->lba_shift = iq->lba_shift;
	m->gen = iq->gen;
	spdk_wmb();
	m->valid = IAU_TILE_MEMBER_VALID;
	iq->d0_tmpl = iau_tile_d0(0, 0, iq->route, 0, iau_tile_cflags(iq->tmo_us != 0 ? IAU_TILE_CF_TMO : 0, iq->gen));
	page_write(qp->ctrlr, page_u64(qp->ctrlr, IAU_QCFG_OFFSET, qp->id), nvme_pcie_iau_qcfg_pack(iq->credits, iq->nsid));
	page_write(qp->ctrlr, page_u64(qp->ctrlr, IAU_QCFG2_OFFSET, qp->id), qcfg2);
	return 0;
}

/* a fresh (or reset) private ring: zeroed memory and positions, GCB d_head = d_tail = r_consumed = 0 */
static void
ring_reset(struct nvme_iau_group *g, uint32_t k)
{
	struct nvme_iau_ring *r = &g->ring[k];

	memset(r->d, 0, (size_t)r->d_entries * r->stride);
	memset((void *)r->r, 0, (size_t)r->r_entries * IAU_TILE_REC_BYTES);
	r->d_tail = r->d_rung = 0;
	r->r_head = 0;
	r->r_phase = 1;
	r->d_written = 0;
	r->r_seq = 0;
	g->gcb->ring[k].d_head = 0;
	g->gcb->ring[k].r_consumed = 0;
	g->gcb->db[k].d_tail = 0;
	spdk_wmb();
}

static void
group_free(struct nvme_iau_group *g)
{
	uint32_t k;

	if (g == NULL) {
		return;
	}
	for (k = 0; k < IAU_TILE_MAX_K; k++) {
		spdk_free(g->ring[k].d);
		spdk_free((void *)g->ring[k].r);
	}
	spdk_free(g->gcb);
	spdk_free(g->ctx);
	free(g);
}

/*
 * P4.10c: the GCB ctx region (A19 as amended by A21; the RTL is the design standard). The tile carves one route block
 * per held route out of it at BIND (iau_mmio.v step 7, :742-774): B(s) = 64 (5 s + ((4 - s mod 4) mod 4)) bytes
 * (f_csize, :254-260; 64 s slot lines, the 256 B-aligned PRP-list slab, s lists of 256 B), s = the route's credits
 * under the tile's reset SLOT_POLICY (:21, :443, :528). The host sizes the region so that every route set the
 * group can hold fits:
 *  - a private group (D4: one route of c credits): B(c);
 *  - a shared group: P4.10c sized it as A21 (the sum over its K rings of M(U_k), U_k = floor(min(d_entries_k - 1,
 *    r_entries_k) / 3), M(U) = 64 (5 U - 2 - ((U + 2) mod 4)): the most any credit mix can hold, the target's
 *    iau_tile_ctx_ring_bytes()); from P4.10f every member has the group's one credit count (A23, pg_join()), and the
 *    region is that count's exact maximum, ctx_ring_bytes_eq() below (never more than M(U_k) per ring).
 * The region is zeroed and 4 KiB aligned, in one allocation (one DMA registration: the target maps it once per member);
 * the GCB publishes it before any member binds and keeps it while any ring of the GCB is bound (the group's life).
 * The testbed engine ignores it unless the target's IAU_TILE_CTX_SPILL=1 (P4.10b). Cold: registration and teardown.
 */
static uint64_t
ctx_block_bytes(uint32_t s)
{
	return 64ull * (5ull * s + (4u - s % 4u) % 4u);
}

/*
 * P4.10f (A23, the equal-credit join rule; the P4.10c/d/e review's I3): a shared group's routes all have the group's c
 * credits, so ring k holds at most n_k = min(floor(U_k / (c + 1)), routes_k) blocks of B(c) (the invariant
 * sum (3 c + 3) <= L_k = min(d_entries_k - 1, r_entries_k), U_k = floor(L_k / 3); routes_k = the routes r < 256 with
 * r mod K == k), and the region is sum_k n_k B(c): every set the invariant admits fits (the sum), and since every held
 * block has the one size B(c) and first fit only moves to block ends from 0, every block starts at a multiple of B(c),
 * so a bind the sum admits always finds a free multiple below the region's end (no CTX_FRAG). It is exact (the
 * maximum set fills it) and never more than A21's credit-free M(U_k) per ring
 */
static uint64_t
ctx_ring_bytes_eq(uint32_t d_entries, uint32_t r_entries, uint32_t k, uint32_t nk, uint32_t c)
{
	uint64_t l = d_entries == 0 ? 0 : (uint64_t)d_entries - 1, n, routes = (IAU_TILE_MAX_ROUTES - 1u - k) / nk + 1u;

	l = r_entries < l ? r_entries : l;
	n = (l / 3u) / ((uint64_t)c + 1u);
	n = n < routes ? n : routes;
	return n * ctx_block_bytes(c);
}

/* the region of `bytes` (A21), zeroed, 4 KiB aligned; 0, -EINVAL (no route fits: 0 bytes, or more than a GCB's u32),
 * -ENOMEM or -EFAULT (no IOVA) */
static int
ctx_region(struct nvme_iau_group *g, struct spdk_nvme_ctrlr *ctrlr, uint64_t bytes)
{
	if (bytes == 0 || bytes > UINT32_MAX) {
		return -EINVAL;
	}
	g->ctx = spdk_zmalloc((size_t)((bytes + 4095u) & ~4095ull), 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (g->ctx == NULL) {
		return -ENOMEM;
	}
	g->ctx_iova = iau_iova(ctrlr, g->ctx);
	if (g->ctx_iova == SPDK_VTOPHYS_ERROR) {
		return -EFAULT;
	}
	g->ctx_bytes = (uint32_t)bytes;
	return 0;
}

/* ------------------------------------------------------------------------------------------ shared groups (P4.8) */

/*
 * D3: a PCIe poll group of a tile process owns ONE shared group: a GCB of K = SPDK_IAU_RINGS ring pairs of
 * SPDK_IAU_RING_ENTRIES entries each (D13 default 16384), n_routes = IAU_TILE_MAX_ROUTES (a member line is valid only
 * while its route has a member). A member gets the lowest free route r at poll_group_add (its credits are known
 * then) and is served by ring r mod K; the target binds ring k on engine (gid + k) mod n_engines. The host's
 * invariant mirrors the engine's member_add (D13, A1): the sum of iau_tile_route_need_d/_r over the routes the
 * target reserves on ring k fits (iau_tile_ring_fits()), else IAU_TILE_JOIN_REFUSED and -ENOSPC. A route's
 * reservation lives from its join until its Delete I/O CQ succeeded (the target's release, P4.4); a route left
 * without one (LOST, a removed or resetting controller, a failed Delete I/O CQ) keeps it (route_held) and is not
 * reused until its ring is reset. The member gen is per (GCB, route) and advances on every join and rejoin
 * (route_gen, A15: the engine refuses the gen of the route's last incarnation while the ring lives).
 */
struct nvme_iau_pg {
	struct nvme_pcie_poll_group	pg;	/* first: the PCIe code finds it from the tgroup (shared stats) */
	struct nvme_iau_group		*g;	/* created at the first tile member's add */
};

static struct nvme_iau_pg *
iau_pg(struct spdk_nvme_transport_poll_group *tg)
{
	return SPDK_CONTAINEROF(tg, struct nvme_iau_pg, pg.group);
}

static struct nvme_iau_group *
group_shared(struct nvme_iau_pg *pg, struct spdk_nvme_ctrlr *ctrlr, uint32_t credits)
{
	uint32_t e = g_nvme_iau_cfg.ring_entries, stride = g_nvme_iau_cfg.stride, k;
	struct nvme_iau_group *g = calloc(1, sizeof(*g));
	uint64_t ctx;

	if (g == NULL) {
		return NULL;
	}
	g->gid = __atomic_fetch_add(&g_next_gid, 1, __ATOMIC_RELAXED);
	g->k = g_nvme_iau_cfg.rings;
	g->n_routes = IAU_TILE_MAX_ROUTES;
	g->is_private = false;
	g->credits = credits;	/* P4.10f (A23): fixed for the group's life */
	g->owner = pthread_self();	/* the poll group's thread: the one that adds its first member (m10) */
	g->db_batch = g_nvme_iau_cfg.db_batch != 0 ? g_nvme_iau_cfg.db_batch : UINT32_MAX;	/* P4.8e; 0: off */
	g->tg = &pg->pg.group;
	g->stat = &pg->pg.stats;
	TAILQ_INIT(&g->members);
	TAILQ_INIT(&g->attn);
	TAILQ_INIT(&g->dead);
	g->gcb = spdk_zmalloc(IAU_TILE_GCB_BYTES, IAU_TILE_GCB_ALIGN, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (g->gcb == NULL) {
		group_free(g);
		return NULL;
	}
	g->gcb_iova = iau_iova(ctrlr, g->gcb);
	for (k = 0; k < g->k; k++) {
		struct nvme_iau_ring *r = &g->ring[k];

		r->d_entries = r->r_entries = e;
		r->stride = stride;
		r->d = spdk_zmalloc((size_t)e * stride, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		r->r = spdk_zmalloc((size_t)e * IAU_TILE_REC_BYTES, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (r->d == NULL || r->r == NULL) {
			group_free(g);
			return NULL;
		}
		r->d_iova = iau_iova(ctrlr, r->d);
		r->r_iova = iau_iova(ctrlr, (void *)r->r);
		if (r->d_iova == SPDK_VTOPHYS_ERROR || r->r_iova == SPDK_VTOPHYS_ERROR) {
			group_free(g);
			return NULL;
		}
		r->db = &g->gcb->db[k].d_tail;
		r->d_head = &g->gcb->ring[k].d_head;
		r->r_consumed = g_nvme_iau_cfg.rcons ? &g->gcb->ring[k].r_consumed : NULL;
		g->gcb->ring[k].d_iova = r->d_iova;
		g->gcb->ring[k].r_iova = r->r_iova;
		g->gcb->ring[k].d_entries = e;
		g->gcb->ring[k].r_entries = e;
		g->gcb->ring[k].d_stride = stride;
		ring_reset(g, k);
		r->budget = NVME_IAU_EMPTY_RING_BUDGET;
	}
	for (k = 0, ctx = 0; k < g->k; k++) {
		/* P4.10c: A21, shared; P4.10f: A23, the group's one credit count */
		ctx += ctx_ring_bytes_eq(g->ring[k].d_entries, g->ring[k].r_entries, k, g->k, credits);
	}
	if (g->gcb_iova == SPDK_VTOPHYS_ERROR || ctx_region(g, ctrlr, ctx) != 0) {
		group_free(g);
		return NULL;
	}
	g->gcb->magic = IAU_TILE_GCB_MAGIC;
	g->gcb->version = IAU_TILE_GCB_VERSION;
	g->gcb->k = g->k;
	g->gcb->n_routes = g->n_routes;
	g->gcb->gid = g->gid;
	g->gcb->ctx_iova = g->ctx_iova;		/* P4.10c: the ctx region, before any member binds (A21) */
	g->gcb->ctx_bytes = g->ctx_bytes;
	g->gcb->flags = g_nvme_iau_cfg.rcons ? IAU_TILE_GCB_F_RCONS : 0;
	spdk_wmb();
	fprintf(stderr, "IAU_TILE_GROUP gid=%u k=%u entries=%u stride=%u private=0\n", g->gid, g->k, e, stride);
	return g;
}

/* P4.10f (A23): a member with more credits than its group takes the group's (it joins IDLE: nothing in flight, the
 * tables keep their size; the free indices are 0 .. c - 1) */
static void
credits_cut(struct nvme_iau_qpair *iq, uint32_t c)
{
	uint32_t i;

	iq->credits = c;
	for (i = 0; i < c; i++) {
		iq->idx_stack[i] = (uint16_t)(c - 1 - i);	/* the first pops hand out 0, 1, 2, ... */
	}
	iq->idx_top = (uint16_t)c;
}

/* poll_group_add of a tile qpair (DISCONNECTED, never connected): its route and ring in the poll group's group */
static int
pg_join(struct nvme_iau_pg *pg, struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct nvme_iau_group *g;
	struct nvme_iau_ring *r;
	uint64_t nd, nr;
	uint32_t rt, k, e;

	if (iq->state != NVME_IAU_RT_IDLE || iq->g != NULL) {
		NVME_QPAIR_ERRLOG(qp, "IAU_TILE_JOIN_REFUSED qid=%u reason=connected: a v6 qpair joins a poll group before its "
				  "first connect\n", qp->id);
		return -EINVAL;
	}
	if (pg->g == NULL) {
		/* P4.10f (A23): the group's credit count is its first member's, so a member that cannot join creates none */
		e = g_nvme_iau_cfg.ring_entries;
		nd = iau_tile_route_need_d(iq->credits);
		nr = iau_tile_route_need_r(iq->credits);
		if (!iau_tile_ring_fits(nd, nr, e, e)) {
			NVME_QPAIR_ERRLOG(qp, "IAU_TILE_JOIN_REFUSED qid=%u gid=none ring=0 credits=%u need_d=%" PRIu64 " need_r=%"
					  PRIu64 " entries=%u\n", qp->id, iq->credits, nd, nr, e);
			return -ENOSPC;
		}
		pg->g = group_shared(pg, qp->ctrlr, iq->credits);
		if (pg->g == NULL) {
			return -ENOMEM;
		}
	}
	g = pg->g;
	if (iq->credits < g->credits) {
		/* P4.10f (A23): its queue holds fewer commands than the group's count */
		NVME_QPAIR_ERRLOG(qp, "IAU_TILE_JOIN_REFUSED qid=%u gid=%u reason=credits credits=%u group_credits=%u "
				  "entries=%u: its queue (or request pool) holds fewer commands than its shared group's "
				  "credit count, which every member takes (A23)\n", qp->id, g->gid, iq->credits, g->credits,
				  iq->entries);
		return -EINVAL;
	}
	for (rt = 0; rt < IAU_TILE_MAX_ROUTES && (g->route[rt] != NULL || g->route_held[rt]); rt++) {
	}
	if (rt == IAU_TILE_MAX_ROUTES) {
		NVME_QPAIR_ERRLOG(qp, "IAU_TILE_JOIN_REFUSED qid=%u gid=%u reason=routes: all %u routes taken\n", qp->id, g->gid,
				  IAU_TILE_MAX_ROUTES);
		return -ENOSPC;
	}
	k = rt % g->k;
	r = &g->ring[k];
	nd = r->need_d + iau_tile_route_need_d(g->credits);	/* P4.10f (A23): every member takes the group's count */
	nr = r->need_r + iau_tile_route_need_r(g->credits);
	if (!iau_tile_ring_fits(nd, nr, r->d_entries, r->r_entries)) {
		NVME_QPAIR_ERRLOG(qp, "IAU_TILE_JOIN_REFUSED qid=%u gid=%u ring=%u credits=%u need_d=%" PRIu64 " need_r=%" PRIu64
				  " entries=%u\n", qp->id, g->gid, k, g->credits, nd, nr, r->d_entries);
		return -ENOSPC;
	}
	if (iq->credits != g->credits) {
		credits_cut(iq, g->credits);	/* P4.11b (the P4.11 review's m2): cut only once the join fits */
	}
	r->need_d = nd;
	r->need_r = nr;
	/* a ring's pass budget: its members' completion caps (a memberless ring keeps a small one, below) */
	r->budget = (g->gr[k].n_on == 0 ? 0 : r->budget) + nvme_pcie_qpair(qp)->max_completions_cap;
	g->route_resv[rt] = (uint16_t)iq->credits;
	g->gr[k].n_on++;
	iq->g = g;
	iq->route = (uint16_t)rt;
	iq->ring = r;
	iq->gen = iau_tile_member_gen_next(g->route_gen[rt]);
	g->route_gen[rt] = iq->gen;
	g->route_qp[rt] = qp;
	g->route[rt] = iq;
	TAILQ_INSERT_TAIL(&g->members, iq, link);
	g->n_members++;
	return 0;
}

/* before a member's registration on ring k: a closed ring (no engine ring bound) starts from the host's zeroed
 * positions (iau_tile_gcb_fresh()); its reservations died with the old engine ring. A tainted ring is never reset. */
static int
group_ring_prep(struct nvme_iau_qpair *iq)
{
	struct nvme_iau_group *g = iq->g;
	uint32_t k = iq->route % g->k, rt;

	if (g->is_private || !g->gr[k].closed) {
		return 0;
	}
	assert(!g->gr[k].tainted);	/* CLOSED proved no engine ring is bound to it (close_ring(), m3) */
	ring_reset(g, k);
	g->gr[k].closed = 0;
	for (rt = k; rt < IAU_TILE_MAX_ROUTES; rt += g->k) {
		if (g->route_held[rt]) {
			g->ring[k].need_d -= iau_tile_route_need_d(g->route_resv[rt]);
			g->ring[k].need_r -= iau_tile_route_need_r(g->route_resv[rt]);
			g->route_held[rt] = 0;
			g->route_resv[rt] = 0;
		}
		if (g->route[rt] != NULL) {
			/* a GONE member not yet deleted: the CLOSED ended every slot it wrote, so its drain must not wait
			 * for the reset ring's d_head to pass the old incarnation's d_written; its reservation stays counted
			 * on the host (more than the new target ring holds: conservative) until its leave */
			g->route[rt]->last_w = 0;
		}
	}
	return 0;
}

static struct spdk_nvme_transport_poll_group *
nvme_iau_pg_create(void)
{
	struct nvme_iau_pg *pg = calloc(1, sizeof(*pg));

	return pg == NULL ? NULL : &pg->pg.group;
}

static int
nvme_iau_pg_add(struct spdk_nvme_transport_poll_group *tg, struct spdk_nvme_qpair *qpair)
{
	if (qpair->iau_kind != NVME_IAU_KIND_TILE) {
		/* a group pass serves tile members only (m7) */
		NVME_QPAIR_ERRLOG(qpair, "IAU_TILE_JOIN_REFUSED qid=%u reason=not_tile\n", qpair->id);
		return -EINVAL;
	}
	return pg_join(iau_pg(tg), nvme_iau_qp(qpair));
}

/* the member stays in its group (its route bound, its records consumed by later passes) until its delete; only
 * the qpair's stats go (the stock remove) */
static int
nvme_iau_pg_remove(struct spdk_nvme_transport_poll_group *tg, struct spdk_nvme_qpair *qpair)
{
	struct nvme_iau_qpair *iq = nvme_iau_qp(qpair);

	if (iq->g != NULL && !iq->g->is_private) {
		/* its records leave the fast path (whose inline completion runs inside the poll group's completion
		 * context): slow_record() delivers them with cold_cb(), in the qpair's own (I1) */
		iq->g->route_qp[iq->route] = NULL;
	}
	return g_stock.poll_group_remove(tg, qpair);
}

/*
 * The group poll (D12, O(K)): the stock disconnected loop; one pass over the K rings with the poll group's
 * in_completion_context set (every member's records; a free a callback asks for is deferred); every 64 polls
 * the sweep; the attention pass only if its list is not empty; at most K doorbells (one per ring whose D tail
 * moved). polls / idle_polls count group polls on the PCIe group stats and in IAU_TILE_GROUP_END (the stock poll
 * counts one poll per connected member); completions counts every delivery of the pass. -ENXIO if a member
 * ENABLED before its attention call failed (R4; disconnected_qpair_cb ran for it), else the deliveries.
 */
static int64_t NVME_IAU_HOT_ALIGN
nvme_iau_pg_process_completions(struct spdk_nvme_transport_poll_group *tg, uint32_t cpq,
				spdk_nvme_disconnected_qpair_cb cb)
{
	struct nvme_iau_group *g = iau_pg(tg)->g;
	struct spdk_nvme_qpair *qp, *tmp;
	uint32_t n_for = 0, k;
	int rc = 0;

	STAILQ_FOREACH_SAFE(qp, &tg->disconnected_qpairs, poll_group_stailq, tmp) {
		cb(qp, tg->group->ctx);
	}
	if (spdk_unlikely(g == NULL || g->in_ring_poll || g->in_member_pass)) {
		return 0;	/* no member yet, or a callback of this group's own pass */
	}
	(void)shared_pass(g, cpq, NVME_IAU_FOR_ALL, &n_for, CNT_ALL, g->stat);
	if (spdk_unlikely(++g->sweep == 64)) {
		g->sweep = 0;
		sweep(g);
	}
	if (spdk_unlikely(!TAILQ_EMPTY(&g->attn))) {
		rc = attn_pass(g, NULL, cb, tg->group->ctx);
	}
	for (k = 0; k < g->k; k++) {
		doorbell(g, &g->ring[k], g->stat);
	}
	g->polls++;
	g->stat->polls++;
	if (n_for != 0) {
		g->stat->completions += n_for;
	} else {
		g->stat->idle_polls++;
		g->idle_polls++;
	}
	return rc != 0 ? rc : (int64_t)n_for;
}

/* DV11: -EBUSY while a member is left (or inside the group's own pass); a LOST member's rings are kept */
static int
nvme_iau_pg_destroy(struct spdk_nvme_transport_poll_group *tg)
{
	struct nvme_iau_pg *pg = iau_pg(tg);
	struct nvme_iau_group *g = pg->g;
	int rc;

	if (g != NULL && (g->n_members != 0 || g->in_ring_poll || g->in_member_pass)) {
		return -EBUSY;
	}
	rc = g_stock.poll_group_destroy(tg);	/* -EBUSY while the generic lists hold a qpair; else frees pg */
	if (rc == 0 && g != NULL && !g->keep) {
		group_free(g);
	}
	return rc;
}

/* D4: a private group of one for iq (route 0, K = 1, d = r = iau_tile_private_entries(credits)) */
static int
group_private(struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_ctrlr *ctrlr = iq->qpair->ctrlr;
	struct nvme_iau_group *g;
	struct nvme_iau_ring *r;
	uint32_t e = iau_tile_private_entries(iq->credits), stride = g_nvme_iau_cfg.stride;
	int rc;

	g = calloc(1, sizeof(*g));
	if (g == NULL) {
		return -ENOMEM;
	}
	/* two controllers' qpairs may be created at once (m1) */
	g->gid = __atomic_fetch_add(&g_next_gid, 1, __ATOMIC_RELAXED);
	g->k = 1;
	g->n_routes = 1;
	g->is_private = true;
	g->gcb = spdk_zmalloc(IAU_TILE_GCB_BYTES, IAU_TILE_GCB_ALIGN, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	r = &g->ring[0];
	r->d_entries = r->r_entries = e;
	r->stride = stride;
	r->d = g->gcb == NULL ? NULL : spdk_zmalloc((size_t)e * stride, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	r->r = r->d == NULL ? NULL : spdk_zmalloc((size_t)e * IAU_TILE_REC_BYTES, 4096, NULL, SPDK_ENV_NUMA_ID_ANY,
			SPDK_MALLOC_DMA);
	if (e == 0 || r->r == NULL) {
		group_free(g);
		return -ENOMEM;
	}
	g->gcb_iova = iau_iova(ctrlr, g->gcb);
	r->d_iova = iau_iova(ctrlr, r->d);
	r->r_iova = iau_iova(ctrlr, (void *)r->r);
	if (g->gcb_iova == SPDK_VTOPHYS_ERROR || r->d_iova == SPDK_VTOPHYS_ERROR || r->r_iova == SPDK_VTOPHYS_ERROR) {
		group_free(g);
		return -EFAULT;
	}
	rc = ctx_region(g, ctrlr, ctx_block_bytes(iq->credits));	/* P4.10c: A21, private: B(c) */
	if (rc != 0) {
		group_free(g);
		return rc;
	}
	r->db = &g->gcb->db[0].d_tail;
	r->d_head = &g->gcb->ring[0].d_head;
	r->r_consumed = g_nvme_iau_cfg.rcons ? &g->gcb->ring[0].r_consumed : NULL;
	r->budget = nvme_pcie_qpair(iq->qpair)->max_completions_cap;
	r->need_d = iau_tile_route_need_d(iq->credits);
	r->need_r = iau_tile_route_need_r(iq->credits);
	if (!iau_tile_ring_fits(r->need_d, r->need_r, r->d_entries, r->r_entries)) {
		NVME_QPAIR_ERRLOG(iq->qpair, "IAU_TILE_JOIN_REFUSED qid=%u credits=%u entries=%u\n", iq->qpair->id,
				  iq->credits, e);
		group_free(g);
		return -ENOSPC;
	}
	g->gcb->magic = IAU_TILE_GCB_MAGIC;
	g->gcb->version = IAU_TILE_GCB_VERSION;
	g->gcb->k = 1;
	g->gcb->n_routes = 1;
	g->gcb->gid = g->gid;
	g->gcb->ctx_iova = g->ctx_iova;		/* P4.10c: the ctx region, before the member binds (A21) */
	g->gcb->ctx_bytes = g->ctx_bytes;
	g->gcb->flags = g_nvme_iau_cfg.rcons ? IAU_TILE_GCB_F_RCONS : 0;
	g->gcb->ring[0].d_iova = r->d_iova;
	g->gcb->ring[0].r_iova = r->r_iova;
	g->gcb->ring[0].d_entries = r->d_entries;
	g->gcb->ring[0].r_entries = r->r_entries;
	g->gcb->ring[0].d_stride = r->stride;
	ring_reset(g, 0);
	iq->g = g;
	iq->route = 0;
	iq->ring = &g->ring[0];
	g->route_qp[0] = iq->qpair;
	g->route[0] = iq;
	g->n_members = 1;
	fprintf(stderr, "IAU_TILE_GROUP gid=%u k=%u entries=%u stride=%u private=1\n", g->gid, g->k, e, stride);
	return 0;
}

/* the timeout the route carries (D9: latched at every (re)connect) */
static void
latch_timeout(struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct spdk_nvme_ctrlr_process *p = qp->active_proc;
	uint64_t us;

	iq->tmo_us = 0;
	if (qp->ctrlr->timeout_enabled && p != NULL && p->timeout_cb_fn != NULL && p->timeout_io_ticks != 0) {
		us = p->timeout_io_ticks * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
		iq->tmo_us = us == 0 ? 1 : us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
	}
}

/* P4.9 (D14): the request-free path on this connection: SPDK_IAU_FAST=1 and a namespace without metadata, PI or
 * extended LBA (refused at create), page size 4096, stride 64; its lba_count bound and stripe latched with it */
static void
latch_fast(struct nvme_iau_qpair *iq)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(qp->ctrlr, iq->nsid);

	qp->iau_fast = 0;
	iq->fast_nlb = 0;
	iq->stripe = 0;
	if (!g_nvme_iau_cfg.fast || ns == NULL || ns->md_size != 0 || (ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) != 0 ||
	    ns->extended_lba_size != ns->sector_size || iq->ring->stride != IAU_TILE_DESC_BYTES ||
	    qp->ctrlr->page_size != IAU_TILE_PAGE) {
		return;
	}
	iq->fast_nlb = spdk_min(iq->nlb_lim, ns->sectors_per_max_io);
	iq->stripe = ns->sectors_per_stripe;
	qp->iau_fast = 1;
}

static void
print_qpair(struct nvme_iau_qpair *iq)
{
	fprintf(stderr, "IAU_TILE_QPAIR qid=%u gid=%u route=%u ring=%u gen=%u credits=%u nsid=%u fast=%u tmo_us=%u "
		"private=%u\n", iq->qpair->id, iq->g->gid, iq->route, iq->route % iq->g->k, iq->gen, iq->credits,
		iq->nsid, iq->qpair->iau_fast, iq->tmo_us, iq->g->is_private);
}

/* ------------------------------------------------------------------------------------------ qpair ops */

/* the per-qpair tables; the echo slab (written by the engine) stays allocated with keep_dma (a LOST route) */
static void
iq_free_tables(struct nvme_iau_qpair *iq, bool keep_dma)
{
	free(iq->inflight);
	free(iq->idx_stack);
	free(iq->pend);
	if (!keep_dma) {
		spdk_free(iq->echo);
	}
	iq->inflight = NULL;
	iq->idx_stack = NULL;
	iq->pend = NULL;
	iq->echo = NULL;
}

/* the IAU_TILE_LOST line, once per incarnation (P4.9 review M1: the generic layer calls qpair_abort_reqs at every poll
 * of a removed controller); n = the request-free IO in flight without a callback (P4.9 review M2: the IO credits still
 * held once abort_leftovers() ended the requests, which hold an in-flight index; controls hold none) */
static void
report_lost(struct nvme_iau_qpair *iq, uint32_t n, const char *reason)
{
	if (iq->lost_reported) {
		return;
	}
	iq->lost_reported = 1;
	fprintf(stderr, "IAU_TILE_LOST qid=%u n=%u reason=%s\n", iq->qpair->id, n, reason);
}

static struct spdk_nvme_qpair *
nvme_iau_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid, const struct spdk_nvme_io_qpair_opts *opts)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_iau_pqpair *x;
	struct nvme_pcie_qpair *pqpair;
	struct spdk_nvme_qpair *qpair;
	struct nvme_iau_qpair *iq;
	struct spdk_nvme_ns *ns = NULL;
	uint32_t nsid, next_nsid, i;
	const char *why = NULL;
	int rc;

	/* refusals (D14, F4, P4.4): before any allocation */
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	next_nsid = nsid != 0 ? spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid) : 0;
	if (nsid != 0 && next_nsid == 0) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	}
	if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_VFIOUSER) {
		why = "not a vfio-user controller (A5 and R4 / I2 rely on stock's non-PCIe check_enabled())";
	} else if (pctrlr->uncore_sq_desc_iova_reg == NULL) {
		why = "the IAU page is not mapped";
	} else if (qid == 0 || qid >= IAU_TILE_QIDS) {
		why = "qid outside the IAU page tables";
	} else if (nsid == 0 || next_nsid != 0 || ns == NULL) {
		why = "not exactly one active namespace (QCFG names one)";
	} else if (ctrlr->shadow_doorbell != NULL) {
		why = "shadow doorbells (the host would skip doorbell writes)";
	} else if (ctrlr->page_size != IAU_TILE_PAGE) {
		why = "page size is not 4096";
	} else if (opts->io_queue_size < 2 || opts->io_queue_size > IAU_TILE_MAX_SLOTS) {
		why = "queue size outside 2..4095 (a route has at most IAU_TILE_MAX_SLOTS slots)";
	} else if ((ns->flags & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED) != 0 || ns->sector_size < 512 ||
		   (ns->sector_size & (ns->sector_size - 1)) != 0) {
		why = "extended-LBA or non-power-of-two sector namespace";
	}
	if (why != NULL) {
		NVME_CTRLR_ERRLOG(ctrlr, "IAU_TILE_REFUSED qid=%u: %s\n", qid, why);
		errno = ENOTSUP;
		return NULL;
	}

	/* nvme_pcie_ctrlr_create_io_qpair() with room for the tile state after the PCIe qpair */
	x = spdk_zmalloc(sizeof(*x), 64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	if (x == NULL) {
		return NULL;
	}
	pqpair = &x->pq;
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
	iq = &x->iq;
	iq->qpair = qpair;
	iq->state = NVME_IAU_RT_IDLE;
	iq->nsid = nsid;
	iq->lba_shift = (uint8_t)__builtin_ctz(ns->sector_size);
	iq->nlb_lim = nvme_iau_nlb_lim(iq->nsid, iq->lba_shift);
	iq->entries = pqpair->num_entries;
	iq->credits = spdk_min((uint32_t)(pqpair->num_entries - pqpair->max_completions_cap), opts->io_queue_requests);
	TAILQ_INIT(&iq->abort_q);
	iq->inflight = calloc(iq->credits, sizeof(*iq->inflight));
	iq->idx_stack = calloc(iq->credits, sizeof(*iq->idx_stack));
	iq->pend = calloc(iq->credits, sizeof(*iq->pend));
	iq->echo = spdk_zmalloc((size_t)iq->entries * sizeof(struct spdk_nvme_cmd), 4096, NULL, SPDK_ENV_NUMA_ID_ANY,
				SPDK_MALLOC_DMA);
	if (iq->credits == 0 || iq->inflight == NULL || iq->idx_stack == NULL || iq->pend == NULL || iq->echo == NULL) {
		iq_free_tables(iq, false);
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}
	iq->echo_iova = iau_iova(ctrlr, iq->echo);
	if (iq->echo_iova == SPDK_VTOPHYS_ERROR) {
		iq_free_tables(iq, false);
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}
	for (i = 0; i < iq->credits; i++) {
		iq->idx_stack[i] = (uint16_t)(iq->credits - 1 - i);	/* the first pops hand out 0, 1, 2, ... */
	}
	iq->idx_top = (uint16_t)iq->credits;
	/* the host never writes the SSD doorbells of a tile queue: both point at the CQ sink (a tripwire) */
	pqpair->sq_tdbl = page_u32(ctrlr, IAU_CQDB_SINK_OFFSET, qid);
	pqpair->cq_hdbl = page_u32(ctrlr, IAU_CQDB_SINK_OFFSET, qid);
	qpair->iau_kind = NVME_IAU_KIND_TILE;
	return qpair;
}

static int
nvme_iau_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_iau_qpair *iq;
	int rc;

	if (nvme_qpair_is_admin_queue(qpair)) {
		return g_stock.ctrlr_connect_qpair(ctrlr, qpair);
	}
	iq = nvme_iau_qp(qpair);
	switch (iq->state) {
	case NVME_IAU_RT_IDLE:
		latch_timeout(iq);
		/* a qpair in a tile poll group joined its shared group at poll_group_add (P4.8); else a private one (D4) */
		rc = iq->g == NULL ? group_private(iq) : group_ring_prep(iq);
		break;
	case NVME_IAU_RT_JOINED:
		/* a connect that never bound (Create I/O SQ failed or never ran): nothing was issued; a private ring is
		 * reset, a shared one only if it is closed (other members may be live on it) */
		latch_timeout(iq);
		if (iq->g->is_private) {
			ring_reset(iq->g, iq->route % iq->g->k);
			rc = 0;
		} else {
			rc = group_ring_prep(iq);
		}
		break;
	case NVME_IAU_RT_LOST:
		NVME_QPAIR_ERRLOG(qpair, "IAU_TILE_RESET_REFUSED qid=%u state=lost (terminal)\n", qpair->id);
		return -EIO;
	default:
		/* a reconnect (A6): the old incarnation ends first (FLUSH once, drain); then a rejoin. Inside a
		 * controller reset (spdk_nvme_ctrlr_reconnect_poll_async()) a -EBUSY / -EIO here fails the whole
		 * controller (nvme_ctrlr_fail(), m4). A shared route that is GONE writes no FLUSH: nothing of the route
		 * may follow its last slot (route_gone()) on a ring other members keep using */
		if (!iq->g->is_private && (ctrlr->is_resetting || (iq->g->in_ring_poll && iq->reset_deferred &&
				pthread_equal(iq->g->owner, pthread_self())))) {
			/* P4.10a (n4): the stock reset reinitializes every IO qpair from the resetting context
			 * (spdk_nvme_ctrlr_reconnect_poll_async(), under the controller lock, on whatever thread resets),
			 * but a shared group's rings are consumed on its poll group's thread only (DV26): no drain, no ring
			 * work and no group list here. The reconnect goes to the member's own poll the way stock hands over a
			 * qpair the resetting context cannot reinitialize (a foreign process's): FAILURE_RESET, and the
			 * qpair CONNECTED; once the reset is over, its generic poll (the attention pass: the sweep finds it)
			 * enables it, disconnects it and reinitializes it there (nvme_qpair_check_enabled()), which runs this
			 * op again on the group's thread with is_resetting clear. The reset itself does not fail. Meanwhile
			 * the group pass consumes the route's records (the target's disable detached it).
			 * P4.10d (P4.10a review I1): that reinitialize can also run nested in a pass of the group, after the
			 * reset ended: a callback of the pass resubmits on the member, the generic submit's check_enabled()
			 * enables it and reinitializes it there. A member carrying a reset deferral is deferred again then
			 * (nested=1): rc 0, FAILURE_RESET and CONNECTED kept (the resubmit is queued, as stock queues a
			 * request on a CONNECTED qpair), on the attention list, whose generic call outside the pass
			 * reconnects it; never the -EBUSY below, which would end it DISCONNECTED with FAILURE_LOCAL */
			fprintf(stderr, "IAU_TILE_RESET_DEFERRED qid=%u gid=%u route=%u%s\n", qpair->id, iq->g->gid, iq->route,
				ctrlr->is_resetting ? "" : " nested=1");
			iq->reset_deferred = 1;
			qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_RESET;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
			if (!ctrlr->is_resetting) {
				attn(iq->g, iq);	/* nested in its group's pass: the group's thread (in a reset: no group state) */
			}
			return 0;
		}
		if (iq->g->in_ring_poll) {
			/* nested in a pass of its own group (a callback's reconnect): the drain and a ring reset cannot run
			 * under consume() (n2) */
			NVME_QPAIR_ERRLOG(qpair, "IAU_TILE_RESET_REFUSED qid=%u reason=nested: a reconnect inside a pass of "
					  "its group\n", qpair->id);
			return -EBUSY;
		}
		if (!iq->g->is_private && !pthread_equal(iq->g->owner, pthread_self())) {
			/* the group's records are consumed on its poll group's thread (m10) */
			NVME_QPAIR_ERRLOG(qpair, "IAU_TILE_RESET_REFUSED qid=%u reason=thread: a group member reconnects on "
					  "its poll group's thread\n", qpair->id);
			return -EBUSY;
		}
		if (iq->g->is_private || iq->state == NVME_IAU_RT_ACTIVE) {
			flush_once(iq);
		}
		if (!drain(iq)) {
			/* (a reconnect nested in a pass of its group was refused above, before any drain: n2) */
			if (iq->state == NVME_IAU_RT_GONE) {
				/* P4.10d: the route ends LOST here (its records stray from now on). P4.10f (the P4.10c/d/e
				 * review's I1): its end (the unfetched request-free IO, the requests, the aborts: application
				 * callbacks) does not run inside this connect op, as stock never calls back from a connect: a
				 * callback that freed the qpair would free it under this op and under the resetting context's walk
				 * of active_io_qpairs. It is owed (lost_owed) and runs under the completion-context guard
				 * (route_lost_end()) in the group's next attention pass (a shared member: the poll group's
				 * completion context), else at the qpair's delete or qpair_abort_reqs (a private qpair: its
				 * generic poll returns -ENXIO for a DISCONNECTED qpair before it reaches the transport,
				 * nvme_qpair.c:1069-1087, so its delete is where it next runs, as stock ends a dead qpair's
				 * trackers at its destroy, nvme_pcie_common.c:2899-2903) */
				route_lost_mark(iq);
				iq->lost_owed = 1;
				if (!iq->g->is_private) {
					attn(iq->g, iq);
				}
			}
			NVME_QPAIR_ERRLOG(qpair, "IAU_TILE_RESET_REFUSED qid=%u state=%u queue_depth=%u ctrl=%u: the route "
					  "is not detached and drained\n", qpair->id, iq->state, qpair->queue_depth,
					  iq->ctrl_inflight);
			return iq->state == NVME_IAU_RT_LOST ? -EIO : -EBUSY;
		}
		abort_q_complete(iq);
		latch_timeout(iq);
		if (iq->g->is_private) {
			/* a private ring's rejoin is a ring reset (nvme_iau.h): the old engine ring left with its last
			 * member, so nothing of the old incarnation can be fetched or written any more */
			ring_reset(iq->g, iq->route % iq->g->k);
			rc = 0;
		} else {
			/* a shared ring's rejoin (P4.8): drained past the route's last slot (route_drained()), so the
			 * ring keeps its positions unless it closed meanwhile */
			rc = group_ring_prep(iq);
		}
		iq->gen = iau_tile_member_gen_next(iq->gen);
		if (!iq->g->is_private) {
			iq->g->route_gen[iq->route] = iq->gen;
		}
		iq->flushed = 0;
		iq->reset_deferred = 0;	/* P4.10d: a deferred reconnect is carried out */
		iq->st.rejoins++;
		break;
	}
	if (rc != 0) {
		return rc;
	}
	rc = publish(iq);
	if (rc != 0) {
		return rc;
	}
	iq->state = NVME_IAU_RT_JOINED;
	iq->lost_reported = 0;	/* a new incarnation */
	latch_fast(iq);
	print_qpair(iq);
	iq->fail_report = 0;	/* a failure not yet reported belongs to the previous connection */
	rc = g_stock.ctrlr_connect_qpair(ctrlr, qpair);
	if (rc == 0 && !iq->g->is_private) {
		attn(iq->g, iq);	/* D12: the group's attention pass drives the connect until ENABLED */
	}
	return rc;
}

static void
nvme_iau_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_iau_qpair *iq;

	if (!nvme_qpair_is_admin_queue(qpair)) {
		iq = nvme_iau_qp(qpair);
		/* D11: FLUSH once (a bound route); in-flight IO completes normally, later */
		if (iq->state == NVME_IAU_RT_ACTIVE && !ctrlr->is_removed) {
			flush_once(iq);
		}
	}
	g_stock.ctrlr_disconnect_qpair(ctrlr, qpair);
}

/* qpair_abort_reqs is called only for a removed controller (nvme_qpair.c, A5): no FLUSH, no drain */
static void
nvme_iau_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_iau_qpair *iq;
	uint32_t lost;

	if (nvme_qpair_is_admin_queue(qpair)) {
		g_stock.qpair_abort_reqs(qpair, dnr);
		return;
	}
	iq = nvme_iau_qp(qpair);
	if (iq->state == NVME_IAU_RT_IDLE || iq->g == NULL) {
		return;	/* never joined, or deleted (its destroy deferred to a member pass end, A2) */
	}
	/* A5 / DV19: the request-backed IO and the pending entries end here (ABORTED_SQ_DELETION, dnr as given); a
	 * removed device sends no ack: every abort context ends (review I2), its control credits with it; the request-free
	 * IO in flight is counted, reported once (M1). DV19 case 1 (P4.10d wording): a testbed artifact, unreachable on
	 * vfio-user (stock sets is_removed only on PCIe hotplug); the testbed's engine leaves with the target, and on
	 * silicon the on-die IAU survives the SSD's removal, so the host DETACHes the route with emit and drains instead */
	lost = leftovers_end(iq, dnr);
	abort_q_complete(iq);
	if (lost != 0) {
		report_lost(iq, lost, "removed");
	}
}

#define QSTAT_FMT(n) " " #n "=%" PRIu64
#define QSTAT_ARG(n) , iq->st.n

static void
print_stats(struct nvme_iau_qpair *iq, uint32_t lost)
{
	fprintf(stderr, "IAU_TILE_STATS gid=%u route=%u qid=%u gen=%u" NVME_IAU_QSTATS(QSTAT_FMT) " lost=%u pend_left=%u\n",
		iq->g != NULL ? iq->g->gid : UINT32_MAX, iq->route, iq->qpair->id, iq->gen NVME_IAU_QSTATS(QSTAT_ARG),
		lost, iq->n_pend);
}

/* leave the group (a private group is freed with its rings unless kept) and clear the qid's registration.
 * released: the target holds no reservation for the route any more (its Delete I/O CQ succeeded, or it never
 * bound); else a shared route stays held (not reused) until its ring resets. */
static void
member_leave(struct nvme_iau_qpair *iq, bool keep_memory, bool released)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct nvme_iau_group *g = iq->g;
	uint32_t k;

	if (g == NULL) {
		return;
	}
	if (!qp->ctrlr->is_removed) {
		page_write(qp->ctrlr, page_u64(qp->ctrlr, IAU_QCFG2_OFFSET, qp->id), 0);
		page_write(qp->ctrlr, page_u64(qp->ctrlr, IAU_QCFG_OFFSET, qp->id), 0);
	}
	g->gcb->member[iq->route].valid = 0;	/* after the drain (m3; a shared ring's re-registration needs it, P4.8) */
	fprintf(stderr, "IAU_TILE_GROUP_END gid=%u records=%" PRIu64 " stray=%" PRIu64 " polls=%" PRIu64 " doorbells=%"
		PRIu64 " kept=%u idle_polls=%" PRIu64 " sweeps=%" PRIu64 " attn_calls=%" PRIu64 "\n", g->gid, g->records,
		g->stray, g->polls, g->doorbells, keep_memory || g->keep,
		g->is_private ? iq_stat(iq)->idle_polls : g->idle_polls, g->sweeps, g->attn_calls);
	g->route_qp[iq->route] = NULL;
	g->route[iq->route] = NULL;
	g->n_members--;
	if (g->is_private) {
		if (!keep_memory) {
			group_free(g);
		}
		/* kept: the target may still write the ring (a LOST route): its memory is never freed */
	} else {
		k = iq->route % g->k;
		TAILQ_REMOVE(&g->members, iq, link);
		if (iq->in_attn) {
			TAILQ_REMOVE(iq->attn_list, iq, attn_link);	/* g->attn or an attention pass's local list (A3) */
			iq->in_attn = 0;
		}
		if (g->attn_cur == iq) {
			g->attn_cur = NULL;	/* deleted inside its attention call: the pass must not touch it (A2) */
		}
		g->ring[k].budget -= nvme_pcie_qpair(qp)->max_completions_cap;
		if (g->gr[k].n_on == 1) {
			/* its last member: the records that may still come (strays, a later CLOSED R_DETACHED, m3) keep
			 * being consumed */
			g->ring[k].budget = NVME_IAU_EMPTY_RING_BUDGET;
		}
		g->gr[k].n_on--;
		if (released) {
			g->ring[k].need_d -= iau_tile_route_need_d(g->route_resv[iq->route]);
			g->ring[k].need_r -= iau_tile_route_need_r(g->route_resv[iq->route]);
			g->route_resv[iq->route] = 0;
		} else {
			g->route_held[iq->route] = 1;
		}
		if (keep_memory && !g->gr[k].closed) {
			/* a LOST member on a ring still bound: the target may still poll it (cleared by its CLOSED, m3) */
			g->gr[k].tainted = 1;
			g->keep = true;
		}
	}
	iq->g = NULL;
	iq->ring = NULL;
}

/* a route whose drain did not end (D11 1 s bound; reason "drain", or "rejoin" at a reconnect) or that cannot wait for a
 * reset's disable (reason "reset", DV19 case 3's residual): LOST (terminal; the ring memory is kept by the caller). P4.10d
 * (DV19 as amended 2026-10-19): it ends now. Its records stop first (route_qp cleared, so they reach slow_record(), which
 * counts a LOST route's records as strays: nothing of the route is delivered after this, so a late fetch of a descriptor
 * ended here never makes a second callback); then the cold D walk ends its request-free IO that was never fetched (the
 * descriptors between the GCB d_head and d_written, its route and gen: ABORTED_SQ_DELETION, dnr, tag NONE, their cb_fn /
 * cb_arg still in D), its requests and pending entries end (abort_leftovers()) and its abort contexts (abort_q_complete());
 * what is left in queue_depth is the request-free IO the engine fetched, whose cb_fn / cb_arg live only in the engine's
 * slot: no callback, withheld by choice (DV5: no buffer returned while the SSD may still DMA), counted and reported
 * (always: the route itself is LOST, a check_l FAIL) */
static void
route_lost_mark(struct nvme_iau_qpair *iq)
{
	iq->state = NVME_IAU_RT_LOST;
	iq->qpair->iau_fast = 0;
	iq->g->route_qp[iq->route] = NULL;
}

/* P4.10f (the P4.10c/d/e review's I1): the LOST route's end, under the completion-context guard: its own
 * in_completion_context is set while its callbacks run, so a callback that frees the qpair is deferred
 * (spdk_nvme_ctrlr_free_io_qpair() sets delete_after_completion_context) instead of freeing it under this walk; the
 * caller carries that free out (the attention pass) or is the delete already. Also ends what a rejoin left owed */
static uint32_t
route_lost_end(struct nvme_iau_qpair *iq, const char *reason)
{
	struct spdk_nvme_qpair *qp = iq->qpair;
	struct nvme_iau_group *g = iq->g;
	uint8_t icc = qp->in_completion_context;
	uint32_t lost, n = 0;

	iq->lost_owed = 0;
	qp->in_completion_context = 1;
	(void)end_unfetched(g, iq->route % g->k, iq, NULL, &n);
	lost = abort_leftovers(iq, 1, NULL, NULL);
	abort_q_complete(iq);
	qp->in_completion_context = icc;
	report_lost(iq, lost, reason);
	return lost;
}

static uint32_t
route_lost(struct nvme_iau_qpair *iq, const char *reason)
{
	route_lost_mark(iq);
	return route_lost_end(iq, reason);
}

/* the requests of a route that will not drain (LOST, a removed controller, a reset's leftovers), at its delete or
 * qpair_abort_reqs; a rejoin's owed end (P4.10f) runs here if nothing ran it before */
static uint32_t
leftovers_end(struct nvme_iau_qpair *iq, uint32_t dnr)
{
	if (iq->lost_owed) {
		return route_lost_end(iq, "rejoin");
	}
	return abort_leftovers(iq, dnr, NULL, NULL);
}

/*
 * P4.10d (DV19 case 3, the P4.10a review's ruling): a qpair freed during a controller reset waits for the reset's
 * disable, whose records end its IO. Returns true once the disable is done: is_disconnecting clear
 * (nvme_ctrlr_disconnect_done() ran), or the admin queue DISCONNECTED (the disable finished, CSTS.RDY = 0 seen, and
 * the resetting context's next admin poll will see -ENXIO and run nvme_ctrlr_disconnect_done()).
 * Before that (is_disconnecting, the admin queue DISCONNECTING: nvme_ctrlr_disconnect() started the disable with
 * nvme_ctrlr_disable()), this delete runs it, as the resetting context would: the PCIe admin poll runs
 * nvme_ctrlr_disable_poll() on whatever thread polls a DISCONNECTING admin queue (nvme_pcie_qpair_process_completions():
 * CHECK_EN, CC.EN = 0, the wait for CSTS.RDY = 0, then nvme_transport_ctrlr_disconnect_qpair_done()). The delete holds
 * the controller lock (spdk_nvme_ctrlr_free_io_qpair(); the mutex is recursive, and that poll takes it), so the
 * resetting context's own admin polls wait for it; this is the admin polling the stock delete does outside a reset
 * (nvme_wait_for_adminq_completion() under the same lock). It never runs nvme_ctrlr_disconnect_done() (the resetting
 * context's), and stops at the drain bound (a disable that does not finish: the residual, false); one
 * IAU_TILE_RESET_DISABLE line per attempt (done=0|1, polls).
 * Reentrancy (P4.10f, the P4.10c/d/e review's I2): an admin command completed by this poll may call back into the
 * application, which may free another IO qpair of this controller. That nested delete runs this function inside this
 * poll and polls the admin queue there, which stock tolerates: the PCIe poll advances the CQ head before it calls a
 * tracker's callback (nvme_pcie_common.c:2575-2578, then :2603), and stock's own delete polls the admin queue from
 * admin callbacks (nvme_wait_for_adminq_completion()). The nested delete therefore runs (or finds done) the same
 * disable and drains; the outer loop then sees the admin queue DISCONNECTED. The resetting context's own admin poll
 * nests the same way (no guard there either). P4.10d's refusal of the nested delete is gone
 */
static bool
reset_disable(struct spdk_nvme_qpair *qpair)
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
		fprintf(stderr, "IAU_TILE_RESET_DISABLE qid=%u done=0 polls=0\n", qpair->id);
		return false;	/* is_disconnecting with the admin queue in neither state (stock never has it) */
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
	fprintf(stderr, "IAU_TILE_RESET_DISABLE qid=%u done=%u polls=%u\n", qpair->id, done, polls);
	return done;
}

static int
nvme_iau_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_iau_qpair *iq = nvme_iau_qp(qpair);
	struct nvme_completion_poll_status *status;
	struct nvme_iau_group *sg = iq->g != NULL && !iq->g->is_private ? iq->g : NULL;
	struct spdk_nvme_ctrlr *held = qpair->ctrlr;
	bool keep = false, sq_deleted = false, released = false, defer;
	uint32_t lost = 0;
	int rc;

	if (sg != NULL && sg->in_ring_poll) {
		/* called nested inside a pass of its own group (I1: a member removed from its poll group, freed from
		 * another member's callback): the drain cannot run here. The whole delete (drain, Delete I/O SQ / CQ,
		 * destroy) moves to the attention pass; the qpair stays a member until then (DV23) */
		iq->delete_deferred = 1;
		attn(iq->g, iq);
		return 0;
	}
	if (iq->state == NVME_IAU_RT_IDLE) {
		released = true;	/* never registered: nothing at the target */
		goto out;	/* never joined: nothing at the target */
	}
	if (iq->state == NVME_IAU_RT_JOINED && pqpair->pcie_state == NVME_PCIE_QPAIR_READY) {
		iq->state = NVME_IAU_RT_ACTIVE;	/* bound (Create I/O SQ succeeded), not yet seen by a poll */
	}
	if (ctrlr->is_removed) {
		/* no FLUSH, no drain, no admin command: the requests end here; request-free IO is lost (DV19 case 1) */
		lost = leftovers_end(iq, 1);
		if (lost != 0) {
			report_lost(iq, lost, "removed");	/* once: qpair_abort_reqs may have reported it (M1) */
		}
		goto out;
	}
	if (ctrlr->prepare_for_reset) {
		/* the controller is being reset: no admin command (as the stock delete) */
		if (pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ || pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ) {
			pqpair->flags.defer_destruction = true;	/* the stock create callbacks destroy it */
		}
		if (iq->state != NVME_IAU_RT_JOINED && iq->state != NVME_IAU_RT_LOST &&
		    qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr)) {
			/* P4.10a (P4.9 review I1): once the reset's disable is done (CSTS.RDY = 0 seen: is_disconnecting
			 * cleared by nvme_ctrlr_disconnect_done(), or the admin queue DISCONNECTED) the target detached every
			 * route with its records before reporting RDY = 0, so this route's records, its CLOSED R_DETACHED
			 * included, are in R. The drain consumes them: every command, request-free IO included, ends once with
			 * the record's status (ABORTED_SQ_DELETION) or the CLOSED walk's, and the closed ring may be freed (a
			 * target whose disable timed out without the records, nvmf's cc_timeout: the drain ends LOST at the
			 * bound, DV19 case 2). P4.10d (DV19 case 3 fixed): before that point this delete runs the disable
			 * itself (reset_disable()), then drains the same way. A LOST route (terminal: its drain already ended
			 * at the bound) is not drained again */
			if (reset_disable(qpair)) {
				if (!drain(iq)) {
					keep = true;
					lost = route_lost(iq, "drain");
				}
				goto out;
			}
			/* P4.10d, DV19 case 3's residual (P4.10f: only a disable not done within the drain bound, the DV5
			 * policy; a delete nested in another delete's run of the disable runs it too, reset_disable()): this
			 * route ends now without its records (route_lost(): the requests, the aborts and the unfetched
			 * request-free IO end, the fetched request-free IO is counted, withheld by choice); the ring memory is
			 * kept (the disable will still write it) */
			keep = true;
			lost = route_lost(iq, "reset");
			goto out;
		}
		/* never bound (JOINED), LOST already, or another process's qpair (its callbacks are not this process's to
		 * wait for): the requests end here; the target's disable may still write the ring, which nobody consumes
		 * again: its memory is kept */
		keep = iq->state != NVME_IAU_RT_JOINED || pqpair->flags.defer_destruction;
		lost = leftovers_end(iq, 1);
		if (lost != 0) {
			report_lost(iq, lost, "reset");
		}
		goto out;
	}
	/* a connect in progress finishes first (as the stock delete) */
	while (pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ || pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ) {
		if (spdk_nvme_qpair_process_completions(ctrlr->adminq, 0) < 0) {
			break;
		}
	}
	if (iq->state == NVME_IAU_RT_JOINED && pqpair->pcie_state == NVME_PCIE_QPAIR_READY) {
		iq->state = NVME_IAU_RT_ACTIVE;	/* the connect that just finished bound it */
	}
	if (iq->state == NVME_IAU_RT_ACTIVE) {
		flush_once(iq);
	}
	/* Admin Delete I/O SQ and WAIT for its completion: the target detaches the route there (V9) */
	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		NVME_QPAIR_ERRLOG(qpair, "Failed to allocate status tracker\n");
	} else if (nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, status) != 0) {
		free(status);
	} else {
		rc = nvme_wait_for_adminq_completion(ctrlr, status, false);
		sq_deleted = rc == 0;
		if (!status->timed_out) {
			free(status);
		}
	}
	if (iq->state == NVME_IAU_RT_JOINED) {
		released = true;	/* never bound: the target reserves nothing for it */
	}
	if (iq->state == NVME_IAU_RT_LOST) {
		/* terminal (a failed rejoin drain): no drain, no Delete I/O CQ; P4.10f: its owed end runs here unless the
		 * attention pass ran it (leftovers_end()) */
		keep = true;
		lost = leftovers_end(iq, 1);
	} else if (iq->state != NVME_IAU_RT_JOINED) {
		/* drain until GONE && queue_depth == 0 && ctrl_inflight == 0 (A4), else LOST: the requests end here, the
		 * request-free IO in flight is counted (DV19 as amended 2026-10-19: a target that stopped answering, or an
		 * engine that dropped the route without records), and the ring memory is kept (nobody consumes it again) */
		/* LOST (m4): no Delete I/O CQ, so the target's CQ of this qid outlives the SSD SQ / CQ memory and the qid
		 * freed below; a later qpair on the qid can fail its Create I/O CQ (fail-safe) */
		if (qpair->active_proc != nvme_ctrlr_get_current_process(ctrlr) || !drain(iq)) {
			keep = true;
			lost = route_lost(iq, "drain");
		}
	}
	abort_q_complete(iq);
	/* Admin Delete I/O CQ: only after the Delete SQ completion and the drain (A18); the target's only
	 * reservation-release trigger, so a LOST route never sends it */
	if (sq_deleted && iq->state != NVME_IAU_RT_LOST) {
		status = calloc(1, sizeof(*status));
		if (status != NULL && nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, status) == 0) {
			rc = nvme_wait_for_adminq_completion(ctrlr, status, true);
			if (rc != 0) {
				NVME_QPAIR_ERRLOG(qpair, "IAU tile: Delete I/O CQ failed (%d)\n", rc);
			} else {
				released = true;	/* the target's reservation release trigger (P4.4) */
			}
		} else {
			free(status);
		}
	}
out:
	abort_q_complete(iq);	/* every path, the removed-controller and in-reset ones too (A4, review I2) */
	print_stats(iq, lost);
	/* A2: a delete inside a shared group's member pass defers the destroy (the qpair and its iq) to the pass end */
	defer = iq->g != NULL && !iq->g->is_private && iq->g->in_member_pass;
	if (defer) {
		struct nvme_iau_group *g = iq->g;

		member_leave(iq, keep, released);
		iq_free_tables(iq, keep);
		if (!pqpair->flags.defer_destruction) {
			TAILQ_INSERT_TAIL(&g->dead, iq, attn_link);
		}
		return 0;
	}
	member_leave(iq, keep, released);
	iq_free_tables(iq, keep);
	if (!pqpair->flags.defer_destruction) {
		nvme_pcie_qpair_destroy(qpair);
	}
	if (sg != NULL) {
		run_deferred(sg, held, NULL);	/* the end of a top-level shared delete (n3) */
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------ abort (P4.10a, D10) */

/*
 * Late abort (D7, D10, DV5, DV6): the IAU marks matching busy slots and acks at once (cdw0 = the commands it marked); a
 * marked command's own record is ABORTED_BY_REQUEST at its CQE, its buffer is never returned while the SSD may still
 * DMA, and no NVMe Abort goes to the SSD. A control (ABORT_COOKIE / ABORT_TAG) takes a control credit (2 per route,
 * ctrl_inflight, D13); more wait in software on abort_q (FIFO) and go out as acks return the credits, so an abort
 * never returns -EAGAIN. Every abort context ends with exactly one callback of the application's: at its last ack,
 * or when the route ends (abort_q_complete(): teardown, rejoin, removed controller, LOST route). Called on the
 * qpair's thread, as its submits (the D ring has one producer).
 */

/* the application's callback of an abort context: cdw0 bit0 = 1 if nothing was marked (DV6), else 0, as stock's
 * abort_ext parent (its status 0); then the context is freed */
static void
abort_ctx_end(struct nvme_iau_qpair *iq, struct nvme_iau_abort_ctx *a)
{
	struct spdk_nvme_cpl cpl;

	TAILQ_REMOVE(&iq->abort_q, a, link);
	memset(&cpl, 0, sizeof(cpl));
	cpl.cdw0 = a->marked != 0 ? 0 : 1;
	if (a->cb_fn != NULL) {
		a->cb_fn(a->cb_arg, &cpl);
	}
	free(a);
}

/* the w0 of every ABORT_* descriptor (d6 = its context): an ack record (slow_record(), its control credit already
 * back), or the CLOSED walk's ack(0) for a control never fetched (end_unfetched(), R04) */
static void
nvme_iau_abort_ack(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_iau_abort_ctx *a = arg;

	a->marked += cpl->cdw0;
	assert(a->pending != 0);
	if (--a->pending == 0) {
		abort_ctx_end(a->iq, a);
	}
}

/* the queued controls, in order, while a control credit is free and the route still takes descriptors (ACTIVE; or
 * FLUSHING, where the engine acks 0: D7); a route that left takes none (its contexts end at its teardown) */
static void
abort_q_issue(struct nvme_iau_qpair *iq)
{
	struct nvme_iau_abort_ctx *a;

	if (iq->state != NVME_IAU_RT_ACTIVE && iq->state != NVME_IAU_RT_FLUSHING) {
		return;
	}
	TAILQ_FOREACH(a, &iq->abort_q, link) {
		while (a->n_issued < a->n_ctrl) {
			if (iq->ctrl_inflight >= IAU_TILE_CTRL_CREDITS) {
				return;
			}
			ctrl(iq, a->n_issued == 0 ? a->kind0 : IAU_TILE_K_ABORT_COOKIE, a->d2[a->n_issued],
			     (uint64_t)(uintptr_t)nvme_iau_abort_ack, (uint64_t)(uintptr_t)a);
			a->n_issued++;
			iq->ctrl_inflight++;
		}
	}
}

/* every abort context still waiting on this route ends now with what it marked (A4: a context whose controls never
 * went out, cdw0 bit0 = 1); no ack is consumed for the route after this (its drain ended, or the route is LOST or its
 * controller removed), so the control credits are back too and a late ack would be a stray (never dereferenced) */
static void
abort_q_complete(struct nvme_iau_qpair *iq)
{
	struct nvme_iau_abort_ctx *a;

	while ((a = TAILQ_FIRST(&iq->abort_q)) != NULL) {
		abort_ctx_end(iq, a);
	}
	iq->ctrl_inflight = 0;
}

/* a context of n_ctrl controls on iq's abort_q, then out as credits allow */
static struct nvme_iau_abort_ctx *
abort_ctx_new(struct nvme_iau_qpair *iq, uint32_t n_ctrl, uint8_t kind0, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_iau_abort_ctx *a = calloc(1, sizeof(*a) + (size_t)n_ctrl * sizeof(a->d2[0]));

	if (a != NULL) {
		a->cb_fn = cb_fn;
		a->cb_arg = cb_arg;
		a->iq = iq;
		a->n_ctrl = n_ctrl;
		a->pending = n_ctrl;
		a->kind0 = kind0;
	}
	return a;
}

/*
 * spdk_nvme_ctrlr_cmd_abort_ext() on an IAU qpair (nvme_ctrlr_cmd.c hook; cmd_cb_arg != NULL there). The queued
 * requests matching cmd_cb_arg are aborted in software as stock does (nvme_qpair_abort_queued_reqs_with_cbarg(): their
 * ABORTED_BY_REQUEST completions come from the next generic poll; a shared group's member goes on the attention list
 * for it) and counted as marked. On a route that takes descriptors: one ABORT_COOKIE(cmd_cb_arg) for the request-free
 * IO (its slots' cb_arg is the application's), then one ABORT_COOKIE(req) per matching request-backed request in flight
 * (nvme_request_abort_match(); its slots' cb_arg is the request); rc 0 always, also when nothing matches (DV6), and the
 * callback follows the acks. Otherwise (no live route, or a removed controller) nothing can be marked: stock's own
 * result, i.e. the callback at once if queued requests matched, else -ENOENT. -ENOMEM: nothing done.
 */
int
nvme_iau_ctrlr_cmd_abort_ext(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, void *cmd_cb_arg,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_iau_qpair *iq;
	struct nvme_iau_abort_ctx *a;
	struct spdk_nvme_cpl cpl;
	uint32_t i, n = 0;

	if (qpair->iau_kind != NVME_IAU_KIND_TILE) {
		/* P4.11: S2's software late abort */
		return qpair->iau_kind == NVME_IAU_KIND_S2 ? nvme_iau_s2_cmd_abort_ext(ctrlr, qpair, cmd_cb_arg, cb_fn, cb_arg) :
		       -ENOTSUP;
	}
	iq = nvme_iau_qp(qpair);
	if (ctrlr->is_removed || iq->g == NULL ||
	    (iq->state != NVME_IAU_RT_ACTIVE && iq->state != NVME_IAU_RT_FLUSHING)) {
		if (nvme_qpair_abort_queued_reqs_with_cbarg(qpair, cmd_cb_arg) == 0) {
			return -ENOENT;
		}
		memset(&cpl, 0, sizeof(cpl));	/* stock: the parent completes at once with status 0, cdw0 0 */
		if (cb_fn != NULL) {
			cb_fn(cb_arg, &cpl);
		}
		return 0;
	}
	for (i = 0; i < iq->credits; i++) {
		n += iq->inflight[i] != NULL && nvme_request_abort_match(iq->inflight[i], cmd_cb_arg);
	}
	a = abort_ctx_new(iq, n + 1, IAU_TILE_K_ABORT_COOKIE, cb_fn, cb_arg);
	if (a == NULL) {
		return -ENOMEM;
	}
	a->d2[0] = (uint64_t)(uintptr_t)cmd_cb_arg;
	for (i = 0, n = 1; i < iq->credits; i++) {
		if (iq->inflight[i] != NULL && nvme_request_abort_match(iq->inflight[i], cmd_cb_arg)) {
			a->d2[n++] = (uint64_t)(uintptr_t)iq->inflight[i];
		}
	}
	a->marked = nvme_qpair_abort_queued_reqs_with_cbarg(qpair, cmd_cb_arg);
	if (a->marked != 0 && !iq->g->is_private) {
		attn(iq->g, iq);	/* the attention pass's generic call completes them (D12) */
	}
	TAILQ_INSERT_TAIL(&iq->abort_q, a, link);
	abort_q_issue(iq);
	return 0;
}

/* spdk_nvme_ctrlr_cmd_abort() on an IAU qpair: cid is the slot tag a record or a timeout notice gave (gen | slot); one
 * ABORT_TAG(cid), whose gen the engine checks (a stale tag marks nothing); rc 0 and the callback after the ack. A route
 * that takes no descriptor (or a removed controller): -ENXIO, nothing sent (the IAU sends no NVMe Abort to the SSD,
 * DV5). P4.11: an S2 qpair's software late abort (fast and tracker cids alike: no NVMe Abort goes to the SSD, as tile);
 * NVME_IAU_DECLINE (stock's admin Abort) for any other qpair */
int
nvme_iau_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, uint16_t cid,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_iau_qpair *iq;
	struct nvme_iau_abort_ctx *a;

	if (qpair->iau_kind != NVME_IAU_KIND_TILE) {
		return qpair->iau_kind == NVME_IAU_KIND_S2 ? nvme_iau_s2_cmd_abort(ctrlr, qpair, cid, cb_fn, cb_arg) :
		       NVME_IAU_DECLINE;
	}
	iq = nvme_iau_qp(qpair);
	if (ctrlr->is_removed || iq->g == NULL ||
	    (iq->state != NVME_IAU_RT_ACTIVE && iq->state != NVME_IAU_RT_FLUSHING)) {
		return -ENXIO;
	}
	a = abort_ctx_new(iq, 1, IAU_TILE_K_ABORT_TAG, cb_fn, cb_arg);
	if (a == NULL) {
		return -ENOMEM;
	}
	a->d2[0] = cid;
	TAILQ_INSERT_TAIL(&iq->abort_q, a, link);
	abort_q_issue(iq);
	return 0;
}

/*
 * P4.10c (plan case 56): stock's PCIe iterate, nvme_pcie_qpair_iterate_requests() (nvme_pcie.c:1214), calls iter_fn on
 * the request of every outstanding tracker (then of F3's tracker-free Mode D requests) and returns the first non-zero
 * rc; stock vfio-user registers no such op (nvme_vfio_user.c's table: nvme_transport_qpair_iterate_requests() would call
 * NULL). A tile IO qpair's requests in flight are inflight[]: request-backed IO kinds, RAW, a parked retry, a refused RAW
 * awaiting its record; request-free IO has no request. The walk re-reads each slot. The admin queue is a PCIe qpair with
 * trackers: its outstanding_tr, as stock's walk.
 */
static int
nvme_iau_qpair_iterate_requests(struct spdk_nvme_qpair *qpair, int (*iter_fn)(struct nvme_request *req, void *arg),
				void *arg)
{
	struct nvme_iau_qpair *iq;
	uint32_t i;
	int rc;

	if (nvme_qpair_is_admin_queue(qpair)) {
		struct nvme_tracker *tr, *tmp;

		TAILQ_FOREACH_SAFE(tr, &nvme_pcie_qpair(qpair)->outstanding_tr, tq_list, tmp) {
			rc = iter_fn(tr->req, arg);
			if (rc != 0) {
				return rc;
			}
		}
		return 0;
	}
	iq = nvme_iau_qp(qpair);
	for (i = 0; iq->inflight != NULL && i < iq->credits; i++) {
		struct nvme_request *req = iq->inflight[i];

		if (req == NULL) {
			continue;
		}
		rc = iter_fn(req, arg);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------ the override */

void
nvme_iau_override_ops(struct spdk_nvme_transport_ops *ops)
{
	g_stock = *ops;
	ops->ctrlr_create_io_qpair = nvme_iau_ctrlr_create_io_qpair;
	ops->ctrlr_delete_io_qpair = nvme_iau_ctrlr_delete_io_qpair;
	ops->ctrlr_connect_qpair = nvme_iau_ctrlr_connect_qpair;
	ops->ctrlr_disconnect_qpair = nvme_iau_ctrlr_disconnect_qpair;
	ops->qpair_abort_reqs = nvme_iau_qpair_abort_reqs;
	ops->qpair_submit_request = g_nvme_iau_cfg.stride == IAU_TILE_DESC48_BYTES ? nvme_iau_qpair_submit_request48 :
				    nvme_iau_qpair_submit_request;
	ops->qpair_process_completions = nvme_iau_qpair_process_completions;
	ops->qpair_iterate_requests = nvme_iau_qpair_iterate_requests;	/* P4.10c */
	if (g_nvme_iau_cfg.group) {
		/* P4.8 (D3, D12): a PCIe poll group owns one shared group of K ring pairs */
		ops->poll_group_create = nvme_iau_pg_create;
		ops->poll_group_add = nvme_iau_pg_add;
		ops->poll_group_remove = nvme_iau_pg_remove;
		ops->poll_group_process_completions = nvme_iau_pg_process_completions;
		ops->poll_group_destroy = nvme_iau_pg_destroy;
	}
	/* else the poll-group ops stay stock (D4): they poll each member through qpair_process_completions */
}
