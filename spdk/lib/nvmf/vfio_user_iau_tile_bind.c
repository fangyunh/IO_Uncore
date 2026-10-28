/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.4: pure-C glue between vfio_user.c and the IAU tile engine core (vfio_user_iau_tile_bind.h has the
 * rules). */
#include "spdk/stdinc.h"
#include "vfio_user_iau_tile_bind.h"
#include "vfio_user_text_pad.h"

VU_TEXT_PAD64();	/* this object's .text is a multiple of 64 B: the objects linked after it keep their phase */

static void __attribute__((format(printf, 3, 4)))
b6_err(char *err, size_t len, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || len == 0) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(err, len, fmt, ap);
	va_end(ap);
}

int
iau_tile_queue_decide(int mode, uint64_t qcfg, uint64_t qcfg2, struct iau_tile_qreg *q)
{
	int rc;

	memset(q, 0, sizeof(*q));
	rc = iau_qcfg_decide(mode, qcfg, &q->credits, &q->nsid);
	(void)iau_tile_qcfg2_unpack(qcfg2, &q->gcb_iova, &q->route, &q->gen);
	if (rc == IAU_QCFG_NONE) {
		/* modes 0 / 4: a tile registration has nobody to read its rings either */
		return qcfg2 == 0 ? IAU_TILE_Q_NONE : -EEXIST;
	}
	if (rc != IAU_QCFG_BIND) {
		return rc;	/* -ENOENT (mode 5, not registered), -EEXIST, -EINVAL (reserved QCFG bit) */
	}
	switch (iau_tile_qcfg2_unpack(qcfg2, &q->gcb_iova, &q->route, &q->gen)) {
	case -ENOENT:
		return IAU_TILE_Q_ENG40;
	case 0:
		return IAU_TILE_Q_TILE;
	default:
		return -EPROTO;
	}
}

int
iau_tile_gcb_check(const volatile struct iau_tile_gcb *g, const struct iau_tile_qreg *q, uint16_t qid, uint32_t sq_entries,
	       struct iau_tile_gcb_view *v, char *err, size_t errlen)
{
	struct iau_tile_gcb_member m;
	struct iau_tile_gcb_ring r;
	uint32_t magic, version, nk;

