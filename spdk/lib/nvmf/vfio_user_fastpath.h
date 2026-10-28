/* SPDX-License-Identifier: BSD-3-Clause
 * NVMeVirt-style fast-path device model for the vfio-user target.
 * Mirrors the gem5 SimpleSSD fast path (LBA-hash channel policy):
 *   ch = slba % channels; start = max(now, next_free[ch]);
 *   next_free[ch] = start + svc; due = start + lmin.
 * Pure C, no SPDK runtime dependency. One fp_sq per SQ, single-thread owned.
 * IAU-next P3: the channel occupancy (next_free[]) is the drive's capacity. By default each SQ has its
 * own (IAU_DEV_MODEL=sq, the P1 model: capacity grows with the SQ count); fp_sq_share() points an SQ at
 * one fp_chan per controller instead (IAU_DEV_MODEL=ctrlr: one drive, whatever its SQ count). Every
 * fp_sq sharing an fp_chan must be driven by the same thread.
 * Lifetime rules (the code checks none of them except where noted):
 *  - an fp_chan must outlive every fp_sq bound to it; its users count must be 0 before it is
 *    re-initialised (fp_chan_init) or freed;
 *  - fp_sq_fini() a used fp_sq before fp_sq_init() on it again (init clears the binding without
 *    releasing it); a second fp_sq_fini() is a no-op for the binding;
 *  - one driving thread per fp_chan: the caller claims it with fp_chan_claim(c, thread), which
 *    refuses a second owner (-EPERM); fp_sq_fini() of the last bound fp_sq clears the owner;
 *  - users never goes below 0, even after a rule violation.
 */
#ifndef SPDK_VFIO_USER_FASTPATH_H
#define SPDK_VFIO_USER_FASTPATH_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#define FP_MAX_CHANNELS 64

struct fp_entry {
	uint64_t due_tsc;
	uint16_t cid;
	uint16_t fault_st;	/* IAU-next P4.4 (D19, V11): the former 2 B hole; FP_FAULT_MARK | an injected status */
	uint32_t sq_head_after;
	uint64_t ts_db_seen, ts_fetched;
	uint64_t aux0, aux1;	/* Mode-D cb_fn/cb_arg later; 0 for baseline */
};
SPDK_STATIC_ASSERT(offsetof(struct fp_entry, cid) == 8 && offsetof(struct fp_entry, fault_st) == 10 &&
		   offsetof(struct fp_entry, sq_head_after) == 12 && offsetof(struct fp_entry, aux1) == 40 &&
		   sizeof(struct fp_entry) == 48, "fp_entry keeps its P1 layout (fault_st fills the hole)");

/* Heap node (P1.2): the key and the slot of its payload, so a sift moves 16 B, not a 48 B entry. */
struct fp_node {
	uint64_t due_tsc;
	uint32_t slot;
};

/* Per-controller channel occupancy (IAU_DEV_MODEL=ctrlr, P3). */
struct fp_chan {
	uint64_t next_free[FP_MAX_CHANNELS];
	uint32_t channels;
	uint32_t users;		/* fp_sq bound by fp_sq_share() */
	uint64_t svc_ticks;
	const void *owner;	/* driving thread, set by fp_chan_claim(); NULL = unset */
};

struct fp_sq {
	struct fp_node *heap;		/* binary min-heap on due_tsc, n nodes */
	struct fp_entry *slots;		/* cap payloads, each referenced by at most one node */
	uint32_t *free_slots;		/* stack of the cap - n unused slot indices */
	uint32_t n, cap, n_free;
	uint64_t next_free[FP_MAX_CHANNELS];
	uint32_t channels;
	uint64_t ch_mask;	/* channels - 1 when channels is a power of two (then slba & mask == slba % channels) */
	bool ch_pow2;
	uint64_t svc_ticks, lmin_ticks;
	uint64_t scheduled, completed;
	uint64_t *nf;		/* occupancy in use: next_free (own) or chan->next_free */
	struct fp_chan *chan;	/* set by fp_sq_share(), else NULL */
	/* IAU-next P4.4 (D19 / A12): the mode-0 error knob, fp_sq_set_fault(); read only by the bind-time
	 * variants fp_schedule_fault() / fp_pop_due_fault(). At the end, so the fields above keep their offsets. */
	uint32_t fault_every;	/* 0 = off */
	uint32_t fault_n;	/* scheduled commands since the last mark */
	uint16_t fault_status;	/* the injected 16-bit cpl status (phase bit 0) */
	bool fault_reported;	/* the target printed this SQ's DEV_FAULT_STATS */
	uint64_t fault_marked, fault_posted;
	void (*fault_post)(void *arg, const struct fp_entry *e);
	void *fault_arg;
};

