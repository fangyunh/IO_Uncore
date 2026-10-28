/* SPDX-License-Identifier: BSD-3-Clause */
#include "vfio_user_devcfg.h"

/* The per-controller keys in check order; the process-wide IAU_DEV_* names are in g_global_keys.
 * devcfg_load() reads [0, 4) (its loop bound is the literal 4); [4, 8) are the mode-0 error knob's keys
 * (IAU-next P4.4, D19), which the typo guard (devcfg_known(), looping over the whole table) accepts with _C<k>
 * like the others and vfio_user_iau_tile_bind.c parses (their ranges are documented there; def/min/max here are
 * informational). No statement was added, but the longer table changes devcfg_scan_unknown()'s register
 * allocation: the object's .text is 0x978, 6 B less, inside the same 16 B slot (test_nvmf_hot_layout.sh). */
static const struct devcfg_key {
	const char *key;
	uint64_t def, min, max;
} g_keys[8] = {
	{ "FASTPATH", DEVCFG_DEFAULT_FASTPATH, 0, 1 },
	{ "LMIN_NS", DEVCFG_DEFAULT_LMIN_NS, 0, DEVCFG_MAX_LMIN_NS },
	{ "TMAX", DEVCFG_DEFAULT_TMAX, 1, DEVCFG_MAX_TMAX },
	{ "CHANNELS", DEVCFG_DEFAULT_CHANNELS, 1, DEVCFG_MAX_CHANNELS },
	{ "FAULT_ERR_EVERY", 0, 0, 1000000000ULL },
	{ "FAULT_ERR_SC", 0x82, 0, 255 },
	{ "FAULT_ERR_SCT", 0, 0, 7 },
	{ "FAULT_ERR_DNR", 0, 0, 1 },
};
/* MODEL is read by devcfg_load(), TRACE and TRACE_DIR by vfio_user_devtrace.c. */
static const char *const g_global_keys[3] = { "MODEL", "TRACE", "TRACE_DIR" };

int
devcfg_ctrlr_index(const char *traddr)
{
	size_t n, end, i;
	int v = 0;

	if (traddr == NULL) {
		return -1;
	}
	n = strlen(traddr);
	while (n > 0 && traddr[n - 1] == '/') {
		n--;
	}
	end = n;
	while (n > 0 && traddr[n - 1] >= '0' && traddr[n - 1] <= '9') {
		n--;
	}
	if (n == end || end - n > 9) {
		return -1;
	}
	for (i = n; i < end; i++) {
		v = v * 10 + (traddr[i] - '0');
	}
	return v;
}

/* Records the first error only. The variable name comes first, so a long value truncates after it;
 * a truncated message ends in "...". */
static void
devcfg_fail(struct devcfg *c, const char *name, const char *v, const char *why)
{
	int n;

	if (c->err[0] != '\0') {
		return;
	}
	n = snprintf(c->err, sizeof(c->err), "%s=%s %s", name, v, why);
	if (n >= (int)sizeof(c->err)) {
		memcpy(c->err + sizeof(c->err) - 4, "...", 4);
	}
}

/* One key: the per-controller override, else the global value, else the default. A value that is
 * not a plain decimal number or is outside [min, max] is an error and leaves the default in *out. */
static int
devcfg_u64(struct devcfg *c, devcfg_getenv_fn get, const struct devcfg_key *k, int idx, uint64_t *out,
	   bool *ovr)
{
	char name[64], why[64];
	const char *v = NULL;
	char *end = NULL;
	unsigned long long x;

	*ovr = false;
	if (idx >= 0) {
		snprintf(name, sizeof(name), "IAU_DEV_%s_C%d", k->key, idx);
		v = get(name);
		*ovr = v != NULL && v[0] != '\0';
	}
	if (!*ovr) {
		snprintf(name, sizeof(name), "IAU_DEV_%s", k->key);
		v = get(name);
	}
	*out = k->def;
	if (v == NULL || v[0] == '\0') {
		return 0;
	}
	errno = 0;
	x = strtoull(v, &end, 10);
	if (v[0] < '0' || v[0] > '9' || errno != 0 || end == v || *end != '\0') {
		devcfg_fail(c, name, v, "is not a plain decimal number");
		return -EINVAL;
	}
	if (x < k->min || x > k->max) {
		snprintf(why, sizeof(why), "is outside %" PRIu64 "..%" PRIu64, k->min, k->max);
		devcfg_fail(c, name, v, why);
		return -EINVAL;
	}
	*out = (uint64_t)x;
	return 0;
}

static void
devcfg_note(struct devcfg *c, const char *key)
{
	size_t n;

	if (strcmp(c->overrides, "-") == 0) {
		c->overrides[0] = '\0';
	}
	n = strlen(c->overrides);
	snprintf(c->overrides + n, sizeof(c->overrides) - n, "%s%s", n != 0 ? "," : "", key);
}

