/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.3: the IAU tile engine core (vfio_user_iau_tile.h has the model, the states and the rules). */
#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/likely.h"
#include "vfio_user_iau_tile.h"

/* D20: x86 TSO on WB memory keeps store order and does not pass a store over older loads, so the SSD SQ and
 * CQ-head doorbells are plain release stores there; elsewhere spdk_wmb() / spdk_mb() first, as the host does. */
#if defined(__x86_64__) || defined(__i386__)
#define eng6_pre_sq_db() spdk_compiler_barrier()
#define eng6_pre_cq_db() spdk_compiler_barrier()
#else
#define eng6_pre_sq_db() spdk_wmb()
#define eng6_pre_cq_db() spdk_mb()
#endif

#define ENG6_SC_SCT	0x0FFEu		/* status word: SC [8:1], SCT [11:9]; both 0 <=> success */
#define ST_SQDEL	iau_tile_status(SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, true)
#define ST_ABRQ		iau_tile_status(SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_BY_REQUEST, true)
#define ST_INVAL	iau_tile_status(SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD, true)

enum { ENG6_NEXT = 0, ENG6_STALL = 1 };

#define ENG6_KEY(n) #n,
const char *const iau_eng6_member_stat_keys[IAU_ENG6_MEMBER_NSTATS] = { IAU_ENG6_MEMBER_STATS(ENG6_KEY) };
const char *const iau_eng6_ring_stat_keys[IAU_ENG6_RING_NSTATS] = { IAU_ENG6_RING_STATS(ENG6_KEY) };
#undef ENG6_KEY
#define ENG6_MOFF(n) offsetof(struct iau_eng6_mstats, n),
#define ENG6_ROFF(n) offsetof(struct iau_eng6_rstats, n),
static const size_t g_moff[IAU_ENG6_MEMBER_NSTATS] = { IAU_ENG6_MEMBER_STATS(ENG6_MOFF) };
static const size_t g_roff[IAU_ENG6_RING_NSTATS] = { IAU_ENG6_RING_STATS(ENG6_ROFF) };
#undef ENG6_MOFF
#undef ENG6_ROFF

uint64_t
iau_eng6_member_stat(const struct iau_eng6_mstats *st, unsigned i)
{
	uint64_t v;

	if (i >= IAU_ENG6_MEMBER_NSTATS) {
		return 0;
	}
	memcpy(&v, (const uint8_t *)st + g_moff[i], sizeof(v));
	return v;
}

uint64_t
iau_eng6_ring_stat(const struct iau_eng6_rstats *st, unsigned i)
{
	uint64_t v;

	if (i >= IAU_ENG6_RING_NSTATS) {
		return 0;
	}
	memcpy(&v, (const uint8_t *)st + g_roff[i], sizeof(v));
	return v;
}

int
iau_eng6_ring_init(struct iau_eng6_ring *r, const struct iau_eng6_ring_cfg *cfg)
{
	unsigned i;

	if (r == NULL || cfg == NULL || cfg->d == NULL || cfg->db == NULL || cfg->d_head_out == NULL || cfg->r == NULL ||
	    cfg->d_entries < IAU_ENG6_RING_MIN || cfg->d_entries > IAU_ENG6_RING_MAX ||
	    cfg->r_entries < IAU_ENG6_RING_MIN || cfg->r_entries > IAU_ENG6_RING_MAX ||
	    !iau_tile_stride_ok(cfg->d_stride) || cfg->nk == 0 || cfg->nk > IAU_TILE_MAX_K || cfg->k >= cfg->nk ||
	    !iau_tile_gcb_flags_ok(cfg->gcb_flags) || ((cfg->gcb_flags & IAU_TILE_GCB_F_RCONS) != 0 && cfg->r_consumed == NULL)) {
		return -EINVAL;
	}
	memset(r, 0, sizeof(*r));
	r->gid = cfg->gid;
	r->k = cfg->k;
	r->nk = cfg->nk;
	r->d = (const volatile uint8_t *)cfg->d;
	r->db = cfg->db;
	r->d_head_out = cfg->d_head_out;
	r->r = (volatile struct iau_tile_rec *)cfg->r;
	r->rcons_on = (cfg->gcb_flags & IAU_TILE_GCB_F_RCONS) != 0;
	r->r_consumed = r->rcons_on ? cfg->r_consumed : NULL;	/* the tripwire only with the GCB flag */
	r->d_entries = cfg->d_entries;
	r->stride = cfg->d_stride;
	r->r_entries = cfg->r_entries;
	for (i = 0; i < 8; i++) {
		r->woff[i] = (int8_t)iau_tile_word(cfg->d_stride, i);
	}
	r->woff[3] = r->woff[7] = -1;	/* d3, d7: defined ignored (D5) */
	r->d_seq = cfg->d_seq0;
	r->d_idx = (uint32_t)(cfg->d_seq0 % cfg->d_entries);
	r->r_seq = cfg->r_seq0;
	r->r_tail = (uint32_t)(cfg->r_seq0 % cfg->r_entries);
	r->r_phase = (uint8_t)(1u ^ ((cfg->r_seq0 / cfg->r_entries) & 1u));	/* the host starts at phase 1 */
	return 0;
}

int
iau_eng6_ring_remap(struct iau_eng6_ring *r, const volatile void *d, const volatile uint32_t *db,
		    volatile uint64_t *d_head_out, volatile void *rec, const volatile uint64_t *r_consumed)
{
	if (r == NULL || d == NULL || db == NULL || d_head_out == NULL || rec == NULL || (r->rcons_on && r_consumed == NULL)) {
		return -EINVAL;
	}
	r->d = (const volatile uint8_t *)d;
	r->db = db;
	r->d_head_out = d_head_out;
	r->r = (volatile struct iau_tile_rec *)rec;
	r->r_consumed = r->rcons_on ? r_consumed : NULL;
	return 0;
}