int fp_sq_init(struct fp_sq *q, uint32_t cap, uint32_t channels,
	       uint64_t svc_ticks, uint64_t lmin_ticks);
void fp_sq_fini(struct fp_sq *q);
int fp_chan_init(struct fp_chan *c, uint32_t channels, uint64_t svc_ticks);
/* Bind an empty q to c. 0; -EINVAL if q or c is NULL or channels / svc_ticks differ; -EBUSY if q holds
 * scheduled entries or is already bound. */
int fp_sq_share(struct fp_sq *q, struct fp_chan *c);
/* Record the thread that drives c. 0 if c has no owner (it becomes owner) or owner is already it;
 * -EPERM if another owner holds c; -EINVAL if c or owner is NULL. */
int fp_chan_claim(struct fp_chan *c, const void *owner);
/* Returns the due tick, or UINT64_MAX if the queue is full. */
uint64_t fp_schedule(struct fp_sq *q, uint64_t now, uint64_t slba, const struct fp_entry *e_in);
/* Pops the earliest entry with due_tsc <= now. Returns 1 if popped, else 0. */
int fp_pop_due(struct fp_sq *q, uint64_t now, struct fp_entry *out);

/*
 * IAU-next P4.4 (plan D19 as amended by A12 and V11): the mode-0 target error knob (IAU_DEV_FAULT_ERR_*).
 * fp_schedule() and fp_pop_due() keep their instructions: the target selects these variants once per SQ at
 * bind (the SQ's call pointers, vfio_user.c), and only when the knob is on.
 *  - fp_schedule_fault(): fp_schedule() of a copy of *e_in whose fault_st is FP_FAULT_MARK | fault_status for
 *    every fault_every-th scheduled command of the queue (1-based: the Nth, 2Nth, ...) and 0 for the others; a
 *    command the queue refuses (full) does not count. fault_every 0: exactly fp_schedule().
 *  - fp_pop_due_fault(): fp_pop_due(), but a due entry with FP_FAULT_MARK is handed to fault_post(fault_arg, e)
 *    (the target posts its CQE with that status) and not returned, so the caller's SUCCESS completion never
 *    sees it; returns the first due unmarked entry (1) or 0 once nothing is due. Without fault_post: fp_pop_due().
 * The status carrier is fault_st, the former hole at offset 10 of struct fp_entry (V11), not aux1 (cb_arg in
 * Mode D). The marks live in the entries, so the order of injected and normal completions is the model's
 * due order. These functions sit in their own section, padded to 64 B, so the .text of this object (and
 * every object linked after it) keeps its size and 64 B phase.
 */
#define FP_FAULT_MARK		0x0001u		/* fault_st bit 0 (a cpl status's phase bit, 0 in any injected status) */
#define FP_FAULT_EVERY_MAX	1000000000u
typedef void (*fp_fault_post_fn)(void *arg, const struct fp_entry *e);
/* every 0 turns the knob off (status, post and arg ignored); else status must be an error (SC or SCT != 0) with
 * phase bit 0, every <= FP_FAULT_EVERY_MAX and post non-NULL. Resets the count. 0 or -EINVAL (q unchanged). */
int fp_sq_set_fault(struct fp_sq *q, uint32_t every, uint16_t status, fp_fault_post_fn post, void *arg);
uint64_t fp_schedule_fault(struct fp_sq *q, uint64_t now, uint64_t slba, const struct fp_entry *e_in);
int fp_pop_due_fault(struct fp_sq *q, uint64_t now, struct fp_entry *out);

#endif
