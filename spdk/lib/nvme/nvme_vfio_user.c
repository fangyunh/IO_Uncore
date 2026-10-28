/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 */

/* VFIO transport extensions for spdk_nvme_ctrlr */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/vfio_user_pci.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include "nvme_iau.h"
#include "nvme_text_pad.h"

#include <linux/vfio.h>

/* IAU-next P4.5: the selector below grows this object's .text; padded to a multiple of 64 B (as at P4_BASE,
 * 0xa35 -> footprint 0xa40), so the libraries linked after libspdk_nvme keep their 64 B phase */
NVME_TEXT_PAD64();

#define NVME_MAX_XFER_SIZE		(131072)
#define NVME_MAX_SGES			(1)

struct nvme_vfio_ctrlr {
	struct nvme_pcie_ctrlr pctrlr;

	volatile uint32_t *doorbell_base;
	struct vfio_device *dev;
};

static inline struct nvme_vfio_ctrlr *
nvme_vfio_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return SPDK_CONTAINEROF(pctrlr, struct nvme_vfio_ctrlr, pctrlr);
}

static volatile struct spdk_nvme_registers *
nvme_vfio_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);

	return vctrlr->pctrlr.regs;
}

static int
nvme_vfio_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%x\n", offset, value);

	return spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					     offset, 4, &value, true);
}

static int
nvme_vfio_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%"PRIx64"\n", offset, value);

	return spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					     offset, 8, &value, true);
}

static int
nvme_vfio_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	int ret;

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);

	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					    offset, 4, value, false);
	if (ret != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "offset %x\n", offset);
		return ret;
	}

	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%x\n", offset, *value);

	return 0;
}

static int
nvme_vfio_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	int ret;

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);

	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					    offset, 8, value, false);
	if (ret != 0) {
		NVME_CTRLR_ERRLOG(ctrlr, "offset %x\n", offset);
		return ret;
	}

	NVME_CTRLR_LOG2(DEBUG, nvme_vfio, ctrlr, "offset 0x%x, value 0x%"PRIx64"\n", offset, *value);

	return 0;
}

static int
nvme_vfio_ctrlr_set_asq(struct spdk_nvme_ctrlr *ctrlr, uint64_t value)
{
	return nvme_vfio_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, asq),
					 value);
}

static int
nvme_vfio_ctrlr_set_acq(struct spdk_nvme_ctrlr *ctrlr, uint64_t value)
{
	return nvme_vfio_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, acq),
					 value);
}

static int
nvme_vfio_ctrlr_set_aqa(struct spdk_nvme_ctrlr *ctrlr, const union spdk_nvme_aqa_register *aqa)
{
	return nvme_vfio_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw),
					 aqa->raw);
}

static int
nvme_vfio_setup_bar0(struct nvme_vfio_ctrlr *vctrlr)
{
	void *doorbell;

	doorbell = spdk_vfio_user_get_bar_addr(vctrlr->dev, 0, 0x1000, 0x1000);
	if (!doorbell) {
		return -EINVAL;
	}

	vctrlr->doorbell_base = (volatile uint32_t *)doorbell;
	return 0;
}