	memset(v, 0, sizeof(*v));
	/* one snapshot of every word the bind uses; nothing is re-read (V9, no TOCTOU on host memory) */
	magic = g->magic;
	version = g->version;
	nk = g->k;
	v->n_routes = g->n_routes;
	v->gid = g->gid;
	v->flags = g->flags;
	if (magic != IAU_TILE_GCB_MAGIC || version != IAU_TILE_GCB_VERSION) {
		b6_err(err, errlen, "GCB magic %#x version %u (want %#x version %u)", magic, version, IAU_TILE_GCB_MAGIC,
		       IAU_TILE_GCB_VERSION);
		return -EINVAL;
	}
	if (nk < 1 || nk > IAU_TILE_MAX_K || v->n_routes < 1 || v->n_routes > IAU_TILE_MAX_ROUTES ||
	    q->route >= v->n_routes) {
		b6_err(err, errlen, "GCB k=%u n_routes=%u route=%u (need 1 <= k <= %u, route < n_routes <= %u)", nk,
		       v->n_routes, q->route, IAU_TILE_MAX_K, IAU_TILE_MAX_ROUTES);
		return -EINVAL;
	}
	if (!iau_tile_gcb_flags_ok(v->flags)) {
		b6_err(err, errlen, "GCB flags %#x has unknown bits (known %#x)", v->flags, IAU_TILE_GCB_F_KNOWN);
		return -EINVAL;
	}
	v->nk = (uint16_t)nk;
	v->k = (uint16_t)(q->route % nk);
	memcpy(&m, (const void *)&g->member[q->route], sizeof(m));
	memcpy(&r, (const void *)&g->ring[v->k], sizeof(r));
	if (m.valid != IAU_TILE_MEMBER_VALID || m.ring != v->k || m.qid != qid) {
		b6_err(err, errlen, "GCB member[%u] valid=%u ring=%u qid=%u (want valid, ring %u, qid %u)", q->route,
		       m.valid, m.ring, m.qid, v->k, qid);
		return -EINVAL;
	}
	if (q->credits == 0 || m.credits != q->credits || m.nsid != q->nsid || m.gen != q->gen) {
		b6_err(err, errlen, "GCB member[%u] credits=%u nsid=%u gen=%u, but QCFG/QCFG2 say credits=%u nsid=%u gen=%u",
		       q->route, m.credits, m.nsid, m.gen, q->credits, q->nsid, q->gen);
		return -EINVAL;
	}
	if (m.entries != sq_entries || m.entries > IAU_TILE_MAX_SLOTS || m.echo_iova == 0 || (m.echo_iova & 63u) != 0 ||
	    m.lba_shift < 9 || m.lba_shift > 31) {
		b6_err(err, errlen, "GCB member[%u] entries=%u (SQ %u, max %u) echo=%#" PRIx64 " lba_shift=%u", q->route,
		       m.entries, sq_entries, IAU_TILE_MAX_SLOTS, m.echo_iova, m.lba_shift);
		return -EINVAL;
	}
	if (r.d_iova == 0 || r.r_iova == 0 || ((r.d_iova | r.r_iova) & 63u) != 0 || r.d_entries < IAU_ENG6_RING_MIN ||
	    r.d_entries > IAU_ENG6_RING_MAX || r.r_entries < IAU_ENG6_RING_MIN || r.r_entries > IAU_ENG6_RING_MAX ||
	    !iau_tile_stride_ok(r.d_stride)) {
		b6_err(err, errlen, "GCB ring[%u] d=%#" PRIx64 "x%u r=%#" PRIx64 "x%u stride=%u (64 B-aligned IOVAs, "
		       "%u..%u entries, stride 64|48)", v->k, r.d_iova, r.d_entries, r.r_iova, r.r_entries, r.d_stride,
		       IAU_ENG6_RING_MIN, IAU_ENG6_RING_MAX);
		return -EINVAL;
	}
	v->d_iova = r.d_iova;
	v->r_iova = r.r_iova;
	v->d_entries = r.d_entries;
	v->r_entries = r.r_entries;
	v->d_stride = r.d_stride;
	v->m_credits = m.credits;
	v->m_nsid = m.nsid;
	v->m_timeout_us = m.timeout_us;
	v->m_entries = m.entries;
	v->m_echo_iova = m.echo_iova;
	v->m_lba_shift = m.lba_shift;
	v->m_gen = m.gen;
	return 0;
}

int
iau_tile_gcb_fresh(const volatile struct iau_tile_gcb *g, const struct iau_tile_gcb_view *v, char *err, size_t errlen)
{
	uint64_t d_head, r_cons = 0;
	uint32_t d_tail;
	uint16_t k = v->k;

	if (k >= IAU_TILE_MAX_K) {
		b6_err(err, errlen, "ring %u out of range", k);
		return -EINVAL;
	}
	d_head = g->ring[k].d_head;
	d_tail = g->db[k].d_tail;
	if ((v->flags & IAU_TILE_GCB_F_RCONS) != 0) {
		r_cons = g->ring[k].r_consumed;
	}
	if (d_head != 0 || d_tail != 0 || r_cons != 0) {
		b6_err(err, errlen, "GCB ring[%u] is not fresh (d_head=%" PRIu64 " d_tail=%u r_consumed=%" PRIu64 "): a new "
		       "ring starts at the host's zeroed positions", k, d_head, d_tail, r_cons);
		return -EINVAL;
	}
	return 0;
}

bool
iau_tile_gcb_same_ring(const struct iau_tile_gcb_view *a, const struct iau_tile_gcb_view *b)
{
	return a->gid == b->gid && a->flags == b->flags && a->nk == b->nk && a->k == b->k && a->d_iova == b->d_iova &&
	       a->r_iova == b->r_iova && a->d_entries == b->d_entries && a->r_entries == b->r_entries &&
	       a->d_stride == b->d_stride;
}

