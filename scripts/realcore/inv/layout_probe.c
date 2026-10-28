/* layout_probe.c — sizeof/offsetof of the per-IO hot structs for one SPDK tree (spec I2).
 * Build (in the build container), e.g. for the fork lean layout:
 *   gcc -DSPDK_NVME_IO_CYCLE_OFF -I<tree>/include -I<tree>/lib/nvme -I<tree>/build/include \
 *       -o .build/tmp/layout_<name> scripts/realcore/inv/layout_probe.c
 * For an upstream tree add -DUPSTREAM: fork-only fields are wrapped in #ifndef UPSTREAM.
 */
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include <stdio.h>

#define P(s, f) printf("%-24s %-22s off=%4zu line=%zu\n", #s, #f, offsetof(struct s, f), offsetof(struct s, f) / 64)

int
main(void)
{
	printf("sizeof nvme_request=%zu nvme_tracker=%zu nvme_pcie_qpair=%zu\n",
	       sizeof(struct nvme_request), sizeof(struct nvme_tracker), sizeof(struct nvme_pcie_qpair));
	P(nvme_request, cmd); P(nvme_request, payload_size); P(nvme_request, payload); P(nvme_request, cb_fn);
	P(nvme_request, cb_arg); P(nvme_request, stailq); P(nvme_request, qpair); P(nvme_request, submit_tick);
	P(nvme_tracker, req); P(nvme_tracker, cid); P(nvme_tracker, cb_fn); P(nvme_tracker, cb_arg);
#ifdef NVME_TRACKER_LIST_PAGE_SIZE
	/* post-F2 tree: the PRP/SGL list lives in a separate per-qpair pool page */
	P(nvme_tracker, list_page);
	printf("sizeof nvme_tracker_list_page=%zu\n", sizeof(struct nvme_tracker_list_page));
#else
	P(nvme_tracker, u);
#endif
	P(nvme_pcie_qpair, cmd); P(nvme_pcie_qpair, cpl); P(nvme_pcie_qpair, tr); P(nvme_pcie_qpair, sq_tail);
	P(nvme_pcie_qpair, cq_head); P(nvme_pcie_qpair, free_tr); P(nvme_pcie_qpair, outstanding_tr);
	P(nvme_pcie_qpair, stat);
#ifndef UPSTREAM
#ifndef SPDK_NVME_IO_CYCLE_OFF
	/* probe fields: compiled out of the lean build since F1 */
	P(nvme_request, t_start); P(nvme_request, t_lib_start); P(nvme_request, t_cb_end);
	P(nvme_pcie_qpair, poll_cycle_accum);
#endif
	P(nvme_pcie_qpair, sq_desc_tail);
	P(nvme_pcie_qpair, sq_desc_ring); P(nvme_pcie_qpair, free_slot_ring);
#endif
	return 0;
}