int
iau_eng6_member_init(struct iau_eng6_member *m, const struct iau_eng6_member_cfg *cfg)
{
	uint32_t i;

	if (m == NULL || cfg == NULL || cfg->route >= IAU_TILE_MAX_ROUTES || cfg->qid == 0 || cfg->qid >= IAU_TILE_QIDS ||
	    cfg->entries < 2 || cfg->entries > IAU_TILE_MAX_SLOTS || cfg->sq_entries != cfg->entries ||
	    cfg->cq_entries < 2 || cfg->credits == 0 || cfg->credits > cfg->entries - 1 ||
	    cfg->credits > cfg->cq_entries - 1 || cfg->nsid == 0 || cfg->lba_shift < 9 || cfg->lba_shift > 31 ||
	    cfg->gen > IAU_TILE_MEMBER_GEN_MASK ||
	    (cfg->fault.err_every != 0 && ((cfg->fault.err_status & ENG6_SC_SCT) == 0 || (cfg->fault.err_status & 1u) != 0)) ||
	    cfg->ssd_sq == NULL || cfg->ssd_cq == NULL || cfg->ssd_sq_db == NULL || cfg->ssd_cq_db == NULL ||
	    cfg->host_sink == NULL || cfg->echo == NULL) {
		return -EINVAL;
	}
	memset(m, 0, sizeof(*m));
	m->slots = calloc(cfg->entries, sizeof(*m->slots));
	m->free_ring = calloc(cfg->entries, sizeof(*m->free_ring));
	/* one 256 B list per slot (IAU_TILE_PRP_MAX x 8 B): 256 B-aligned, so no list crosses a page */
	m->prp = aligned_alloc(256, (size_t)cfg->entries * IAU_TILE_PRP_MAX * sizeof(uint64_t));
	if (m->slots == NULL || m->free_ring == NULL || m->prp == NULL) {
		free(m->slots);
		free(m->free_ring);
		free(m->prp);
		memset(m, 0, sizeof(*m));
		return -ENOMEM;
	}
	memset(m->prp, 0, (size_t)cfg->entries * IAU_TILE_PRP_MAX * sizeof(uint64_t));
	for (i = 0; i < cfg->entries; i++) {
		m->free_ring[i] = (uint16_t)i;	/* FIFO: hands out 0, 1, 2, ... (head = tail = 0, full) */
		m->slots[i].route = cfg->route;
		m->slots[i].gen = (uint8_t)(cfg->gen & IAU_TILE_TAG_GEN_MASK);	/* m8: tags differ per incarnation */
	}
	m->n_free = cfg->entries;
	m->route = cfg->route;
	m->qid = cfg->qid;
	m->ctrlr = cfg->ctrlr;
	m->gen = cfg->gen;
	m->lba_shift = cfg->lba_shift;
	m->state = IAU_ENG6_M_ACTIVE;
	m->cq_phase = 1;		/* the SSD's CQ starts at phase 1 */
	m->entries = cfg->entries;
	m->credits = cfg->credits;
	m->nsid = cfg->nsid;
	m->timeout_us = cfg->timeout_us;
	m->sq_entries = cfg->sq_entries;
	m->cq_entries = cfg->cq_entries;
	m->fault = cfg->fault;
	m->sq = (volatile struct spdk_nvme_cmd *)cfg->ssd_sq;
	m->cq = (const volatile struct spdk_nvme_cpl *)cfg->ssd_cq;
	m->sq_db = cfg->ssd_sq_db;
	m->cq_db = cfg->ssd_cq_db;
	m->host_sink = cfg->host_sink;
	m->echo = (volatile struct spdk_nvme_cmd *)cfg->echo;
	return 0;
}

void
iau_eng6_member_fini(struct iau_eng6_member *m)
{
	if (m == NULL) {
		return;
	}
	free(m->slots);
	free(m->free_ring);
	free(m->prp);
	m->slots = NULL;
	m->free_ring = NULL;
	m->prp = NULL;
	m->n_free = m->entries;
}

int
iau_eng6_member_add(struct iau_eng6_ring *r, struct iau_eng6_member *m)
{
	uint64_t nd, nr;
	uint32_t i;

	if (r == NULL || m == NULL || m->slots == NULL || m->state != IAU_ENG6_M_ACTIVE ||
	    m->route >= IAU_TILE_MAX_ROUTES || m->route % r->nk != r->k) {
		return -EINVAL;
	}
	for (i = 0; i < r->n; i++) {
		if (r->list[i] == m) {
			return -EINVAL;
		}
	}
	if (r->route[m->route] != NULL) {
		return -EEXIST;
	}
	if (r->gen_seen[m->route] && r->last_gen[m->route] == m->gen) {
		return -ESTALE;
	}
	nd = r->need_d + iau_tile_route_need_d(m->credits);
	nr = r->need_r + iau_tile_route_need_r(m->credits);
	if (r->resv[m->route] != 0) {		/* a rejoin replaces its route's reservation (I2) */
		nd -= iau_tile_route_need_d(r->resv[m->route]);
		nr -= iau_tile_route_need_r(r->resv[m->route]);
	}
	if (!iau_tile_ring_fits(nd, nr, r->d_entries, r->r_entries)) {
		return -ENOSPC;
	}
	if (r->resv[m->route] != 0) {
		r->resv[m->route] = 0;
		r->n_resv--;
	}
	r->need_d = nd;
	r->need_r = nr;
	r->list[r->n++] = m;
	r->route[m->route] = m;
	r->last_gen[m->route] = m->gen;
	r->gen_seen[m->route] = 1;
	return 0;
}

/* one 32 B record (iau_tile_rec_publish: w0, w1, w2, then w3 with the phase as a release store) */
static void
eng6_emit(struct iau_eng6_ring *r, uint64_t w0, uint64_t w1, uint64_t w2, uint16_t route, uint16_t tag, uint16_t st)
{
	if (spdk_unlikely(r->d_pub != r->d_cur)) {
		/* a record emitted while fetching answers a descriptor the GCB d_head does not cover yet: publish
		 * it first, so a host that sees the record also sees the D slots it frees (A15 assert) */
		__atomic_store_n(r->d_head_out, r->d_cur, __ATOMIC_RELEASE);
		r->d_pub = r->d_cur;
	}
	if (spdk_unlikely(r->r_consumed != NULL) &&
	    r->r_seq - __atomic_load_n(r->r_consumed, __ATOMIC_RELAXED) >= r->r_entries) {
		r->st.r_overflow++;	/* this record overwrites one the host has not consumed (a counter only) */
	}
	iau_tile_rec_publish(&r->r[r->r_tail], w0, w1, w2, iau_tile_w3((uint16_t)r->d_idx, route, tag, st), r->r_phase);
	if (++r->r_tail == r->r_entries) {
		r->r_tail = 0;
		r->r_phase ^= 1;
	}
	r->r_seq++;
	r->st.records_emitted++;
}

