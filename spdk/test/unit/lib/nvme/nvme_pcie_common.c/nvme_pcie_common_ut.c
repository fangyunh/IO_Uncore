/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "nvme/nvme_pcie_common.c"
#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;
DEFINE_STUB(nvme_ctrlr_get_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr, pid_t pid), NULL);

DEFINE_STUB(nvme_ctrlr_submit_admin_request, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_request *req), 0);

DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));
DEFINE_STUB(nvme_wait_for_adminq_completion, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_completion_poll_status *status, bool release), 0);

DEFINE_STUB(nvme_completion_is_retry, bool, (const struct spdk_nvme_cpl *cpl), false);

DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));

DEFINE_STUB_V(spdk_nvme_qpair_print_completion, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cpl *cpl));

DEFINE_STUB_V(nvme_qpair_deinit, (struct spdk_nvme_qpair *qpair));

DEFINE_STUB(nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB(spdk_nvme_qpair_process_completions, int32_t,
	    (struct spdk_nvme_qpair *qpair, uint32_t max_completions), 0);

DEFINE_STUB(nvme_request_check_timeout, int, (struct nvme_request *req, uint16_t cid,
		struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick), 0);
DEFINE_STUB(spdk_strerror, const char *, (int errnum), NULL);

DEFINE_STUB_V(nvme_ctrlr_disable, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB(nvme_ctrlr_disable_poll, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB_V(nvme_transport_ctrlr_disconnect_qpair_done, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB(spdk_nvme_ctrlr_get_numa_id, int32_t, (struct spdk_nvme_ctrlr *ctrlr),
	    SPDK_ENV_NUMA_ID_ANY);
DEFINE_STUB(nvme_qpair_state_string, const char *, (enum nvme_qpair_state state), NULL);
/* The IAU-engine registration hook (nvme_pcie_qpair_iau_engine_register(), P3.10) names these two; it
 * runs only with SPDK_UNCORE_MODE_D=1 and SPDK_IAU_ENGINE=1, which no test here sets, so they only link. */
DEFINE_STUB(spdk_nvme_ctrlr_get_first_active_ns, uint32_t, (struct spdk_nvme_ctrlr *ctrlr), 0);
DEFINE_STUB(spdk_nvme_ctrlr_get_next_active_ns, uint32_t, (struct spdk_nvme_ctrlr *ctrlr,
		uint32_t prev_nsid), 0);

int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		struct spdk_nvme_ctrlr *ctrlr,
		enum spdk_nvme_qprio qprio,
		uint32_t num_requests, bool async)
{
	qpair->id = id;
	qpair->qprio = qprio;
	qpair->ctrlr = ctrlr;
	qpair->async = async;

	return 0;
}

static void
test_nvme_pcie_ctrlr_alloc_cmb(void)
{

	struct nvme_pcie_ctrlr pctrlr = {};
	void *vaddr = NULL;
	uint64_t alignment;
	uint64_t phys_addr_var;
	uint64_t size;

	size = 64;
	alignment = 4096;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	pctrlr.cmb.size = 1 << 16;

	/* Allocate CMB */
	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == (void *)0xF9001000);
	CU_ASSERT(phys_addr_var == 0xF8001000);
	CU_ASSERT(pctrlr.cmb.current_offset == 4160);

	/* CMB size overload */
	size = 0x1000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	SPDK_CU_ASSERT_FATAL(vaddr == NULL);

	/* BAR is mapped for data */
	pctrlr.cmb.mem_register_addr = (void *)0xF0000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == NULL);
}

/* Tracker i owns page i of the qpair's 4 KiB-aligned PRP/SGL list pool. */
static bool
ut_check_prp_pool(struct nvme_pcie_qpair *pqpair)
{
	uint16_t num_trackers = pqpair->num_entries - pqpair->max_completions_cap;
	uint16_t i;

	if (pqpair->prp_pool == NULL || ((uintptr_t)pqpair->prp_pool & 0xFFF) != 0) {
		return false;
	}
	for (i = 0; i < num_trackers; i++) {
		if (pqpair->tr[i].list_page != &pqpair->prp_pool[i] || pqpair->tr[i].cid != i) {
			return false;
		}
	}
	return true;
}

/* With test_env's identity spdk_vtophys(), every tracker's bus address is its own page. */
static bool
ut_check_prp_pool_bus_addrs(struct nvme_pcie_qpair *pqpair)
{
	uint16_t num_trackers = pqpair->num_entries - pqpair->max_completions_cap;
	uint16_t i;

	for (i = 0; i < num_trackers; i++) {
		if (pqpair->tr[i].prp_sgl_bus_addr != (uint64_t)(uintptr_t)&pqpair->prp_pool[i] ||
		    (pqpair->tr[i].prp_sgl_bus_addr & 0xFFF) != 0) {
			return false;
		}
	}
	return true;
}

static void
test_nvme_pcie_qpair_construct_destroy(void)
{
	struct spdk_nvme_io_qpair_opts opts = {};
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_nvme_cpl cpl[2] = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	size_t page_align = sysconf(_SC_PAGESIZE);
	uint64_t cmb_offset;
	int rc;

	opts.sq.paddr = 0xDEADBEEF;
	opts.cq.paddr = 0xDBADBEEF;
	opts.sq.vaddr = (void *)0xDCADBEEF;
	opts.cq.vaddr = cpl;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.transport_retry_count = 1;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	cmb_offset = pctrlr.cmb.current_offset;
	/* Make sure that CMB size is big enough and includes page alignment */
	pctrlr.cmb.size = (1 << 16) + page_align;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;

	/* Allocate memory for destroying. */
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	/* Enable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = true;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == true);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == (((pctrlr.cmb.bar_pa + cmb_offset) + page_align - 1) & ~
					   (page_align - 1)));
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	CU_ASSERT(ut_check_prp_pool(pqpair));
	CU_ASSERT(pctrlr.cmb.current_offset == (uintptr_t)pqpair->cmd + (pqpair->num_entries * sizeof(
				struct spdk_nvme_cmd)) - (uintptr_t)pctrlr.cmb.bar_va);
	cmb_offset = pctrlr.cmb.current_offset;
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDEADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	CU_ASSERT(ut_check_prp_pool(pqpair));
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer, sq_vaddr and cq_vaddr invalid. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;
	MOCK_SET(spdk_vtophys, 0xDAADBEEF);

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDAADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000c);
	CU_ASSERT(pqpair->flags.phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	CU_ASSERT(ut_check_prp_pool(pqpair));
	/* Every list page is translated on its own (mocked translation here). */
	CU_ASSERT(pqpair->tr[0].prp_sgl_bus_addr == 0xDAADBEEF);
	nvme_pcie_qpair_destroy(&pqpair->qpair);
	MOCK_CLEAR(spdk_vtophys);
}

static void
test_nvme_pcie_ctrlr_cmd_create_delete_io_queue(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_pcie_qpair pqpair = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req = {};
	int rc;

	ctrlr.adminq = &adminq;
	STAILQ_INIT(&ctrlr.adminq->free_req);
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;

	rc = nvme_pcie_ctrlr_cmd_create_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0xDEADBEEF);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0xDDADBEEF);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	/* No free request available */
	rc = nvme_pcie_ctrlr_cmd_create_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	/* Delete cq or sq */
	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	CU_ASSERT(req.cmd.cdw10_bits.delete_io_q.qid == 1);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_DELETE_IO_SQ);
	CU_ASSERT(req.cmd.cdw10_bits.delete_io_q.qid == 1);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	/* No free request available */
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_nvme_pcie_ctrlr_connect_qpair(void)
{
	struct nvme_pcie_ctrlr	pctrlr = {};
	struct nvme_pcie_qpair	pqpair = {};
	struct spdk_nvme_transport_poll_group poll_group = {};
	struct spdk_nvme_cpl cpl = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req[3] = {};
	int rc;

	pqpair.cpl = &cpl;
	pqpair.num_entries = 1;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	pctrlr.ctrlr.page_size = 4096;

	/* Shadow doorbell available */
	pctrlr.doorbell_stride_u32 = 1;
	pctrlr.ctrlr.shadow_doorbell = spdk_zmalloc(pctrlr.ctrlr.page_size, pctrlr.ctrlr.page_size,
				       NULL, SPDK_ENV_LCORE_ID_ANY,
				       SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	pctrlr.ctrlr.eventidx = spdk_zmalloc(pctrlr.ctrlr.page_size, pctrlr.ctrlr.page_size,
					     NULL, SPDK_ENV_LCORE_ID_ANY,
					     SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	pctrlr.ctrlr.adminq = &adminq;
	STAILQ_INIT(&pctrlr.ctrlr.adminq->free_req);
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[0].cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req[0].cmd.dptr.prp.prp1 == 0xDEADBEEF);

	/* Complete the first request, which triggers the second. */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req[1].cmd.dptr.prp.prp1 == 0xDDADBEEF);

	pqpair.qpair.state = NVME_QPAIR_CONNECTING;
	/* Complete the second request */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_READY);
	/* State is still CONNECTING until the thread is polled again. */
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_CONNECTING);

	/* doorbell stride and qid are 1 */
	CU_ASSERT(pqpair.shadow_doorbell.sq_tdbl == pctrlr.ctrlr.shadow_doorbell + 2);
	CU_ASSERT(pqpair.shadow_doorbell.cq_hdbl == pctrlr.ctrlr.shadow_doorbell + 3);
	CU_ASSERT(pqpair.shadow_doorbell.sq_eventidx == pctrlr.ctrlr.eventidx + 2);
	CU_ASSERT(pqpair.shadow_doorbell.cq_eventidx == pctrlr.ctrlr.eventidx + 3);
	CU_ASSERT(pqpair.flags.has_shadow_doorbell == 1);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	spdk_free(pctrlr.ctrlr.shadow_doorbell);
	spdk_free(pctrlr.ctrlr.eventidx);
	pctrlr.ctrlr.shadow_doorbell = NULL;
	pctrlr.ctrlr.eventidx = NULL;

	/* Shadow doorbell 0 */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[0].cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req[0].cmd.dptr.prp.prp1 == 0xDEADBEEF);

	/* Complete the first request, which triggers the second. */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req[1].cmd.dptr.prp.prp1 == 0xDDADBEEF);

	pqpair.qpair.state = NVME_QPAIR_CONNECTING;
	/* Complete the second request */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_READY);
	/* State is still CONNECTING until the thread is polled again. */
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_CONNECTING);

	CU_ASSERT(pqpair.shadow_doorbell.sq_tdbl == NULL);
	CU_ASSERT(pqpair.shadow_doorbell.sq_eventidx == NULL);
	CU_ASSERT(pqpair.flags.has_shadow_doorbell == 0);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	/* Completion error for CQ */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	/* Modify cpl such that CQ fails */
	pqpair.cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);

	/* Request to complete callback in async operation */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_FAILED);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);

	/* Remove unused request */
	STAILQ_REMOVE_HEAD(&pctrlr.ctrlr.adminq->free_req, stailq);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	/* Completion error for SQ */
	memset(req, 0, sizeof(struct nvme_request) * 3);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	pqpair.qpair.poll_group = &poll_group;
	for (int i = 0; i < 3; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ);

	/* Request to complete cq callback in async operation */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ);
	/* Modify cpl such that SQ fails */
	pqpair.cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;

	/* Request to complete sq callback in async operation */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(req[2].cmd.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	/* Modify cpl back to success */
	pqpair.cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	req[2].cb_fn(req[2].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_FAILED);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);
	/* No need to remove unused requests here */
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));


	/* No available request used */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_nvme_pcie_ctrlr_construct_admin_qpair(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	int rc = 0;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.admin_queue_size = 32;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;
	pctrlr.ctrlr.flags = 0;
	pctrlr.ctrlr.free_io_qids = NULL;
	pctrlr.ctrlr.is_resetting = false;
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_destructed = false;
	pctrlr.ctrlr.outstanding_aborts = 0;
	pctrlr.ctrlr.ana_log_page = NULL;
	pctrlr.ctrlr.ana_log_page_size = 0;

	TAILQ_INIT(&pctrlr.ctrlr.active_io_qpairs);
	STAILQ_INIT(&pctrlr.ctrlr.queued_aborts);
	TAILQ_INIT(&pctrlr.ctrlr.active_procs);

	rc = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr.ctrlr, 32);
	CU_ASSERT(rc == 0);
	pqpair = nvme_pcie_qpair(pctrlr.ctrlr.adminq);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	CU_ASSERT(pqpair->num_entries == 32);
	CU_ASSERT(pqpair->flags.delay_cmd_submit == 0);
	CU_ASSERT(pqpair->qpair.id == 0);
	CU_ASSERT(pqpair->qpair.qprio == SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT(pqpair->qpair.ctrlr == &pctrlr.ctrlr);
	CU_ASSERT(pqpair->stat != NULL);
	/* The admin queue gets its own list-page pool too, each page translated on its own. */
	CU_ASSERT(ut_check_prp_pool(pqpair));
	CU_ASSERT(ut_check_prp_pool_bus_addrs(pqpair));

	rc = nvme_pcie_qpair_destroy(pctrlr.ctrlr.adminq);
	CU_ASSERT(rc == 0);
}