static struct spdk_nvme_ctrlr *
	nvme_vfio_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts,
			  void *devhandle)
{
	struct nvme_vfio_ctrlr *vctrlr;
	struct nvme_pcie_ctrlr *pctrlr;
	uint16_t cmd_reg;
	union spdk_nvme_cap_register cap;
	int ret;
	char ctrlr_path[PATH_MAX];

	snprintf(ctrlr_path, sizeof(ctrlr_path), "%s/cntrl", trid->traddr);
	ret = access(ctrlr_path, F_OK);
	if (ret != 0) {
		SPDK_ERRLOG("Access path %s failed\n", ctrlr_path);
		return NULL;
	}

	vctrlr = calloc(1, sizeof(*vctrlr));
	if (!vctrlr) {
		return NULL;
	}

	vctrlr->dev = spdk_vfio_user_setup(ctrlr_path);
	if (!vctrlr->dev) {
		SPDK_ERRLOG("Error to setup vfio device\n");
		free(vctrlr);
		return NULL;
	}

	ret = nvme_vfio_setup_bar0(vctrlr);
	if (ret != 0) {
		SPDK_ERRLOG("Error to get device BAR0\n");
		goto exit;
	}

	pctrlr = &vctrlr->pctrlr;
	pctrlr->doorbell_base = vctrlr->doorbell_base;

	/* IAU Mode C/D (realcore T4.3): map the BAR0 ring-base register page
	 * (SQ_DESC_RING_IOVA_OFFSET / CQ_REC_RING_IOVA_OFFSET), mirroring
	 * nvme_pcie.c. The target exposes the page as a sparse-mmap area, so the
	 * unchanged host setup code writes the ring IOVAs with plain stores.
	 * IAU-next P4.5: IAU tile (SPDK_IAU_TILE=1) writes QCFG, QCFG2 and the CQ sink there. */
	{
		const char *env_c = getenv("SPDK_UNCORE_MODE_C");
		const char *env_d = getenv("SPDK_UNCORE_MODE_D");

		if ((env_c != NULL && env_c[0] == '1') || (env_d != NULL && env_d[0] == '1') || g_nvme_iau_cfg.tile) {
			void *pg = spdk_vfio_user_get_bar_addr(vctrlr->dev, 0, SQ_DESC_RING_IOVA_OFFSET, 0x1000);

			if (pg == NULL) {
				SPDK_ERRLOG("IAU vfio-user: Mode C/D or IAU tile requested but BAR0+0x%x is not mappable\n",
					    SQ_DESC_RING_IOVA_OFFSET);
				goto exit;
			}
			pctrlr->uncore_sq_desc_iova_reg = (volatile uint64_t *)pg;
			pctrlr->uncore_cq_rec_iova_reg = (volatile uint64_t *)((uint8_t *)pg +
							 (CQ_REC_RING_IOVA_OFFSET - SQ_DESC_RING_IOVA_OFFSET));
			SPDK_NOTICELOG("IAU vfio-user: ring-base page mapped at BAR0+0x%x\n",
				       SQ_DESC_RING_IOVA_OFFSET);
		}
	}

	pctrlr->ctrlr.is_removed = false;
	pctrlr->ctrlr.opts = *opts;
	pctrlr->ctrlr.trid = *trid;
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
	pctrlr->ctrlr.opts.admin_queue_size = spdk_max(pctrlr->ctrlr.opts.admin_queue_size,
					      NVME_PCIE_MIN_ADMIN_QUEUE_SIZE);

	ret = nvme_ctrlr_construct(&pctrlr->ctrlr);
	if (ret != 0) {
		goto exit;
	}

	/* Enable PCI busmaster and disable INTx */
	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					    &cmd_reg, false);
	if (ret != 0) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "Read PCI CMD REG failed\n");
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}
	cmd_reg |= 0x404;
	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					    &cmd_reg, true);
	if (ret != 0) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "Write PCI CMD REG failed\n");
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	if (nvme_ctrlr_get_cap(&pctrlr->ctrlr, &cap)) {
		NVME_CTRLR_ERRLOG(&pctrlr->ctrlr, "get_cap() failed\n");
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	ret = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr->ctrlr, pctrlr->ctrlr.opts.admin_queue_size);
	if (ret != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	/* Construct the primary process properties */
	ret = nvme_ctrlr_add_process(&pctrlr->ctrlr, 0);
	if (ret != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	return &pctrlr->ctrlr;

exit:
	spdk_vfio_user_release(vctrlr->dev);
	free(vctrlr);
	return NULL;
}

static int
nvme_vfio_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		     bool direct_connect)
{
	int ret;

	if (probe_ctx->trid.trtype != SPDK_NVME_TRANSPORT_VFIOUSER) {
		SPDK_ERRLOG("Can only use SPDK_NVME_TRANSPORT_VFIOUSER");
		return -EINVAL;
	}

	ret = access(probe_ctx->trid.traddr, F_OK);
	if (ret != 0) {
		SPDK_ERRLOG("Error to access file %s\n", probe_ctx->trid.traddr);
		return ret;
	}
	SPDK_DEBUGLOG(nvme_vfio, "Scan controller : %s\n", probe_ctx->trid.traddr);

	return nvme_ctrlr_probe(&probe_ctx->trid, probe_ctx, NULL);
}