static inline uint16_t
slot_tag(const struct iau_eng6_slot *s, uint32_t idx)
{
	return iau_tile_tag(s->gen, (uint16_t)idx);
}

static inline void
slot_free(struct iau_eng6_ring *r, struct iau_eng6_member *m, uint32_t idx)
{
	struct iau_eng6_slot *s = &m->slots[idx];

	if (s->state == IAU_ENG6_S_PARKED) {
		m->n_parked--;
	}
	s->state = IAU_ENG6_S_FREE;
	s->gen = iau_tile_gen_next(s->gen);	/* the old tag is stale from now on */
	s->bits = 0;
	m->free_ring[m->free_tail] = (uint16_t)idx;
	if (++m->free_tail == m->entries) {
		m->free_tail = 0;
	}
	m->n_free++;
	r->in_use--;
}

/* the SQE of an IO-kind slot from its replay fields, with the slot's own PRP list (D17) */
static inline int
slot_replay(const struct iau_eng6_member *m, uint32_t idx, struct spdk_nvme_cmd *c)
{
	const struct iau_eng6_slot *s = &m->slots[idx];
	uint64_t *list = &m->prp[(size_t)idx * IAU_TILE_PRP_MAX];

	return iau_tile_replay_to_sqe(s->opc, s->flags, s->nlb, s->slba, s->buf, m->nsid, slot_tag(s, idx), m->lba_shift,
				    list, (uint64_t)(uintptr_t)list, c);
}

static inline bool
sq_full(struct iau_eng6_member *m)
{
	uint32_t next = m->sq_tail + 1 == m->sq_entries ? 0 : m->sq_tail + 1;

	if (spdk_unlikely(next == m->sq_head_seen)) {
		m->st.sq_full_stalls++;	/* the SSD has not consumed enough (a tripwire: credits < sq_entries) */
		return true;
	}
	return false;
}

static void
write_sqe(struct iau_eng6_ring *r, struct iau_eng6_member *m, const struct spdk_nvme_cmd *c)
{
	volatile uint64_t *dst = (volatile uint64_t *)&m->sq[m->sq_tail];
	uint64_t w[8];
	unsigned i;

	memcpy(w, c, sizeof(w));
	for (i = 0; i < 8; i++) {
		dst[i] = w[i];
	}
	/* the doorbell may expose everything up to an open FUSE_FIRST, not the FIRST itself (D20) */
	m->fuse_open = c->fuse == SPDK_NVME_CMD_FUSE_FIRST;
	if (m->fuse_open) {
		m->fuse_tail = m->sq_tail;
	}
	m->sq_tail = m->sq_tail + 1 == m->sq_entries ? 0 : m->sq_tail + 1;
	m->st.sqes_written++;
	if (!m->sq_dirty) {
		m->sq_dirty = 1;
		r->dirty[r->n_dirty++] = m;
	}
}

static inline void
note_alloc(struct iau_eng6_ring *r, struct iau_eng6_member *m)
{
	uint32_t inflight;

	if (++m->free_head == m->entries) {
		m->free_head = 0;
	}
	m->n_free--;
	inflight = m->entries - m->n_free;
	if (inflight > m->st.peak_inflight) {
		m->st.peak_inflight = inflight;
	}
	if (++r->in_use > r->st.peak_slots) {
		r->st.peak_slots = r->in_use;
	}
}

#ifdef IAU_ENG6_SPILL
/* A19 / P4.10b: the silicon's slot-line spill, as core-side traffic. One 64 B store of the slot (48 B record + 16 B
 * zero) into slot idx's line of the route's ctx block, then (member_cq) one 64 B load of it. idx < the slots
 * iau_eng6_member_set_ctx() armed (credits under the target's policy, P4.10e) <= the block's slot lines. */
static inline void
spill_store(struct iau_eng6_member *m, uint32_t idx, const struct iau_eng6_slot *s)
{
	volatile uint64_t *line;
	uint64_t w[8];
	unsigned i;

	if (m->ctx == NULL) {
		return;
	}
	line = (volatile uint64_t *)(m->ctx + (size_t)idx * IAU_ENG6_SPILL_LINE);
	memset(w, 0, sizeof(w));
	memcpy(w, s, sizeof(*s));
	for (i = 0; i < 8; i++) {
		line[i] = w[i];
	}
	m->spill_stores++;
}

static inline void
spill_load(struct iau_eng6_member *m, uint32_t idx)
{
	const volatile uint64_t *line;
	uint64_t x = 0;
	unsigned i;

	if (m->ctx == NULL) {
		return;
	}
	line = (const volatile uint64_t *)(m->ctx + (size_t)idx * IAU_ENG6_SPILL_LINE);
	for (i = 0; i < 8; i++) {
		x ^= line[i];
	}
	m->spill_sink ^= x;
	m->spill_loads++;
}
#endif