void
iau_tile_ring_ptrs(const struct iau_tile_gcb_view *v, volatile struct iau_tile_gcb *g, const volatile uint32_t **db,
	       volatile uint64_t **d_head, const volatile uint64_t **r_consumed)
{
	*db = &g->db[v->k].d_tail;
	*d_head = &g->ring[v->k].d_head;
	*r_consumed = (v->flags & IAU_TILE_GCB_F_RCONS) != 0 ? &g->ring[v->k].r_consumed : NULL;
}

void
iau_tile_ring_cfg(const struct iau_tile_gcb_view *v, volatile struct iau_tile_gcb *g, const volatile void *d, volatile void *r,
	      struct iau_eng6_ring_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->gid = v->gid;
	c->k = v->k;
	c->nk = v->nk;
	c->d = d;
	c->d_entries = v->d_entries;
	c->d_stride = v->d_stride;
	c->r = r;
	c->r_entries = v->r_entries;
	c->d_seq0 = 0;
	c->r_seq0 = 0;
	c->gcb_flags = v->flags;
	iau_tile_ring_ptrs(v, g, &c->db, &c->d_head_out, &c->r_consumed);
}

void
iau_tile_member_cfg(const struct iau_tile_gcb_view *v, const struct iau_tile_qreg *q, uint16_t qid, int ctrlr,
		struct iau_eng6_member_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->route = q->route;
	c->qid = qid;
	c->ctrlr = ctrlr;
	c->gen = v->m_gen;
	c->lba_shift = v->m_lba_shift;
	c->entries = v->m_entries;
	c->credits = v->m_credits;
	c->nsid = v->m_nsid;
	c->timeout_us = v->m_timeout_us;
}

uint64_t
iau_tile_ctrlr_key(uint64_t *slot, uint64_t *next)
{
	if (*slot == 0) {
		*slot = ++*next;
	}
	return *slot;
}

void
iau_tile_ctrlr_key_reset(uint64_t *slot)
{
	*slot = 0;
}

void
iau_tile_note_bind(struct iau_tile_note *n, uint64_t ckey, uint16_t cqid, uint8_t gen)
{
	n->ckey = ckey;
	n->cqid = cqid;
	n->gen = gen;
	n->state = IAU_TILE_N_LIVE;
}

void
iau_tile_note_detach(struct iau_tile_note *n, bool ctrlr_going)
{
	if (n->state == IAU_TILE_N_LIVE) {
		n->state = IAU_TILE_N_RESV;
	}
	if (ctrlr_going) {
		n->ckey = 0;
	}
}

bool
iau_tile_note_cq_deleted(struct iau_tile_note *n, uint64_t ckey, uint16_t cqid, uint8_t *gen)
{
	if (n->state != IAU_TILE_N_RESV || n->ckey == 0 || n->ckey != ckey || n->cqid != cqid) {
		return false;
	}
	*gen = n->gen;
	memset(n, 0, sizeof(*n));
	return true;
}

int
iau_tile_src_plan(const struct iau_tile_rm *m, uint32_t n, uint32_t src, uint32_t *to)
{
	uint32_t i, leaver = UINT32_MAX;
	bool others = false;

	if (src < n && m[src].alive && !m[src].ring_in && !m[src].leaving) {
		return IAU_TILE_SRC_KEEP;
	}
	for (i = 0; i < n; i++) {
		if (i == src || !m[i].alive) {
			continue;
		}
		others = true;
		if (m[i].ring_in) {
			continue;
		}
		if (!m[i].leaving) {
			*to = i;
			return IAU_TILE_SRC_MOVE;
		}
		if (leaver == UINT32_MAX) {
			leaver = i;
		}
	}
	if (leaver != UINT32_MAX) {
		*to = leaver;
		return IAU_TILE_SRC_MOVE;
	}
	if (src < n && m[src].alive && !m[src].ring_in && !others) {
		return IAU_TILE_SRC_KEEP;	/* the last member: it detaches itself (emit), then the ring unbinds */
	}
	return IAU_TILE_SRC_DROP;
}