static bool
ut_bytes_zero(const void *p, size_t len)
{
	const uint8_t *b = p;
	size_t i;

	for (i = 0; i < len; i++) {
		if (b[i] != 0) {
			return false;
		}
	}
	return true;
}

static void
test_nvme_pcie_qpair_prp_pool(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair *pqpair;
	struct nvme_tracker *tr;
	struct nvme_tracker_list_page *page3;
	struct nvme_request req = {};
	uint32_t prp_index = 0;
	uint32_t k;
	int rc;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.transport_retry_count = 1;
	pctrlr.ctrlr.page_size = 0x1000;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 8;
	pqpair->qpair.id = 1;

	/* 8 entries -> max_completions_cap 2 -> 6 trackers, 6 list pages. */
	rc = nvme_pcie_qpair_construct(&pqpair->qpair, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(pqpair->max_completions_cap == 2);
	SPDK_CU_ASSERT_FATAL(ut_check_prp_pool(pqpair));

	/* Dense tracker array: one cache line per tracker; one 4 KiB list page per tracker. */
	CU_ASSERT(sizeof(struct nvme_tracker) == 64);
	CU_ASSERT((uintptr_t)&pqpair->tr[1] - (uintptr_t)&pqpair->tr[0] == 64);
	CU_ASSERT(sizeof(struct nvme_tracker_list_page) == 4096);
	CU_ASSERT(offsetof(struct nvme_tracker_list_page, meta_sgl) == 4080);

	tr = &pqpair->tr[3];
	page3 = (struct nvme_tracker_list_page *)((uint8_t *)pqpair->prp_pool + 3 * 4096);
	CU_ASSERT(tr->list_page == page3);
	/* test_env's spdk_vtophys() is the identity map. */
	CU_ASSERT(tr->prp_sgl_bus_addr == (uint64_t)(uintptr_t)page3);
	CU_ASSERT((tr->prp_sgl_bus_addr & 0xFFF) == 0);

	/* 256 KiB at a 2 KiB page offset: prp1 + a 64-entry PRP list in tracker 3's page. */
	req.qpair = &pqpair->qpair;
	tr->req = &req;
	rc = nvme_pcie_prp_list_append(&pctrlr.ctrlr, tr, &prp_index, (void *)0x10000800, 0x40000,
				       0x1000);
	CU_ASSERT(rc == 0);
	CU_ASSERT(prp_index == 65);
	CU_ASSERT(req.cmd.psdt == SPDK_NVME_PSDT_PRP);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0x10000800);
	CU_ASSERT(req.cmd.dptr.prp.prp2 == tr->prp_sgl_bus_addr);
	for (k = 0; k < 64; k++) {
		CU_ASSERT(page3->u.prp[k] == 0x10001000 + (uint64_t)k * 0x1000);
	}
	/* The list stays inside its own page; the rest of it and the neighbour pages stay zero. */
	CU_ASSERT((uintptr_t)&page3->u.prp[64] <= (uintptr_t)page3 + 4096);
	CU_ASSERT(ut_bytes_zero(&page3->u.prp[64], sizeof(*page3) - 64 * sizeof(uint64_t)));
	CU_ASSERT(ut_bytes_zero(&pqpair->prp_pool[2], sizeof(pqpair->prp_pool[2])));
	CU_ASSERT(ut_bytes_zero(&pqpair->prp_pool[4], sizeof(pqpair->prp_pool[4])));

	tr->req = NULL;
	nvme_pcie_qpair_destroy(&pqpair->qpair);
}

