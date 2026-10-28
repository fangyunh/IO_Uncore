/* SPDX-License-Identifier: BSD-3-Clause */
#include "vfio_user_fastpath.h"

int
fp_sq_init(struct fp_sq *q, uint32_t cap, uint32_t channels,
	   uint64_t svc_ticks, uint64_t lmin_ticks)
{
	uint32_t s;

	if (q == NULL || cap == 0 || channels == 0 || channels > FP_MAX_CHANNELS) {
		return -EINVAL;
	}
	memset(q, 0, sizeof(*q));
	q->heap = calloc(cap, sizeof(*q->heap));
	q->slots = calloc(cap, sizeof(*q->slots));
	q->free_slots = calloc(cap, sizeof(*q->free_slots));
	if (q->heap == NULL || q->slots == NULL || q->free_slots == NULL) {
		free(q->heap);
		free(q->slots);
		free(q->free_slots);
		memset(q, 0, sizeof(*q));
		return -ENOMEM;
	}
	for (s = 0; s < cap; s++) {
		q->free_slots[s] = cap - 1 - s;
	}
	q->n_free = cap;
	q->cap = cap;
	q->channels = channels;
	q->ch_pow2 = (channels & (channels - 1)) == 0;
	q->ch_mask = q->ch_pow2 ? (uint64_t)channels - 1 : 0;
	q->svc_ticks = svc_ticks;
	q->lmin_ticks = lmin_ticks;
	q->nf = q->next_free;
	return 0;
}

void
fp_sq_fini(struct fp_sq *q)
{
	if (q == NULL) {
		return;
	}
	if (q->chan != NULL) {
		if (q->chan->users > 0) {
			q->chan->users--;
		}
		if (q->chan->users == 0) {
			q->chan->owner = NULL;
		}
		q->chan = NULL;
	}
	free(q->heap);
	free(q->slots);
	free(q->free_slots);
	q->heap = NULL;
	q->slots = NULL;
	q->free_slots = NULL;
	q->n = q->cap = q->n_free = 0;
	q->nf = q->next_free;
}

/*
 * The heap below runs the same sift-up / sift-down on due_tsc as the former
 * heap of whole entries, so dues, pop order (ties included) and payloads are
 * unchanged; only the 16 B nodes move. tests/realcore/test_vfio_user_fastpath.c
 * (g) checks it op for op against the former code.
 */

uint64_t
fp_schedule(struct fp_sq *q, uint64_t now, uint64_t slba, const struct fp_entry *e_in)
{
	uint32_t ch, i, slot;
	uint64_t start, due;

	if (q->n >= q->cap) {
		return UINT64_MAX;
	}
	/* Same channel as slba % channels; the mask skips a 64-bit divide (the
	 * harness's 32/64-channel cells), other channel counts keep the divide. */
	ch = q->ch_pow2 ? (uint32_t)(slba & q->ch_mask) : (uint32_t)(slba % q->channels);
	start = q->nf[ch] > now ? q->nf[ch] : now;
	q->nf[ch] = start + q->svc_ticks;
	due = start + q->lmin_ticks;

	slot = q->free_slots[--q->n_free];
	q->slots[slot] = *e_in;
	q->slots[slot].due_tsc = due;

	i = q->n++;
	while (i > 0) {
		uint32_t p = (i - 1) / 2;
		if (!(due < q->heap[p].due_tsc)) {
			break;
		}
		q->heap[i] = q->heap[p];
		i = p;
	}
	q->heap[i].due_tsc = due;
	q->heap[i].slot = slot;
	q->scheduled++;
	return due;
}

int
fp_pop_due(struct fp_sq *q, uint64_t now, struct fp_entry *out)
{
	struct fp_node last;
	uint32_t i = 0, top;

	if (q->n == 0 || q->heap[0].due_tsc > now) {
		return 0;
	}
	top = q->heap[0].slot;
	*out = q->slots[top];
	q->free_slots[q->n_free++] = top;
	last = q->heap[--q->n];
	while (true) {
		uint32_t c = 2 * i + 1;
		if (c >= q->n) {
			break;
		}
		if (c + 1 < q->n && q->heap[c + 1].due_tsc < q->heap[c].due_tsc) {
			c++;
		}
		if (!(q->heap[c].due_tsc < last.due_tsc)) {
			break;
		}
		q->heap[i] = q->heap[c];
		i = c;
	}
	if (q->n > 0) {
		q->heap[i] = last;
	}
	q->completed++;
	return 1;
}