int
iau_tile_clock_init(struct iau_tile_clock *c, uint64_t hz)
{
	unsigned __int128 m;

	if (hz < 1000) {
		return -EINVAL;
	}
	m = ((unsigned __int128)1000000u << IAU_TILE_CLOCK_SHIFT) / hz;
	if (m > UINT64_MAX) {
		return -EINVAL;
	}
	c->mult = (uint64_t)m;
	return 0;
}

int
iau_tile_eng_add(struct iau_tile_eng_rings *e, struct iau_eng6_ring *r)
{
	uint32_t i;

	for (i = 0; i < e->n; i++) {
		if (e->r[i] == r) {
			return -EEXIST;
		}
	}
	if (e->n == IAU_TILE_ENG_MAX_RINGS) {
		return -ENOSPC;
	}
	e->r[e->n++] = r;
	return 0;
}

int
iau_tile_eng_del(struct iau_tile_eng_rings *e, struct iau_eng6_ring *r)
{
	uint32_t i;

	for (i = 0; i < e->n; i++) {
		if (e->r[i] == r) {
			e->r[i] = e->r[--e->n];
			e->r[e->n] = NULL;
			return 0;
		}
	}
	return -ENOENT;
}

int
iau_tile_eng_member_last_out(struct iau_tile_eng_rings *e, struct iau_eng6_ring *r, struct iau_eng6_member *m)
{
	int rc = iau_eng6_member_detach(r, m, true);

	if (rc < 0) {
		return rc;
	}
	if (r->n != 0) {
		return -EBUSY;	/* not the last member: a shared ring keeps polling (orphans follow R_DETACHED) */
	}
	(void)iau_tile_eng_del(e, r);	/* the same step: nothing polls this ring after its R_DETACHED */
	return rc;
}

uint32_t
iau_tile_eng_poll(struct iau_tile_eng_rings *e, uint64_t now_us)
{
	uint64_t in_use = 0;
	uint32_t i, work = 0;

	for (i = 0; i < e->n; i++) {
		work += iau_eng6_ring_poll(e->r[i], now_us);
		in_use += iau_eng6_ring_in_use(e->r[i]);
	}
	if (in_use > e->peak_slots) {
		e->peak_slots = in_use;
	}
	e->passes++;
	return work;
}

/* ---------- fault knobs ---------- */

/* strict unsigned parse: decimal digits only, or (hex_ok) 0x followed by hex digits; no sign, no blanks */
static int
b6_parse(const char *s, bool hex_ok, uint64_t max, uint64_t *out)
{
	unsigned long long x;
	const char *p = s;
	char *end = NULL;
	int base = 10;

	if (hex_ok && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
		base = 16;
		if (!isxdigit((unsigned char)p[0])) {
			return -EINVAL;
		}
	} else if (p[0] < '0' || p[0] > '9') {
		return -EINVAL;
	}
	errno = 0;
	x = strtoull(p, &end, base);
	if (errno != 0 || end == p || *end != '\0' || x > max) {
		return -EINVAL;
	}
	*out = (uint64_t)x;
	return 0;
}

static const struct b6_key {
	const char *key;
	uint64_t def, max;
} g_b6_keys[4] = {
	{ "EVERY", 0, IAU_TILE_FAULT_EVERY_MAX },
	{ "SC", 0x82, 255 },
	{ "SCT", 0, 7 },
	{ "DNR", 0, 1 },
};