static int
do_io(struct iau_eng6_ring *r, struct iau_eng6_member *m, const uint64_t d[8], const volatile uint64_t *raw,
      uint8_t kind, uint32_t now32)
{
	struct spdk_nvme_cmd cmd;
	struct iau_eng6_slot *s;
	uint32_t idx;
	uint16_t tag;

	if (m->state != IAU_ENG6_M_ACTIVE) {
		/* FLUSHED / STOPPED: no new SSD command, one immediate final record */
		if (kind == IAU_TILE_K_IO) {
			m->st.io_fetched++;
		} else {
			m->st.raw_fetched++;
		}
		eng6_emit(r, d[5], d[6], iau_tile_w2(0, IAU_TILE_R_ABORTED, 0, 0), m->route, IAU_TILE_TAG_NONE, ST_SQDEL);
		m->st.rec_sqdel++;
		return ENG6_NEXT;
	}
	if (kind == IAU_TILE_K_RAW && raw == NULL) {	/* RAW at stride 48 (D5): refused, one slot */
		m->st.raw_fetched++;
		eng6_emit(r, d[5], d[6], iau_tile_w2(0, IAU_TILE_R_CPL, 0, 0), m->route, IAU_TILE_TAG_NONE, ST_INVAL);
		m->st.rec_inval++;
		return ENG6_NEXT;
	}
	if (sq_full(m)) {
		return ENG6_STALL;
	}
	if (spdk_unlikely(m->n_free <= m->entries - m->credits)) {
		m->st.credit_stalls++;	/* slots in use == credits: the host over-submitted (a tripwire) */
		return ENG6_STALL;
	}
	idx = m->free_ring[m->free_head];
	s = &m->slots[idx];
	tag = slot_tag(s, idx);
	if (kind == IAU_TILE_K_IO) {
		uint64_t *list = &m->prp[(size_t)idx * IAU_TILE_PRP_MAX];

		m->st.io_fetched++;
		if (iau_tile_sqe_from_io(d, tag, m->nsid, m->lba_shift, list, (uint64_t)(uintptr_t)list, &cmd) != 0) {
			eng6_emit(r, d[5], d[6], iau_tile_w2(0, IAU_TILE_R_CPL, 0, 0), m->route, IAU_TILE_TAG_NONE, ST_INVAL);
			m->st.rec_inval++;
			return ENG6_NEXT;
		}
		s->opc = iau_tile_d0_opc(d[0]);
		s->flags = (uint8_t)(iau_tile_d0_flags(d[0]) & IAU_TILE_F_REPLAY_MASK);
		s->nlb = iau_tile_d0_nlb(d[0]);
		s->slba = d[1];
		s->buf = d[2];
	} else {
		uint64_t w[8];
		unsigned i;

		m->st.raw_fetched++;
		for (i = 0; i < 8; i++) {
			w[i] = raw[i];
		}
		memcpy(&cmd, w, sizeof(cmd));
		cmd.cid = tag;
		s->opc = cmd.opc;
		s->flags = (uint8_t)(IAU_ENG6_F_RAW | cmd.fuse);
		s->nlb = 0;
		s->slba = 0;
		s->buf = 0;
	}
	note_alloc(r, m);
	s->cb_fn = d[5];
	s->cb_arg = d[6];
	s->ts32 = now32;
	s->state = IAU_ENG6_S_BUSY;
	s->retries = 0;
	s->bits = ((iau_tile_d0_cflags(d[0]) & IAU_TILE_CF_TMO_MASK) == IAU_TILE_CF_TMO && m->timeout_us != 0) ?
		  IAU_ENG6_B_TMO : 0;
#ifdef IAU_ENG6_SPILL
	spill_store(m, idx, s);
#endif
	write_sqe(r, m, &cmd);
	return ENG6_NEXT;
}

/* REISSUE / RELEASE of a parked slot (D8; tag gen checked) */
static int
do_dispose(struct iau_eng6_ring *r, struct iau_eng6_member *m, uint8_t kind, uint16_t tag)
{
	uint32_t idx = iau_tile_tag_slot(tag);
	struct iau_eng6_slot *s;
	struct spdk_nvme_cmd cmd;

	if (idx >= m->entries || m->slots[idx].state != IAU_ENG6_S_PARKED || m->slots[idx].gen != iau_tile_tag_gen(tag)) {
		m->st.bad_ctrl++;
		return ENG6_NEXT;
	}
	s = &m->slots[idx];
	if (kind == IAU_TILE_K_RELEASE) {
		slot_free(r, m, idx);
		m->st.releases++;
		return ENG6_NEXT;
	}
	if ((s->flags & IAU_ENG6_F_RAW) != 0 || m->state != IAU_ENG6_M_ACTIVE) {
		/* a RAW is never replayed (the host retries it itself); a route that issues nothing ends it */
		if (s->flags & IAU_ENG6_F_RAW) {
			m->st.bad_ctrl++;
		}
		eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2(0, IAU_TILE_R_ABORTED, s->retries, 0), m->route, tag, ST_SQDEL);
		m->st.rec_sqdel++;
		slot_free(r, m, idx);
		return ENG6_NEXT;
	}
	if (sq_full(m)) {
		return ENG6_STALL;
	}
	if (spdk_unlikely(slot_replay(m, idx, &cmd) != 0)) {	/* cannot happen: the same fields built it before */
		eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2(0, IAU_TILE_R_CPL, s->retries, 0), m->route, tag, ST_INVAL);
		m->st.rec_inval++;
		slot_free(r, m, idx);
		return ENG6_NEXT;
	}
	s->state = IAU_ENG6_S_BUSY;	/* ts32 and notified are kept: one TIMEOUT per command lifetime (D9) */
	m->n_parked--;
	if (s->retries != UINT8_MAX) {
		s->retries++;
	}
	m->st.reissues++;
	write_sqe(r, m, &cmd);
	return ENG6_NEXT;
}

/* late abort (C1/DV5): mark BUSY slots, ack now; the CQE of a marked slot becomes ABORTED_BY_REQUEST */
static void
do_abort(struct iau_eng6_ring *r, struct iau_eng6_member *m, uint8_t kind, const uint64_t d[8])
{
	uint32_t marked = 0, idx;

	m->st.aborts++;
	if (m->state == IAU_ENG6_M_ACTIVE) {
		if (kind == IAU_TILE_K_ABORT_COOKIE) {
			for (idx = 0; idx < m->entries; idx++) {
				struct iau_eng6_slot *s = &m->slots[idx];

				if (s->state == IAU_ENG6_S_BUSY && (s->bits & IAU_ENG6_B_ABORT) == 0 && s->cb_arg == d[2]) {
					s->bits |= IAU_ENG6_B_ABORT;
					marked++;
				}
			}
		} else {
			uint16_t tag = (uint16_t)d[2];

			idx = iau_tile_tag_slot(tag);
			if (idx < m->entries && m->slots[idx].state == IAU_ENG6_S_BUSY &&
			    m->slots[idx].gen == iau_tile_tag_gen(tag) && (m->slots[idx].bits & IAU_ENG6_B_ABORT) == 0) {
				m->slots[idx].bits |= IAU_ENG6_B_ABORT;
				marked++;
			}
		}
	}
	m->st.marked += marked;
	eng6_emit(r, d[5], d[6], iau_tile_w2(marked, IAU_TILE_R_CPL, 0, IAU_TILE_RF_ACK), m->route, IAU_TILE_TAG_NONE, 0);
	m->st.rec_ack++;
}