static void
test_nvme_pcie_qpair_prp_pool_vtophys_fail(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_nvme_io_qpair_opts opts = {};
	struct nvme_pcie_qpair *pqpair;
	static struct spdk_nvme_cmd sq[64] __attribute__((aligned(4096)));
	static struct spdk_nvme_cpl cq[64] __attribute__((aligned(4096)));
	int rc;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.transport_retry_count = 1;
	pctrlr.ctrlr.page_size = 0x1000;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;

	/* User-provided rings with explicit bus addresses: the first vtophys call in construct
	 * is the one for list-pool page 0. */
	opts.sq.vaddr = sq;
	opts.sq.paddr = 0x1000000;
	opts.cq.vaddr = cq;
	opts.cq.paddr = 0x2000000;

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 64;
	pqpair->qpair.id = 1;

	/* Translation of a list-pool page fails: construct fails with -EFAULT ... */
	MOCK_SET(spdk_vtophys, SPDK_VTOPHYS_ERROR);
	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	MOCK_CLEAR(spdk_vtophys);
	CU_ASSERT(rc == -EFAULT);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->prp_pool != NULL);
	/* ... and the caller's destroy frees tr and prp_pool once (and not the user rings). */
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Success path with user rings: every pool page is translated on its own. */
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 64;
	pqpair->qpair.id = 1;
	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->cmd_bus_addr == 0x1000000);
	CU_ASSERT(pqpair->cpl_bus_addr == 0x2000000);
	CU_ASSERT(ut_check_prp_pool(pqpair));
	CU_ASSERT(ut_check_prp_pool_bus_addrs(pqpair));
	nvme_pcie_qpair_destroy(&pqpair->qpair);
}