/* the four values (prefix "IAU_ENG_FAULT_ERR_" or "IAU_DEV_FAULT_ERR_"), with the _C<k> override for idx >= 0 */
static int
b6_fault_load(struct iau_tile_fault *f, const char *prefix, int idx, bool hex_sc, iau_tile_getenv_fn get, char *err,
	      size_t errlen)
{
	uint64_t val[4];
	char name[64];
	unsigned i;

	memset(f, 0, sizeof(*f));
	for (i = 0; i < 4; i++) {
		const char *v = NULL;

		val[i] = g_b6_keys[i].def;
		if (idx >= 0) {
			snprintf(name, sizeof(name), "%s%s_C%d", prefix, g_b6_keys[i].key, idx);
			v = get(name);
		}
		if (v == NULL || v[0] == '\0') {
			snprintf(name, sizeof(name), "%s%s", prefix, g_b6_keys[i].key);
			v = get(name);
		}
		if (v == NULL || v[0] == '\0') {
			continue;
		}
		if (b6_parse(v, hex_sc && i == 1, g_b6_keys[i].max, &val[i]) != 0) {
			b6_err(err, errlen, "%s=%s is not %s 0..%" PRIu64, name, v,
			       hex_sc && i == 1 ? "a decimal or 0x-hex number in" : "a plain decimal number in",
			       g_b6_keys[i].max);
			return -EINVAL;
		}
	}
	f->every = (uint32_t)val[0];
	f->sc = (uint8_t)val[1];
	f->sct = (uint8_t)val[2];
	f->dnr = (uint8_t)val[3];
	if (f->every != 0 && f->sc == 0 && f->sct == 0) {
		if (idx >= 0) {
			b6_err(err, errlen, "%sEVERY=%u (controller %d) needs an error status: %sSC and %sSCT are both 0",
			       prefix, f->every, idx, prefix, prefix);
		} else {
			b6_err(err, errlen, "%sEVERY=%u needs an error status: %sSC and %sSCT are both 0", prefix, f->every,
			       prefix, prefix);
		}
		return -EINVAL;
	}
	f->status = f->every != 0 ? iau_tile_status(f->sct, f->sc, f->dnr != 0) : 0;
	return 0;
}

int
iau_tile_eng_fault_load(struct iau_tile_fault *f, iau_tile_getenv_fn get, char *err, size_t errlen)
{
	return b6_fault_load(f, "IAU_ENG_FAULT_ERR_", -1, true, get, err, errlen);
}

int
iau_tile_dev_fault_load(struct iau_tile_fault *f, int ctrlr_idx, iau_tile_getenv_fn get, char *err, size_t errlen)
{
	return b6_fault_load(f, "IAU_DEV_FAULT_ERR_", ctrlr_idx, false, get, err, errlen);
}

int
iau_tile_dev_fault_scan(char **envp, iau_tile_getenv_fn get, bool *any_on, char *err, size_t errlen)
{
	static const char pfx[] = "IAU_DEV_FAULT_ERR_";
	const size_t pl = sizeof(pfx) - 1;
	struct iau_tile_fault f;
	size_t i;

	*any_on = false;
	if (iau_tile_dev_fault_load(&f, -1, get, err, errlen) != 0) {
		return -EINVAL;
	}
	*any_on = f.every != 0;
	for (i = 0; envp != NULL && envp[i] != NULL; i++) {
		const char *e = envp[i], *eq = strchr(e, '='), *rest;
		size_t len, kl, d;
		unsigned k;
		long idx = -1;
		bool known = false;

		if (strncmp(e, pfx, pl) != 0) {
			continue;
		}
		if (eq == NULL) {
			b6_err(err, errlen, "%s has no value", e);
			return -EINVAL;
		}
		rest = e + pl;
		len = (size_t)(eq - rest);
		for (k = 0; k < 4 && !known; k++) {
			kl = strlen(g_b6_keys[k].key);
			if (len < kl || memcmp(rest, g_b6_keys[k].key, kl) != 0) {
				continue;
			}
			if (len == kl) {
				known = true;
				break;
			}
			/* _C<k>: 1..9 digits, no leading zero (the loader looks up "_C%d" exactly) */
			if (len - kl < 3 || len - kl > 11 || rest[kl] != '_' || rest[kl + 1] != 'C' ||
			    (rest[kl + 2] == '0' && len - kl > 3)) {
				continue;
			}
			for (d = kl + 2, idx = 0; d < len && rest[d] >= '0' && rest[d] <= '9'; d++) {
				idx = idx * 10 + (rest[d] - '0');
			}
			if (d == len) {
				known = true;
				break;
			}
			idx = -1;
		}
		if (!known) {
			b6_err(err, errlen, "%.*s is not an IAU_DEV_FAULT_ERR_{EVERY,SC,SCT,DNR}[_C<k>] setting",
			       (int)(eq - e), e);
			return -EINVAL;
		}
		{
			uint64_t x;

			if (eq[1] != '\0' && b6_parse(eq + 1, false, g_b6_keys[k].max, &x) != 0) {
				b6_err(err, errlen, "%s is not a plain decimal number in 0..%" PRIu64, e, g_b6_keys[k].max);
				return -EINVAL;
			}
		}
		if (idx >= 0) {
			if (iau_tile_dev_fault_load(&f, (int)idx, get, err, errlen) != 0) {
				return -EINVAL;
			}
			*any_on |= f.every != 0;
		}
	}
	return 0;
}

