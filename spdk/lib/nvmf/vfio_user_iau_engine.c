/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/likely.h"
#include "vfio_user_iau_engine.h"
#include "vfio_user_iau_record.h"

#if defined(__x86_64__) || defined(__i386__)
/* x86 does not reorder loads with older loads: the CQE words read after the phase word are at least as
 * new (the host's own CQ poll has no barrier on x86 either). Nor does it reorder a store with older loads:
 * the CQ-head doorbell store cannot pass the loads of the CQEs it frees. */
#define iau_eng_cq_rmb() spdk_compiler_barrier()
#define iau_eng_cq_release() spdk_compiler_barrier()
#else
#define iau_eng_cq_rmb() spdk_rmb()
#define iau_eng_cq_release() spdk_mb()
#endif

/* cpl bytes [8:16) as a u64: the status code [56:49] and type [59:57]; both 0 <=> success */
#define IAU_ENG_CPL_SCT_SC_MASK	(0x7FFULL << 49)

int
iau_eng_q_init(struct iau_eng_q *q, const struct iau_eng_q_cfg *cfg)
{
	uint32_t i;

	if (q == NULL || cfg == NULL || cfg->qid == 0 || cfg->qid >= IAU_MAX_QID ||
	    cfg->entries < 2 || cfg->entries > IAU_ENG_MAX_ENTRIES ||
	    cfg->sq_entries != cfg->entries || cfg->cq_entries < 2 || cfg->credits == 0 ||
	    cfg->credits > cfg->entries - 1 || cfg->credits > cfg->cq_entries - 1 || cfg->nsid == 0 ||
	    cfg->desc_ring == NULL || cfg->rec_ring == NULL || cfg->host_db == NULL ||
	    cfg->host_cq_sink == NULL || cfg->ssd_sq == NULL || cfg->ssd_cq == NULL ||
	    cfg->ssd_sq_db == NULL || cfg->ssd_cq_db == NULL) {
		return -EINVAL;
	}
	memset(q, 0, sizeof(*q));
	q->slots = calloc(cfg->entries, sizeof(*q->slots));
	q->free_ring = calloc(cfg->entries, sizeof(*q->free_ring));
	if (q->slots == NULL || q->free_ring == NULL) {
		free(q->slots);
		free(q->free_ring);
		memset(q, 0, sizeof(*q));
		return -ENOMEM;
	}
	for (i = 0; i < cfg->entries; i++) {
		q->free_ring[i] = (uint16_t)i;	/* FIFO: hands out 0, 1, 2, ... (free_head = free_tail = 0, full) */
	}
	q->n_free = cfg->entries;
	q->free_floor = cfg->entries - cfg->credits;
	q->desc_ring = (const volatile struct iau_desc_d *)cfg->desc_ring;
	q->rec_ring = (volatile uint64_t *)cfg->rec_ring;
	q->host_db = cfg->host_db;
	q->host_cq_sink = cfg->host_cq_sink;
	q->ssd_sq = (volatile struct spdk_nvme_cmd *)cfg->ssd_sq;
	q->ssd_cq = (const volatile struct spdk_nvme_cpl *)cfg->ssd_cq;
	q->ssd_sq_db = cfg->ssd_sq_db;
	q->ssd_cq_db = cfg->ssd_cq_db;
	q->entries = cfg->entries;
	q->sq_entries = cfg->sq_entries;
	q->cq_entries = cfg->cq_entries;
	q->rec_phase = 1;	/* the host starts at cq_rec_phase = 1 */
	q->cq_phase = 1;	/* the SSD's CQ starts at phase 1 (handle_create_io_cq) */
	q->qid = cfg->qid;
	q->nsid = cfg->nsid;
	q->credits = cfg->credits;
	return 0;
}

void
iau_eng_q_fini(struct iau_eng_q *q)
{
	if (q == NULL) {
		return;
	}
	free(q->slots);
	free(q->free_ring);
	q->slots = NULL;
	q->free_ring = NULL;
	q->n_free = 0;
}

