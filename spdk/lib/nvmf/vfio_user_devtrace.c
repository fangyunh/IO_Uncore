/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "vfio_user_devtrace.h"

int
devtrace_init(struct devtrace *dt, uint32_t cap)
{
	const char *e = getenv("IAU_DEV_TRACE");

	memset(dt, 0, sizeof(*dt));
	if (e == NULL || e[0] != '1') {
		return 0;
	}
	dt->cap = cap ? cap : DEVTRACE_DEFAULT_CAP;
	dt->buf = calloc(dt->cap, sizeof(*dt->buf));
	if (dt->buf == NULL) {
		return -ENOMEM;
	}
	dt->on = true;
	return 0;
}

int
devtrace_dump(struct devtrace *dt, uint16_t qid)
{
	const char *dir = getenv("IAU_DEV_TRACE_DIR");
	char path[PATH_MAX];
	uint64_t first, i, count;
	FILE *fp;

	if (!dt->on || dt->n == 0) {
		return 0;
	}
	snprintf(path, sizeof(path), "%s/device_q%u.csv", dir ? dir : ".", qid);
	fp = fopen(path, "w");
	if (fp == NULL) {
		return -errno;
	}
	fprintf(fp, "qid,cid,mode,ts_db_seen,ts_fetched,ts_exec,ts_done,ts_posted\n");
	count = dt->n < dt->cap ? dt->n : dt->cap;
	first = dt->n - count;
	for (i = first; i < dt->n; i++) {
		const struct devtrace_rec *r = &dt->buf[i % dt->cap];
		fprintf(fp, "%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
			r->qid, r->cid, r->mode, r->ts_db_seen, r->ts_fetched, r->ts_exec,
			r->ts_done, r->ts_posted);
	}
	fclose(fp);
	return 0;
}

void
devtrace_fini(struct devtrace *dt)
{
	free(dt->buf);
	memset(dt, 0, sizeof(*dt));
}