static int
nvme_vfio_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_qpair *vadminq = nvme_pcie_qpair(ctrlr->adminq);
	union spdk_nvme_aqa_register aqa;

	if (nvme_vfio_ctrlr_set_asq(ctrlr, vadminq->cmd_bus_addr)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_asq() failed\n");
		return -EIO;
	}

	if (nvme_vfio_ctrlr_set_acq(ctrlr, vadminq->cpl_bus_addr)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_acq() failed\n");
		return -EIO;
	}

	aqa.raw = 0;
	/* acqs and asqs are 0-based. */
	aqa.bits.acqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;
	aqa.bits.asqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;

	if (nvme_vfio_ctrlr_set_aqa(ctrlr, &aqa)) {
		NVME_CTRLR_ERRLOG(ctrlr, "set_aqa() failed\n");
		return -EIO;
	}

	return 0;
}

static int
nvme_vfio_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_pcie_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	spdk_vfio_user_release(vctrlr->dev);
	free(vctrlr);

	return 0;
}

static  uint32_t
nvme_vfio_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_XFER_SIZE;
}

static uint16_t
nvme_vfio_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_SGES;
}

const struct spdk_nvme_transport_ops vfio_ops = {
	.name = "VFIOUSER",
	.type = SPDK_NVME_TRANSPORT_VFIOUSER,
	.ctrlr_construct = nvme_vfio_ctrlr_construct,
	.ctrlr_scan = nvme_vfio_ctrlr_scan,
	.ctrlr_destruct = nvme_vfio_ctrlr_destruct,
	.ctrlr_enable = nvme_vfio_ctrlr_enable,

	.ctrlr_get_registers = nvme_vfio_ctrlr_get_registers,
	.ctrlr_set_reg_4 = nvme_vfio_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_vfio_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_vfio_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_vfio_ctrlr_get_reg_8,

	.ctrlr_get_max_xfer_size = nvme_vfio_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_vfio_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_pcie_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_pcie_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_pcie_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_pcie_ctrlr_disconnect_qpair,
	.admin_qpair_abort_aers = nvme_pcie_admin_qpair_abort_aers,

	.qpair_reset = nvme_pcie_qpair_reset,
	.qpair_abort_reqs = nvme_pcie_qpair_abort_reqs,
	.qpair_submit_request = nvme_pcie_qpair_submit_request,
	.qpair_process_completions = nvme_pcie_qpair_process_completions,

	.poll_group_create = nvme_pcie_poll_group_create,
	.poll_group_connect_qpair = nvme_pcie_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_pcie_poll_group_disconnect_qpair,
	.poll_group_add = nvme_pcie_poll_group_add,
	.poll_group_remove = nvme_pcie_poll_group_remove,
	.poll_group_process_completions = nvme_pcie_poll_group_process_completions,
	.poll_group_destroy = nvme_pcie_poll_group_destroy,
	.poll_group_get_stats = nvme_pcie_poll_group_get_stats,
	.poll_group_free_stats = nvme_pcie_poll_group_free_stats
};

/*
 * IAU-next P4.5 (D2): the table the registration below copies.  nvme_vfio_select_ops() runs before it (a
 * prioritized constructor: GNU ld places .init_array.<prio> before the plain .init_array) and makes it vfio_ops
 * byte for byte, or, with SPDK_IAU_TILE=1, vfio_ops overridden by nvme_iau_override_ops() (it aborts on a bad
 * SPDK_IAU_* environment, IAU_TILE_CONFIG_ERROR): one table per process, chosen once.  The selector is placed in
 * .text (padded above), and the registration constructor keeps its two instructions, so .text.startup does
 * not change (tests/realcore/test_pcie_hot_layout.sh).
 */
static struct spdk_nvme_transport_ops g_vfio_reg_ops;

static void __attribute__((constructor(1000), section(".text")))
nvme_vfio_select_ops(void)
{
	g_vfio_reg_ops = vfio_ops;
	nvme_iau_select_ops(&g_vfio_reg_ops);
}

SPDK_NVME_TRANSPORT_REGISTER(vfio, &g_vfio_reg_ops);

SPDK_LOG_REGISTER_COMPONENT(nvme_vfio)
