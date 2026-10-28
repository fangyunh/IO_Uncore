/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P3: per-controller device-model settings for the vfio-user testbed (spec §3.4: IAU_DEV_*
 * "now per controller, not per SQ"). Each key reads IAU_DEV_<KEY>_C<k> for controller k first, then
 * IAU_DEV_<KEY>, then the default; k is the trailing number of the endpoint directory (start_target.sh
 * names them vfu0, vfu1, ...). IAU_DEV_MODEL (sq | ctrlr, default sq) is process-wide.
 * Pure C: the environment is read through a caller-supplied getenv.
 *
 * Contract for the caller (P3.8 wiring):
 * - Every -EINVAL from devcfg_load() is FATAL: print c->err and fail the target start (or the
 *   controller creation). There is no silent fast-path-off fallback; the pre-P3 fp_load_cfg() had one.
 * - c->err holds the FIRST error found. Keys are checked in the order FASTPATH, LMIN_NS, TMAX, CHANNELS,
 *   then MODEL, and the message starts with the variable actually read (IAU_DEV_<KEY>_C<k> or
 *   IAU_DEV_<KEY>); a message longer than 127 chars ends in "...". A key that failed holds its
 *   default in *c, so *c never carries an out-of-range value.
 * - get(name) returns NULL when name is unset. An empty value counts as unset, so an empty
 *   IAU_DEV_<KEY>_C<k> falls through to IAU_DEV_<KEY>.
 * - Values are plain decimal: no sign, no blanks, no exponent, no hex, below 2^64. Ranges:
 *   FASTPATH 0|1; LMIN_NS 0..1e9 (1 s, so lmin_ns * hz fits in 64 bits for hz < 18 GHz);
 *   TMAX 1..1e9 (so svc_ticks = hz / tmax >= 1 while hz >= 1 GHz; the caller checks its own hz);
 *   CHANNELS 1..64.
 * - c->overrides is informational (the DEV_MODEL evidence line). It truncates at 63 chars; the four
 *   keys need at most 30.
 * - IAU-next P4.4: IAU_DEV_FAULT_ERR_{EVERY,SC,SCT,DNR} (+_C<k>), the mode-0 error knob (D19), are known to
 *   devcfg_scan_unknown() but not read by devcfg_load(); vfio_user_iau_tile_bind.h parses them.
 */
#ifndef SPDK_VFIO_USER_DEVCFG_H
#define SPDK_VFIO_USER_DEVCFG_H

#include "spdk/stdinc.h"

#define DEVCFG_DEFAULT_FASTPATH	1
#define DEVCFG_DEFAULT_LMIN_NS	1000
#define DEVCFG_DEFAULT_TMAX	250000
#define DEVCFG_DEFAULT_CHANNELS	32
#define DEVCFG_MAX_CHANNELS	64		/* == FP_MAX_CHANNELS */
#define DEVCFG_MAX_LMIN_NS	1000000000ULL	/* 1 s */
#define DEVCFG_MAX_TMAX		1000000000ULL	/* commands/s per channel */

struct devcfg {
	int ctrlr_idx;		/* -1: no per-controller override applies */
	bool fastpath;
	bool model_ctrlr;	/* IAU_DEV_MODEL=ctrlr */
	uint64_t lmin_ns;
	uint64_t tmax;
	uint32_t channels;
	char overrides[64];	/* keys taken from IAU_DEV_<KEY>_C<k>, comma separated, "-" if none */
	char err[128];		/* the first reason devcfg_load() returned -EINVAL, "" on success */
};

typedef char *(*devcfg_getenv_fn)(const char *name);

/* Numeric value of the trailing decimal digits of traddr, after trailing '/' are stripped
 * ("/r/vfu12" -> 12, "/r/vfu12/" -> 12); -1 if there are none or more than 9.
 * Aliasing: only the number counts. Leading zeros are ignored (vfu012 == vfu12 == 12) and no "vfu"
 * prefix is checked (/r/run2 -> 2); start_target.sh names are vfu<k> without leading zeros. */
int devcfg_ctrlr_index(const char *traddr);
/* 0, or -EINVAL with c->err set (see the contract above). c is filled either way. */
int devcfg_load(struct devcfg *c, int ctrlr_idx, devcfg_getenv_fn get);
/* "6,7", "6-7", "6+7" -> out[] (at most max entries, CPUs < 1024, no CPU twice: engines and pollers
 * must not share a core). Returns the count (0 for ""), or -1. */
int devcfg_cpu_list(const char *s, uint32_t *out, int max);
/* Optional typo guard over a NULL-terminated "NAME=value" array (environ). Returns -EINVAL with err
 * naming the first offender if an IAU_DEV_* name is not one devcfg_load() or the device trace reads:
 * an unknown key, a _C<k> on a process-wide key (MODEL, TRACE, TRACE_DIR), a _C<k> that devcfg_load()
 * never looks up (leading zero, more than 9 digits, no digits), or k >= n_ctrlr. n_ctrlr < 0 skips
 * the bound (the target learns its controller count only as listeners are added). envp NULL -> 0;
 * err may be NULL. */
int devcfg_scan_unknown(char **envp, int n_ctrlr, char *err, size_t errlen);

#endif