/* One 32 B record, published as iau_flush() does: w0..w2, wmb, w3 (the phase word). Bytes [8:16) of the
 * cpl as the in-target engine builds them (iau_cpl_hi_word): sqhd = the descriptor consumer index, sqid =
 * the host qid, cid = the host's cid, then the SSD's 15 status bits; the packer puts the ring phase at 48. */
static inline void
eng_emit(struct iau_eng_q *q, const struct iau_eng_slot *s, uint64_t cpl_lo, uint64_t cqe_hi)
{
	volatile uint64_t *dst = &q->rec_ring[(size_t)q->rec_tail * 4];
	uint64_t hi = (uint64_t)(uint16_t)q->desc_head | ((uint64_t)q->qid << 16) |
		      ((uint64_t)s->host_cid << 32) | (cqe_hi & 0xFFFF000000000000ULL);
	uint8_t rec[IAU_REC_BYTES];
	uint64_t w[4];

	iau_pack_moded_record(s->cb_fn, s->cb_arg, cpl_lo, hi, q->rec_phase, rec);
	memcpy(w, rec, sizeof(w));
	dst[0] = w[0];
	dst[1] = w[1];
	dst[2] = w[2];
	spdk_wmb();	/* payload visible before the phase word the host polls */
	dst[3] = w[3];
	if (++q->rec_tail == q->entries) {
		q->rec_tail = 0;
		q->rec_phase ^= 1;
	}
	q->records_emitted++;
}

uint32_t
iau_eng_q_poll(struct iau_eng_q *q)
{
	uint32_t n = 0, m = 0, tail;

	/* 1. SSD completions -> host records, at most IAU_ENG_POLL_BUDGET. */
	while (n < IAU_ENG_POLL_BUDGET) {
		const volatile uint64_t *cqe = (const volatile uint64_t *)&q->ssd_cq[q->cq_head];
		uint64_t hi = cqe[1];
		uint64_t lo;
		uint32_t sqhd;
		uint16_t tag;
		struct iau_eng_slot *s;

		if ((uint8_t)((hi >> 48) & 1u) != q->cq_phase) {
			break;
		}
		iau_eng_cq_rmb();
		lo = cqe[0];
		if (++q->cq_head == q->cq_entries) {
			q->cq_head = 0;
			q->cq_phase ^= 1;
		}
		q->cqes_consumed++;
		n++;
		sqhd = (uint32_t)(hi & 0xFFFFu);
		if (spdk_likely(sqhd < q->sq_entries)) {
			q->sq_head_seen = sqhd;
		} else {
			q->bad_sqhd++;	/* keeps the SQ-full guard on the last valid SQHD */
		}
		tag = (uint16_t)(hi >> 32);
		if (spdk_unlikely(tag >= q->entries || !q->slots[tag].busy)) {
			q->bad_cid++;	/* a CQE for no command in flight: stray, or a duplicate before reuse */
			continue;
		}
		s = &q->slots[tag];
		if (spdk_unlikely((hi & IAU_ENG_CPL_SCT_SC_MASK) != 0)) {
			q->err_records++;
		}
		eng_emit(q, s, lo, hi);
		s->busy = 0;
		q->free_ring[q->free_tail] = tag;
		if (++q->free_tail == q->entries) {
			q->free_tail = 0;
		}
		q->n_free++;
	}
	if (n != 0) {
		iau_eng_cq_release();	/* the CQE loads before the doorbell that lets the SSD reuse them */
		*q->ssd_cq_db = q->cq_head;
		q->cq_db_writes++;
	}

	/* 2. host descriptors -> 64 B SQEs in the SSD's SQ, at most IAU_ENG_POLL_BUDGET. */
	tail = *q->host_db;
	if (tail == q->desc_head) {
		return n;
	}
	if (spdk_unlikely(tail >= q->entries)) {
		q->bad_db++;	/* per poll that reads the bad value */
		return n;
	}
	spdk_rmb();	/* descriptors after the doorbell value (as handle_sq_tdbl_write()) */
	while (q->desc_head != tail && m < IAU_ENG_POLL_BUDGET) {
		const volatile struct iau_desc_d *src = &q->desc_ring[q->desc_head];
		uint32_t next = q->sq_tail + 1 == q->sq_entries ? 0 : q->sq_tail + 1;
		struct iau_desc_d d;
		struct spdk_nvme_cmd cmd;
		uint64_t cb_fn, cb_arg, w[8];
		volatile uint64_t *dst;
		struct iau_eng_slot *s;
		uint16_t tag;
		int i;

		if (spdk_unlikely(next == q->sq_head_seen)) {
			q->sq_full_stalls++;	/* the SSD has not consumed enough; leave the rest in the ring */
			break;
		}
		if (spdk_unlikely(q->n_free <= q->free_floor)) {
			q->credit_stalls++;	/* in flight == credits: the host over-submitted; wait for a CQE */
			break;
		}
		d.w0 = src->w0;
		d.w1 = src->w1;
		d.w2 = src->w2;
		d.w3 = src->w3;
		d.w4 = src->w4;
		iau_decode_desc(&d, &cmd, &cb_fn, &cb_arg);
		tag = q->free_ring[q->free_head];
		if (++q->free_head == q->entries) {
			q->free_head = 0;
		}
		q->n_free--;
		s = &q->slots[tag];
		s->cb_fn = cb_fn;
		s->cb_arg = cb_arg;
		s->host_cid = cmd.cid;
		s->busy = 1;
		cmd.cid = tag;
		cmd.nsid = q->nsid;
		memcpy(w, &cmd, sizeof(w));
		dst = (volatile uint64_t *)&q->ssd_sq[q->sq_tail];
		for (i = 0; i < 8; i++) {
			dst[i] = w[i];
		}
		q->sq_tail = next;
		if (++q->desc_head == q->entries) {
			q->desc_head = 0;
		}
		q->desc_fetched++;
		q->sqes_written++;
		m++;
	}
	if (m != 0) {
		spdk_wmb();	/* SQEs before the doorbell: the tile's "fence until acked" */
		*q->ssd_sq_db = q->sq_tail;
		q->ssd_db_writes++;
	}
	return n + m;
}

