/* SPDX-License-Identifier: BSD-3-Clause
 * Per-SQ device-side timestamp ring for the realcore harness (Stage 3).
 * Enabled by IAU_DEV_TRACE=1; output dir IAU_DEV_TRACE_DIR (default ".").
 * Each SQ is owned by one poll-group thread, so no locking is needed.
 */
#ifndef SPDK_VFIO_USER_DEVTRACE_H
#define SPDK_VFIO_USER_DEVTRACE_H

#include "spdk/stdinc.h"
#include "spdk/likely.h"

#define DEVTRACE_DEFAULT_CAP (1u << 20)

struct devtrace_rec {
	uint16_t qid;
	uint16_t cid;
	uint32_t mode;
	uint64_t ts_db_seen;	/* poller observed the doorbell value that covered this command */
	uint64_t ts_fetched;	/* SQE (or Mode-D descriptor) read */
	uint64_t ts_exec;	/* request handed to the nvmf/bdev layer */
	uint64_t ts_done;	/* bdev completion callback */
	uint64_t ts_posted;	/* CQE written (baseline) or record staged (Mode D) */
};

struct devtrace {
	bool on;
	uint32_t cap;
	uint64_t n;
	struct devtrace_rec *buf;
};

/* Reads IAU_DEV_TRACE; allocates the ring only when enabled. Returns 0 or -ENOMEM. */
int devtrace_init(struct devtrace *dt, uint32_t cap);
static inline void
devtrace_add(struct devtrace *dt, const struct devtrace_rec *r)
{
	if (spdk_likely(!dt->on)) {
		return;
	}
	dt->buf[dt->n % dt->cap] = *r;
	dt->n++;
}
/* Writes <IAU_DEV_TRACE_DIR>/device_q<qid>.csv (oldest first). No-op when off. */
int devtrace_dump(struct devtrace *dt, uint16_t qid);
void devtrace_fini(struct devtrace *dt);

#endif