/* FLUSH (D7): at most one per route; parked slots disposed; no new IO from now on */
static void
do_flush(struct iau_eng6_ring *r, struct iau_eng6_member *m)
{
	uint32_t idx;

	if (++m->st.flushes > 1) {
		m->st.bad_ctrl++;
		return;
	}
	for (idx = 0; idx < m->entries && m->n_parked != 0; idx++) {
		struct iau_eng6_slot *s = &m->slots[idx];

		if (s->state == IAU_ENG6_S_PARKED) {
			eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2(0, IAU_TILE_R_ABORTED, s->retries, 0), m->route,
				  slot_tag(s, idx), ST_SQDEL);
			m->st.rec_sqdel++;
			slot_free(r, m, idx);
		}
	}
	m->state = IAU_ENG6_M_FLUSHED;
}

static int
do_desc(struct iau_eng6_ring *r, const uint64_t d[8], const volatile uint64_t *raw, uint32_t now32)
{
	uint8_t kind = iau_tile_d0_kind(d[0]);
	uint16_t route = iau_tile_d0_route(d[0]);
	struct iau_eng6_member *m = route < IAU_TILE_MAX_ROUTES ? r->route[route] : NULL;

	if (spdk_unlikely(kind > IAU_TILE_K_FLUSH)) {
		r->st.bad_kind++;
		return ENG6_NEXT;
	}
	if (spdk_unlikely(m == NULL || m->state == IAU_ENG6_M_GONE || iau_tile_d0_gen(d[0]) != m->gen)) {
		/* no live member, or a descriptor of another incarnation of the route (H3): still exactly one final
		 * record per credit-charged descriptor (C2), and never an SSD command */
		bool stale = m != NULL && m->state != IAU_ENG6_M_GONE;

		switch (kind) {
		case IAU_TILE_K_IO:
		case IAU_TILE_K_RAW:
			eng6_emit(r, d[5], d[6], iau_tile_w2(0, IAU_TILE_R_ABORTED, 0, 0), route, IAU_TILE_TAG_NONE, ST_SQDEL);
			if (!stale) {
				r->st.orphan_io++;
			}
			break;
		case IAU_TILE_K_ABORT_COOKIE:
		case IAU_TILE_K_ABORT_TAG:
			eng6_emit(r, d[5], d[6], iau_tile_w2(0, IAU_TILE_R_CPL, 0, IAU_TILE_RF_ACK), route, IAU_TILE_TAG_NONE, 0);
			if (!stale) {
				r->st.orphan_ack++;
			}
			break;
		default:
			if (!stale) {
				r->st.bad_route++;
			}
			break;
		}
		if (stale) {
			r->st.stale_gen++;
		}
		return ENG6_NEXT;
	}
	switch (kind) {
	case IAU_TILE_K_IO:
	case IAU_TILE_K_RAW:
		return do_io(r, m, d, raw, kind, now32);
	case IAU_TILE_K_REISSUE:
	case IAU_TILE_K_RELEASE:
		return do_dispose(r, m, kind, (uint16_t)d[2]);
	case IAU_TILE_K_ABORT_COOKIE:
	case IAU_TILE_K_ABORT_TAG:
		do_abort(r, m, kind, d);
		return ENG6_NEXT;
	case IAU_TILE_K_FLUSH:
		do_flush(r, m);
		return ENG6_NEXT;
	default:			/* unreachable: kinds above FLUSH were counted above */
		r->st.bad_kind++;
		return ENG6_NEXT;
	}
}

static void
ring_doorbells(struct iau_eng6_ring *r)
{
	uint32_t i;

	for (i = 0; i < r->n_dirty; i++) {
		struct iau_eng6_member *m = r->dirty[i];
		uint32_t t = m->fuse_open ? m->fuse_tail : m->sq_tail;

		m->sq_dirty = 0;
		if (m->fuse_open) {
			m->st.fuse_waits++;
		}
		if (t != m->sq_db_last) {
			eng6_pre_sq_db();	/* the SQEs before the doorbell */
			__atomic_store_n(m->sq_db, t, __ATOMIC_RELEASE);
			m->sq_db_last = t;
			m->st.ssd_db_writes++;
		}
	}
	r->n_dirty = 0;
}

/* the descriptors from d_idx up to tail (a doorbell value read with acquire semantics), at most budget of them */
static uint32_t
fetch_to(struct iau_eng6_ring *r, uint32_t now32, uint32_t tail, uint32_t budget)
{
	uint32_t n = 0;

	if (tail == r->d_idx) {
		return 0;
	}
	if (spdk_unlikely(tail >= r->d_entries)) {
		r->st.bad_db++;
		return 0;
	}
	r->d_pub = r->d_cur = r->d_seq;
	while (r->d_idx != tail && n < budget) {
		const volatile uint64_t *slot = (const volatile uint64_t *)(r->d + (size_t)r->d_idx * r->stride);
		uint32_t nx = r->d_idx + 1 == r->d_entries ? 0 : r->d_idx + 1, adv = 1;
		const volatile uint64_t *raw = NULL;
		uint64_t d[8];
		unsigned i;

		for (i = 0; i < 8; i++) {
			d[i] = r->woff[i] < 0 ? 0 : slot[r->woff[i]];
		}
		if (iau_tile_d0_kind(d[0]) == IAU_TILE_K_RAW && r->stride == IAU_TILE_DESC_BYTES) {
			if (nx == tail) {
				r->st.raw_waits++;	/* the SQE slot is not published yet */
				break;
			}
			raw = (const volatile uint64_t *)(r->d + (size_t)nx * r->stride);
			adv = 2;
		}
		r->d_cur = r->d_seq + adv;
		if (do_desc(r, d, raw, now32) != ENG6_NEXT) {
			r->d_cur = r->d_seq;
			break;
		}
		r->d_idx = adv == 2 ? (nx + 1 == r->d_entries ? 0 : nx + 1) : nx;
		r->d_seq += adv;
		r->st.d_slots += adv;
		r->st.desc_fetched++;
		n++;
	}
	if (r->d_pub != r->d_seq) {
		__atomic_store_n(r->d_head_out, r->d_seq, __ATOMIC_RELEASE);	/* GCB d_head after the batch (A15) */
	}
	r->d_pub = r->d_cur = r->d_seq;
	ring_doorbells(r);
	return n;
}

