/* SPDX-License-Identifier: BSD-3-Clause
 * IAU v4.1 (Mode D) LOCKED 32-byte completion-callback record — C port of
 * SimpleSSD hil/nvme/moded_record.hh packModeDRecord(). Must stay
 * byte-identical; scripts/rtl/check_rtl_consistency.sh proves RTL == gem5 == this.
 *   w0 = cb_fn, w1 = cb_arg, w2 = cpl bytes [0:8), w3 = cpl bytes [8:16)
 *   with bit 48 (status phase) overwritten by the ring phase.
 * Publish order is the caller's job: w0..w2 before w3.
 */
#ifndef SPDK_VFIO_USER_IAU_RECORD_H
#define SPDK_VFIO_USER_IAU_RECORD_H

#include <stdint.h>
#include <string.h>

#define IAU_MODED_PHASE_BIT 48u

static inline void
iau_pack_moded_record(uint64_t cb_fn, uint64_t cb_arg, uint64_t cpl_lo,
		      uint64_t cpl_hi_raw, uint8_t ring_phase, uint8_t out[32])
{
	uint64_t cpl_hi = (cpl_hi_raw & ~(1ULL << IAU_MODED_PHASE_BIT)) |
			  ((uint64_t)(ring_phase & 0x1u) << IAU_MODED_PHASE_BIT);

	memcpy(out + 0, &cb_fn, 8);
	memcpy(out + 8, &cb_arg, 8);
	memcpy(out + 16, &cpl_lo, 8);
	memcpy(out + 24, &cpl_hi, 8);
}

/* Inject cid into w3[47:32] of a raw cpl_hi seed (what the device path does
 * before packing; a real NVMe cpl already carries cid there). */
static inline uint64_t
iau_cpl_hi_with_cid(uint64_t cpl_hi_seed, uint16_t cid)
{
	return (cpl_hi_seed & ~(0xFFFFULL << 32)) | ((uint64_t)cid << 32);
}

#endif