uint32_t
iau_tile_eng_poll_fn(struct iau_tile_eng_rings *e, uint64_t now_us, iau_tile_ring_poll_fn poll)
{
	uint64_t in_use = 0;
	uint32_t i, work = 0;

	for (i = 0; i < e->n; i++) {
		work += poll(e->r[i], now_us);
		in_use += iau_eng6_ring_in_use(e->r[i]);
	}
	if (in_use > e->peak_slots) {
		e->peak_slots = in_use;
	}
	e->passes++;
	return work;
}

/* ---------- ctx spill (P4.10b, A19) ---------- */

int
iau_tile_spill_load(bool *on, iau_tile_getenv_fn get, char *err, size_t errlen)
{
	const char *v = get("IAU_TILE_CTX_SPILL");

	*on = false;
	if (v == NULL || v[0] == '\0' || strcmp(v, "0") == 0) {
		return 0;
	}
	if (strcmp(v, "1") != 0) {
		b6_err(err, errlen, "IAU_TILE_CTX_SPILL=%s is not 0 or 1", v);
		return -EINVAL;
	}
	*on = true;
	return 0;
}

void
iau_tile_gcb_ctx(const volatile struct iau_tile_gcb *g, struct iau_tile_ctx_region *c)
{
	c->iova = g->ctx_iova;
	c->bytes = g->ctx_bytes;
}

int
iau_tile_ctx_region_check(const struct iau_tile_ctx_region *v, char *err, size_t errlen)
{
	/* iau_mmio.v:742: ctx_iova 0 or not 4 KiB aligned -> -EINVAL CTX; ctx_bytes is the sum check's (:743) */
	if (v->iova == 0 && v->bytes == 0) {
		b6_err(err, errlen, "the GCB publishes no ctx region (ctx_iova 0, ctx_bytes 0)");
		return -EINVAL;
	}
	if (v->iova == 0 || (v->iova & 4095u) != 0) {
		b6_err(err, errlen, "GCB ctx region %#" PRIx64 " + %u bytes: ctx_iova is 0 or not 4 KiB-aligned", v->iova,
		       v->bytes);
		return -EINVAL;
	}
	return 0;
}

void
iau_tile_ctx_map_init(struct iau_tile_ctx_map *c, uint32_t ctx_bytes)
{
	memset(c, 0, sizeof(*c));
	c->bytes = ctx_bytes;
}

int
iau_tile_ctx_free(struct iau_tile_ctx_map *c, uint16_t route)
{
	if (route >= IAU_TILE_MAX_ROUTES || c->blk[route].len == 0) {
		return -ENOENT;
	}
	c->blk[route].len = 0;
	return 0;
}