static uint32_t
fetch(struct iau_eng6_ring *r, uint32_t now32)
{
	/* acquire: no descriptor word is read before the doorbell value that covers it (the host stores its
	 * descriptors before d_tail with spdk_wmb or a release store) */
	return fetch_to(r, now32, __atomic_load_n(r->db, __ATOMIC_ACQUIRE), IAU_ENG6_POLL_BUDGET);
}

/* the echo slab (D8): a parked IO-kind slot's SQE rebuilt from its replay fields, before its PARKED record */
static void
echo_write(struct iau_eng6_member *m, uint32_t idx)
{
	volatile uint64_t *e = (volatile uint64_t *)&m->echo[idx];
	struct spdk_nvme_cmd c;
	uint64_t w[8];
	unsigned i;

	if (slot_replay(m, idx, &c) != 0) {
		memset(&c, 0, sizeof(c));
	}
	memcpy(w, &c, sizeof(w));
	for (i = 0; i < 8; i++) {
		e[i] = w[i];
	}
}

static uint32_t
member_cq(struct iau_eng6_ring *r, struct iau_eng6_member *m, uint32_t budget)
{
	uint32_t n = 0;

	while (n < budget) {
		const volatile uint64_t *cqe = (const volatile uint64_t *)&m->cq[m->cq_head];
		uint64_t hi = __atomic_load_n(&cqe[1], __ATOMIC_ACQUIRE), lo;
		struct iau_eng6_slot *s;
		uint32_t idx, sqhd;
		uint16_t tag, st;

		if ((uint8_t)((hi >> 48) & 1u) != m->cq_phase) {
			break;
		}
		lo = cqe[0];
		if (++m->cq_head == m->cq_entries) {
			m->cq_head = 0;
			m->cq_phase ^= 1;
		}
		m->st.cqes_consumed++;
		n++;
		sqhd = (uint32_t)(hi & 0xFFFFu);
		if (spdk_likely(sqhd < m->sq_entries)) {
			m->sq_head_seen = sqhd;
		} else {
			m->st.bad_sqhd++;	/* keeps the SQ-full guard on the last valid SQHD */
		}
		tag = (uint16_t)(hi >> 32);
		idx = iau_tile_tag_slot(tag);
		if (spdk_unlikely(idx >= m->entries || m->slots[idx].state != IAU_ENG6_S_BUSY ||
				  m->slots[idx].gen != iau_tile_tag_gen(tag))) {
			m->st.bad_cid++;	/* no command in flight with this tag: stray, duplicate or stale */
			continue;
		}
		s = &m->slots[idx];
#ifdef IAU_ENG6_SPILL
		spill_load(m, idx);	/* before the record: the slot line comes back from the ctx block */
#endif
		st = (uint16_t)((hi >> 48) & 0xFFFEu);
		if (spdk_unlikely(s->bits & IAU_ENG6_B_ABORT)) {
			eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2(0, IAU_TILE_R_ABORTED, s->retries, 0), m->route, tag, ST_ABRQ);
			m->st.rec_abort_req++;
			slot_free(r, m, idx);
			continue;
		}
		if (spdk_unlikely(m->fault.err_every != 0) && s->retries == 0 && (st & ENG6_SC_SCT) == 0 &&
		    ++m->first_cpls % m->fault.err_every == 0) {
			st = m->fault.err_status;
			m->st.injected++;
		}
		if (spdk_unlikely((st & ENG6_SC_SCT) != 0)) {
			/* parked for a disposition only on an ACTIVE route and never for a fused command (DV21: a
			 * re-issued FIRST alone would break the pair; the error is delivered) */
			if (m->state == IAU_ENG6_M_ACTIVE && (s->flags & IAU_TILE_F_FUSE_MASK) == 0) {
				if ((s->flags & IAU_ENG6_F_RAW) == 0) {
					echo_write(m, idx);
				}
				eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2((uint32_t)lo, IAU_TILE_R_CPL, s->retries, IAU_TILE_RF_PARKED),
					  m->route, tag, st);
				m->st.rec_err_parked++;
				s->state = IAU_ENG6_S_PARKED;
				m->n_parked++;
				continue;
			}
			eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2((uint32_t)lo, IAU_TILE_R_CPL, s->retries, 0), m->route, tag, st);
			m->st.rec_err_final++;
			slot_free(r, m, idx);
			continue;
		}
		eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2((uint32_t)lo, IAU_TILE_R_CPL, s->retries, 0), m->route, tag, st);
		m->st.rec_cpl++;
		slot_free(r, m, idx);
	}
	if (n != 0) {
		eng6_pre_cq_db();	/* the CQE loads before the doorbell that lets the SSD reuse them */
		__atomic_store_n(m->cq_db, m->cq_head, __ATOMIC_RELEASE);
		m->st.cq_db_writes++;
	}
	return n;
}

static uint32_t
timer(struct iau_eng6_ring *r, uint64_t now_us)
{
	uint32_t i, idx, n = 0, now32 = (uint32_t)now_us;

	if (now_us < r->tmo_next) {
		return 0;
	}
	r->tmo_next = now_us + IAU_ENG6_TMO_SCAN_US;
	for (i = 0; i < r->n; i++) {
		struct iau_eng6_member *m = r->list[i];

		if (m->timeout_us == 0 || m->state == IAU_ENG6_M_GONE || m->entries - m->n_free == m->n_parked) {
			continue;
		}
		for (idx = 0; idx < m->entries; idx++) {
			struct iau_eng6_slot *s = &m->slots[idx];

			if (s->state == IAU_ENG6_S_BUSY && (s->bits & (IAU_ENG6_B_TMO | IAU_ENG6_B_NOTIFIED)) == IAU_ENG6_B_TMO &&
			    (uint32_t)(now32 - s->ts32) >= m->timeout_us) {
				eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2(0, IAU_TILE_R_TIMEOUT, s->retries, 0), m->route,
					  slot_tag(s, idx), IAU_TILE_MARK_STATUS);
				s->bits |= IAU_ENG6_B_NOTIFIED;
				m->st.rec_timeout++;
				n++;
			}
		}
	}
	return n;
}

uint32_t
iau_eng6_ring_poll(struct iau_eng6_ring *r, uint64_t now_us)
{
	uint32_t i, work = 0;

	for (i = 0; i < r->n; i++) {
		if (r->list[i]->state != IAU_ENG6_M_GONE) {
			work += member_cq(r, r->list[i], IAU_ENG6_POLL_BUDGET);
		}
	}
	work += fetch(r, (uint32_t)now_us);
	work += timer(r, now_us);
	return work;
}