int
fp_chan_init(struct fp_chan *c, uint32_t channels, uint64_t svc_ticks)
{
	if (c == NULL || channels == 0 || channels > FP_MAX_CHANNELS) {
		return -EINVAL;
	}
	memset(c, 0, sizeof(*c));
	c->channels = channels;
	c->svc_ticks = svc_ticks;
	return 0;
}

int
fp_sq_share(struct fp_sq *q, struct fp_chan *c)
{
	if (q == NULL || c == NULL || q->channels != c->channels || q->svc_ticks != c->svc_ticks) {
		return -EINVAL;
	}
	if (q->n != 0 || q->chan != NULL) {
		return -EBUSY;
	}
	q->chan = c;
	q->nf = c->next_free;
	c->users++;
	return 0;
}

int
fp_chan_claim(struct fp_chan *c, const void *owner)
{
	if (c == NULL || owner == NULL) {
		return -EINVAL;
	}
	if (c->owner != NULL && c->owner != owner) {
		return -EPERM;
	}
	c->owner = owner;
	return 0;
}

/*
 * IAU-next P4.4: the error-knob variants (see the header). Their own section, kept in source order between
 * the BEGIN label and the END padding, which rounds the section up to a multiple of 64 B (GCC; other
 * compilers get the section without the padding).
 */
#if defined(__GNUC__) && !defined(__clang__)
#define FP_FAULT_OOL	__attribute__((noipa, no_reorder, section(".text.vfio_user_fp_fault")))
__asm__(".pushsection .text.vfio_user_fp_fault,\"ax\",@progbits\n.Lfp_fault_ool_begin:\n\t.popsection");
#else
#define FP_FAULT_OOL	__attribute__((noipa, section(".text.vfio_user_fp_fault")))
#endif

int FP_FAULT_OOL
fp_sq_set_fault(struct fp_sq *q, uint32_t every, uint16_t status, fp_fault_post_fn post, void *arg)
{
	if (q == NULL) {
		return -EINVAL;
	}
	if (every != 0 && (every > FP_FAULT_EVERY_MAX || (status & 0x0FFEu) == 0 || (status & FP_FAULT_MARK) != 0 ||
			   post == NULL)) {
		return -EINVAL;
	}
	q->fault_every = every;
	q->fault_n = 0;
	q->fault_status = every != 0 ? status : 0;
	q->fault_post = every != 0 ? post : NULL;
	q->fault_arg = every != 0 ? arg : NULL;
	return 0;
}

uint64_t FP_FAULT_OOL
fp_schedule_fault(struct fp_sq *q, uint64_t now, uint64_t slba, const struct fp_entry *e_in)
{
	struct fp_entry e;
	uint64_t due;
	bool mark;

	if (q->fault_every == 0) {
		return fp_schedule(q, now, slba, e_in);
	}
	e = *e_in;
	mark = ++q->fault_n >= q->fault_every;
	e.fault_st = mark ? (uint16_t)(FP_FAULT_MARK | q->fault_status) : 0;	/* the knob owns fault_st */
	due = fp_schedule(q, now, slba, &e);
	if (due == UINT64_MAX) {
		q->fault_n--;		/* refused: not a scheduled command, the next one takes its place */
	} else if (mark) {
		q->fault_n = 0;
		q->fault_marked++;
	}
	return due;
}

int FP_FAULT_OOL
fp_pop_due_fault(struct fp_sq *q, uint64_t now, struct fp_entry *out)
{
	while (fp_pop_due(q, now, out)) {
		if ((out->fault_st & FP_FAULT_MARK) == 0 || q->fault_post == NULL) {
			return 1;
		}
		q->fault_posted++;
		q->fault_post(q->fault_arg, out);
	}
	return 0;
}

#if defined(__GNUC__) && !defined(__clang__)
__asm__(".pushsection .text.vfio_user_fp_fault,\"ax\",@progbits\n"
	"\t.skip (-(. - .Lfp_fault_ool_begin)) & 63, 0xcc\n\t.popsection");
#endif
