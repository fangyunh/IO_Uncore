/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/nvme_spec.h"
#include "vfio_user_iau.h"
#include "vfio_user_iau_record.h"

static uint64_t
env_u64(const char *name, uint64_t dflt, bool *bad)
{
	const char *v = getenv(name);
	char *end = NULL;
	unsigned long long x;

	if (v == NULL || v[0] == '\0') {
		return dflt;
	}
	errno = 0;
	x = strtoull(v, &end, 10);
	if (errno != 0 || end == v || *end != '\0') {
		*bad = true;
		return dflt;
	}
	return (uint64_t)x;
}

int
iau_ctx_init(struct iau_ctx *ctx, const volatile void *uncore_page, uint64_t ticks_hz)
{
	bool bad = false;
	uint64_t mode, batch_t_ns;

	memset(ctx, 0, sizeof(*ctx));
	mode = env_u64("IAU_UNCORE_MODE", IAU_MODE_BASELINE, &bad);
	ctx->batch_n = (uint32_t)env_u64("IAU_CQ_BATCH_N", IAU_CQ_BATCH_N_DEFAULT, &bad);
	batch_t_ns = env_u64("IAU_CQ_BATCH_T_NS", IAU_CQ_BATCH_T_NS_DEFAULT, &bad);
	if (bad || (mode != IAU_MODE_BASELINE && mode != IAU_MODE_D && mode != IAU_MODE_ENGINE) || ctx->batch_n == 0) {
		return -EINVAL;
	}
	ctx->mode = (int)mode;
	ctx->batch_t_ticks = batch_t_ns * ticks_hz / 1000000000ULL;
	ctx->uncore_page = (const volatile uint64_t *)uncore_page;
	if (posix_memalign((void **)&ctx->sq, IAU_SQ_ALIGN, IAU_MAX_QID * sizeof(struct iau_sq)) != 0) {
		ctx->sq = NULL;
		return -ENOMEM;
	}
	memset(ctx->sq, 0, IAU_MAX_QID * sizeof(struct iau_sq));
	return 0;
}

void
iau_ctx_fini(struct iau_ctx *ctx)
{
	uint32_t q;

	if (ctx->sq == NULL) {
		return;
	}
	for (q = 0; q < IAU_MAX_QID; q++) {
		iau_sq_detach(ctx, (uint16_t)q);
	}
	free(ctx->sq);
	ctx->sq = NULL;
}

uint64_t
iau_desc_ring_iova(const struct iau_ctx *ctx, uint16_t qid)
{
	if (ctx->uncore_page == NULL || qid >= IAU_MAX_QID) {
		return 0;
	}
	return ctx->uncore_page[(IAU_SQ_DESC_RING_IOVA_OFFSET - IAU_BAR0_UNCORE_PAGE_OFFSET) / 8 + qid];
}

uint64_t
iau_rec_ring_iova(const struct iau_ctx *ctx, uint16_t qid)
{
	if (ctx->uncore_page == NULL || qid >= IAU_MAX_QID) {
		return 0;
	}
	return ctx->uncore_page[(IAU_CQ_REC_RING_IOVA_OFFSET - IAU_BAR0_UNCORE_PAGE_OFFSET) / 8 + qid];
}

int
iau_sq_attach(struct iau_ctx *ctx, uint16_t qid, uint32_t entries,
	      const volatile void *desc_ring, volatile void *rec_ring)
{
	struct iau_sq *sq;

	if (qid == 0 || qid >= IAU_MAX_QID || entries < 2 || desc_ring == NULL || rec_ring == NULL) {
		return -EINVAL;
	}
	sq = &ctx->sq[qid];
	if (sq->active) {
		return -EEXIST;
	}
	memset(sq, 0, sizeof(*sq));
	sq->staging = calloc(entries, sizeof(*sq->staging));
	if (sq->staging == NULL) {
		return -ENOMEM;
	}
	sq->entries = entries;
	sq->desc_ring = (const volatile struct iau_desc_d *)desc_ring;
	sq->rec_ring = (volatile uint64_t *)rec_ring;
	sq->rec_phase = 1;
	sq->active = true;
	return 0;
}

void
iau_sq_detach(struct iau_ctx *ctx, uint16_t qid)
{
	struct iau_sq *sq;

	if (ctx->sq == NULL || qid >= IAU_MAX_QID) {
		return;
	}
	sq = &ctx->sq[qid];
	free(sq->staging);
	memset(sq, 0, sizeof(*sq));
}