void
iau_eng6_member_stop(struct iau_eng6_ring *r, struct iau_eng6_member *m)
{
	(void)r;
	if (m->state == IAU_ENG6_M_ACTIVE) {
		m->state = IAU_ENG6_M_STOPPED;
	}
}

uint32_t
iau_eng6_member_foreign_db(const struct iau_eng6_member *m)
{
	if (m->sq_db == NULL) {
		return m->foreign_db_latched;
	}
	return (*m->host_sink != 0) || (*m->sq_db != m->sq_db_last);
}

int
iau_eng6_member_detach(struct iau_eng6_ring *r, struct iau_eng6_member *m, bool emit)
{
	uint64_t rec0 = r->st.records_emitted;
	uint32_t idx;
	bool last;

	if (m->state == IAU_ENG6_M_GONE) {
		return 0;
	}
	for (idx = 0; idx < r->n && r->list[idx] != m; idx++) {
	}
	if (idx == r->n) {
		return -EINVAL;		/* not this ring's member: nothing done */
	}
	last = emit && r->n == 1;	/* the ring's last member (a private ring's only one): the final fetch below */
	/* m3: without emit the doorbell page and the CQ sink may be gone too: not read (host_ssd_db reads 0) */
	m->foreign_db_latched = emit ? (uint8_t)iau_eng6_member_foreign_db(m) : 0;
	if (m->state == IAU_ENG6_M_ACTIVE) {
		m->state = IAU_ENG6_M_STOPPED;	/* drained errors are final, never parked */
	}
	if (emit) {
		member_cq(r, m, m->cq_entries);	/* the SSD stopped: at most one lap of posted CQEs */
	}
	for (idx = 0; idx < m->entries; idx++) {
		struct iau_eng6_slot *s = &m->slots[idx];

		if (s->state == IAU_ENG6_S_BUSY) {
			if (emit) {
				eng6_emit(r, s->cb_fn, s->cb_arg, iau_tile_w2(0, IAU_TILE_R_ABORTED, s->retries, 0), m->route,
					  slot_tag(s, idx), ST_SQDEL);
				m->st.rec_sqdel++;
			}
			m->st.dropped_busy++;
			slot_free(r, m, idx);
		} else if (s->state == IAU_ENG6_S_PARKED) {
			m->st.parked_dropped++;	/* the host holds its PARKED record (its pending list covers RETRYs) */
			slot_free(r, m, idx);
		}
	}
	if (emit && !last) {
		/* a shared ring: the ring keeps polling, so a descriptor of this route fetched later still gets its one
		 * (orphan) record, after R_DETACHED (the host's queue_depth == 0 drain covers it, A15) */
		eng6_emit(r, 0, 0, iau_tile_w2(0, IAU_TILE_R_DETACHED, 0, 0), m->route, IAU_TILE_TAG_NONE, IAU_TILE_MARK_STATUS);
		m->st.rec_detached++;
	}
	m->state = IAU_ENG6_M_GONE;
	m->fuse_open = 0;
	m->sq = NULL;
	m->cq = NULL;
	m->sq_db = NULL;
	m->cq_db = NULL;
	m->host_sink = NULL;
	m->echo = NULL;
	/* the member leaves the ring; its route's need stays reserved until the host deletes the CQ (I2) */
	for (idx = 0; idx < r->n; idx++) {
		if (r->list[idx] == m) {
			r->list[idx] = r->list[--r->n];
			r->list[r->n] = NULL;
			if (r->route[m->route] == m) {
				r->route[m->route] = NULL;
				if (r->resv[m->route] == 0) {
					r->n_resv++;
				}
				r->resv[m->route] = m->credits;
				r->resv_gen[m->route] = m->gen;
			}
			break;
		}
	}
	if (last) {
		/* The private-ring rule (P4.5 fix A): the ring's last member is out (above), so read d_tail ONCE and
		 * fetch D up to it, however many descriptors that is (not one poll budget): each gets the memberless
		 * rule's one final record (IO / RAW: ABORTED_SQ_DELETION, ABORT_*: ack 0) and no SSD command; REISSUE /
		 * RELEASE / FLUSH change nothing (their parked slots were dropped above, the host's pending list covers
		 * the RETRYs). fetch_to() stores the GCB d_head at its end. Then R_DETACHED, the ring's LAST record:
		 * the caller unbinds the ring in the same engine message (vfio_user.c iau_engine6_member_last_msg(),
		 * iau_tile_eng_member_last_out()), so no poll can fetch a descriptor written after this d_tail read; the host
		 * ends those itself when it reads this R_DETACHED, which alone carries rflags CLOSED (nvme_iau.c
		 * detached(); without CLOSED the host waits for records instead).
		 * R room (A1): per credit at most 3 unconsumed records (a TIMEOUT notice, a PARKED record, and one final
		 * record: the BUSY sweep's, a CQE's, or an orphan of the credit's unfetched IO / RAW, never two), per
		 * route the CTRL acks (an unfetched ABORT_* gets its orphan ack instead) and one R_DETACHED: route_need_r
		 * = 3c + 1 + CTRL, the bound the ring was registered under. */
		uint32_t tail = __atomic_load_n(r->db, __ATOMIC_ACQUIRE);

		(void)fetch_to(r, 0, tail, UINT32_MAX);
		eng6_emit(r, 0, 0, iau_tile_w2(0, IAU_TILE_R_DETACHED, 0, IAU_TILE_RF_CLOSED), m->route, IAU_TILE_TAG_NONE,
			  IAU_TILE_MARK_STATUS);
		m->st.rec_detached++;
	}
	return (int)(r->st.records_emitted - rec0);
}

int
iau_eng6_route_release(struct iau_eng6_ring *r, uint16_t route, uint8_t gen)
{
	if (route >= IAU_TILE_MAX_ROUTES) {
		return -EINVAL;
	}
	if (r->route[route] != NULL) {
		return -EBUSY;
	}
	if (r->resv[route] == 0) {
		return -ENOENT;
	}
	if (r->resv_gen[route] != gen) {
		return -ESTALE;		/* a late release of an older incarnation (H1): the newer reservation stays */
	}
	r->need_d -= iau_tile_route_need_d(r->resv[route]);
	r->need_r -= iau_tile_route_need_r(r->resv[route]);
	r->resv[route] = 0;
	r->n_resv--;
	return 0;
}