static void
test_nvme_pcie_poll_group_get_stats(void)
{
	int rc = 0;
	struct nvme_pcie_poll_group *pgroup = NULL;
	struct spdk_nvme_transport_poll_group *tgroup = NULL;
	struct spdk_nvme_transport_poll_group_stat *tgroup_stat = NULL;

	tgroup = nvme_pcie_poll_group_create();
	CU_ASSERT(tgroup != NULL);
	pgroup = SPDK_CONTAINEROF(tgroup, struct nvme_pcie_poll_group, group);
	CU_ASSERT(pgroup != NULL);

	/* Invalid group pointer, expect fail and return -EINVAL */
	rc = nvme_pcie_poll_group_get_stats(NULL, &tgroup_stat);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(tgroup_stat == NULL);

	/* Invalid stats, expect fail and return -EINVAL */
	rc = nvme_pcie_poll_group_get_stats(tgroup, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Get state success */
	rc = nvme_pcie_poll_group_get_stats(tgroup, &tgroup_stat);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(tgroup_stat != NULL);
	CU_ASSERT(tgroup_stat->trtype == SPDK_NVME_TRANSPORT_PCIE);
	CU_ASSERT(memcmp(&tgroup_stat->pcie, &pgroup->stats, sizeof(struct spdk_nvme_pcie_stat)) == 0);

	nvme_pcie_poll_group_free_stats(tgroup, tgroup_stat);
	rc = nvme_pcie_poll_group_destroy(tgroup);
	CU_ASSERT(rc == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvme_pcie_common", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_alloc_cmb);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_construct_destroy);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_cmd_create_delete_io_queue);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_connect_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_construct_admin_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_prp_pool);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_prp_pool_vtophys_fail);
	CU_ADD_TEST(suite, test_nvme_pcie_poll_group_get_stats);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