void
iau_decode_desc(const struct iau_desc_d *d, struct spdk_nvme_cmd *cmd,
		uint64_t *cb_fn, uint64_t *cb_arg)
{
	/* Every field of the 64 B command is assigned exactly once (P1.2: no memset
	 * followed by overwrites); the result equals the former memset + fields. */
	cmd->opc = (uint8_t)(d->w0 >> 56);
	cmd->fuse = (uint8_t)((d->w0 >> 48) & 0x3u);
	cmd->rsvd1 = 0;
	cmd->psdt = 0;
	cmd->cid = (uint16_t)(d->w0 >> 32);
	cmd->nsid = 1;
	cmd->rsvd2 = 0;
	cmd->rsvd3 = 0;
	cmd->mptr = 0;
	cmd->dptr.prp.prp1 = 0;		/* D1(a): no payload address */
	cmd->dptr.prp.prp2 = 0;
	cmd->cdw10 = (uint32_t)(d->w1 & 0xFFFFFFFFu);
	cmd->cdw11 = (uint32_t)(d->w1 >> 32);
	cmd->cdw12 = ((uint32_t)(uint16_t)d->w0 << 16) | (uint32_t)(uint16_t)(d->w0 >> 16);
	cmd->cdw13 = 0;
	cmd->cdw14 = 0;
	cmd->cdw15 = 0;
	*cb_arg = d->w3;
	*cb_fn = d->w4;
}

void
iau_fetch_one(struct iau_ctx *ctx, uint16_t qid, struct spdk_nvme_cmd *cmd,
	      uint64_t *cb_fn, uint64_t *cb_arg)
{
	struct iau_sq *sq = &ctx->sq[qid];
	const volatile struct iau_desc_d *src = &sq->desc_ring[sq->dev_tail];
	struct iau_desc_d d;

	d.w0 = src->w0;
	d.w1 = src->w1;
	d.w2 = src->w2;
	d.w3 = src->w3;
	d.w4 = src->w4;
	iau_decode_desc(&d, cmd, cb_fn, cb_arg);
	/* Compare-and-wrap == (dev_tail + 1) % entries, since dev_tail < entries always
	 * (0 at attach, advanced only here); avoids a per-descriptor divide. */
	if (++sq->dev_tail == sq->entries) {
		sq->dev_tail = 0;
	}
	sq->fetched++;
}

void
iau_flush(struct iau_ctx *ctx, uint16_t qid)
{
	struct iau_sq *sq = &ctx->sq[qid];
	uint32_t i;

	for (i = 0; i < sq->staged; i++) {
		const struct iau_staged *st = &sq->staging[i];
		volatile uint64_t *dst = &sq->rec_ring[(size_t)sq->rec_tail * 4];
		uint8_t rec[IAU_REC_BYTES];
		uint64_t w[4];

		iau_pack_moded_record(st->cb_fn, st->cb_arg, st->cpl_lo, st->cpl_hi_raw,
				      sq->rec_phase, rec);
		memcpy(w, rec, sizeof(w));
		dst[0] = w[0];
		dst[1] = w[1];
		dst[2] = w[2];
		spdk_wmb();	/* payload visible before the phase word the host polls */
		dst[3] = w[3];
		if (++sq->rec_tail == sq->entries) {
			sq->rec_tail = 0;
			sq->rec_phase ^= 1;
		}
		sq->credits_in_use--;	/* RTL credit_inc: one slot returned per emitted record */
		sq->emitted++;
	}
	sq->staged = 0;
}

void
iau_complete(struct iau_ctx *ctx, uint16_t qid, uint64_t cb_fn, uint64_t cb_arg,
	     const struct spdk_nvme_cpl *cpl, uint64_t now_tsc)
{
	uint64_t lo, hi;

	memcpy(&lo, (const uint8_t *)cpl, 8);
	memcpy(&hi, (const uint8_t *)cpl + 8, 8);
	iau_complete_words(ctx, qid, cb_fn, cb_arg, lo, hi, now_tsc);
}

void
iau_complete_words(struct iau_ctx *ctx, uint16_t qid, uint64_t cb_fn, uint64_t cb_arg,
		   uint64_t cpl_lo, uint64_t cpl_hi_raw, uint64_t now_tsc)
{
	struct iau_sq *sq = &ctx->sq[qid];
	struct iau_staged *st;

	assert(sq->active);
	assert(sq->credits_in_use < sq->entries);
	st = &sq->staging[sq->staged];
	st->cb_fn = cb_fn;
	st->cb_arg = cb_arg;
	st->cpl_lo = cpl_lo;
	st->cpl_hi_raw = cpl_hi_raw;
	if (sq->staged == 0) {
		sq->oldest_tsc = now_tsc;
	}
	sq->staged++;
	sq->credits_in_use++;
	if (sq->staged >= ctx->batch_n) {
		sq->flush_by_n++;
		iau_flush(ctx, qid);
	}
}

void
iau_sq_poll_timer(struct iau_ctx *ctx, uint16_t qid, uint64_t now_tsc)
{
	struct iau_sq *sq = &ctx->sq[qid];

	if (!sq->active || sq->staged == 0) {
		return;
	}
	if (now_tsc - sq->oldest_tsc >= ctx->batch_t_ticks) {
		sq->flush_by_t++;
		iau_flush(ctx, qid);
	}
}