int
iau_eng_fmt_start(char *buf, size_t len, uint32_t engine, uint32_t lcore, uint32_t want)
{
	return snprintf(buf, len, "IAU_ENGINE_START engine=%" PRIu32 " lcore=%" PRIu32 " want=%" PRIu32,
			engine, lcore, want);
}

int
iau_eng_q_fmt_bind(char *buf, size_t len, const struct iau_eng_q *q, int ctrlr, uint32_t engine,
		   uint32_t lcore)
{
	return snprintf(buf, len, "IAU_ENGINE_BIND ctrlr=%d qid=%u engine=%" PRIu32 " lcore=%" PRIu32
			" nsid=%" PRIu32 " credits=%" PRIu32,
			ctrlr, (unsigned)q->qid, engine, lcore, q->nsid, q->credits);
}

int
iau_eng_q_fmt_stats(char *buf, size_t len, const struct iau_eng_q *q, int ctrlr, uint32_t engine)
{
	return snprintf(buf, len, "IAU_ENGINE_STATS ctrlr=%d qid=%u engine=%" PRIu32
			" desc_fetched=%" PRIu64 " sqes_written=%" PRIu64 " ssd_db_writes=%" PRIu64
			" cqes_consumed=%" PRIu64 " cq_db_writes=%" PRIu64 " records_emitted=%" PRIu64
			" inflight=%" PRIu32 " bad_cid=%" PRIu64 " bad_db=%" PRIu64 " host_ssd_db=%" PRIu32
			" reissues=0 err_records=%" PRIu64 " bad_sqhd=%" PRIu64,
			ctrlr, (unsigned)q->qid, engine, q->desc_fetched, q->sqes_written, q->ssd_db_writes,
			q->cqes_consumed, q->cq_db_writes, q->records_emitted, iau_eng_q_inflight(q),
			q->bad_cid, q->bad_db, iau_eng_q_foreign_db(q), q->err_records, q->bad_sqhd);
}