int
iau_tile_ctx_alloc(struct iau_tile_ctx_map *c, uint16_t route, uint32_t s, uint32_t *off, struct iau_tile_ctx_undo *u, int *reason)
{
	uint64_t need, cand = 0, held = 0, lim = c->bytes & ~(uint64_t)63u;	/* the RTL compares 64 B units, :743 */
	unsigned i;
	bool moved;

	if (route >= IAU_TILE_MAX_ROUTES || s == 0 || s > IAU_TILE_MAX_SLOTS) {
		return -EINVAL;
	}
	need = iau_tile_ctx_block_bytes(s);
	/* 1. the sum over the other held blocks (pass 0 does not sum the own entry, :726-730) */
	for (i = 0; i < IAU_TILE_MAX_ROUTES; i++) {
		if (i != route) {
			held += c->blk[i].len;
		}
	}
	if (held + need > lim) {
		if (reason != NULL) {
			*reason = IAU_TILE_CTX_R_SUM;
		}
		return -ENOSPC;
	}
	/* 2. a rejoin with s <= its held block keeps the offset; the block is then B(s) (:449, :744-745) */
	if (c->blk[route].len != 0 && c->blk[route].slots >= s) {
		cand = c->blk[route].off;
	} else {
		/* 3. first fit over the other held blocks (:751-774): the lowest offset not overlapping any (blocks are
		 * 256 B multiples, so cand stays aligned); the own old block is free for the new one */
		do {
			moved = false;
			for (i = 0; i < IAU_TILE_MAX_ROUTES; i++) {
				if (i != route && c->blk[i].len != 0 && cand < (uint64_t)c->blk[i].off + c->blk[i].len &&
				    (uint64_t)c->blk[i].off < cand + need) {
					cand = (uint64_t)c->blk[i].off + c->blk[i].len;
					moved = true;
				}
			}
		} while (moved);
		if (cand + need > lim) {
			if (reason != NULL) {
				*reason = IAU_TILE_CTX_R_FRAG;
			}
			return -ENOSPC;
		}
	}
	if (u != NULL) {
		u->route = route;
		u->off = c->blk[route].off;
		u->len = c->blk[route].len;
		u->slots = c->blk[route].slots;
		u->valid = true;
	}
	c->blk[route].off = (uint32_t)cand;
	c->blk[route].len = (uint32_t)need;
	c->blk[route].slots = (uint16_t)s;
	*off = (uint32_t)cand;
	return 0;
}

int
iau_tile_ctx_lines_check(const struct iau_tile_ctx_map *c, uint32_t off, uint32_t s)
{
	if (s == 0 || (off & 63u) != 0 || (uint64_t)off + iau_tile_ctx_lines_bytes(s) > c->bytes) {
		return -ERANGE;
	}
	return 0;
}

void
iau_tile_ctx_undo(struct iau_tile_ctx_map *c, const struct iau_tile_ctx_undo *u)
{
	if (u == NULL || !u->valid || u->route >= IAU_TILE_MAX_ROUTES) {
		return;
	}
	c->blk[u->route].off = u->off;
	c->blk[u->route].len = u->len;
	c->blk[u->route].slots = u->slots;
}

uint32_t
iau_tile_ctx_ring_free(struct iau_tile_ctx_map *c, uint16_t nk, uint16_t k)
{
	uint32_t i, n = 0;

	if (nk == 0) {
		return 0;
	}
	for (i = 0; i < IAU_TILE_MAX_ROUTES; i++) {
		if (i % nk == k && c->blk[i].len != 0) {
			c->blk[i].len = 0;
			n++;
		}
	}
	return n;
}

struct iau_tile_ctxgcb *
iau_tile_ctxgcb_get(struct iau_tile_ctxgcb_list *l, uint64_t gcb_iova, const struct iau_tile_ctx_region *cr, int *rc)
{
	struct iau_tile_ctxgcb *cg;

	TAILQ_FOREACH(cg, l, link) {
		if (cg->gcb_iova == gcb_iova) {
			if (cg->ctx_iova != cr->iova || cg->ctx_bytes != cr->bytes) {
				*rc = -EINVAL;	/* the GCB header must not change under its binds and rings */
				return NULL;
			}
			cg->refs++;
			return cg;
		}
	}
	cg = calloc(1, sizeof(*cg));
	if (cg == NULL) {
		*rc = -ENOMEM;
		return NULL;
	}
	cg->gcb_iova = gcb_iova;
	cg->ctx_iova = cr->iova;
	cg->ctx_bytes = cr->bytes;
	cg->refs = 1;
	iau_tile_ctx_map_init(&cg->map, cr->bytes);
	TAILQ_INSERT_TAIL(l, cg, link);
	return cg;
}

bool
iau_tile_ctxgcb_put(struct iau_tile_ctxgcb_list *l, struct iau_tile_ctxgcb *cg)
{
	if (cg == NULL) {
		return false;
	}
	assert(cg->refs > 0);
	if (--cg->refs != 0) {
		return false;
	}
	TAILQ_REMOVE(l, cg, link);
	free(cg);
	return true;
}