int
devcfg_load(struct devcfg *c, int ctrlr_idx, devcfg_getenv_fn get)
{
	uint64_t val[4];
	const char *model;
	bool ovr;
	int i, rc = 0;

	memset(c, 0, sizeof(*c));
	c->ctrlr_idx = ctrlr_idx;
	snprintf(c->overrides, sizeof(c->overrides), "-");
	for (i = 0; i < 4; i++) {
		if (devcfg_u64(c, get, &g_keys[i], ctrlr_idx, &val[i], &ovr) != 0) {
			rc = -EINVAL;
		}
		if (ovr) {
			devcfg_note(c, g_keys[i].key);
		}
	}
	c->fastpath = val[0] != 0;
	c->lmin_ns = val[1];
	c->tmax = val[2];
	c->channels = (uint32_t)val[3];
	model = get("IAU_DEV_MODEL");
	if (model != NULL && strcmp(model, "ctrlr") == 0) {
		c->model_ctrlr = true;
	} else if (model != NULL && model[0] != '\0' && strcmp(model, "sq") != 0) {
		devcfg_fail(c, "IAU_DEV_MODEL", model, "is not sq or ctrlr");
		rc = -EINVAL;
	}
	return rc;
}

int
devcfg_cpu_list(const char *s, uint32_t *out, int max)
{
	uint64_t seen[1024 / 64] = { 0 };
	int n = 0;

	if (s == NULL) {
		return -1;
	}
	while (*s != '\0') {
		char *end;
		unsigned long a, b;

		if (*s < '0' || *s > '9') {
			return -1;
		}
		a = strtoul(s, &end, 10);
		b = a;
		if (*end == '-') {
			s = end + 1;
			if (*s < '0' || *s > '9') {
				return -1;
			}
			b = strtoul(s, &end, 10);
			if (b < a) {
				return -1;
			}
		}
		if (b > 1023) {
			return -1;
		}
		for (; a <= b; a++) {
			if (n == max || ((seen[a / 64] >> (a % 64)) & 1) != 0) {
				return -1;
			}
			seen[a / 64] |= 1ULL << (a % 64);
			out[n++] = (uint32_t)a;
		}
		if (*end == ',' || *end == '+') {
			end++;
			if (*end == '\0') {
				return -1;
			}
		} else if (*end != '\0') {
			return -1;
		}
		s = end;
	}
	return n;
}

/* rest = name after "IAU_DEV_" (len bytes, not terminated). 1 if devcfg_load() or the device trace
 * reads it; *idx = k for a _C<k> name, else -1. */
static int
devcfg_known(const char *rest, size_t len, long *idx)
{
	size_t i, kl, d;

	*idx = -1;
	for (i = 0; i < sizeof(g_global_keys) / sizeof(g_global_keys[0]); i++) {
		if (strlen(g_global_keys[i]) == len && memcmp(rest, g_global_keys[i], len) == 0) {
			return 1;
		}
	}
	for (i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); i++) {
		kl = strlen(g_keys[i].key);
		if (len < kl || memcmp(rest, g_keys[i].key, kl) != 0) {
			continue;
		}
		if (len == kl) {
			return 1;
		}
		/* _C<k>: 1..9 digits, no leading zero (devcfg_load() looks up "_C%d" exactly) */
		if (len - kl < 3 || len - kl > 11 || rest[kl] != '_' || rest[kl + 1] != 'C' ||
		    (rest[kl + 2] == '0' && len - kl > 3)) {
			continue;
		}
		*idx = 0;
		for (d = kl + 2; d < len && rest[d] >= '0' && rest[d] <= '9'; d++) {
			*idx = *idx * 10 + (rest[d] - '0');
		}
		if (d == len) {
			return 1;
		}
		*idx = -1;
	}
	return 0;
}

int
devcfg_scan_unknown(char **envp, int n_ctrlr, char *err, size_t errlen)
{
	static const char pfx[] = "IAU_DEV_";
	const size_t pl = sizeof(pfx) - 1;
	size_t i, len;
	long idx;

	if (err != NULL && errlen > 0) {
		err[0] = '\0';
	}
	for (i = 0; envp != NULL && envp[i] != NULL; i++) {
		const char *e = envp[i];

		if (strncmp(e, pfx, pl) != 0) {
			continue;
		}
		len = strcspn(e, "=");
		if (!devcfg_known(e + pl, len - pl, &idx)) {
			if (err != NULL && errlen > 0) {
				snprintf(err, errlen, "%.*s is not an IAU_DEV_* setting the target reads", (int)len, e);
			}
			return -EINVAL;
		}
		if (idx >= 0 && n_ctrlr >= 0 && idx >= n_ctrlr) {
			if (err != NULL && errlen > 0) {
				snprintf(err, errlen, "%.*s names controller %ld, but there are %d", (int)len, e, idx,
					 n_ctrlr);
			}
			return -EINVAL;
		}
	}
	return 0;
}