/* snprintf-style appends: the total length, the buffer always NUL-terminated (if len != 0) */
struct eng6_fmt {
	char *b;
	size_t n, pos;
};

static void __attribute__((format(printf, 2, 3)))
fmt_add(struct eng6_fmt *f, const char *fmt, ...)
{
	va_list ap;
	int k;

	va_start(ap, fmt);
	k = vsnprintf(f->pos < f->n ? f->b + f->pos : NULL, f->pos < f->n ? f->n - f->pos : 0, fmt, ap);
	va_end(ap);
	if (k > 0) {
		f->pos += (size_t)k;
	}
}

static const char *
state_name(uint8_t st)
{
	switch (st) {
	case IAU_ENG6_M_ACTIVE:
		return "active";
	case IAU_ENG6_M_FLUSHED:
		return "flushed";
	case IAU_ENG6_M_STOPPED:
		return "stopped";
	default:
		return "gone";
	}
}

int
iau_eng6_fmt_ring(char *buf, size_t len, const struct iau_eng6_ring *r, uint32_t engine, uint32_t lcore)
{
	return snprintf(buf, len, "IAU_ENGINE6_RING gid=%" PRIu32 " ring=%u nk=%u engine=%" PRIu32 " lcore=%" PRIu32
			" d_entries=%" PRIu32 " r_entries=%" PRIu32 " stride=%" PRIu32,
			r->gid, (unsigned)r->k, (unsigned)r->nk, engine, lcore, r->d_entries, r->r_entries, r->stride);
}

int
iau_eng6_fmt_bind(char *buf, size_t len, const struct iau_eng6_ring *r, const struct iau_eng6_member *m,
		  uint32_t engine, uint32_t lcore)
{
	return snprintf(buf, len, "IAU_ENGINE6_BIND ctrlr=%d qid=%u gid=%" PRIu32 " route=%u ring=%u gen=%u engine=%" PRIu32
			" lcore=%" PRIu32 " nsid=%" PRIu32 " credits=%" PRIu32 " entries=%" PRIu32 " timeout_us=%" PRIu32,
			m->ctrlr, (unsigned)m->qid, r->gid, (unsigned)m->route, (unsigned)r->k, (unsigned)m->gen, engine,
			lcore, m->nsid, m->credits, m->entries, m->timeout_us);
}

int
iau_eng6_fmt_stats(char *buf, size_t len, const struct iau_eng6_ring *r, const struct iau_eng6_member *m,
		   uint32_t engine)
{
	struct eng6_fmt f = { buf, len, 0 };
	unsigned i;

	if (len != 0) {
		buf[0] = '\0';
	}
	fmt_add(&f, "IAU_ENGINE6_STATS ctrlr=%d qid=%u gid=%" PRIu32 " route=%u ring=%u engine=%" PRIu32, m->ctrlr,
		(unsigned)m->qid, r->gid, (unsigned)m->route, (unsigned)r->k, engine);
	for (i = 0; i < IAU_ENG6_MEMBER_NSTATS; i++) {
		fmt_add(&f, " %s=%" PRIu64, iau_eng6_member_stat_keys[i], iau_eng6_member_stat(&m->st, i));
	}
	fmt_add(&f, " inflight=%" PRIu32 " parked=%" PRIu32 " state=%s host_ssd_db=%" PRIu32, iau_eng6_inflight(m),
		m->n_parked, state_name(m->state), iau_eng6_member_foreign_db(m));
	return (int)f.pos;
}

int
iau_eng6_fmt_ring_end(char *buf, size_t len, const struct iau_eng6_ring *r, uint32_t engine)
{
	struct eng6_fmt f = { buf, len, 0 };
	unsigned i;

	if (len != 0) {
		buf[0] = '\0';
	}
	fmt_add(&f, "IAU_ENGINE6_RING_END gid=%" PRIu32 " ring=%u engine=%" PRIu32, r->gid, (unsigned)r->k, engine);
	for (i = 0; i < IAU_ENG6_RING_NSTATS; i++) {
		fmt_add(&f, " %s=%" PRIu64, iau_eng6_ring_stat_keys[i], iau_eng6_ring_stat(&r->st, i));
	}
	fmt_add(&f, " members=%" PRIu32 " in_use=%" PRIu32 " reserved=%" PRIu32, r->n, r->in_use, r->n_resv);
	return (int)f.pos;
}

int
iau_eng6_fmt_spill(char *buf, size_t len, const struct iau_eng6_ring *r, const struct iau_eng6_member *m)
{
	if (m->ctx == NULL) {
		if (len != 0) {
			buf[0] = '\0';
		}
		return 0;
	}
	return snprintf(buf, len, "IAU_ENGINE6_CTX_SPILL_STATS ctrlr=%d qid=%u gid=%u route=%u ring=%u stores=%" PRIu64
			" loads=%" PRIu64, m->ctrlr, m->qid, r->gid, m->route, r->k, m->spill_stores, m->spill_loads);
}

int
iau_eng6_member_set_ctx(struct iau_eng6_member *m, volatile void *ctx, uint32_t ctx_bytes, uint32_t slots)
{
	/* P4.10e: the member's slot FIFO must be as member_init left it (full, head = tail = 0: no slot ever taken) */
	if (m == NULL || ctx == NULL || ((uintptr_t)ctx & 63u) != 0 || slots < m->credits || slots > m->entries ||
	    (uint64_t)ctx_bytes < (uint64_t)slots * IAU_ENG6_SPILL_LINE || m->n_free != m->entries || m->free_head != 0 ||
	    m->free_tail != 0) {
		return -EINVAL;
	}
	/* credits policy (slots < entries): only slots 0..slots-1 circulate, handed out 0, 1, 2, ... as the tile's
	 * extent of slots_r = credits (iau_mmio.v:443); the FIFO holds them at 0..slots-1 and wraps at entries as
	 * before. n_free keeps counting every FREE slot, so the credit check stalls at credits <= slots in use and the
	 * FIFO is never popped empty. */
	if (slots < m->entries) {
		m->free_tail = slots;
	}
	m->ctx = (volatile uint8_t *)ctx;
	return 0;
}
