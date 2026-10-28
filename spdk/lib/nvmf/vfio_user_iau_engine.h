/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P3: the IAU-engine core for the vfio-user testbed (spec §3.4). For one bound queue it plays the
 * on-die tile's SSD-facing role: it reads the host's 40 B Mode D descriptor ring, writes real 64 B NVMe
 * SQEs into the SSD's own I/O SQ (host memory), rings the SSD's SQ doorbell, polls the SSD's CQ, rings its
 * CQ-head doorbell, and is the only producer of the host's 32 B record ring. The SSD-facing CID is the
 * engine's slot tag; the slot holds {cb_fn, cb_arg, host cid} (the cookie store). Records are byte-identical
 * to the in-target engine's (vfio_user_iau.c) for successes; the SSD's status bits (SC, SCT, CRD, M, DNR)
 * and its cpl bytes [0:8) (cdw0) pass through, so the host's P2.1 retry policy sees what the SSD posted.
 * N = 1: every record is published as iau_flush() publishes one (w0..w2, spdk_wmb, w3).
 * Pure C, no SPDK runtime: an iau_eng_q is driven by one engine thread (vfio_user.c binds it).
 *
 * Lifetime: iau_eng_q_init() on a zeroed or finished q; every other call from the one thread that polls q;
 * print the STATS line (iau_eng_q_fmt_stats()) before iau_eng_q_fini(), which frees the slot pool (after
 * it iau_eng_q_inflight() reads entries and a poll would dereference NULL). Never init a live q again
 * without fini (the pool would leak).
 *
 * Notes for the binding code (P3.9, vfio_user.c):
 * - Stop polling q, then print STATS, then fini, all BEFORE the host DMA regions behind the rings and
 *   the SSD SQ/CQ are unmapped, and BEFORE Delete I/O SQ resets the SQ doorbell: host_ssd_db
 *   (iau_eng_q_foreign_db()) reads the SSD SQ doorbell and the host's CQ sink, and a reset doorbell
 *   reads as a foreign write.
 * - On queue re-creation (Delete then Create I/O SQ/CQ with the same qid) fini the old q and init a new
 *   one: the SSD restarts its SQ head at 0 and its CQ at phase 1, and the host restarts both rings at
 *   index 0 / phase 1, which is exactly the state init sets.
 *
 * Contracts and guards:
 * - Credits. Registration enforces credits <= entries - 1 and <= cq_entries - 1. The engine also holds
 *   descriptors back while its own in-flight count is at credits (credit_stalls), so the SSD SQ, the SSD
 *   CQ and the slot pool cannot overflow even if the host over-submits. The record ring has no head
 *   doorbell: the engine cannot see which records the host has consumed, so the record ring relies on the
 *   host's credit contract alone (records not yet consumed + commands in flight <= the host's
 *   outstanding commands <= credits <= entries - 1). A host that breaks it can have records overwritten.
 * - Tags. Free tags are a FIFO ring handed out 0, 1, 2, ... A freed tag is reused only after every tag
 *   freed before it: no earlier than the (entries - inflight)-th allocation after its free (>= 2 with
 *   credits = entries - 1; with one command in flight, after entries - 1 further completions).
 *   bad_cid catches a stray or duplicate CQE (tag out of range or not in flight) only until its tag is
 *   reused; after that it is indistinguishable from the new command's completion. entries <= 65535, so
 *   tag 0xFFFF (the NVMe error log's "not command specific" CID) is never handed out.
 * - SQ full. An SQE slot is free once a CQE reports an SQHD past it; the engine keeps one slot empty
 *   (sq_full_stalls). A CQE whose sqhd >= sq_entries is counted (bad_sqhd) and does not move the SQHD
 *   the guard uses.
 * - bad_db counts the POLLS that read an out-of-range DESC_TAIL (one bad host write read by k polls
 *   counts k); any value > 0 is the failure, the magnitude is not meaningful.
 *
 * STATS counters and P2.1 re-issues (the accounting check_l.py NX_ENGINE relies on): on a retryable
 * error the host consumes the error record and re-issues the same 40 B descriptor on the same host cid at
 * its descriptor tail (nvme_pcie_md_reissue()), exactly as it submits a new one. The engine therefore
 * fetches it as a new descriptor and gives it a new SQE, a CQE and a record: desc_fetched, sqes_written,
 * cqes_consumed and records_emitted all include re-issues and stay equal. The engine cannot tell a
 * re-issue from a new IO on a recycled cid (the retry decision is host state: retries vs
 * transport_retry_count and the nvme_completion_is_retry() list; a delivered error frees the cid for the
 * next IO), so the STATS line keeps reissues= for check_l and it is always 0. err_records counts records
 * whose status is an error (SCT/SC != 0), an upper bound on the host's re-issues of this queue.
 */
#ifndef SPDK_VFIO_USER_IAU_ENGINE_H
#define SPDK_VFIO_USER_IAU_ENGINE_H

#include "spdk/stdinc.h"
#include "spdk/nvme_spec.h"
#include "vfio_user_iau.h"

#define IAU_ENG_MAX_ENTRIES 65535u	/* tags 0..65534: 0xFFFF is never an SSD CID */

struct iau_eng_slot {
	uint64_t cb_fn;
	uint64_t cb_arg;
	uint16_t host_cid;
	uint16_t busy;
	uint32_t rsvd;
};

struct iau_eng_q_cfg {
	uint16_t qid;				/* host qid == SSD qid in the testbed (record sqid); 1..IAU_MAX_QID-1 */
	uint32_t entries;			/* descriptor ring == record ring entries */
	uint32_t nsid;				/* QCFG nsid */
	uint32_t credits;			/* QCFG credits: the host's in-flight cap */
	const volatile void *desc_ring;		/* entries * 40 B */
	volatile void *rec_ring;		/* entries * 32 B */
	const volatile uint32_t *host_db;	/* DESC_TAIL[qid] */
	const volatile uint32_t *host_cq_sink;	/* the host's redirected CQ-head doorbell (must stay 0) */
	volatile void *ssd_sq;			/* sq_entries * 64 B */
	const volatile void *ssd_cq;		/* cq_entries * 16 B */
	uint32_t sq_entries, cq_entries;
	volatile uint32_t *ssd_sq_db;		/* the SSD's SQ tail doorbell */
	volatile uint32_t *ssd_cq_db;		/* the SSD's CQ head doorbell */
};

struct iau_eng_q {
	const volatile struct iau_desc_d *desc_ring;
	volatile uint64_t *rec_ring;
	const volatile uint32_t *host_db;
	volatile struct spdk_nvme_cmd *ssd_sq;
	const volatile struct spdk_nvme_cpl *ssd_cq;
	volatile uint32_t *ssd_sq_db;
	volatile uint32_t *ssd_cq_db;
	struct iau_eng_slot *slots;		/* entries; index = slot tag = SSD CID */
	uint16_t *free_ring;			/* entries; FIFO of free tags */
	uint32_t free_head, free_tail;		/* pop at head, push at tail */
	uint32_t n_free;
	uint32_t free_floor;			/* entries - credits: no fetch while n_free <= free_floor */
	uint32_t entries, sq_entries, cq_entries;
	uint32_t desc_head;			/* next descriptor to fetch */
	uint32_t rec_tail;			/* next record slot */
	uint32_t sq_tail;			/* next SSD SQ slot */
	uint32_t sq_head_seen;			/* SQHD of the latest CQE with sqhd < sq_entries */
	uint32_t cq_head;			/* next SSD CQ slot */
	uint8_t rec_phase, cq_phase;
	uint16_t qid;
	uint32_t nsid, credits;
	const volatile uint32_t *host_cq_sink;
	uint64_t desc_fetched, sqes_written, ssd_db_writes;
	uint64_t cqes_consumed, cq_db_writes, records_emitted;
	uint64_t bad_cid, bad_db, bad_sqhd;
	uint64_t err_records;			/* records with an error status (SCT/SC != 0) */
	uint64_t sq_full_stalls, credit_stalls;	/* not on the STATS line */
};

/* Validates cfg and initializes q (allocates the slot pool). -EINVAL refuses what the tile's registration
 * refuses: qid 0 or >= IAU_MAX_QID, entries < 2 or > IAU_ENG_MAX_ENTRIES, sq_entries != entries,
 * cq_entries < 2, credits 0 or > entries - 1 or > cq_entries - 1 (credit invariant: neither the SSD SQ
 * nor the SSD CQ nor the record ring can overflow while the host keeps its credits), nsid 0, a NULL
 * pointer; q is not touched then. -ENOMEM on allocation failure. */
int iau_eng_q_init(struct iau_eng_q *q, const struct iau_eng_q_cfg *cfg);
void iau_eng_q_fini(struct iau_eng_q *q);
/* Work bound of one iau_eng_q_poll() (P3.9 review m6): at most IAU_ENG_POLL_BUDGET CQEs consumed and
 * at most IAU_ENG_POLL_BUDGET descriptors moved; the rest waits in the rings for the next poll. An
 * engine pass polls each of its queues once, so a queue that bursts delays the others by at most one
 * budget of each kind per pass. */
#define IAU_ENG_POLL_BUDGET 64u
/* One poll: SSD CQEs -> records (one CQ-head doorbell if any), then descriptors -> SQEs (one SQ
 * doorbell if any), each bounded by IAU_ENG_POLL_BUDGET. Returns CQEs consumed + descriptors moved. */
uint32_t iau_eng_q_poll(struct iau_eng_q *q);

static inline uint32_t
iau_eng_q_inflight(const struct iau_eng_q *q)
{
	return q->entries - q->n_free;
}

/* 1 if anyone but the engine wrote this queue's SSD doorbells: the host's redirected CQ-head doorbell is
 * not 0, or the SSD SQ doorbell differs from the engine's tail. */
static inline uint32_t
iau_eng_q_foreign_db(const struct iau_eng_q *q)
{
	return (*q->host_cq_sink != 0) || (*q->ssd_sq_db != q->sq_tail);
}

/* Evidence lines, without the trailing newline, exactly as scripts/realcore/inv/check_l.py (NX_ENGINE:
 * ENG_START, ENG_BIND, ENG_STATS, ENG_NEED) parses them. snprintf semantics: the return value is the
 * length of the whole line; a short buffer truncates. IAU_ENG_LINE_MAX holds any line.
 *   IAU_ENGINE_START engine=<e> lcore=<l> want=<l>
 *   IAU_ENGINE_BIND ctrlr=<k> qid=<q> engine=<e> lcore=<l> nsid=<n> credits=<c>
 *   IAU_ENGINE_STATS ctrlr=<k> qid=<q> engine=<e> desc_fetched=<n> sqes_written=<n> ssd_db_writes=<n>
 *     cqes_consumed=<n> cq_db_writes=<n> records_emitted=<n> inflight=<n> bad_cid=<n> bad_db=<n>
 *     host_ssd_db=<0|1> reissues=0 err_records=<n> bad_sqhd=<n>
 * (one line; host_ssd_db = iau_eng_q_foreign_db(); reissues is always 0, see above). */
#define IAU_ENG_LINE_MAX 448
int iau_eng_fmt_start(char *buf, size_t len, uint32_t engine, uint32_t lcore, uint32_t want);
int iau_eng_q_fmt_bind(char *buf, size_t len, const struct iau_eng_q *q, int ctrlr, uint32_t engine,
		       uint32_t lcore);
int iau_eng_q_fmt_stats(char *buf, size_t len, const struct iau_eng_q *q, int ctrlr, uint32_t engine);

/* P3.9 bind/unbind handshake (vfio_user.c). A requester (a controller or poll-group thread) arms a
 * request's word (PENDING) and sends the request to the engine thread as an SPDK message. The engine
 * thread takes it when it runs the message (PENDING -> RUNNING), does the work and publishes DONE; it
 * never touches the request after that store, and the requester owns the request again once it reads
 * DONE. A requester that gives up while the word is still PENDING abandons it (PENDING -> ABANDONED):
 * from then on the request belongs to the engine thread, whose take fails and which then disposes of it
 * without touching any memory the requester may release. A request the engine has taken cannot be
 * abandoned (the requester keeps waiting). Take and abandon are compare-and-swaps, so exactly one side
 * wins a race between them. vfio_user.c abandons only binds (a queue not yet polled); it never abandons
 * an unbind, since the engine may still be polling the memory the caller is about to release. Release/acquire order everything written before arm (requester) and
 * before done (engine) for the other side. */
enum {
	IAU_ENG_HS_IDLE = 0,
	IAU_ENG_HS_PENDING,
	IAU_ENG_HS_RUNNING,
	IAU_ENG_HS_DONE,
	IAU_ENG_HS_ABANDONED,
};

/* Requester, before sending the request. */
static inline void
iau_eng_hs_arm(uint32_t *hs)
{
	__atomic_store_n(hs, IAU_ENG_HS_PENDING, __ATOMIC_RELEASE);
}

/* Engine thread, first thing in the message: true if it now owns the request, false if the requester
 * abandoned it (or never armed it). */
static inline bool
iau_eng_hs_take(uint32_t *hs)
{
	uint32_t want = IAU_ENG_HS_PENDING;

	return __atomic_compare_exchange_n(hs, &want, IAU_ENG_HS_RUNNING, false, __ATOMIC_ACQ_REL,
					   __ATOMIC_ACQUIRE);
}

/* Engine thread, after its last access to a request it took. */
static inline void
iau_eng_hs_done(uint32_t *hs)
{
	__atomic_store_n(hs, IAU_ENG_HS_DONE, __ATOMIC_RELEASE);
}

/* Requester, one check while waiting: 1 = DONE (the request is the requester's again); -ETIMEDOUT =
 * abandoned (expired while the engine had not taken it; the request is the engine's now); 0 = keep
 * waiting (also when expired but the engine has taken it). Not called again after a non-zero return. */
static inline int
iau_eng_hs_check(uint32_t *hs, bool expired)
{
	uint32_t st = __atomic_load_n(hs, __ATOMIC_ACQUIRE);

	if (st == IAU_ENG_HS_DONE) {
		return 1;
	}
	if (expired && st == IAU_ENG_HS_PENDING &&
	    __atomic_compare_exchange_n(hs, &st, IAU_ENG_HS_ABANDONED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return -ETIMEDOUT;
	}
	return 0;
}

#endif
