/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2019-2022, Nutanix Inc. All rights reserved.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over vfio-user transport
 */

#include <sys/param.h>

#include <vfio-user/libvfio-user.h>
#include <vfio-user/pci_defs.h>

#include "spdk/barrier.h"
#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/nvmf_transport.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "transport.h"

#include "nvmf_internal.h"
#include "vfio_user_devtrace.h"
#include "vfio_user_fastpath.h"
#include "vfio_user_devcfg.h"
#include "vfio_user_iau_engine.h"
#include "vfio_user_iau.h"
#include "vfio_user_iau_tile_bind.h"

/* IAU_DEV_CHANNELS (devcfg: 1..DEVCFG_MAX_CHANNELS) indexes the fast path's next_free[FP_MAX_CHANNELS]. */
SPDK_STATIC_ASSERT(DEVCFG_MAX_CHANNELS == FP_MAX_CHANNELS,
		   "devcfg channel cap must equal the fast-path array size");

#define SWAP(x, y)                  \
	do                          \
	{                           \
		typeof(x) _tmp = x; \
		x = y;              \
		y = _tmp;           \
	} while (0)

#define NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH 256
#define NVMF_VFIO_USER_DEFAULT_AQ_DEPTH 32
#define NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE ((NVMF_REQ_MAX_BUFFERS - 1) << SHIFT_4KB)
#define NVMF_VFIO_USER_DEFAULT_IO_UNIT_SIZE NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE

#define NVME_DOORBELLS_OFFSET	0x1000
#define NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT 2
#define NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS 3
#define NVMF_VFIO_USER_EVENTIDX_POLL UINT32_MAX

#define NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR 512
#define NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR (NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR / 4)

/* NVMe spec 1.4, section 5.21.1.7 */
SPDK_STATIC_ASSERT(NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR >= 2 &&
		   NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR <= SPDK_NVME_MAX_IO_QUEUES,
		   "bad number of queues");

/*
 * NVMe driver reads 4096 bytes, which is the extended PCI configuration space
 * available on PCI-X 2.0 and PCI Express buses
 */
#define NVME_REG_CFG_SIZE       0x1000

/*
 * Doorbells must be page aligned so that they can memory mapped.
 *
 * TODO does the NVMe spec also require this? Document it.
 */
#define NVMF_VFIO_USER_DOORBELLS_SIZE \
	SPDK_ALIGN_CEIL( \
		(NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR * 2 * SPDK_NVME_DOORBELL_REGISTER_SIZE), \
		0x1000)
/*
 * BAR0 is a power of two and also covers the IAU uncore page at
 * IAU_BAR0_UNCORE_PAGE_OFFSET (0x5000, 4 KiB). The backing devmem_fd is sized
 * to the whole BAR0 and BAR0 offset X lives at fd offset X.
 */
#define NVME_REG_BAR0_SIZE 0x8000
SPDK_STATIC_ASSERT(NVME_DOORBELLS_OFFSET + NVMF_VFIO_USER_DOORBELLS_SIZE <= IAU_BAR0_UNCORE_PAGE_OFFSET,
		   "doorbells overlap IAU page");
SPDK_STATIC_ASSERT(IAU_BAR0_UNCORE_PAGE_OFFSET + IAU_BAR0_UNCORE_PAGE_SIZE <= NVME_REG_BAR0_SIZE,
		   "IAU page does not fit in BAR0");

/*
 * TODO check the PCI spec whether BAR4 and BAR5 really have to be at least one
 * page and a multiple of page size (maybe QEMU also needs this?). Document all
 * this.
 */

#define NVMF_VFIO_USER_MSIX_NUM MAX(CHAR_BIT, NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR)

#define NVMF_VFIO_USER_MSIX_TABLE_BIR (4)
#define NVMF_VFIO_USER_BAR4_SIZE SPDK_ALIGN_CEIL((NVMF_VFIO_USER_MSIX_NUM * 16), 0x1000)
SPDK_STATIC_ASSERT(NVMF_VFIO_USER_BAR4_SIZE > 0, "Incorrect size");

/*
 * TODO according to the PCI spec we need one bit per vector, document the
 * relevant section.
 */
#define NVMF_VFIO_USER_MSIX_PBA_BIR (5)
#define NVMF_VFIO_USER_BAR5_SIZE SPDK_ALIGN_CEIL((NVMF_VFIO_USER_MSIX_NUM / CHAR_BIT), 0x1000)
SPDK_STATIC_ASSERT(NVMF_VFIO_USER_BAR5_SIZE > 0, "Incorrect size");
struct nvmf_vfio_user_req;
/* IAU-next P3.9 (mode 5): an IAU-engine thread, and one I/O SQ bound to it (see iau_engines_create()). */
struct iau_engine_thread;
struct iau_eng_qb;
struct iau_eng6_qb;
struct iau_tile_gring;

typedef int (*nvmf_vfio_user_req_cb_fn)(struct nvmf_vfio_user_req *req, void *cb_arg);

/* 1 more for PRP2 list itself */
#define NVMF_VFIO_USER_MAX_IOVECS	(NVMF_REQ_MAX_BUFFERS + 1)

enum nvmf_vfio_user_req_state {
	VFIO_USER_REQUEST_STATE_FREE = 0,
	VFIO_USER_REQUEST_STATE_EXECUTING,
};

struct nvmf_vfio_user_req  {
	struct spdk_nvmf_request		req;
	struct spdk_nvme_cpl			rsp;
	struct spdk_nvme_cmd			cmd;

	enum nvmf_vfio_user_req_state		state;
	nvmf_vfio_user_req_cb_fn		cb_fn;
	void					*cb_arg;

	/* old CC before prop_set_cc fabric command */
	union spdk_nvme_cc_register		cc;

	TAILQ_ENTRY(nvmf_vfio_user_req)		link;

	struct iovec				iov[NVMF_VFIO_USER_MAX_IOVECS];
	uint8_t					iovcnt;

	/* devtrace timestamps (only written when sq->dt.on) */
	uint64_t				ts_db_seen, ts_fetched, ts_exec, ts_done;

	/* NVMF_VFIO_USER_MAX_IOVECS worth of dma_sg_t. */
	uint8_t					sg[];
};

#define MAP_R			(0)
#define MAP_RW			(1 << 0)
#define MAP_INITIALIZE		(1 << 1)
#define MAP_QUIET		(1 << 2)

/*
 * Mapping of an NVMe queue.
 *
 * This holds the information tracking a local process mapping of an NVMe queue
 * shared by the client.
 */
struct nvme_q_mapping {
	/* iov of local process mapping. */
	struct iovec iov;
	/* Stored sg, needed for unmap. */
	dma_sg_t *sg;
	/* Client PRP of queue. */
	uint64_t prp1;
	/* Total length in bytes. */
	uint64_t len;
};

enum nvmf_vfio_user_sq_state {
	VFIO_USER_SQ_UNUSED = 0,
	VFIO_USER_SQ_CREATED,
	VFIO_USER_SQ_DELETED,
	VFIO_USER_SQ_ACTIVE,
	VFIO_USER_SQ_INACTIVE
};

enum nvmf_vfio_user_cq_state {
	VFIO_USER_CQ_UNUSED = 0,
	VFIO_USER_CQ_CREATED,
	VFIO_USER_CQ_DELETED,
};

enum nvmf_vfio_user_ctrlr_state {
	VFIO_USER_CTRLR_CREATING = 0,
	VFIO_USER_CTRLR_RUNNING,
	/* Quiesce requested by libvfio-user */
	VFIO_USER_CTRLR_PAUSING,
	/* NVMf subsystem is paused, it's safe to do PCI reset, memory register,
	 * memory unregister, etc in this state.
	 */
	VFIO_USER_CTRLR_PAUSED,
	/*
	 * Implies that the NVMf subsystem is paused. Device will be unquiesced (PCI
	 * reset, memory register and unregister, controller in destination VM has
	 * been restored).  NVMf subsystem resume has been requested.
	 */
	VFIO_USER_CTRLR_RESUMING,
};

struct nvmf_vfio_user_sq {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_transport_poll_group	*group;
	struct nvmf_vfio_user_ctrlr		*ctrlr;

	uint32_t				qid;
	/* Number of entries in queue. */
	uint32_t				size;
	struct nvme_q_mapping			mapping;
	enum nvmf_vfio_user_sq_state		sq_state;

	uint32_t				head;
	volatile uint32_t			*dbl_tailp;

	/* Whether a shadow doorbell eventidx needs setting. */
	bool					need_rearm;

	/* multiple SQs can be mapped to the same CQ */
	uint16_t				cqid;

	/* handle_queue_connect_rsp() can be used both for CREATE IO SQ response
	 * and SQ re-connect response in the destination VM, for the prior case,
	 * we will post a NVMe completion to VM, we will not set this flag when
	 * re-connecting SQs in the destination VM.
	 */
	bool					post_create_io_sq_completion;
	/* Copy of Create IO SQ command, this field is used together with
	 * `post_create_io_sq_completion` flag.
	 */
	struct spdk_nvme_cmd			create_io_sq_cmd;

	struct vfio_user_delete_sq_ctx		*delete_ctx;

	/* Device-side timestamp ring (IAU_DEV_TRACE=1) */
	struct devtrace				dt;
	uint64_t				ts_last_db_seen;

	/* Fast-path timer queue (NULL unless IAU_DEV_FASTPATH is on; I/O SQs only).
	 * Touched only from the poll-group thread that owns this SQ. */
	struct fp_sq				*fp;

	/* Per-opcode counters of an I/O SQ (P1.2), both modes: [0] reads, [1] writes,
	 * [2] other. Armed at create, printed once as OPC_STATS at teardown. */
	uint64_t				opc_cnt[3];
	bool					opc_armed;

	/* IAU Mode-D ring mappings (I/O SQs only; poll-group thread owns the
	 * engine state, these are set/cleared on the admin/teardown path). */
	bool					iau_attached;
	dma_sg_t				*iau_sg_desc;
	dma_sg_t				*iau_sg_rec;
	struct iovec				iau_iov_desc;
	struct iovec				iau_iov_rec;

	/* Currently unallocated reqs. */
	TAILQ_HEAD(, nvmf_vfio_user_req)	free_reqs;
	/* Poll group entry */
	TAILQ_ENTRY(nvmf_vfio_user_sq)		link;
	/* Connected SQ entry */
	TAILQ_ENTRY(nvmf_vfio_user_sq)		tailq;

	/* IAU-next P3.9 (mode 5): this SQ's IAU-engine binding, NULL if none; set and cleared under the
	 * transport's eng_lock. Its ring maps reuse iau_sg_* / iau_iov_* (iau_attached stays false, so the
	 * legacy Mode D path never runs). At the end of the struct so the P1 field offsets are unchanged. */
	struct iau_eng_qb			*eng_qb;

	/* IAU-next P4.4 (D19 / A12): the device model's schedule and pop calls on the mode 0 fast path, chosen
	 * once per SQ when it connects (sq_fp_connect()): fp_schedule / fp_pop_due, or with the mode-0 error
	 * knob (IAU_DEV_FAULT_ERR_*) fp_schedule_fault / fp_pop_due_fault. fp_schedule and fp_pop_due keep their
	 * instructions; the Mode D path calls them directly. */
	uint64_t				(*fp_sched)(struct fp_sq *q, uint64_t now, uint64_t slba,
							    const struct fp_entry *e_in);
	int					(*fp_pop)(struct fp_sq *q, uint64_t now, struct fp_entry *out);
	/* IAU-next P4.4 (mode 5): this SQ's IAU tile group-ring membership, NULL if none; set and cleared under
	 * the transport's eng_lock (sq_eng6_attach() / sq_eng6_detach()). */
	struct iau_eng6_qb			*eng6_qb;
};

struct nvmf_vfio_user_cq {
	struct spdk_nvmf_transport_poll_group	*group;
	int					cq_ref;

	uint32_t				qid;
	/* Number of entries in queue. */
	uint32_t				size;
	struct nvme_q_mapping			mapping;
	enum nvmf_vfio_user_cq_state		cq_state;

	uint32_t				tail;
	volatile uint32_t			*dbl_headp;

	bool					phase;

	uint16_t				iv;
	bool					ien;

	/* Number of outstanding IOs that will complete in this queue. */
	size_t					nr_outstanding;

	uint32_t				last_head;
	uint32_t				last_trigger_irq_tail;
};

struct nvmf_vfio_user_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	TAILQ_ENTRY(nvmf_vfio_user_poll_group)	link;
	TAILQ_HEAD(, nvmf_vfio_user_sq)		sqs;
	uint32_t				lcore;	/* reactor core this group was created on */
	struct spdk_interrupt			*intr;
	int					intr_fd;
	struct {

		/*
		 * ctrlr_intr and ctrlr_kicks will be zero for all other poll
		 * groups. However, they can be zero even for the poll group
		 * the controller belongs are if no vfio-user message has been
		 * received or the controller hasn't been kicked yet.
		 */

		/*
		 * Number of times vfio_user_ctrlr_intr() has run:
		 * vfio-user file descriptor has been ready or explicitly
		 * kicked (see below).
		 */
		uint64_t ctrlr_intr;

		/*
		 * Kicks to the controller by ctrlr_kick().
		 * ctrlr_intr - ctrlr_kicks is the number of times the
		 * vfio-user poll file descriptor has been ready.
		 */
		uint64_t ctrlr_kicks;

		/*
		 * Number of times this poll group was kicked.
		 */
		uint64_t pg_kicks;

		/*
		 * How many times we won the race arming an SQ.
		 */
		uint64_t won;

		/*
		 * How many times we lost the race arming an SQ
		 */
		uint64_t lost;

		/*
		 * How many requests we processed in total each time we lost
		 * the rearm race.
		 */
		uint64_t lost_count;

		/*
		 * Number of attempts we attempted to rearm all the SQs in the
		 * poll group.
		 */
		uint64_t rearms;

		/*
		 * Number of times we had to apply flow control to this SQ.
		 */
		uint64_t cq_full;

		uint64_t pg_process_count;
		uint64_t intr;
		uint64_t polls;
		uint64_t polls_spurious;
		uint64_t poll_reqs;
		uint64_t poll_reqs_squared;
		uint64_t cqh_admin_writes;
		uint64_t cqh_io_writes;
	} stats;

	/* Whether this PG needs kicking to wake up again. */
	bool need_kick;
};

struct nvmf_vfio_user_shadow_doorbells {
	volatile uint32_t			*shadow_doorbells;
	volatile uint32_t			*eventidxs;
	dma_sg_t				*sgs;
	struct iovec				*iovs;
};

struct nvmf_vfio_user_ctrlr {
	struct nvmf_vfio_user_endpoint		*endpoint;
	struct nvmf_vfio_user_transport		*transport;

	/* Fast-path device model config (loaded at controller creation; invalid IAU_DEV_* fails it) */
	bool					fp_cfg_loaded;
	bool					fp_on;
	uint32_t				fp_channels;
	uint64_t				fp_lmin_ns, fp_tmax;
	uint64_t				fp_svc_ticks, fp_lmin_ticks;

	/* IAU Mode-D engine (mode 0 = inert) */
	struct iau_ctx				iau;

	/* Connected SQs list */
	TAILQ_HEAD(, nvmf_vfio_user_sq)		connected_sqs;
	enum nvmf_vfio_user_ctrlr_state		state;

	struct spdk_thread			*thread;
	struct spdk_poller			*vfu_ctx_poller;
	struct spdk_interrupt			*intr;
	int					intr_fd;

	bool					queued_quiesce;

	bool					reset_shn;
	bool					disconnect;

	uint16_t				cntlid;
	struct spdk_nvmf_ctrlr			*ctrlr;

	struct nvmf_vfio_user_sq		*sqs[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];
	struct nvmf_vfio_user_cq		*cqs[NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR];

	TAILQ_ENTRY(nvmf_vfio_user_ctrlr)	link;

	volatile uint32_t			*bar0_doorbells;
	struct nvmf_vfio_user_shadow_doorbells	*sdbl;
	/*
	 * Shadow doorbells PRPs to provide during the stop-and-copy state.
	 */
	uint64_t				shadow_doorbell_buffer;
	uint64_t				eventidx_buffer;

	bool					adaptive_irqs_enabled;

	/* IAU-next P3: per-controller device model. dev_idx = the endpoint's number (vfu<k>, -1 if
	 * none), which selects the IAU_DEV_*_C<k> overrides. With IAU_DEV_MODEL=ctrlr every I/O SQ of
	 * this controller shares fp_chan and is polled by io_pg, whose thread binds each SQ to
	 * fp_chan and claims it when the SQ connects there (sq_fp_bind()). fp_chan is embedded here,
	 * so it outlives every SQ: free_ctrlr() finishes all SQs before _free_ctrlr(). Kept at the end
	 * of the struct so the offsets of the P1 fields (iau, cqs, bar0_doorbells, ...) are unchanged. */
	int					dev_idx;
	bool					fp_model_ctrlr;
	struct fp_chan				fp_chan;
	struct nvmf_vfio_user_poll_group	*io_pg;
	/* IAU-next P4.4: the DEV_FAULT line of this controller was printed (sq_fp_connect()). */
	bool					dev_fault_noted;
	/* IAU-next P4.4 (review I2): this controller incarnation's tile key (eng_lock; 0 = none yet): set at its
	 * first tile bind, reset when the controller is disabled, so a Delete I/O CQ names only the current
	 * incarnation's routes (iau_tile_ctrlr_key()). */
	uint64_t				tile_key;
};

/* Endpoint in vfio-user is associated with a socket file, which
 * is the representative of a PCI endpoint.
 */
struct nvmf_vfio_user_endpoint {
	struct nvmf_vfio_user_transport		*transport;
	vfu_ctx_t				*vfu_ctx;
	struct spdk_poller			*accept_poller;
	struct spdk_thread			*accept_thread;
	bool					interrupt_mode;
	struct msixcap				*msix;
	vfu_pci_config_space_t			*pci_config_space;
	int					devmem_fd;
	int					accept_intr_fd;
	struct spdk_interrupt			*accept_intr;

	volatile uint32_t			*bar0;
	volatile uint32_t			*bar0_doorbells;
	/* IAU uncore page: BAR0 + 0x5000 inside the shared devmem_fd mapping */
	volatile uint8_t			*iau_page;

	struct spdk_nvme_transport_id		trid;
	struct spdk_nvmf_subsystem		*subsystem;

	/* Controller is associated with an active socket connection,
	 * the lifecycle of the controller is same as the VM.
	 * Currently we only support one active connection, as the NVMe
	 * specification defines, we may support multiple controllers in
	 * future, so that it can support e.g: RESERVATION.
	 */
	struct nvmf_vfio_user_ctrlr		*ctrlr;
	pthread_mutex_t				lock;

	bool					need_async_destroy;
	/* Start the accept poller again after destroying the controller */
	bool					need_relisten;

	TAILQ_ENTRY(nvmf_vfio_user_endpoint)	link;
};

struct nvmf_vfio_user_transport_opts {
	bool					disable_mappable_bar0;
	bool					disable_adaptive_irq;
	bool					disable_shadow_doorbells;
	bool					disable_compare;
	bool					enable_intr_mode_sq_spreading;
};

struct nvmf_vfio_user_transport {
	struct spdk_nvmf_transport		transport;
	struct nvmf_vfio_user_transport_opts    transport_opts;
	bool					intr_mode_supported;
	pthread_mutex_t				lock;
	TAILQ_HEAD(, nvmf_vfio_user_endpoint)	endpoints;

	pthread_mutex_t				pg_lock;
	TAILQ_HEAD(, nvmf_vfio_user_poll_group)	poll_groups;
	struct nvmf_vfio_user_poll_group	*next_pg;
	bool					pg_by_qid;	/* IAU_PG_BY_QID: map I/O qid -> poll group by lcore rank */

	/* IAU-next P3.9: IAU-engine threads (IAU_UNCORE_MODE=5). engines[] is fixed from transport create
	 * to destroy; eng_lock orders every bind and unbind and the engines' n_bound. */
	pthread_mutex_t				eng_lock;
	struct iau_engine_thread		**engines;
	uint32_t				n_engines;

	/* IAU-next P4.4: the IAU tile group rings bound on this transport (eng_lock), the tile engine's fault knob
	 * (IAU_ENG_FAULT_ERR_*, mode 5) and the tick -> us clock of the engine threads (iau_faults_init()). */
	TAILQ_HEAD(, iau_tile_gring)		grings;
	struct iau_eng6_fault			eng6_fault;
	struct iau_tile_clock			eng6_clock;
	uint64_t				tile_key_next;	/* eng_lock: the last controller key handed out */
	/* IAU-next P4.10b: IAU_TILE_CTX_SPILL (mode 5; iau_faults_init()) and the GCB ctx regions the knob uses
	 * (eng_lock): one map of route blocks per GCB, referenced by its binds in flight and bound members (P4.10e). */
	bool					eng6_spill;
	struct iau_tile_ctxgcb_list			ctxgcbs;
};

/*
 * function prototypes
 */
static void nvmf_vfio_user_req_free(struct spdk_nvmf_request *req);

static struct nvmf_vfio_user_req *get_nvmf_vfio_user_req(struct nvmf_vfio_user_sq *sq);

/*
 * Local process virtual address of a queue.
 */
static inline void *
q_addr(struct nvme_q_mapping *mapping)
{
	return mapping->iov.iov_base;
}

static inline int
queue_index(uint16_t qid, bool is_cq)
{
	return (qid * 2) + is_cq;
}

static inline volatile uint32_t *
sq_headp(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);
	return &sq->head;
}

static inline volatile uint32_t *
sq_dbl_tailp(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);
	return sq->dbl_tailp;
}

static inline volatile uint32_t *
cq_dbl_headp(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);
	return cq->dbl_headp;
}

static inline volatile uint32_t *
cq_tailp(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);
	return &cq->tail;
}

static inline void
sq_head_advance(struct nvmf_vfio_user_sq *sq)
{
	assert(sq != NULL);

	assert(*sq_headp(sq) < sq->size);
	(*sq_headp(sq))++;

	if (spdk_unlikely(*sq_headp(sq) == sq->size)) {
		*sq_headp(sq) = 0;
	}
}

static inline void
cq_tail_advance(struct nvmf_vfio_user_cq *cq)
{
	assert(cq != NULL);

	assert(*cq_tailp(cq) < cq->size);
	(*cq_tailp(cq))++;

	if (spdk_unlikely(*cq_tailp(cq) == cq->size)) {
		*cq_tailp(cq) = 0;
		cq->phase = !cq->phase;
	}
}

static bool
io_q_exists(struct nvmf_vfio_user_ctrlr *vu_ctrlr, const uint16_t qid, const bool is_cq)
{
	assert(vu_ctrlr != NULL);

	if (qid == 0 || qid >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR) {
		return false;
	}

	if (is_cq) {
		if (vu_ctrlr->cqs[qid] == NULL) {
			return false;
		}

		return (vu_ctrlr->cqs[qid]->cq_state != VFIO_USER_CQ_DELETED &&
			vu_ctrlr->cqs[qid]->cq_state != VFIO_USER_CQ_UNUSED);
	}

	if (vu_ctrlr->sqs[qid] == NULL) {
		return false;
	}

	return (vu_ctrlr->sqs[qid]->sq_state != VFIO_USER_SQ_DELETED &&
		vu_ctrlr->sqs[qid]->sq_state != VFIO_USER_SQ_UNUSED);
}

static char *
endpoint_id(struct nvmf_vfio_user_endpoint *endpoint)
{
	return endpoint->trid.traddr;
}

static char *
ctrlr_id(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	if (!ctrlr || !ctrlr->endpoint) {
		return "Null Ctrlr";
	}

	return endpoint_id(ctrlr->endpoint);
}

/* Return the poll group for the admin queue of the controller. */
static inline struct nvmf_vfio_user_poll_group *
ctrlr_to_poll_group(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	return SPDK_CONTAINEROF(vu_ctrlr->sqs[0]->group,
				struct nvmf_vfio_user_poll_group,
				group);
}

static inline struct nvmf_vfio_user_poll_group *
sq_to_poll_group(struct nvmf_vfio_user_sq *sq)
{
	return SPDK_CONTAINEROF(sq->group, struct nvmf_vfio_user_poll_group,
				group);
}

static inline struct spdk_thread *
poll_group_to_thread(struct nvmf_vfio_user_poll_group *vu_pg)
{
	return vu_pg->group.group->thread;
}

static dma_sg_t *
index_to_sg_t(void *arr, size_t i)
{
	return (dma_sg_t *)((uintptr_t)arr + i * dma_sg_size());
}

static inline bool
in_interrupt_mode(struct nvmf_vfio_user_transport *vu_transport)
{
	return spdk_interrupt_mode_is_enabled() &&
	       vu_transport->intr_mode_supported;
}

static int vfio_user_ctrlr_intr(void *ctx);

static void
vfio_user_ctrlr_intr_msg(void *ctx)
{
	vfio_user_ctrlr_intr(ctx);
}

/*
 * Kick (force a wakeup) of all poll groups for this controller.
 * vfio_user_ctrlr_intr() itself arranges for kicking other poll groups if
 * needed.
 */
static void
ctrlr_kick(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct nvmf_vfio_user_poll_group *vu_ctrlr_group;

	SPDK_DEBUGLOG(vfio_user_db, "%s: kicked\n", ctrlr_id(vu_ctrlr));

	vu_ctrlr_group = ctrlr_to_poll_group(vu_ctrlr);

	vu_ctrlr_group->stats.ctrlr_kicks++;

	spdk_thread_send_msg(poll_group_to_thread(vu_ctrlr_group),
			     vfio_user_ctrlr_intr_msg, vu_ctrlr);
}

/*
 * Force a wake-up for this particular poll group and its contained SQs.
 */
static void
poll_group_kick(struct nvmf_vfio_user_poll_group *vu_group)
{
	vu_group->stats.pg_kicks++;
	assert(vu_group->need_kick);
	vu_group->need_kick = false;
	eventfd_write(vu_group->intr_fd, 1);
}

/*
 * Make the given DMA address and length available (locally mapped) via iov.
 */
static void *
map_one(vfu_ctx_t *ctx, uint64_t addr, uint64_t len, dma_sg_t *sg,
	struct iovec *iov, int32_t flags)
{
	int prot = PROT_READ;
	int ret;

	if (flags & MAP_RW) {
		prot |= PROT_WRITE;
	}

	assert(ctx != NULL);
	assert(sg != NULL);
	assert(iov != NULL);

	ret = vfu_addr_to_sgl(ctx, (void *)(uintptr_t)addr, len, sg, 1, prot);
	if (ret < 0) {
		if (ret == -1) {
			if (!(flags & MAP_QUIET)) {
				SPDK_ERRLOG("failed to translate IOVA [%#lx, %#lx) (prot=%d) to local VA: %m\n",
					    addr, addr + len, prot);
			}
		} else {
			SPDK_ERRLOG("failed to translate IOVA [%#lx, %#lx) (prot=%d) to local VA: %d segments needed\n",
				    addr, addr + len, prot, -(ret + 1));
		}
		return NULL;
	}

	ret = vfu_sgl_get(ctx, sg, iov, 1, 0);
	if (ret != 0) {
		SPDK_ERRLOG("failed to get iovec for IOVA [%#lx, %#lx): %m\n",
			    addr, addr + len);
		return NULL;
	}

	assert(iov->iov_base != NULL);
	return iov->iov_base;
}

static int
nvme_cmd_map_prps(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs,
		  uint32_t max_iovcnt, uint32_t len, size_t mps,
		  void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	uint64_t prp1, prp2;
	void *vva;
	uint32_t i;
	uint32_t residue_len, nents;
	uint64_t *prp_list;
	uint32_t iovcnt;

	assert(max_iovcnt > 0);

	prp1 = cmd->dptr.prp.prp1;
	prp2 = cmd->dptr.prp.prp2;

	/* PRP1 may started with unaligned page address */
	residue_len = mps - (prp1 % mps);
	residue_len = spdk_min(len, residue_len);

	vva = gpa_to_vva(prv, prp1, residue_len, MAP_RW);
	if (spdk_unlikely(vva == NULL)) {
		SPDK_ERRLOG("GPA to VVA failed\n");
		return -EINVAL;
	}
	len -= residue_len;
	if (len && max_iovcnt < 2) {
		SPDK_ERRLOG("Too many page entries, at least two iovs are required\n");
		return -ERANGE;
	}
	iovs[0].iov_base = vva;
	iovs[0].iov_len = residue_len;

	if (len) {
		if (spdk_unlikely(prp2 == 0)) {
			SPDK_ERRLOG("no PRP2, %d remaining\n", len);
			return -EINVAL;
		}

		if (len <= mps) {
			/* 2 PRP used */
			iovcnt = 2;
			vva = gpa_to_vva(prv, prp2, len, MAP_RW);
			if (spdk_unlikely(vva == NULL)) {
				SPDK_ERRLOG("no VVA for %#" PRIx64 ", len%#x\n",
					    prp2, len);
				return -EINVAL;
			}
			iovs[1].iov_base = vva;
			iovs[1].iov_len = len;
		} else {
			/* PRP list used */
			nents = (len + mps - 1) / mps;
			if (spdk_unlikely(nents + 1 > max_iovcnt)) {
				SPDK_ERRLOG("Too many page entries\n");
				return -ERANGE;
			}

			vva = gpa_to_vva(prv, prp2, nents * sizeof(*prp_list), MAP_R);
			if (spdk_unlikely(vva == NULL)) {
				SPDK_ERRLOG("no VVA for %#" PRIx64 ", nents=%#x\n",
					    prp2, nents);
				return -EINVAL;
			}
			prp_list = vva;
			i = 0;
			while (len != 0) {
				residue_len = spdk_min(len, mps);
				vva = gpa_to_vva(prv, prp_list[i], residue_len, MAP_RW);
				if (spdk_unlikely(vva == NULL)) {
					SPDK_ERRLOG("no VVA for %#" PRIx64 ", residue_len=%#x\n",
						    prp_list[i], residue_len);
					return -EINVAL;
				}
				iovs[i + 1].iov_base = vva;
				iovs[i + 1].iov_len = residue_len;
				len -= residue_len;
				i++;
			}
			iovcnt = i + 1;
		}
	} else {
		/* 1 PRP used */
		iovcnt = 1;
	}

	assert(iovcnt <= max_iovcnt);
	return iovcnt;
}

static int
nvme_cmd_map_sgls_data(void *prv, struct spdk_nvme_sgl_descriptor *sgls, uint32_t num_sgls,
		       struct iovec *iovs, uint32_t max_iovcnt,
		       void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	uint32_t i;
	void *vva;

	if (spdk_unlikely(max_iovcnt < num_sgls)) {
		return -ERANGE;
	}

	for (i = 0; i < num_sgls; i++) {
		if (spdk_unlikely(sgls[i].unkeyed.type != SPDK_NVME_SGL_TYPE_DATA_BLOCK)) {
			SPDK_ERRLOG("Invalid SGL type %u\n", sgls[i].unkeyed.type);
			return -EINVAL;
		}
		vva = gpa_to_vva(prv, sgls[i].address, sgls[i].unkeyed.length, MAP_RW);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}
		iovs[i].iov_base = vva;
		iovs[i].iov_len = sgls[i].unkeyed.length;
	}

	return num_sgls;
}

static int
nvme_cmd_map_sgls(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs, uint32_t max_iovcnt,
		  uint32_t len, size_t mps,
		  void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	struct spdk_nvme_sgl_descriptor *sgl, *last_sgl;
	uint32_t num_sgls, seg_len;
	void *vva;
	int ret;
	uint32_t total_iovcnt = 0;

	/* SGL cases */
	sgl = &cmd->dptr.sgl1;

	/* only one SGL segment */
	if (sgl->unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
		assert(max_iovcnt > 0);
		vva = gpa_to_vva(prv, sgl->address, sgl->unkeyed.length, MAP_RW);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}
		iovs[0].iov_base = vva;
		iovs[0].iov_len = sgl->unkeyed.length;
		assert(sgl->unkeyed.length == len);

		return 1;
	}

	for (;;) {
		if (spdk_unlikely((sgl->unkeyed.type != SPDK_NVME_SGL_TYPE_SEGMENT) &&
				  (sgl->unkeyed.type != SPDK_NVME_SGL_TYPE_LAST_SEGMENT))) {
			SPDK_ERRLOG("Invalid SGL type %u\n", sgl->unkeyed.type);
			return -EINVAL;
		}

		seg_len = sgl->unkeyed.length;
		if (spdk_unlikely(seg_len % sizeof(struct spdk_nvme_sgl_descriptor))) {
			SPDK_ERRLOG("Invalid SGL segment len %u\n", seg_len);
			return -EINVAL;
		}

		num_sgls = seg_len / sizeof(struct spdk_nvme_sgl_descriptor);
		vva = gpa_to_vva(prv, sgl->address, sgl->unkeyed.length, MAP_R);
		if (spdk_unlikely(vva == NULL)) {
			SPDK_ERRLOG("GPA to VVA failed\n");
			return -EINVAL;
		}

		/* sgl point to the first segment */
		sgl = (struct spdk_nvme_sgl_descriptor *)vva;
		last_sgl = &sgl[num_sgls - 1];

		/* we are done */
		if (last_sgl->unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			/* map whole sgl list */
			ret = nvme_cmd_map_sgls_data(prv, sgl, num_sgls, &iovs[total_iovcnt],
						     max_iovcnt - total_iovcnt, gpa_to_vva);
			if (spdk_unlikely(ret < 0)) {
				return ret;
			}
			total_iovcnt += ret;

			return total_iovcnt;
		}

		if (num_sgls > 1) {
			/* map whole sgl exclude last_sgl */
			ret = nvme_cmd_map_sgls_data(prv, sgl, num_sgls - 1, &iovs[total_iovcnt],
						     max_iovcnt - total_iovcnt, gpa_to_vva);
			if (spdk_unlikely(ret < 0)) {
				return ret;
			}
			total_iovcnt += ret;
		}

		/* move to next level's segments */
		sgl = last_sgl;
	}

	return 0;
}

static int
nvme_map_cmd(void *prv, struct spdk_nvme_cmd *cmd, struct iovec *iovs, uint32_t max_iovcnt,
	     uint32_t len, size_t mps,
	     void *(*gpa_to_vva)(void *prv, uint64_t addr, uint64_t len, uint32_t flags))
{
	if (cmd->psdt == SPDK_NVME_PSDT_PRP) {
		return nvme_cmd_map_prps(prv, cmd, iovs, max_iovcnt, len, mps, gpa_to_vva);
	}

	return nvme_cmd_map_sgls(prv, cmd, iovs, max_iovcnt, len, mps, gpa_to_vva);
}

/*
 * For each queue, update the location of its doorbell to the correct location:
 * either our own BAR0, or the guest's configured shadow doorbell area.
 *
 * The Admin queue (qid: 0) does not ever use shadow doorbells.
 */
static void
vfio_user_ctrlr_switch_doorbells(struct nvmf_vfio_user_ctrlr *ctrlr, bool shadow)
{
	volatile uint32_t *doorbells = shadow ? ctrlr->sdbl->shadow_doorbells :
				       ctrlr->bar0_doorbells;

	assert(doorbells != NULL);

	for (size_t i = 1; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; i++) {
		struct nvmf_vfio_user_sq *sq = ctrlr->sqs[i];
		struct nvmf_vfio_user_cq *cq = ctrlr->cqs[i];

		if (sq != NULL) {
			sq->dbl_tailp = doorbells + queue_index(sq->qid, false);

			ctrlr->sqs[i]->need_rearm = shadow;
		}

		if (cq != NULL) {
			cq->dbl_headp = doorbells + queue_index(cq->qid, true);
		}
	}
}

static void
unmap_sdbl(vfu_ctx_t *vfu_ctx, struct nvmf_vfio_user_shadow_doorbells *sdbl)
{
	assert(vfu_ctx != NULL);
	assert(sdbl != NULL);

	/*
	 * An allocation error would result in only one of the two being
	 * non-NULL.  If that is the case, no memory should have been mapped.
	 */
	if (sdbl->iovs == NULL || sdbl->sgs == NULL) {
		return;
	}

	for (size_t i = 0; i < NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT; ++i) {
		struct iovec *iov;
		dma_sg_t *sg;

		if (!sdbl->iovs[i].iov_len) {
			continue;
		}

		sg = index_to_sg_t(sdbl->sgs, i);
		iov = sdbl->iovs + i;

		vfu_sgl_put(vfu_ctx, sg, iov, 1);
	}
}

static void
free_sdbl(vfu_ctx_t *vfu_ctx, struct nvmf_vfio_user_shadow_doorbells *sdbl)
{
	if (sdbl == NULL) {
		return;
	}

	unmap_sdbl(vfu_ctx, sdbl);

	/*
	 * sdbl->shadow_doorbells and sdbl->eventidxs were mapped,
	 * not allocated, so don't free() them.
	 */
	free(sdbl->sgs);
	free(sdbl->iovs);
	free(sdbl);
}

static struct nvmf_vfio_user_shadow_doorbells *
map_sdbl(vfu_ctx_t *vfu_ctx, uint64_t prp1, uint64_t prp2, size_t len)
{
	struct nvmf_vfio_user_shadow_doorbells *sdbl = NULL;
	dma_sg_t *sg2 = NULL;
	void *p;

	assert(vfu_ctx != NULL);

	sdbl = calloc(1, sizeof(*sdbl));
	if (sdbl == NULL) {
		goto err;
	}

	sdbl->sgs = calloc(NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT, dma_sg_size());
	sdbl->iovs = calloc(NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT, sizeof(*sdbl->iovs));
	if (sdbl->sgs == NULL || sdbl->iovs == NULL) {
		goto err;
	}

	/* Map shadow doorbell buffer (PRP1). */
	p = map_one(vfu_ctx, prp1, len, sdbl->sgs, sdbl->iovs, MAP_RW);

	if (p == NULL) {
		goto err;
	}

	/*
	 * Map eventidx buffer (PRP2).
	 * Should only be written to by the controller.
	 */

	sg2 = index_to_sg_t(sdbl->sgs, 1);

	p = map_one(vfu_ctx, prp2, len, sg2, sdbl->iovs + 1, MAP_RW);

	if (p == NULL) {
		goto err;
	}

	sdbl->shadow_doorbells = (uint32_t *)sdbl->iovs[0].iov_base;
	sdbl->eventidxs = (uint32_t *)sdbl->iovs[1].iov_base;

	return sdbl;

err:
	free_sdbl(vfu_ctx, sdbl);
	return NULL;
}

/*
 * Copy doorbells from one buffer to the other, during switches between BAR0
 * doorbells and shadow doorbells.
 */
static void
copy_doorbells(struct nvmf_vfio_user_ctrlr *ctrlr,
	       const volatile uint32_t *from, volatile uint32_t *to)
{
	assert(ctrlr != NULL);
	assert(from != NULL);
	assert(to != NULL);

	SPDK_DEBUGLOG(vfio_user_db,
		      "%s: migrating shadow doorbells from %p to %p\n",
		      ctrlr_id(ctrlr), from, to);

	/* Can't use memcpy because it doesn't respect volatile semantics. */
	for (size_t i = 0; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; ++i) {
		if (ctrlr->sqs[i] != NULL) {
			to[queue_index(i, false)] = from[queue_index(i, false)];
		}

		if (ctrlr->cqs[i] != NULL) {
			to[queue_index(i, true)] = from[queue_index(i, true)];
		}
	}
}

static void
fail_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	const struct spdk_nvmf_registers *regs;

	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	regs = spdk_nvmf_ctrlr_get_regs(vu_ctrlr->ctrlr);
	if (regs->csts.bits.cfs == 0) {
		SPDK_ERRLOG(":%s failing controller\n", ctrlr_id(vu_ctrlr));
	}

	nvmf_ctrlr_set_fatal_status(vu_ctrlr->ctrlr);
}

static inline bool
ctrlr_interrupt_enabled(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->endpoint != NULL);

	vfu_pci_config_space_t *pci = vu_ctrlr->endpoint->pci_config_space;

	return (!pci->hdr.cmd.id || vu_ctrlr->endpoint->msix->mxc.mxe);
}

static void
nvmf_vfio_user_destroy_endpoint(struct nvmf_vfio_user_endpoint *endpoint)
{
	SPDK_DEBUGLOG(nvmf_vfio, "destroy endpoint %s\n", endpoint_id(endpoint));

	spdk_interrupt_unregister(&endpoint->accept_intr);
	spdk_poller_unregister(&endpoint->accept_poller);

	if (endpoint->bar0) {
		munmap((void *)endpoint->bar0, NVME_REG_BAR0_SIZE);
		endpoint->bar0 = NULL;
		endpoint->bar0_doorbells = NULL;
		endpoint->iau_page = NULL;
	}

	if (endpoint->devmem_fd > 0) {
		close(endpoint->devmem_fd);
	}

	if (endpoint->vfu_ctx) {
		vfu_destroy_ctx(endpoint->vfu_ctx);
	}

	pthread_mutex_destroy(&endpoint->lock);
	free(endpoint);
}

/*
 * IAU-next P3.9: IAU-engine threads (IAU_UNCORE_MODE=5, arm I1e; spec §3.4, plan D1/D2/D4). The target
 * splits into SSD threads (the nvmf poll groups, running the mode 0 NVMe fast path byte for byte) and
 * IAU-engine threads, which do the IAU's work through the SSD's own I/O SQ/CQ.
 *
 * Threads. One SPDK thread per core of IAU_ENGINE_CPUS, named iau_engine<e> (device_stats.py books its
 * busy/idle as an engine), created at transport create with a one-core cpumask and bound, so it runs on
 * that reactor only. start_target.sh makes those cores reactors and keeps the poll groups off them
 * (nvmf_set_config -m); vu_dev_cfg_check() fails a listener or a controller if a poll group sits on an
 * engine core. Each engine runs one busy poller over the queues bound to it: iau_eng_q_poll() each.
 *
 * Ownership. An engine thread owns the engine state of its queues (struct iau_eng_q) and its list of
 * bound queues; no other thread touches either while a queue is bound. The SSD side of a queue is an
 * ordinary I/O SQ/CQ pair, created by the host through the admin queue and polled by its poll-group
 * thread as in mode 0; the engine thread is that pair's host: it writes SQEs into the SSD SQ (the
 * target's map of host memory), spdk_wmb, stores the SQ tail doorbell; it loads a CQE's phase word, then
 * the rest, and stores the CQ-head doorbell after consuming (vfio_user_iau_engine.c), the protocol a host
 * and the SSD thread share. The host itself rings DESC_TAIL[q] in the IAU page and never writes the SSD
 * doorbells of an engine-bound queue (its CQ-head doorbell points at the CQ sink, which must stay 0).
 *
 * Bind and unbind are SPDK messages to the engine thread with a synchronous handshake (iau_eng_hs_*,
 * vfio_user_iau_engine.h), issued under the transport's eng_lock:
 *  - bind (sq_eng_attach() from handle_queue_connect_rsp(), on the SQ's poll-group thread, before the
 *    Create I/O SQ completion is posted, so before the host can ring DESC_TAIL[q]): the requester maps
 *    the rings, iau_eng_q_init()s the queue (a refusal fails the Create I/O SQ), zeroes DESC_TAIL[q] and
 *    the CQ sink, sends the bind message to engine (k + qid - 1) mod n_engines (k = the controller's
 *    endpoint number vfu<k>, 0 if it has none; the same engine every run, the engine= of
 *    IAU_ENGINE_BIND) and waits; the engine thread appends the queue to its list and prints
 *    IAU_ENGINE_BIND;
 *  - unbind (sq_eng_detach(), the first step of every path that takes a queue's memory away or changes
 *    its doorbells or the IAU page: delete_sq_done() for Delete I/O SQ and for controller reset,
 *    shutdown or disconnect; memory_region_remove_cb() when the host unregisters a DMA region under one
 *    of the queue's four rings (ctrlr_eng_detach_region()); disable_ctrlr() before the IAU page is
 *    zeroed and handle_queue_connect_rsp() before it frees the controller on a failed qpair connect
 *    (ctrlr_eng_detach_all()). free_ctrlr() needs no hook otherwise: a bound SQ is on connected_sqs
 *    from its bind (under endpoint->lock) until close_qpair() unbinds it in delete_sq_done(), and the
 *    other callers free a controller only once that list is empty):
 *    the requester sends the unbind message and waits; the engine thread drops the queue from its list
 *    (it is never polled again), prints IAU_ENGINE_STATS while the doorbells and the CQ sink are as it
 *    left them, iau_eng_q_fini()s it and publishes DONE. Only then does the requester release the ring
 *    maps and go on with the unmap or the doorbell and page resets.
 * An engine thread runs its messages before its pollers on every iteration, so a request is served
 * within one poll pass (each queue's poll is bounded by IAU_ENG_POLL_BUDGET). A bind with no answer
 * within 1 s is abandoned (IAU_ENGINE_BIND_TIMEOUT; the Create I/O SQ fails): an abandoned bind was
 * never polled, and the engine thread disposes of it without touching shared memory. An unbind is never
 * abandoned, since the engine may be polling the very memory the caller is about to release: after 1 s
 * it prints IAU_ENGINE_DETACH_TIMEOUT once (check_l FAILs the point) and keeps waiting, and after 10 s
 * it aborts the target instead of releasing that memory. eng_lock
 * serialises binds and unbinds, so two teardown paths racing for one SQ (disable_ctrlr() and
 * close_qpair(), say) both return only after the engine has let go of it. Lock order: endpoint->lock,
 * eng_lock, pg_lock; the engine thread takes none of them. Modes 0 and 4 run none of this (every hook
 * is behind iau_mode_engine()), apart from the QCFG check at connect (sq_eng_connect()).
 *
 * Teardown. Transport destroy runs iau_engines_fini() before its endpoints munmap BAR0 (the doorbells
 * and the IAU page the engine reads). The nvmf target closes every qpair before it destroys its
 * transports, so normally no queue is bound; an engine that still has bound queues is reported ("queues
 * still bound at transport destroy", check_l FAILs it) and sent a drop-list message: it stops polling
 * all of them, prints their IAU_ENGINE_STATS and finis them, and the destroy path unbinds their SQs and
 * releases their ring maps. Then every engine is stopped and frees itself. An unbind that meets a queue
 * already dropped only answers DONE.
 *
 * Doorbells. The engine writes the SSD doorbells at the BAR0 addresses it bound, so mode 5 needs a
 * mappable BAR0 (iau_engines_create() refuses disable_mappable_bar0; the host also maps the IAU page
 * through it) and runs without shadow doorbells: a Doorbell Buffer Config never reaches
 * handle_doorbell_buffer_config() (it goes to the generic layer, which answers INVALID_OPCODE), so no
 * controller ever moves the doorbells under a bound queue. The refusal is a transport option, not a
 * check in the admin path, which is inlined into the SQ poller.
 *
 * Layout (lean GCC build). This code sits in a section of its own, .text.vfio_user_iau_engine, linked
 * right after this object's .text (as the host's .text.nvme_pcie_construct_ool), and each hook in a
 * .text function is one call. noipa: the callers compile as against an external function. no_reorder
 * keeps these functions in source order between VU_IAU_ENG_OOL_BEGIN and VU_IAU_ENG_OOL_END, which pads
 * the section to a multiple of 64 B (its alignment stays 16), so the objects linked after this one
 * (vfio_user_fastpath.o: fp_schedule, fp_pop_due) move by whole cache lines. The hooks are shaped so
 * the .text functions they grow keep the 64 B phase of every function after them: the mode 0/4 hot
 * path (nvmf_vfio_user_sq_poll, nvmf_vfio_user_poll_group_poll, post_completion, handle_cmd_req,
 * iau_stage_cpl) compiles to the same instructions as at P3.8 and moves by multiples of 64 B. Re-check
 * both after any change here (instruction compare and offsets, as in the P3.9 ledger entry).
 */
#define IAU_ENGINE_MAX		16	/* engine threads per transport */
#define IAU_ENGINE_MAX_Q	256	/* queues per engine thread */
#if defined(__GNUC__) && !defined(__clang__)
#define VU_IAU_ENG_OOL		__attribute__((noipa, no_reorder, section(".text.vfio_user_iau_engine")))
#define VU_IAU_ENG_OOL_BEGIN	\
	__asm__(".pushsection .text.vfio_user_iau_engine,\"ax\",@progbits\n.Lvu_iau_eng_ool_begin:\n\t.popsection")
#define VU_IAU_ENG_OOL_END	\
	__asm__(".pushsection .text.vfio_user_iau_engine,\"ax\",@progbits\n" \
		"\t.skip (-(. - .Lvu_iau_eng_ool_begin)) & 63, 0xcc\n\t.popsection")
#else
#define VU_IAU_ENG_OOL		__attribute__((noipa, section(".text.vfio_user_iau_engine")))
#define VU_IAU_ENG_OOL_BEGIN	SPDK_STATIC_ASSERT(1, "no OOL padding")
#define VU_IAU_ENG_OOL_END	SPDK_STATIC_ASSERT(1, "no OOL padding")
#endif
VU_IAU_ENG_OOL_BEGIN;

struct iau_engine_thread {
	struct spdk_thread	*thread;
	struct spdk_poller	*poller;
	uint32_t		index;
	uint32_t		want_lcore;
	uint32_t		lcore;		/* set by the engine thread at start (release), UINT32_MAX before */
	uint32_t		n_bound;	/* eng_lock: queues bound (bind answered, unbind not answered) */
	/* the engine thread's only */
	uint32_t		n;
	struct iau_eng_qb	*qs[IAU_ENGINE_MAX_Q];
	/* IAU-next P4.4: IAU tile group rings (the second poller, iau_engine6_poll()); the engine thread's. */
	struct spdk_poller	*poller6;
	struct iau_tile_eng_rings	rings_tile;
	struct iau_tile_clock	clock;
	bool			tile_used;
};

#define IAU_ENG_SLOT_NONE	UINT32_MAX	/* iau_eng_qb.slot: not in its engine's list */

/* One I/O SQ bound to an engine. q is the engine thread's while the queue is in its list (and the
 * requester's before the bind and after the unbind answered); the requester writes the rest before the
 * bind, and afterwards only hs and rc change, through the handshake, and slot, on the engine thread. */
struct iau_eng_qb {
	struct iau_eng_q		q;
	struct iau_engine_thread	*eng;
	struct nvmf_vfio_user_ctrlr	*ctrlr;		/* for the transport-destroy drop list */
	struct nvmf_vfio_user_sq	*sq;
	const void			*mem[4];	/* desc ring, record ring, SSD SQ, SSD CQ: what DMA unmaps */
	int				ctrlr_idx;
	uint16_t			qid;
	uint32_t			slot;		/* engine thread: index in eng->qs[], or IAU_ENG_SLOT_NONE */
	uint32_t			hs;		/* IAU_ENG_HS_* */
	int				rc;		/* bind: the engine thread's answer */
};

/* The drop-list request of transport destroy: the engine thread moves every queue of its list here. */
struct iau_eng_drop {
	struct iau_engine_thread	*eng;
	uint32_t			hs;
	uint32_t			n;
	struct iau_eng_qb		*qs[IAU_ENGINE_MAX_Q];
};

static void sq_eng_unmap(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq);
/* IAU-next P4.4 (IAU tile on the engine threads; defined after sq_eng_connect()) */
static void iau_eng6_destroy_all(struct nvmf_vfio_user_transport *vt);
static int iau_faults_init(struct nvmf_vfio_user_transport *vt);
static void sq_eng6_detach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, const void *s,
			   const void *e);
static void ctrlr_eng6_disable(struct nvmf_vfio_user_ctrlr *ctrlr);
static void ctrlr_eng6_region(struct nvmf_vfio_user_ctrlr *ctrlr, const void *s, const void *e);
static int sq_eng6_attach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, const struct iau_tile_qreg *q);
static uint16_t sq_fp_connect(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq);
static void sq_fp_fault_report(struct nvmf_vfio_user_sq *sq);
static uint64_t iau_qcfg2_read(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t qid);

static int VU_IAU_ENG_OOL
iau_engine_poll(void *arg)
{
	struct iau_engine_thread *eng = arg;
	uint32_t i, n = eng->n, work = 0;

	for (i = 0; i < n; i++) {
		work += iau_eng_q_poll(&eng->qs[i]->q);
	}
	return work != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void VU_IAU_ENG_OOL
iau_engine_start_msg(void *arg)
{
	struct iau_engine_thread *eng = arg;
	uint32_t lcore = spdk_env_get_current_core();
	char line[IAU_ENG_LINE_MAX];

	eng->poller = SPDK_POLLER_REGISTER(iau_engine_poll, eng, 0);
	if (eng->poller == NULL) {
		SPDK_ERRLOG("IAU engine %u: poller registration failed; it binds no queue\n", eng->index);
		return;
	}
	if (lcore != eng->want_lcore) {
		SPDK_ERRLOG("IAU engine %u runs on lcore %u, not on lcore %u; it binds no queue\n", eng->index, lcore,
			    eng->want_lcore);
	}
	iau_eng_fmt_start(line, sizeof(line), eng->index, lcore, eng->want_lcore);
	SPDK_NOTICELOG("%s\n", line);
	__atomic_store_n(&eng->lcore, lcore, __ATOMIC_RELEASE);
}

/* Sent only to an engine with an empty list (iau_engines_destroy()); the engine frees itself. */
static void VU_IAU_ENG_OOL
iau_engine_stop_msg(void *arg)
{
	struct iau_engine_thread *eng = arg;

	spdk_poller_unregister(&eng->poller);
	if (eng->n != 0) {
		SPDK_ERRLOG("IAU engine %u stopped with %u queues in its list\n", eng->index, eng->n);
	}
	/* P4.4: the tile poller (normally gone with the engine's last ring) and the engine's peak (D17) */
	spdk_poller_unregister(&eng->poller6);
	if (eng->rings_tile.n != 0) {
		SPDK_ERRLOG("IAU engine %u stopped with %u IAU tile rings in its list\n", eng->index, eng->rings_tile.n);
	}
	if (eng->tile_used) {
		SPDK_NOTICELOG("IAU_ENGINE6_PEAK engine=%u peak_slots=%" PRIu64 "\n", eng->index, eng->rings_tile.peak_slots);
	}
	free(eng);
	spdk_thread_exit(spdk_get_thread());
}

static void VU_IAU_ENG_OOL
iau_engine_bind_msg(void *arg)
{
	struct iau_eng_qb *b = arg;
	struct iau_engine_thread *eng = b->eng;
	char line[IAU_ENG_LINE_MAX];

	if (!iau_eng_hs_take(&b->hs)) {
		/* Abandoned: never polled, and its rings may be gone. */
		iau_eng_q_fini(&b->q);
		free(b);
		return;
	}
	if (eng->poller == NULL || eng->n == IAU_ENGINE_MAX_Q) {
		b->rc = eng->poller == NULL ? -ENODEV : -ENOSPC;
	} else {
		b->slot = eng->n;
		eng->qs[eng->n++] = b;
		b->rc = 0;
		iau_eng_q_fmt_bind(line, sizeof(line), &b->q, b->ctrlr_idx, eng->index, spdk_env_get_current_core());
		SPDK_NOTICELOG("%s\n", line);
	}
	iau_eng_hs_done(&b->hs);
}

/* Stop polling queue b (it leaves the list), print its IAU_ENGINE_STATS with the doorbells and the CQ
 * sink as the engine left them, fini it. Engine thread. */
static void VU_IAU_ENG_OOL
iau_engine_drop_one(struct iau_engine_thread *eng, struct iau_eng_qb *b)
{
	char line[IAU_ENG_LINE_MAX];
	uint32_t last = --eng->n;

	eng->qs[b->slot] = eng->qs[last];
	eng->qs[b->slot]->slot = b->slot;
	eng->qs[last] = NULL;
	b->slot = IAU_ENG_SLOT_NONE;
	iau_eng_q_fmt_stats(line, sizeof(line), &b->q, b->ctrlr_idx, eng->index);
	SPDK_NOTICELOG("%s\n", line);
	iau_eng_q_fini(&b->q);
}

static void VU_IAU_ENG_OOL
iau_engine_unbind_msg(void *arg)
{
	struct iau_eng_qb *b = arg;

	if (!iau_eng_hs_take(&b->hs)) {
		return;	/* not reached: an unbind is never abandoned */
	}
	if (b->slot != IAU_ENG_SLOT_NONE) {
		iau_engine_drop_one(b->eng, b);
	}	/* else a drop list already stopped, reported and finished it */
	iau_eng_hs_done(&b->hs);
}

static void VU_IAU_ENG_OOL
iau_engine_drop_msg(void *arg)
{
	struct iau_eng_drop *d = arg;
	struct iau_engine_thread *eng = d->eng;

	if (!iau_eng_hs_take(&d->hs)) {
		return;	/* not reached: a drop is never abandoned */
	}
	while (eng->n != 0) {
		struct iau_eng_qb *b = eng->qs[eng->n - 1];

		iau_engine_drop_one(eng, b);
		d->qs[d->n++] = b;
	}
	iau_eng_hs_done(&d->hs);
}

/*
 * Run fn(arg) on engine eng's thread and wait for its answer on *hs (the handshake above). A bind
 * (abandon) gives up after 1 s: -ETIMEDOUT, the request is the engine thread's now. An unbind or a drop
 * (!abandon) is never given up: the engine may be polling the memory the caller is about to release.
 * It prints IAU_ENGINE_DETACH_TIMEOUT once after 1 s (check_l FAILs the point) and keeps waiting; after
 * 10 s it aborts the target. Returns 0 once the engine answered.
 */
static int VU_IAU_ENG_OOL
iau_engine_call(struct iau_engine_thread *eng, uint32_t *hs, spdk_msg_fn fn, void *arg, bool abandon,
		int ctrlr_idx, uint32_t qid)
{
	uint64_t hz = spdk_get_ticks_hz(), t0;
	bool warned = false;
	int rc;

	iau_eng_hs_arm(hs);
	if (spdk_get_thread() == eng->thread) {
		fn(arg);	/* not reached: an engine core hosts no controller and no poll group */
	} else {
		spdk_thread_send_msg(eng->thread, fn, arg);	/* aborts rather than fail */
	}
	t0 = spdk_get_ticks();
	for (;;) {
		uint64_t dt = spdk_get_ticks() - t0;

		rc = iau_eng_hs_check(hs, abandon && dt > hz);
		if (rc != 0) {
			return rc == 1 ? 0 : rc;
		}
		if (!abandon && !warned && dt > hz) {
			SPDK_ERRLOG("IAU_ENGINE_DETACH_TIMEOUT ctrlr=%d qid=%u engine=%u: the engine thread has not "
				    "answered in 1 s; still waiting (it may be polling the queue's memory)\n", ctrlr_idx, qid,
				    eng->index);
			warned = true;
		}
		if (!abandon && dt > 10 * hz) {
			SPDK_ERRLOG("IAU engine %u: no answer in 10 s to an unbind of ctrlr=%d qid=%u; aborting rather "
				    "than release memory the engine thread may still be polling\n", eng->index, ctrlr_idx, qid);
			abort();
		}
		spdk_pause();
	}
}

/*
 * Transport destroy (before the endpoints munmap BAR0) and a failed create. An engine that still has
 * bound queues is reported and sent a drop list: it stops polling them and prints their STATS, and here
 * their SQs are unbound and their ring maps released (the endpoints and their vfu_ctx still exist). Then
 * every engine is stopped (it frees itself). eng_lock is left initialised: the endpoint teardown that
 * follows may run DMA-unregister callbacks, which take it (sq_eng_detach(), a no-op by then).
 */
static void VU_IAU_ENG_OOL
iau_engines_destroy(struct nvmf_vfio_user_transport *vt)
{
	uint32_t i, j;

	pthread_mutex_lock(&vt->eng_lock);
	iau_eng6_destroy_all(vt);	/* P4.4: tile rings still bound (reported, dropped) */
	for (i = 0; i < vt->n_engines; i++) {
		struct iau_engine_thread *eng = vt->engines != NULL ? vt->engines[i] : NULL;

		if (eng == NULL) {
			continue;
		}
		if (eng->n_bound != 0) {
			struct iau_eng_drop *d = calloc(1, sizeof(*d));

			SPDK_ERRLOG("IAU engine %u: %u queues still bound at transport destroy; dropping them "
				    "(IAU_ENGINE_STATS follow)\n", eng->index, eng->n_bound);
			if (d == NULL) {
				SPDK_ERRLOG("IAU engine %u: no memory for the drop list; aborting rather than unmap "
					    "memory it may be polling\n", eng->index);
				abort();
			}
			d->eng = eng;
			iau_engine_call(eng, &d->hs, iau_engine_drop_msg, d, false, -1, 0);
			for (j = 0; j < d->n; j++) {
				struct iau_eng_qb *b = d->qs[j];

				b->sq->eng_qb = NULL;
				sq_eng_unmap(b->ctrlr, b->sq);
				free(b);
			}
			eng->n_bound = 0;
			free(d);
		}
		spdk_thread_send_msg(eng->thread, iau_engine_stop_msg, eng);
	}
	free(vt->engines);
	vt->engines = NULL;
	vt->n_engines = 0;
	pthread_mutex_unlock(&vt->eng_lock);
}

/* Transport create: in mode 5, one engine thread per core of IAU_ENGINE_CPUS (devcfg_cpu_list(): no CPU
 * twice), each a reactor core other than the main core. 0 (also in modes 0 and 4: no engines) or a
 * negative errno after an ERRLOG, which fails the transport. */
static int VU_IAU_ENG_OOL
iau_engines_create(struct nvmf_vfio_user_transport *vt)
{
	const char *cpus = getenv("IAU_ENGINE_CPUS");
	uint32_t lcores[IAU_ENGINE_MAX];
	struct spdk_cpuset mask;
	struct iau_ctx ic;
	char name[32];
	int n, i, mode;

	/* The mode as controllers parse it; a bad IAU_* environment fails every listener (vu_dev_cfg_check()). */
	if (iau_ctx_init(&ic, NULL, spdk_get_ticks_hz()) != 0) {
		iau_ctx_fini(&ic);
		return 0;
	}
	mode = ic.mode;
	iau_ctx_fini(&ic);
	if (mode != IAU_MODE_ENGINE) {
		return 0;
	}
	/* The engine writes the SSD doorbells where it bound them, and the host maps the IAU page: BAR0 must
	 * be mappable and the doorbells must never move to a shadow buffer (see "Doorbells" above). */
	if (vt->transport_opts.disable_mappable_bar0) {
		SPDK_ERRLOG("IAU mode 5 needs a mappable BAR0 (disable_mappable_bar0 is set): the host maps the IAU "
			    "page and the engine writes the BAR0 doorbells\n");
		return -EINVAL;
	}
	vt->transport_opts.disable_shadow_doorbells = true;	/* already so with a mappable BAR0; stated */
	n = devcfg_cpu_list(cpus, lcores, IAU_ENGINE_MAX);
	if (n <= 0) {
		SPDK_ERRLOG("IAU mode 5: IAU_ENGINE_CPUS=%s is not a list of 1 to %d distinct CPUs\n",
			    cpus != NULL ? cpus : "(unset)", IAU_ENGINE_MAX);
		return -EINVAL;
	}
	for (i = 0; i < n; i++) {
		bool reactor = false;
		uint32_t c;

		SPDK_ENV_FOREACH_CORE(c) {
			reactor |= c == lcores[i];
		}
		if (!reactor || lcores[i] == spdk_env_get_main_core()) {
			SPDK_ERRLOG("IAU mode 5: engine CPU %u (IAU_ENGINE_CPUS=%s) must be a reactor core (-m) and not "
				    "the main core (-p)\n", lcores[i], cpus);
			return -EINVAL;
		}
	}
	vt->engines = calloc((size_t)n, sizeof(*vt->engines));
	if (vt->engines == NULL) {
		return -ENOMEM;
	}
	vt->n_engines = (uint32_t)n;
	for (i = 0; i < n; i++) {
		struct iau_engine_thread *eng = calloc(1, sizeof(*eng));

		if (eng == NULL) {
			iau_engines_destroy(vt);
			return -ENOMEM;
		}
		eng->index = (uint32_t)i;
		eng->want_lcore = lcores[i];
		eng->lcore = UINT32_MAX;
		eng->clock = vt->eng6_clock;	/* P4.4: ticks -> us once per tile pass */
		spdk_cpuset_zero(&mask);
		spdk_cpuset_set_cpu(&mask, lcores[i], true);
		snprintf(name, sizeof(name), "iau_engine%d", i);
		eng->thread = spdk_thread_create(name, &mask);
		if (eng->thread == NULL) {
			SPDK_ERRLOG("IAU mode 5: cannot create thread %s on lcore %u\n", name, lcores[i]);
			free(eng);
			iau_engines_destroy(vt);
			return -ENOMEM;
		}
		spdk_thread_bind(eng->thread, true);
		vt->engines[i] = eng;
		spdk_thread_send_msg(eng->thread, iau_engine_start_msg, eng);
	}
	SPDK_NOTICELOG("IAU mode 5: %d IAU-engine threads (IAU_ENGINE_CPUS=%s)\n", n, cpus);
	return 0;
}

/* The last step of transport create: eng_lock and the engines. Returns the transport, or NULL after
 * tearing it down as nvmf_vfio_user_create()'s `cleanup` does (one tail call in .text). */
static struct spdk_nvmf_transport *VU_IAU_ENG_OOL
iau_engines_init(struct nvmf_vfio_user_transport *vt)
{
	int err = pthread_mutex_init(&vt->eng_lock, NULL);

	if (err != 0) {
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
	} else if (iau_faults_init(vt) != 0 || iau_engines_create(vt) != 0) {	/* P4.4: the fault knobs first */
		pthread_mutex_destroy(&vt->eng_lock);
	} else {
		return &vt->transport;
	}
	pthread_mutex_destroy(&vt->lock);
	pthread_mutex_destroy(&vt->pg_lock);
	free(vt);
	return NULL;
}

/* Transport destroy, before the endpoints are destroyed (see iau_engines_destroy()). */
static void VU_IAU_ENG_OOL
iau_engines_fini(struct nvmf_vfio_user_transport *vt)
{
	iau_engines_destroy(vt);
}

/* Mode 5, for vu_dev_cfg_check(): the build has the engine, controller dev_idx has the fast path,
 * IAU_CQ_BATCH_N is 1, the transport has engine threads and no poll group (SSD thread) runs on an engine
 * core. 0, or -EINVAL after an ERRLOG prefixed with who. */
static int VU_IAU_ENG_OOL
iau_engines_check(struct nvmf_vfio_user_transport *vt, const char *who, int dev_idx, const struct iau_ctx *ic,
		  const struct devcfg *c)
{
	struct nvmf_vfio_user_poll_group *pg;
	uint32_t i, clash = UINT32_MAX;

	if (!IAU_ENGINE_AVAILABLE) {
		SPDK_ERRLOG("%s: IAU mode 5 needs the IAU engine (P3.9); this build lacks it\n", who);
		return -EINVAL;
	}
	if (!c->fastpath) {
		SPDK_ERRLOG("%s: IAU mode 5 (IAU_UNCORE_MODE=5) requires the fast path, but controller %d has "
			    "IAU_DEV_FASTPATH=0\n", who, dev_idx);
		return -EINVAL;
	}
	if (ic->batch_n != 1) {
		SPDK_ERRLOG("%s: IAU mode 5 publishes every record at once and needs IAU_CQ_BATCH_N=1 (got %u)\n",
			    who, ic->batch_n);
		return -EINVAL;
	}
	if (vt->n_engines == 0) {
		SPDK_ERRLOG("%s: IAU mode 5 needs IAU-engine threads (IAU_ENGINE_CPUS); the transport has none\n", who);
		return -EINVAL;
	}
	pthread_mutex_lock(&vt->pg_lock);
	TAILQ_FOREACH(pg, &vt->poll_groups, link) {
		for (i = 0; i < vt->n_engines; i++) {
			if (vt->engines[i] != NULL && pg->lcore == vt->engines[i]->want_lcore) {
				clash = pg->lcore;
			}
		}
	}
	pthread_mutex_unlock(&vt->pg_lock);
	if (clash != UINT32_MAX) {
		SPDK_ERRLOG("%s: IAU mode 5: an nvmf poll group runs on engine core %u; keep the poll groups off "
			    "IAU_ENGINE_CPUS (nvmf_set_config -m)\n", who, clash);
		return -EINVAL;
	}
	return 0;
}

/* called when process exits */
static void
nvmf_vfio_user_destroy(struct spdk_nvmf_transport *transport,
		       spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;

	SPDK_DEBUGLOG(nvmf_vfio, "destroy transport\n");

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	/* P3.9: the IAU engines stop, and let go of any queue still bound, before BAR0 is unmapped. */
	iau_engines_fini(vu_transport);
	pthread_mutex_destroy(&vu_transport->lock);
	pthread_mutex_destroy(&vu_transport->pg_lock);

	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		TAILQ_REMOVE(&vu_transport->endpoints, endpoint, link);
		nvmf_vfio_user_destroy_endpoint(endpoint);
	}

	free(vu_transport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static const struct spdk_json_object_decoder vfio_user_transport_opts_decoder[] = {
	{
		"disable_mappable_bar0",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_mappable_bar0),
		spdk_json_decode_bool, true
	},
	{
		"disable_adaptive_irq",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_adaptive_irq),
		spdk_json_decode_bool, true
	},
	{
		"disable_shadow_doorbells",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_shadow_doorbells),
		spdk_json_decode_bool, true
	},
	{
		"disable_compare",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.disable_compare),
		spdk_json_decode_bool, true
	},
	{
		"enable_intr_mode_sq_spreading",
		offsetof(struct nvmf_vfio_user_transport, transport_opts.enable_intr_mode_sq_spreading),
		spdk_json_decode_bool, true
	},
};

SPDK_LOG_DEPRECATION_REGISTER(disable_compare, "", "v26.05", SPDK_LOG_DEPRECATION_EVERY_24H);

static struct spdk_nvmf_transport *
nvmf_vfio_user_create(struct spdk_nvmf_transport_opts *opts)
{
	struct nvmf_vfio_user_transport *vu_transport;
	int err;

	if (opts->max_qpairs_per_ctrlr > NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR) {
		SPDK_ERRLOG("Invalid max_qpairs_per_ctrlr=%d, supported max_qpairs_per_ctrlr=%d\n",
			    opts->max_qpairs_per_ctrlr, NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR);
		return NULL;
	}

	/* IAU-next P3: a misspelt IAU_DEV_* name (unknown key, _C<k> on a process-wide key, a _C<k>
	 * that devcfg_load() never reads) would otherwise be ignored silently. The controller count
	 * is not known yet (listeners come later), so the _C<k> bound is not checked (n_ctrlr = -1). */
	{
		char dev_err[160];

		if (devcfg_scan_unknown(environ, -1, dev_err, sizeof(dev_err)) != 0) {
			SPDK_ERRLOG("IAU: invalid device-model environment: %s\n", dev_err);
			return NULL;
		}
	}

	vu_transport = calloc(1, sizeof(*vu_transport));
	if (vu_transport == NULL) {
		SPDK_ERRLOG("Transport alloc fail: %m\n");
		return NULL;
	}

	err = pthread_mutex_init(&vu_transport->lock, NULL);
	if (err != 0) {
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
		goto err;
	}
	TAILQ_INIT(&vu_transport->endpoints);

	err = pthread_mutex_init(&vu_transport->pg_lock, NULL);
	if (err != 0) {
		pthread_mutex_destroy(&vu_transport->lock);
		SPDK_ERRLOG("Pthread initialisation failed (%d)\n", err);
		goto err;
	}
	TAILQ_INIT(&vu_transport->poll_groups);
	{
		const char *pg_env = getenv("IAU_PG_BY_QID");

		vu_transport->pg_by_qid = (pg_env != NULL && pg_env[0] != '\0' && pg_env[0] != '0');
	}

	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, vfio_user_transport_opts_decoder,
					    SPDK_COUNTOF(vfio_user_transport_opts_decoder),
					    vu_transport)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		goto cleanup;
	}

	if (vu_transport->transport_opts.disable_compare) {
		SPDK_LOG_DEPRECATED(disable_compare);
	}

	/*
	 * To support interrupt mode, the transport must be configured with
	 * mappable BAR0 disabled: we need a vfio-user message to wake us up
	 * when a client writes new doorbell values to BAR0, via the
	 * libvfio-user socket fd.
	 */
	vu_transport->intr_mode_supported =
		vu_transport->transport_opts.disable_mappable_bar0;

	/*
	 * If BAR0 is mappable, it doesn't make sense to support shadow
	 * doorbells, so explicitly turn it off.
	 */
	if (!vu_transport->transport_opts.disable_mappable_bar0) {
		vu_transport->transport_opts.disable_shadow_doorbells = true;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		if (!vu_transport->intr_mode_supported) {
			SPDK_ERRLOG("interrupt mode not supported\n");
			goto cleanup;
		}

		/*
		 * If we are in interrupt mode, we cannot support adaptive IRQs,
		 * as there is no guarantee the SQ poller will run subsequently
		 * to send pending IRQs.
		 */
		vu_transport->transport_opts.disable_adaptive_irq = true;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_mappable_bar0=%d\n",
		      vu_transport->transport_opts.disable_mappable_bar0);
	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_adaptive_irq=%d\n",
		      vu_transport->transport_opts.disable_adaptive_irq);
	SPDK_DEBUGLOG(nvmf_vfio, "vfio_user transport: disable_shadow_doorbells=%d\n",
		      vu_transport->transport_opts.disable_shadow_doorbells);

	/* IAU-next P3.9: the IAU-engine threads of mode 5 (none in modes 0 and 4); NULL after the same
	 * teardown as `cleanup` below if they cannot be created. */
	return iau_engines_init(vu_transport);

cleanup:
	pthread_mutex_destroy(&vu_transport->lock);
	pthread_mutex_destroy(&vu_transport->pg_lock);
err:
	free(vu_transport);
	return NULL;
}

static uint32_t
max_queue_size(struct nvmf_vfio_user_ctrlr const *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	return vu_ctrlr->ctrlr->vcprop.cap.bits.mqes + 1;
}

static uint32_t
doorbell_stride(const struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->ctrlr != NULL);

	return vu_ctrlr->ctrlr->vcprop.cap.bits.dstrd;
}

static uintptr_t
memory_page_size(const struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	uint32_t memory_page_shift = vu_ctrlr->ctrlr->vcprop.cc.bits.mps + 12;
	return 1ul << memory_page_shift;
}

static uintptr_t
memory_page_mask(const struct nvmf_vfio_user_ctrlr *ctrlr)
{
	return ~(memory_page_size(ctrlr) - 1);
}

static int
map_q(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvme_q_mapping *mapping,
      uint32_t flags)
{
	void *ret;

	assert(mapping->len != 0);
	assert(q_addr(mapping) == NULL);

	ret = map_one(vu_ctrlr->endpoint->vfu_ctx, mapping->prp1, mapping->len,
		      mapping->sg, &mapping->iov, flags);
	if (ret == NULL) {
		return -EFAULT;
	}

	if (flags & MAP_INITIALIZE) {
		memset(q_addr(mapping), 0, mapping->len);
	}

	return 0;
}

static inline void
unmap_q(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvme_q_mapping *mapping)
{
	if (q_addr(mapping) != NULL) {
		vfu_sgl_put(vu_ctrlr->endpoint->vfu_ctx, mapping->sg,
			    &mapping->iov, 1);
		mapping->iov.iov_base = NULL;
	}
}

static int
asq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_sq *sq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);

	sq = ctrlr->sqs[0];

	assert(sq != NULL);
	assert(q_addr(&sq->mapping) == NULL);
	/* XXX ctrlr->asq == 0 is a valid memory address */

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->ctrlr);
	sq->qid = 0;
	sq->size = regs->aqa.bits.asqs + 1;
	sq->mapping.prp1 = regs->asq;
	sq->mapping.len = sq->size * sizeof(struct spdk_nvme_cmd);
	*sq_headp(sq) = 0;
	sq->cqid = 0;

	ret = map_q(ctrlr, &sq->mapping, MAP_INITIALIZE);
	if (ret) {
		return ret;
	}

	/* The Admin queue (qid: 0) does not ever use shadow doorbells. */
	sq->dbl_tailp = ctrlr->bar0_doorbells + queue_index(0, false);

	*sq_dbl_tailp(sq) = 0;

	return 0;
}

/*
 * Updates eventidx to set an SQ into interrupt or polling mode.
 *
 * Returns false if the current SQ tail does not match the SQ head, as
 * this means that the host has submitted more items to the queue while we were
 * not looking - or during the event index update. In that case, we must retry,
 * or otherwise make sure we are going to wake up again.
 */
static bool
set_sq_eventidx(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	volatile uint32_t *sq_tail_eidx;
	uint32_t old_tail, new_tail;

	assert(sq != NULL);
	assert(sq->ctrlr != NULL);
	assert(sq->ctrlr->sdbl != NULL);
	assert(sq->need_rearm);
	assert(sq->qid != 0);

	ctrlr = sq->ctrlr;

	SPDK_DEBUGLOG(vfio_user_db, "%s: updating eventidx of sqid:%u\n",
		      ctrlr_id(ctrlr), sq->qid);

	sq_tail_eidx = ctrlr->sdbl->eventidxs + queue_index(sq->qid, false);

	assert(ctrlr->endpoint != NULL);

	if (!ctrlr->endpoint->interrupt_mode) {
		/* No synchronisation necessary. */
		*sq_tail_eidx = NVMF_VFIO_USER_EVENTIDX_POLL;
		return true;
	}

	old_tail = *sq_dbl_tailp(sq);
	*sq_tail_eidx = old_tail;

	/*
	 * Ensure that the event index is updated before re-reading the tail
	 * doorbell. If it's not, then the host might race us and update the
	 * tail after the second read but before the event index is written, so
	 * it won't write to BAR0 and we'll miss the update.
	 *
	 * The driver should provide similar ordering with an mb().
	 */
	spdk_mb();

	/*
	 * Check if the host has updated the tail doorbell after we've read it
	 * for the first time, but before the event index was written. If that's
	 * the case, then we've lost the race and we need to update the event
	 * index again (after polling the queue, since the host won't write to
	 * BAR0).
	 */
	new_tail = *sq_dbl_tailp(sq);

	/*
	 * We might poll the queue straight after this function returns if the
	 * tail has been updated, so we need to ensure that any changes to the
	 * queue will be visible to us if the doorbell has been updated.
	 *
	 * The driver should provide similar ordering with a wmb() to ensure
	 * that the queue is written before it updates the tail doorbell.
	 */
	spdk_rmb();

	SPDK_DEBUGLOG(vfio_user_db, "%s: sqid:%u, old_tail=%u, new_tail=%u, "
		      "sq_head=%u\n", ctrlr_id(ctrlr), sq->qid, old_tail,
		      new_tail, *sq_headp(sq));

	if (new_tail == *sq_headp(sq)) {
		sq->need_rearm = false;
		return true;
	}

	/*
	 * We've lost the race: the tail was updated since we last polled,
	 * including if it happened within this routine.
	 *
	 * The caller should retry after polling (think of this as a cmpxchg
	 * loop); if we go to sleep while the SQ is not empty, then we won't
	 * process the remaining events.
	 */
	return false;
}

static int nvmf_vfio_user_sq_poll(struct nvmf_vfio_user_sq *sq);

/*
 * Arrange for an SQ to interrupt us if written. Returns non-zero if we
 * processed some SQ entries.
 */
static int
vfio_user_sq_rearm(struct nvmf_vfio_user_ctrlr *ctrlr,
		   struct nvmf_vfio_user_sq *sq,
		   struct nvmf_vfio_user_poll_group *vu_group)
{
	int count = 0;
	size_t i;

	assert(sq->need_rearm);

	for (i = 0; i < NVMF_VFIO_USER_SET_EVENTIDX_MAX_ATTEMPTS; i++) {
		int ret;

		if (set_sq_eventidx(sq)) {
			/* We won the race and set eventidx; done. */
			vu_group->stats.won++;
			return count;
		}

		ret = nvmf_vfio_user_sq_poll(sq);

		count += (ret < 0) ? 1 : ret;
	}

	/*
	 * We couldn't arrange an eventidx guaranteed to cause a BAR0 write, as
	 * we raced with the producer too many times; force ourselves to wake up
	 * instead. We'll process all queues at that point.
	 */
	vu_group->need_kick = true;

	SPDK_DEBUGLOG(vfio_user_db,
		      "%s: set_sq_eventidx() lost the race %zu times\n",
		      ctrlr_id(ctrlr), i);

	vu_group->stats.lost++;
	vu_group->stats.lost_count += count;

	return count;
}

/*
 * We're in interrupt mode, and potentially about to go to sleep. We need to
 * make sure any further I/O submissions are guaranteed to wake us up: for
 * shadow doorbells that means we may need to go through set_sq_eventidx() for
 * every SQ that needs re-arming.
 *
 * Returns non-zero if we processed something.
 */
static int
vfio_user_poll_group_rearm(struct nvmf_vfio_user_poll_group *vu_group)
{
	struct nvmf_vfio_user_sq *sq;
	int count = 0;

	vu_group->stats.rearms++;

	TAILQ_FOREACH(sq, &vu_group->sqs, link) {
		if (spdk_unlikely(sq->sq_state != VFIO_USER_SQ_ACTIVE || !sq->size)) {
			continue;
		}

		if (sq->need_rearm) {
			count += vfio_user_sq_rearm(sq->ctrlr, sq, vu_group);
		}
	}

	if (vu_group->need_kick) {
		poll_group_kick(vu_group);
	}

	return count;
}

static int
acq_setup(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_cq *cq;
	const struct spdk_nvmf_registers *regs;
	int ret;

	assert(ctrlr != NULL);

	cq = ctrlr->cqs[0];

	assert(cq != NULL);

	assert(q_addr(&cq->mapping) == NULL);

	regs = spdk_nvmf_ctrlr_get_regs(ctrlr->ctrlr);
	assert(regs != NULL);
	cq->qid = 0;
	cq->size = regs->aqa.bits.acqs + 1;
	cq->mapping.prp1 = regs->acq;
	cq->mapping.len = cq->size * sizeof(struct spdk_nvme_cpl);
	*cq_tailp(cq) = 0;
	cq->ien = true;
	cq->phase = true;
	cq->nr_outstanding = 0;

	ret = map_q(ctrlr, &cq->mapping, MAP_RW | MAP_INITIALIZE);
	if (ret) {
		return ret;
	}

	/* The Admin queue (qid: 0) does not ever use shadow doorbells. */
	cq->dbl_headp = ctrlr->bar0_doorbells + queue_index(0, true);

	*cq_dbl_headp(cq) = 0;

	return 0;
}

static void *
_map_one(void *prv, uint64_t addr, uint64_t len, uint32_t flags)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)prv;
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_vfio_user_req *vu_req;
	struct nvmf_vfio_user_sq *sq;
	void *ret;

	assert(req != NULL);
	qpair = req->qpair;
	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);

	assert(vu_req->iovcnt < NVMF_VFIO_USER_MAX_IOVECS);
	ret = map_one(sq->ctrlr->endpoint->vfu_ctx, addr, len,
		      index_to_sg_t(vu_req->sg, vu_req->iovcnt),
		      &vu_req->iov[vu_req->iovcnt], flags);
	if (spdk_likely(ret != NULL)) {
		vu_req->iovcnt++;
	}
	return ret;
}

static int
vfio_user_map_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req,
		  struct iovec *iov, uint32_t length)
{
	/* Map PRP list to from Guest physical memory to
	 * virtual memory address.
	 */
	return nvme_map_cmd(req, &req->cmd->nvme_cmd, iov, NVMF_REQ_MAX_BUFFERS,
			    length, 4096, _map_one);
}

static int handle_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
			  struct nvmf_vfio_user_sq *sq);

static uint32_t
cq_free_slots(struct nvmf_vfio_user_cq *cq)
{
	uint32_t free_slots;

	assert(cq != NULL);

	if (cq->tail == cq->last_head) {
		free_slots = cq->size;
	} else if (cq->tail > cq->last_head) {
		free_slots = cq->size - (cq->tail - cq->last_head);
	} else {
		free_slots = cq->last_head - cq->tail;
	}
	assert(free_slots > 0);

	return free_slots - 1;
}

/*
 * Since reading the head doorbell is relatively expensive, we use the cached
 * value, so we only have to read it for real if it appears that we are full.
 */
static inline bool
cq_is_full(struct nvmf_vfio_user_cq *cq)
{
	uint32_t free_cq_slots;

	assert(cq != NULL);

	free_cq_slots = cq_free_slots(cq);

	if (spdk_unlikely(free_cq_slots == 0)) {
		cq->last_head = *cq_dbl_headp(cq);
		free_cq_slots = cq_free_slots(cq);
	}

	return free_cq_slots == 0;
}

/*
 * Posts a CQE in the completion queue.
 *
 * @ctrlr: the vfio-user controller
 * @cq: the completion queue
 * @cdw0: cdw0 as reported by NVMf
 * @sqid: submission queue ID
 * @cid: command identifier in NVMe command
 * @sc: the NVMe CQE status code
 * @sct: the NVMe CQE status code type
 */
static int
post_completion(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq,
		uint32_t cdw0, uint16_t sqid, uint16_t cid, uint16_t sc, uint16_t sct)
{
	struct spdk_nvme_status cpl_status = { 0 };
	struct spdk_nvme_cpl *cpl;
	int err;

	assert(ctrlr != NULL);

	if (spdk_unlikely(cq == NULL || q_addr(&cq->mapping) == NULL)) {
		return 0;
	}

	if (cq->qid == 0) {
		assert(spdk_get_thread() == cq->group->group->thread);
	}

	/*
	 * As per NVMe Base spec 3.3.1.2.1, we are supposed to implement CQ flow
	 * control: that is, we should handle running out of free CQ slots.
	 *
	 * Instead, we implement this by applying flow control on the submission
	 * side: see handle_sq_tdbl_write().
	 */
	if (cq_is_full(cq)) {
		SPDK_ERRLOG("%s: cqid:%d full (tail=%d, head=%d)\n",
			    ctrlr_id(ctrlr), cq->qid, *cq_tailp(cq),
			    *cq_dbl_headp(cq));
		return -1;
	}

	cpl = ((struct spdk_nvme_cpl *)q_addr(&cq->mapping)) + *cq_tailp(cq);

	assert(ctrlr->sqs[sqid] != NULL);
	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: request complete sqid:%d cid=%d status=%#x "
		      "sqhead=%d cq tail=%d\n", ctrlr_id(ctrlr), sqid, cid, sc,
		      *sq_headp(ctrlr->sqs[sqid]), *cq_tailp(cq));

	cpl->sqhd = *sq_headp(ctrlr->sqs[sqid]);
	cpl->sqid = sqid;
	cpl->cid = cid;
	cpl->cdw0 = cdw0;

	/*
	 * This is a bitfield: instead of setting the individual bits we need
	 * directly in cpl->status, which would cause a read-modify-write cycle,
	 * we'll avoid reading from the CPL altogether by filling in a local
	 * cpl_status variable, then writing the whole thing.
	 */
	cpl_status.sct = sct;
	cpl_status.sc = sc;
	cpl_status.p = cq->phase;
	cpl->status = cpl_status;

	cq->nr_outstanding--;

	/* Ensure the Completion Queue Entry is visible. */
	spdk_wmb();
	cq_tail_advance(cq);

	if ((cq->qid == 0 || !ctrlr->adaptive_irqs_enabled) &&
	    cq->ien && ctrlr_interrupt_enabled(ctrlr)) {
		err = vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv);
		if (err != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n",
				    ctrlr_id(ctrlr));
			return err;
		}
	}

	return 0;
}

static void
free_sq_reqs(struct nvmf_vfio_user_sq *sq)
{
	while (!TAILQ_EMPTY(&sq->free_reqs)) {
		struct nvmf_vfio_user_req *vu_req = TAILQ_FIRST(&sq->free_reqs);
		TAILQ_REMOVE(&sq->free_reqs, vu_req, link);
		free(vu_req);
	}
}

static void
delete_cq_done(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq)
{
	assert(cq->cq_ref == 0);
	unmap_q(ctrlr, &cq->mapping);
	cq->size = 0;
	cq->cq_state = VFIO_USER_CQ_DELETED;
	cq->group = NULL;
	cq->nr_outstanding = 0;
}

/* Count one I/O command by opcode (P1.2), branch-free so a read/write mix adds
 * no mispredicted branch: reads (0x02) -> [0], writes (0x01) -> [1], else [2]. */
static inline void
sq_count_opc(struct nvmf_vfio_user_sq *sq, uint8_t opc)
{
	uint32_t not_read = opc != SPDK_NVME_OPC_READ;

	sq->opc_cnt[not_read + (not_read & (opc != SPDK_NVME_OPC_WRITE))]++;
}

/* One OPC_STATS line per I/O SQ lifetime, next to IAU_STATS; idempotent
 * (reached from both delete_sq_done() and free_qp()). */
static void
sq_opc_stats_dump(struct nvmf_vfio_user_sq *sq)
{
	if (!sq->opc_armed) {
		return;
	}
	sq->opc_armed = false;
	/* ctrlr= and fp_done= (fast-path completions posted, P3) are appended: P1 regexes still match. */
	SPDK_NOTICELOG("OPC_STATS qid=%u reads=%" PRIu64 " writes=%" PRIu64 " other=%" PRIu64
		       " ctrlr=%d fp_done=%" PRIu64 "\n", sq->qid, sq->opc_cnt[0], sq->opc_cnt[1], sq->opc_cnt[2],
		       sq->ctrlr->dev_idx, sq->fp != NULL ? sq->fp->completed : 0);
}

static void
sq_fp_free(struct nvmf_vfio_user_sq *sq)
{
	if (sq->fp != NULL) {
		fp_sq_fini(sq->fp);
		free(sq->fp);
		sq->fp = NULL;
	}
}

/*
 * Tear down the IAU rings of an I/O SQ. Idempotent (reached from both
 * delete_sq_done() and free_qp()). With flush=false the rings may already be
 * unmapped by the host (DMA unregister), so nothing is written to them.
 */
static void
sq_iau_detach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, bool flush)
{
	vfu_ctx_t *vfu_ctx = ctrlr->endpoint->vfu_ctx;
	const struct iau_sq *is;

	if (!sq->iau_attached) {
		return;
	}

	is = &ctrlr->iau.sq[sq->qid];
	if (flush) {
		iau_flush(&ctrlr->iau, sq->qid);
		SPDK_NOTICELOG("IAU_STATS qid=%u fetched=%" PRIu64 " emitted=%" PRIu64
			       " flush_n=%" PRIu64 " flush_t=%" PRIu64 " credits_in_use=%u\n",
			       sq->qid, is->fetched, is->emitted, is->flush_by_n, is->flush_by_t,
			       is->credits_in_use);
	}
	iau_sq_detach(&ctrlr->iau, sq->qid);

	if (sq->iau_iov_desc.iov_base != NULL) {
		vfu_sgl_put(vfu_ctx, sq->iau_sg_desc, &sq->iau_iov_desc, 1);
		sq->iau_iov_desc.iov_base = NULL;
	}
	if (sq->iau_iov_rec.iov_base != NULL) {
		vfu_sgl_put(vfu_ctx, sq->iau_sg_rec, &sq->iau_iov_rec, 1);
		sq->iau_iov_rec.iov_base = NULL;
	}
	free(sq->iau_sg_desc);
	free(sq->iau_sg_rec);
	sq->iau_sg_desc = NULL;
	sq->iau_sg_rec = NULL;
	sq->iau_attached = false;
}

/* IAU-next P3.9 (mode 5): word qid (< IAU_MAX_QID) of a 4 B-per-qid table of the IAU page
 * (IAU_DESC_TAIL_OFFSET: DESC_TAIL[q]; IAU_CQDB_SINK_OFFSET: the CQ sink[q]). */
static inline volatile uint32_t *
iau_page_u32(struct nvmf_vfio_user_ctrlr *ctrlr, uint32_t offset, uint16_t qid)
{
	return (volatile uint32_t *)(ctrlr->endpoint->iau_page + (offset - IAU_BAR0_UNCORE_PAGE_OFFSET)) + qid;
}

/* QCFG[qid] as the host wrote it into the IAU page (8 B store at construct); 0 past the page's table. */
static uint64_t VU_IAU_ENG_OOL
iau_qcfg_read(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t qid)
{
	const volatile uint8_t *pg = ctrlr->endpoint->iau_page;

	if (pg == NULL || qid >= IAU_MAX_QID) {
		return 0;
	}
	return ((const volatile uint64_t *)(pg + (IAU_QCFG_OFFSET - IAU_BAR0_UNCORE_PAGE_OFFSET)))[qid];
}

/* Release the IAU ring maps of a mode-5 I/O SQ (idempotent; mode 4 uses sq_iau_detach()). */
static void VU_IAU_ENG_OOL
sq_eng_unmap(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq)
{
	vfu_ctx_t *vfu_ctx = ctrlr->endpoint->vfu_ctx;

	if (sq->iau_iov_desc.iov_base != NULL) {
		vfu_sgl_put(vfu_ctx, sq->iau_sg_desc, &sq->iau_iov_desc, 1);
		sq->iau_iov_desc.iov_base = NULL;
	}
	if (sq->iau_iov_rec.iov_base != NULL) {
		vfu_sgl_put(vfu_ctx, sq->iau_sg_rec, &sq->iau_iov_rec, 1);
		sq->iau_iov_rec.iov_base = NULL;
	}
	free(sq->iau_sg_desc);
	free(sq->iau_sg_rec);
	sq->iau_sg_desc = NULL;
	sq->iau_sg_rec = NULL;
}

/*
 * Bind a registered I/O SQ (mode 5, QCFG credits/nsid) to the next IAU engine, round robin in bind order
 * (see iau_engines_create() for the threads and the handshake). Runs on the SQ's poll-group thread in
 * handle_queue_connect_rsp(), under endpoint->lock, before the Create I/O SQ completion is posted, so the
 * host has not rung DESC_TAIL[q] yet and the SQ is not ACTIVE. The engine's registration rules are
 * iau_eng_q_init()'s (credits 1..entries-1 and <= cq_entries-1, nsid != 0, qid < IAU_MAX_QID); the
 * binding also needs BAR0 doorbells (no shadow doorbells: a Doorbell Buffer Config would move them
 * under the engine), a CQ of the SQ's own (the engine polls it whole) and the host's ring IOVAs.
 * Returns 0, or a negative errno after an ERRLOG with nothing of the queue left mapped or bound.
 */
static int VU_IAU_ENG_OOL
sq_eng_attach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, uint32_t credits, uint32_t nsid)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	vfu_ctx_t *vfu_ctx = ctrlr->endpoint->vfu_ctx;
	struct nvmf_vfio_user_poll_group *pg;
	struct iau_engine_thread *eng;
	struct iau_eng_q_cfg cfg;
	struct iau_eng_qb *b = NULL;
	uint16_t qid = (uint16_t)sq->qid;
	uint64_t desc_iova = iau_desc_ring_iova(&ctrlr->iau, qid);
	uint64_t rec_iova = iau_rec_ring_iova(&ctrlr->iau, qid);
	void *desc_va, *rec_va;
	uint32_t e, lcore;
	bool clash = false;
	int rc;

	if (sq->eng_qb != NULL) {	/* a re-connect without a new Create I/O SQ (migration; not in the testbed) */
		SPDK_ERRLOG("%s: IAU engine: sqid:%u is already bound\n", ctrlr_id(ctrlr), qid);
		return -EEXIST;
	}
	if (vt->n_engines == 0 || ctrlr->sdbl != NULL || cq == NULL || cq->cq_ref != 1 || desc_iova == 0 ||
	    rec_iova == 0) {
		SPDK_ERRLOG("%s: IAU engine: sqid:%u cannot be bound (engines=%u shadow_doorbells=%d cqid:%u "
			    "sqs_on_cq=%d desc=%#" PRIx64 " rec=%#" PRIx64 "): mode 5 needs engine threads, BAR0 "
			    "doorbells, a CQ of the SQ's own and the host's ring IOVAs\n", ctrlr_id(ctrlr), qid,
			    vt->n_engines, ctrlr->sdbl != NULL, sq->cqid, cq != NULL ? cq->cq_ref : -1, desc_iova,
			    rec_iova);
		return -EINVAL;
	}
	sq->iau_sg_desc = calloc(1, dma_sg_size());
	sq->iau_sg_rec = calloc(1, dma_sg_size());
	if (sq->iau_sg_desc == NULL || sq->iau_sg_rec == NULL) {
		rc = -ENOMEM;
		goto fail;
	}
	desc_va = map_one(vfu_ctx, desc_iova, (uint64_t)sq->size * IAU_DESC_D_BYTES, sq->iau_sg_desc,
			  &sq->iau_iov_desc, MAP_R);
	if (desc_va == NULL) {
		sq->iau_iov_desc.iov_base = NULL;	/* map_one() leaves it unset on failure */
		rc = -EFAULT;
		goto fail;
	}
	rec_va = map_one(vfu_ctx, rec_iova, (uint64_t)sq->size * IAU_REC_BYTES, sq->iau_sg_rec,
			 &sq->iau_iov_rec, MAP_RW);
	if (rec_va == NULL) {
		sq->iau_iov_rec.iov_base = NULL;
		rc = -EFAULT;
		goto fail;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.qid = qid;
	cfg.entries = sq->size;			/* descriptor ring == record ring == SSD SQ entries */
	cfg.nsid = nsid;
	cfg.credits = credits;
	cfg.desc_ring = desc_va;
	cfg.rec_ring = rec_va;
	cfg.host_db = iau_page_u32(ctrlr, IAU_DESC_TAIL_OFFSET, qid);
	cfg.host_cq_sink = iau_page_u32(ctrlr, IAU_CQDB_SINK_OFFSET, qid);
	cfg.ssd_sq = q_addr(&sq->mapping);
	cfg.sq_entries = sq->size;
	cfg.ssd_cq = q_addr(&cq->mapping);
	cfg.cq_entries = cq->size;
	cfg.ssd_sq_db = sq->dbl_tailp;		/* the doorbells the SSD thread reads */
	cfg.ssd_cq_db = cq->dbl_headp;
	if (posix_memalign((void **)&b, 64, sizeof(*b)) != 0) {
		b = NULL;
		rc = -ENOMEM;
		goto fail;
	}
	memset(b, 0, sizeof(*b));
	rc = iau_eng_q_init(&b->q, &cfg);
	if (rc != 0) {
		SPDK_ERRLOG("%s: IAU engine registration refused for sqid:%u (%d): QCFG credits=%u nsid=%u, "
			    "entries=%u cq_entries=%u (needs 1 <= credits <= entries-1 and <= cq_entries-1, nsid != 0, "
			    "qid < %u)\n", ctrlr_id(ctrlr), qid, rc, credits, nsid, sq->size, cq->size, IAU_MAX_QID);
		goto fail;
	}
	b->ctrlr = ctrlr;
	b->sq = sq;
	b->ctrlr_idx = ctrlr->dev_idx;
	b->qid = qid;
	b->slot = IAU_ENG_SLOT_NONE;
	b->mem[0] = desc_va;
	b->mem[1] = rec_va;
	b->mem[2] = q_addr(&sq->mapping);
	b->mem[3] = q_addr(&cq->mapping);
	/* The queue's host-side doorbells start at 0, as handle_create_io_sq/cq() reset the SSD's. */
	*iau_page_u32(ctrlr, IAU_DESC_TAIL_OFFSET, qid) = 0;
	*iau_page_u32(ctrlr, IAU_CQDB_SINK_OFFSET, qid) = 0;

	pthread_mutex_lock(&vt->eng_lock);
	/* The same engine every run: (k + qid - 1) mod n_engines, k = vfu<k> (0 without a number). Controller
	 * k's queue 1 lands on engine k mod n, its next queues on the following engines. */
	e = ((uint32_t)(ctrlr->dev_idx > 0 ? ctrlr->dev_idx : 0) + qid - 1u) % vt->n_engines;
	eng = vt->engines[e];
	lcore = __atomic_load_n(&eng->lcore, __ATOMIC_ACQUIRE);
	pthread_mutex_lock(&vt->pg_lock);
	TAILQ_FOREACH(pg, &vt->poll_groups, link) {
		clash |= pg->lcore == eng->want_lcore;
	}
	pthread_mutex_unlock(&vt->pg_lock);
	if (lcore != eng->want_lcore || clash) {
		SPDK_ERRLOG("%s: IAU engine %u is not running on a core of its own (lcore %u, want %u, poll group "
			    "there %d)\n", ctrlr_id(ctrlr), e, lcore, eng->want_lcore, clash);
		rc = -EINVAL;
	} else {
		b->eng = eng;
		rc = iau_engine_call(eng, &b->hs, iau_engine_bind_msg, b, true, ctrlr->dev_idx, qid);
		if (rc == 0) {
			rc = b->rc;
		} else {
			SPDK_ERRLOG("IAU_ENGINE_BIND_TIMEOUT ctrlr=%d qid=%u engine=%u: no answer in 1 s\n",
				    ctrlr->dev_idx, qid, e);
			b = NULL;	/* abandoned: the engine thread disposes of it, unpolled */
		}
		if (rc == 0) {
			eng->n_bound++;
			sq->eng_qb = b;
		} else if (b != NULL) {
			SPDK_ERRLOG("%s: IAU engine %u refused sqid:%u (%d)\n", ctrlr_id(ctrlr), e, qid, rc);
		}
	}
	pthread_mutex_unlock(&vt->eng_lock);
	if (rc == 0) {
		return 0;
	}
fail:
	if (b != NULL) {
		iau_eng_q_fini(&b->q);	/* never polled; fini of a zeroed q is a no-op */
		free(b);
	}
	sq_eng_unmap(ctrlr, sq);
	return rc;
}

/*
 * Unbind an I/O SQ from its IAU engine (idempotent; any thread; a no-op unless mode 5 and qid != 0, so
 * the hooks in modes 0 and 4 cost one call). With s == NULL always, else only if one of the queue's four
 * rings (descriptor, record, SSD SQ, SSD CQ) starts inside [s, e], the DMA region being removed. The
 * first step of every path that unmaps the queue's memory or changes its doorbells or the IAU page; it
 * returns only once the engine has stopped polling the queue and printed IAU_ENGINE_STATS (handshake:
 * iau_engines_create()). An unbind is never abandoned: IAU_ENGINE_DETACH_TIMEOUT after 1 s, abort()
 * after 10 s (iau_engine_call()).
 */
static void VU_IAU_ENG_OOL
sq_eng40_detach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, const void *s, const void *e)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct iau_engine_thread *eng;
	struct iau_eng_qb *b;
	bool hit = (s == NULL);
	int i;

	if (!iau_mode_engine(&ctrlr->iau) || sq->qid == 0) {
		return;
	}
	pthread_mutex_lock(&vt->eng_lock);
	b = sq->eng_qb;
	for (i = 0; b != NULL && !hit && i < 4; i++) {
		hit = b->mem[i] != NULL && b->mem[i] >= s && b->mem[i] <= e;
	}
	if (b != NULL && hit) {
		eng = b->eng;
		sq->eng_qb = NULL;
		iau_engine_call(eng, &b->hs, iau_engine_unbind_msg, b, false, ctrlr->dev_idx, sq->qid);
		eng->n_bound--;
		free(b);
		sq_eng_unmap(ctrlr, sq);
	}
	pthread_mutex_unlock(&vt->eng_lock);
}

/* The unbind of every path (the P3 40 B binding above, P4.4: and the tile membership, V9 phase 2), plus the
 * mode-0 error knob's DEV_FAULT_STATS (any mode, once) on the teardown paths (s == NULL). */
static void VU_IAU_ENG_OOL
sq_eng_detach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, const void *s, const void *e)
{
	if (s == NULL) {
		sq_fp_fault_report(sq);
	}
	if (!iau_mode_engine(&ctrlr->iau) || sq->qid == 0) {
		return;
	}
	sq_eng6_detach(ctrlr, sq, s, e);
	sq_eng40_detach(ctrlr, sq, s, e);
}

/* Mode 5, before the controller's IAU page is zeroed (disable_ctrlr()): unbind every I/O SQ. */
static void VU_IAU_ENG_OOL
ctrlr_eng_detach_all(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	uint32_t i;

	if (!iau_mode_engine(&ctrlr->iau)) {
		return;
	}
	/* P4.4 (V9): at a controller disable (CC.EN 1 -> 0 or a shutdown: reset_shn, and the controller is still
	 * the endpoint's) the tile members only stop (phase 1, ctrlr_eng6_disable(), under eng_lock): the SSD threads
	 * still serve their SQs, and the final detach runs in delete_sq_done() once they stopped. The 40 B path
	 * unbinds here as before. Every other caller (a failed qpair connect that frees the controller next)
	 * detaches everything whole. */
	if (ctrlr->reset_shn && ctrlr->endpoint->ctrlr == ctrlr) {
		ctrlr_eng6_disable(ctrlr);
		for (i = 1; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
			if (ctrlr->sqs[i] != NULL) {
				sq_eng40_detach(ctrlr, ctrlr->sqs[i], NULL, NULL);
			}
		}
		return;
	}
	for (i = 1; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		if (ctrlr->sqs[i] != NULL) {
			sq_eng_detach(ctrlr, ctrlr->sqs[i], NULL, NULL);
		}
	}
}

/* Mode 5, memory_region_remove_cb() under endpoint->lock: unbind every connected I/O SQ with a ring in
 * the DMA region [s, e] that is being removed. */
static void VU_IAU_ENG_OOL
ctrlr_eng_detach_region(struct nvmf_vfio_user_ctrlr *ctrlr, const void *s, const void *e)
{
	struct nvmf_vfio_user_sq *sq;

	if (!iau_mode_engine(&ctrlr->iau)) {
		return;
	}
	/* P4.4 (review I3): every tile ring whose source mapping lies in the region hands its source over (or is
	 * dropped) before any member detaches, so no member's emit depends on the order of connected_sqs */
	ctrlr_eng6_region(ctrlr, s, e);
	TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
		sq_eng_detach(ctrlr, sq, s, e);
	}
}

/*
 * IAU-next P3.9 (D5): an I/O SQ that connects, checked against QCFG[q] and the target's mode
 * (iau_qcfg_decide()). Mode 5 binds a registered SQ to an IAU engine; any mismatch between the host's
 * registration and the target's mode fails the Create I/O SQ loudly with INVALID_FIELD: mode 5 with an
 * unregistered queue, mode 0 or 4 with a queue the host registered (it rings DESC_TAIL[q], which only
 * an engine reads), a reserved QCFG bit, or an engine that refuses the queue; a target-side allocation
 * failure (-ENOMEM) is INTERNAL_DEVICE_ERROR instead. Runs on the SQ's poll-group thread in
 * handle_queue_connect_rsp(), under endpoint->lock, before the Create I/O SQ completion; there and not
 * in handle_create_io_sq(), which is inlined into the SQ poller, so the poller's code is unchanged.
 * Returns the Create I/O SQ status code.
 */
static uint16_t VU_IAU_ENG_OOL
sq_eng_connect(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq)
{
	uint16_t qid = (uint16_t)sq->qid;
	uint64_t qcfg = iau_qcfg_read(ctrlr, qid);
	uint64_t qcfg2 = iau_qcfg2_read(ctrlr, qid);
	uint16_t sc = sq_fp_connect(ctrlr, sq);	/* P4.4: every I/O SQ, every mode: the device model's calls */
	struct iau_tile_qreg q;
	int rc;

	if (sc != SPDK_NVME_SC_SUCCESS) {
		return sc;
	}
	/* P4.4: QCFG2[q] picks the path in mode 5 (iau_tile_queue_decide(): 0 -> the P3 40 B engine, a tile
	 * registration -> a tile group-ring member); with QCFG2 0 the outcome is iau_qcfg_decide()'s, as in P3. */
	rc = iau_tile_queue_decide(ctrlr->iau.mode, qcfg, qcfg2, &q);
	if (rc == IAU_TILE_Q_NONE) {
		return SPDK_NVME_SC_SUCCESS;
	}
	if (rc == IAU_TILE_Q_ENG40 || rc == IAU_TILE_Q_TILE) {
		rc = rc == IAU_TILE_Q_TILE ? sq_eng6_attach(ctrlr, sq, &q) : sq_eng_attach(ctrlr, sq, q.credits, q.nsid);
		if (rc == 0) {
			return SPDK_NVME_SC_SUCCESS;
		}
		SPDK_ERRLOG("%s: IAU mode 5: sqid:%u cannot be bound to an IAU engine (%d, QCFG2=%#" PRIx64 "); failing "
			    "its Create I/O SQ\n", ctrlr_id(ctrlr), qid, rc, qcfg2);
		if (rc == -ENOMEM) {	/* a target-side allocation, not the host's command */
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	} else if (rc == -ENOENT) {
		SPDK_ERRLOG("%s: IAU mode 5, but the host did not register sqid:%u with the IAU engine "
			    "(QCFG=%#" PRIx64 "; it needs SPDK_IAU_ENGINE=1 on a Mode D qpair, qid < %u); failing its "
			    "Create I/O SQ\n", ctrlr_id(ctrlr), qid, qcfg, IAU_MAX_QID);
	} else if (rc == -EEXIST) {
		SPDK_ERRLOG("%s: IAU mode %d, but the host registered sqid:%u with an IAU engine (QCFG=%#" PRIx64
			    " QCFG2=%#" PRIx64 "; SPDK_IAU_ENGINE=1 and SPDK_IAU_TILE=1 need IAU_UNCORE_MODE=5); failing its "
			    "Create I/O SQ\n", ctrlr_id(ctrlr), ctrlr->iau.mode, qid, qcfg, qcfg2);
	} else if (rc == -EPROTO) {
		SPDK_ERRLOG("%s: sqid:%u has a malformed QCFG2 (%#" PRIx64 ": V6 bit or GCB IOVA missing); failing its "
			    "Create I/O SQ\n", ctrlr_id(ctrlr), qid, qcfg2);
	} else {
		SPDK_ERRLOG("%s: sqid:%u has a reserved QCFG bit set (QCFG=%#" PRIx64 "); failing its Create I/O "
			    "SQ\n", ctrlr_id(ctrlr), qid, qcfg);
	}
	return SPDK_NVME_SC_INVALID_FIELD;
}
/*
 * IAU-next P4.4: mode 5 serves IAU host interface tile queues (plan rev 2 D1, D3, D9, D11, D16-D19 as amended by
 * A12; P4.0 V9-V11; the P4.3 decisions on reservations, peak slots and the R-consumed word). The tile engine core
 * (vfio_user_iau_tile.c) runs on the P3 engine threads through a second poller (iau_engine6_poll()); the 40 B
 * path (I1e) is untouched: a queue is tile iff the host wrote QCFG2[q] (iau_tile_queue_decide(), sq_eng_connect()).
 *
 * Objects (all requester-side state under eng_lock; the engine ring is the engine thread's while bound):
 *  - struct iau_tile_gring: one bound (GCB, ring k), keyed by the GCB IOVA and k. Assumption (M10): ONE host IOVA
 *    domain per target, shared by every controller whose routes a GCB groups (the testbed's one host process,
 *    IOVA == VA); a second host with its own IOVA space must not reuse a GCB IOVA. Its engine ring polls D_k /
 *    R_k and the GCB words through the maps of one member, its source (src); its members are a list; note[]
 *    drives the reservation release, keyed by controller incarnation (tile_key).
 *  - struct iau_eng6_qb: one member (route), the engine member plus this SQ's own maps of the GCB, D, R and
 *    the echo slab through its controller (struct iau_tile_maps, owned by the requester), so the ring can move to
 *    another member's maps without mapping anything across controllers (ring_remap on owner loss).
 *
 * Flow (file:line in the P4.4 report):
 *  bind    sq_eng_connect() -> sq_eng6_attach(): map and check the GCB (iau_tile_gcb_check()), map echo, D, R; init
 *          the member; under eng_lock find the ring or create it (fresh positions, engine (gid + k) mod
 *          n_engines, ring bind message), then the member bind message (member_add: invariant -ENOSPC, stale
 *          gen -ESTALE refuse the Create I/O SQ, IAU_ENGINE6_REFUSED); note[route] = (ctrlr, cqid, gen).
 *  stop    disable_ctrlr() -> ctrlr_eng_detach_all() -> ctrlr_eng6_disable() before the IAU page memset (V9 phase
 *          1, under eng_lock: a new controller key next time, member_stop for every member; the SSD threads still
 *          serve the SQs; the engine snapshotted everything at bind).
 *  detach  delete_sq_done() -> sq_eng_detach() -> sq_eng6_detach() on the SSD's thread after the SSD stopped
 *          serving the SQ (V9 phase 2): source handover if this member is the source (iau_tile_src_plan():
 *          ring_remap to a member whose mapping survives), member_detach(emit) on the engine thread,
 *          IAU_ENGINE6_STATS after it, the note goes RESV (forgotten if the controller goes away); the last
 *          member's detach (emit) and the ring's unbind are ONE engine message (iau_engine6_member_last_msg():
 *          the engine's final D fetch, then R_DETACHED as the ring's last record, then IAU_ENGINE6_RING_END;
 *          P4.5 fix A, the private-ring rule). A failed connect takes the same path. A DMA region
 *          removal first hands over the source of every ring whose source mapping is in the region
 *          (ctrlr_eng6_region()), then detaches the hit members; a full ring drop (every member emit=false)
 *          happens only when no member's mapping of the ring survives (host death, or the host freeing live
 *          ring memory), and a failed remap also drops (the old maps are never released under a polling engine).
 *  release handle_del_io_q() -> host_delete_cq() -> sq_eng6_cq_deleted(): only the host's own successful Admin
 *          Delete I/O CQ releases the reservation of the route bound to that CQ in the current controller
 *          incarnation, with the member gen of its bind (iau_eng6_route_release(); IAU_ENGINE6_RELEASE).
 *          Target-internal CQ deletions never come here. Host precondition (M4, P4.5): the Delete I/O CQ follows
 *          the Delete I/O SQ completion (never pipelined), else the CQ is unmapped while the engine still drains
 *          it (as for the P3 40 B path; refusing it would change the re-baselined handle_del_io_q code).
 * Messages use the P3 handshake (iau_engine_call()); only binds are ever abandoned.
 */
struct iau_tile_maps {
	dma_sg_t		*sg[5];
	struct iovec		iov[5];
};
enum { IAU_TILE_MAP_ECHO = 0, IAU_TILE_MAP_GCB = 1, IAU_TILE_MAP_D = 2, IAU_TILE_MAP_R = 3, IAU_TILE_MAP_CTX = 4 };

struct iau_eng6_qb {
	struct iau_eng6_member		m;		/* the engine thread's while in the ring */
	TAILQ_ENTRY(iau_eng6_qb)	glink;		/* gr->qbs */
	struct iau_tile_gring		*gr;
	struct nvmf_vfio_user_ctrlr	*ctrlr;
	struct nvmf_vfio_user_sq	*sq;
	struct iau_tile_maps		*mp;		/* requester-owned (an abandoned bind's qb is the engine's) */
	struct iau_tile_qreg		reg;
	uint16_t			qid, cqid;
	const void			*mem[7];	/* SSD SQ, SSD CQ, echo, GCB, D, R, ctx: what a region removal hits */
	struct iau_tile_ctxgcb		*cg;		/* P4.10b: the GCB's ctx map, this member's reference (P4.10e: the
							 * bind's, kept once bound; NULL: knob off) */
	uint32_t			ctx_off;	/* P4.10b: this route's block in the region */
	uint32_t			hs;
	int				rc;
	bool				emit;		/* member unbind argument */
	bool				stopped, dropped;
};

struct iau_tile_gring {
	TAILQ_ENTRY(iau_tile_gring)	link;		/* vt->grings while listed */
	TAILQ_HEAD(, iau_eng6_qb)	qbs;
	struct iau_eng6_ring		ring;
	struct iau_tile_gcb_view		view;		/* the first bind's snapshot */
	uint64_t			gcb_iova;
	struct iau_engine_thread	*eng;
	struct iau_eng6_qb		*src;		/* whose maps the ring polls */
	uint32_t			n_qb, hs;
	int				rc;
	bool				bound, listed;
	struct iau_tile_note		note[IAU_TILE_MAX_ROUTES];
	struct iau_tile_ctxgcb		*cg;		/* P4.10b: the GCB's ctx map (NULL: knob off). P4.10e: no reference of
							 * its own; a listed ring has members or a bind in flight, each holding
							 * one */
	/* message arguments (eng_lock) */
	uint16_t			arg_route;
	uint8_t				arg_gen;
	const volatile void		*rm_d;
	volatile void			*rm_r;
	const volatile uint32_t		*rm_db;
	volatile uint64_t		*rm_dh;
	const volatile uint64_t		*rm_rc;
};

#define IAU_TILE_LINE_MAX	IAU_ENG6_LINE_MAX

static int VU_IAU_ENG_OOL
iau_engine6_poll(void *arg)
{
	struct iau_engine_thread *eng = arg;

	/* one tick read and one conversion per pass (D9: the engine's clock is microseconds) */
	return iau_tile_eng_poll(&eng->rings_tile, iau_tile_clock_us(&eng->clock, spdk_get_ticks())) != 0 ?
	       SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/* IAU_TILE_CTX_SPILL: the same pass with the spill ring poll (a poller chosen once, at the first ring's bind) */
static int VU_IAU_ENG_OOL
iau_engine6_poll_spill(void *arg)
{
	struct iau_engine_thread *eng = arg;

	return iau_tile_eng_poll_fn(&eng->rings_tile, iau_tile_clock_us(&eng->clock, spdk_get_ticks()), iau_eng6_ring_poll_spill) != 0 ?
	       SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void VU_IAU_ENG_OOL
iau_engine6_ring_bind_msg(void *arg)
{
	struct iau_tile_gring *gr = arg;
	struct iau_engine_thread *eng = gr->eng;
	char line[IAU_TILE_LINE_MAX];
	int rc;

	if (!iau_eng_hs_take(&gr->hs)) {
		free(gr);	/* abandoned: never polled; its maps are the requester's */
		return;
	}
	rc = eng->poller == NULL ? -ENODEV : iau_tile_eng_add(&eng->rings_tile, &gr->ring);
	if (rc == 0 && eng->poller6 == NULL) {
		/* P4.10b: a ring with a ctx map (IAU_TILE_CTX_SPILL=1) gets the spill poll; one choice per engine thread, as
		 * the knob is process-wide */
		eng->poller6 = gr->cg != NULL ? SPDK_POLLER_REGISTER(iau_engine6_poll_spill, eng, 0) :
			       SPDK_POLLER_REGISTER(iau_engine6_poll, eng, 0);
		if (eng->poller6 == NULL) {
			iau_tile_eng_del(&eng->rings_tile, &gr->ring);
			rc = -ENOMEM;
		}
	}
	if (rc == 0) {
		eng->tile_used = true;
		iau_eng6_fmt_ring(line, sizeof(line), &gr->ring, eng->index, spdk_env_get_current_core());
		SPDK_NOTICELOG("%s\n", line);
	}
	gr->rc = rc;
	iau_eng_hs_done(&gr->hs);
}

/* the ring leaves the engine (RING_END); the tile poller goes with the last ring. Engine thread. */
static void VU_IAU_ENG_OOL
iau_engine6_ring_out(struct iau_engine_thread *eng, struct iau_tile_gring *gr)
{
	char line[IAU_TILE_LINE_MAX];

	iau_tile_eng_del(&eng->rings_tile, &gr->ring);
	iau_eng6_fmt_ring_end(line, sizeof(line), &gr->ring, eng->index);
	SPDK_NOTICELOG("%s\n", line);
	if (eng->rings_tile.n == 0) {
		spdk_poller_unregister(&eng->poller6);
	}
}

static void VU_IAU_ENG_OOL
iau_engine6_ring_unbind_msg(void *arg)
{
	struct iau_tile_gring *gr = arg;

	if (!iau_eng_hs_take(&gr->hs)) {
		return;	/* not reached: an unbind is never abandoned */
	}
	iau_engine6_ring_out(gr->eng, gr);
	iau_eng_hs_done(&gr->hs);
}

static void VU_IAU_ENG_OOL
iau_engine6_ring_remap_msg(void *arg)
{
	struct iau_tile_gring *gr = arg;

	if (!iau_eng_hs_take(&gr->hs)) {
		return;
	}
	gr->rc = iau_eng6_ring_remap(&gr->ring, gr->rm_d, gr->rm_db, gr->rm_dh, gr->rm_r, gr->rm_rc);
	iau_eng_hs_done(&gr->hs);
}

static void VU_IAU_ENG_OOL
iau_engine6_route_release_msg(void *arg)
{
	struct iau_tile_gring *gr = arg;

	if (!iau_eng_hs_take(&gr->hs)) {
		return;
	}
	gr->rc = iau_eng6_route_release(&gr->ring, gr->arg_route, gr->arg_gen);
	iau_eng_hs_done(&gr->hs);
}

/* member_detach on the engine thread, then its STATS (dropped_busy, parked_dropped, rec_detached final) */
static void VU_IAU_ENG_OOL
iau_engine6_member_out(struct iau_engine_thread *eng, struct iau_tile_gring *gr, struct iau_eng6_qb *qb, bool emit)
{
	char line[IAU_TILE_LINE_MAX];

	qb->rc = iau_eng6_member_detach(&gr->ring, &qb->m, emit);
	if (qb->rc < 0) {
		SPDK_ERRLOG("IAU engine %u: v6 member ctrlr=%d qid=%u route=%u is not in its ring (%d)\n", eng->index,
			    qb->m.ctrlr, qb->qid, qb->reg.route, qb->rc);
	}
	iau_eng6_fmt_stats(line, sizeof(line), &gr->ring, &qb->m, eng->index);
	SPDK_NOTICELOG("%s\n", line);
	if (iau_eng6_fmt_spill(line, sizeof(line), &gr->ring, &qb->m) > 0) {
		SPDK_NOTICELOG("%s\n", line);
	}
}

/* Drop every member (emit=false: the ring's memory is going away) and the ring itself. Engine thread. */
static void VU_IAU_ENG_OOL
iau_engine6_ring_drop_msg(void *arg)
{
	struct iau_tile_gring *gr = arg;
	struct iau_eng6_qb *qb;

	if (!iau_eng_hs_take(&gr->hs)) {
		return;
	}
	TAILQ_FOREACH(qb, &gr->qbs, glink) {	/* stable: the requester holds eng_lock and waits */
		if (!qb->dropped) {
			iau_engine6_member_out(gr->eng, gr, qb, false);
			qb->dropped = true;
		}
	}
	iau_engine6_ring_out(gr->eng, gr);
	iau_eng_hs_done(&gr->hs);
}

static void VU_IAU_ENG_OOL
iau_engine6_member_bind_msg(void *arg)
{
	struct iau_eng6_qb *qb = arg;
	struct iau_tile_gring *gr = qb->gr;
	char line[IAU_TILE_LINE_MAX];

	if (!iau_eng_hs_take(&qb->hs)) {
		/* abandoned: never in the ring; its maps are the requester's (qb->mp is not touched) */
		iau_eng6_member_fini(&qb->m);
		free(qb);
		return;
	}
	qb->rc = iau_eng6_member_add(&gr->ring, &qb->m);
	if (qb->rc == 0) {
		iau_eng6_fmt_bind(line, sizeof(line), &gr->ring, &qb->m, gr->eng->index, spdk_env_get_current_core());
		SPDK_NOTICELOG("%s\n", line);
	}
	iau_eng_hs_done(&qb->hs);
}

static void VU_IAU_ENG_OOL
iau_engine6_member_stop_msg(void *arg)
{
	struct iau_eng6_qb *qb = arg;

	if (!iau_eng_hs_take(&qb->hs)) {
		return;
	}
	iau_eng6_member_stop(&qb->gr->ring, &qb->m);
	iau_eng_hs_done(&qb->hs);
}

static void VU_IAU_ENG_OOL
iau_engine6_member_unbind_msg(void *arg)
{
	struct iau_eng6_qb *qb = arg;

	if (!iau_eng_hs_take(&qb->hs)) {
		return;
	}
	if (!qb->dropped) {
		iau_engine6_member_out(qb->gr->eng, qb->gr, qb, qb->emit);
	}
	iau_eng_hs_done(&qb->hs);
}

/* The ring's last member leaves with emit (P4.5 fix A, the private-ring rule), ONE message: member_detach() with
 * its final D fetch (every descriptor up to the detach-time d_tail gets its one record, then R_DETACHED as the
 * ring's last record) and the ring off this engine (iau_tile_eng_member_last_out()), with no poll in between; then
 * STATS and RING_END. qb->rc < 0: the ring is still bound (-EINVAL: not its member, nothing done; -EBUSY, not reached
 * with n_qb == 1: detached as a shared ring's member) and sq_eng6_detach() unbinds it as before. */
static void VU_IAU_ENG_OOL
iau_engine6_member_last_msg(void *arg)
{
	struct iau_eng6_qb *qb = arg;
	struct iau_tile_gring *gr = qb->gr;
	struct iau_engine_thread *eng = gr->eng;
	char line[IAU_TILE_LINE_MAX];

	if (!iau_eng_hs_take(&qb->hs)) {
		return;
	}
	qb->rc = iau_tile_eng_member_last_out(&eng->rings_tile, &gr->ring, &qb->m);
	if (qb->rc < 0 && qb->rc != -EBUSY) {
		SPDK_ERRLOG("IAU engine %u: v6 member ctrlr=%d qid=%u route=%u is not in its ring (%d)\n", eng->index,
			    qb->m.ctrlr, qb->qid, qb->reg.route, qb->rc);
	}
	iau_eng6_fmt_stats(line, sizeof(line), &gr->ring, &qb->m, eng->index);
	SPDK_NOTICELOG("%s\n", line);
	if (iau_eng6_fmt_spill(line, sizeof(line), &gr->ring, &qb->m) > 0) {
		SPDK_NOTICELOG("%s\n", line);
	}
	if (qb->rc >= 0) {
		iau_eng6_fmt_ring_end(line, sizeof(line), &gr->ring, eng->index);
		SPDK_NOTICELOG("%s\n", line);
		if (eng->rings_tile.n == 0) {
			spdk_poller_unregister(&eng->poller6);
		}
	}
	iau_eng_hs_done(&qb->hs);
}

/* Map [iova, iova + len) through ctrlr into mp slot i; the VA or NULL (logged by map_one()). */
static void *VU_IAU_ENG_OOL
iau_tile_map(struct nvmf_vfio_user_ctrlr *ctrlr, struct iau_tile_maps *mp, int i, uint64_t iova, uint64_t len, int flags)
{
	void *va;

	mp->sg[i] = calloc(1, dma_sg_size());
	if (mp->sg[i] == NULL) {
		return NULL;
	}
	va = map_one(ctrlr->endpoint->vfu_ctx, iova, len, mp->sg[i], &mp->iov[i], flags);
	if (va == NULL) {
		mp->iov[i].iov_base = NULL;	/* map_one() leaves it unset on failure */
	}
	return va;
}

static void VU_IAU_ENG_OOL
iau_tile_unmap(struct nvmf_vfio_user_ctrlr *ctrlr, struct iau_tile_maps *mp)
{
	int i;

	if (mp == NULL) {
		return;
	}
	for (i = 0; i < 5; i++) {
		if (mp->iov[i].iov_base != NULL) {
			vfu_sgl_put(ctrlr->endpoint->vfu_ctx, mp->sg[i], &mp->iov[i], 1);
		}
		free(mp->sg[i]);
	}
	free(mp);
}

static uint64_t VU_IAU_ENG_OOL
iau_qcfg2_read(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t qid)
{
	const volatile uint8_t *pg = ctrlr->endpoint->iau_page;

	if (pg == NULL || qid >= IAU_TILE_QIDS) {
		return 0;
	}
	return ((const volatile uint64_t *)(pg + (IAU_QCFG2_OFFSET - IAU_BAR0_UNCORE_PAGE_OFFSET)))[qid];
}

/* The engine for ring k of group gid, checked as P3 checks its engines. NULL after an ERRLOG. */
static struct iau_engine_thread *VU_IAU_ENG_OOL
iau_tile_pick_engine(struct nvmf_vfio_user_ctrlr *ctrlr, uint32_t gid, uint16_t k, uint32_t *e_out)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct nvmf_vfio_user_poll_group *pg;
	struct iau_engine_thread *eng;
	uint32_t e = (uint32_t)(((uint64_t)gid + k) % vt->n_engines), lcore;
	bool clash = false;

	eng = vt->engines[e];
	lcore = __atomic_load_n(&eng->lcore, __ATOMIC_ACQUIRE);
	pthread_mutex_lock(&vt->pg_lock);
	TAILQ_FOREACH(pg, &vt->poll_groups, link) {
		clash |= pg->lcore == eng->want_lcore;
	}
	pthread_mutex_unlock(&vt->pg_lock);
	if (lcore != eng->want_lcore || clash) {
		SPDK_ERRLOG("%s: IAU engine %u is not running on a core of its own (lcore %u, want %u, poll group "
			    "there %d)\n", ctrlr_id(ctrlr), e, lcore, eng->want_lcore, clash);
		return NULL;
	}
	*e_out = e;
	return eng;
}

/* eng_lock. Ring gr leaves the transport's list, so a ring of the same (GCB, k) can be bound again from now on. As
 * the RTL's unbind (the last member's DETACH, RING_RESET) frees every CAM entry of the ring (iau_mmio.v:842-846),
 * the blocks of the ring's routes go back to the GCB's ctx map here (P4.10e; a dropped ring's members are no longer
 * polled, so nothing spills into them). */
static void VU_IAU_ENG_OOL
gring_unlist(struct nvmf_vfio_user_transport *vt, struct iau_tile_gring *gr)
{
	if (!gr->listed) {
		return;
	}
	TAILQ_REMOVE(&vt->grings, gr, link);
	gr->listed = false;
	if (gr->cg != NULL) {
		iau_tile_ctx_ring_free(&gr->cg->map, gr->view.nk, gr->view.k);
	}
}

/*
 * Bind I/O SQ sq (mode 5, QCFG2 valid: q) as a member of its tile group ring. Runs where sq_eng_attach() runs
 * (the SQ's poll-group thread in handle_queue_connect_rsp(), under endpoint->lock, before the Create I/O SQ
 * completion). 0, or a negative errno after an ERRLOG with nothing of the queue left mapped or bound.
 */
static int VU_IAU_ENG_OOL
sq_eng6_attach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, const struct iau_tile_qreg *q)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	struct iau_tile_gring *gr = NULL, *ng = NULL;
	struct iau_engine_thread *eng;
	struct iau_eng6_member_cfg mc;
	struct iau_eng6_ring_cfg rcfg;
	struct iau_tile_gcb_view v;
	struct iau_tile_ctx_region cr = { 0, 0 };
	struct iau_eng6_qb *qb = NULL;
	struct iau_tile_maps *mp = NULL;
	uint16_t qid = (uint16_t)sq->qid;
	void *gcb_va, *echo_va, *d_va, *r_va, *ctx_va = NULL;
	struct iau_tile_ctxgcb *cg = NULL;		/* P4.10e: this bind's reference (I4) */
	struct iau_tile_ctx_undo ctx_undo = { 0 };
	uint32_t ctx_s = 0, ctx_off = 0;
	int ctx_why = 0;
	char err[256];
	bool new_ring = false;
	uint32_t e = 0;
	int rc;

	if (sq->eng_qb != NULL || sq->eng6_qb != NULL) {
		SPDK_ERRLOG("%s: IAU tile: sqid:%u is already bound\n", ctrlr_id(ctrlr), qid);
		return -EEXIST;
	}
	if (vt->n_engines == 0 || ctrlr->sdbl != NULL || cq == NULL || cq->cq_ref != 1 || qid >= IAU_TILE_QIDS) {
		SPDK_ERRLOG("%s: IAU tile: sqid:%u cannot be bound (engines=%u shadow_doorbells=%d cqid:%u sqs_on_cq=%d): "
			    "mode 5 needs engine threads, BAR0 doorbells and a CQ of the SQ's own\n", ctrlr_id(ctrlr), qid,
			    vt->n_engines, ctrlr->sdbl != NULL, sq->cqid, cq != NULL ? cq->cq_ref : -1);
		return -EINVAL;
	}
	mp = calloc(1, sizeof(*mp));
	if (mp == NULL) {
		return -ENOMEM;
	}
	gcb_va = iau_tile_map(ctrlr, mp, IAU_TILE_MAP_GCB, q->gcb_iova, IAU_TILE_GCB_BYTES, MAP_RW);
	if (gcb_va == NULL) {
		rc = mp->sg[IAU_TILE_MAP_GCB] == NULL ? -ENOMEM : -EFAULT;
		goto fail;
	}
	if (iau_tile_gcb_check(gcb_va, q, qid, sq->size, &v, err, sizeof(err)) != 0) {
		SPDK_ERRLOG("%s: IAU tile: sqid:%u GCB %#" PRIx64 " route %u refused: %s\n", ctrlr_id(ctrlr), qid,
			    q->gcb_iova, q->route, err);
		rc = -EINVAL;
		goto fail;
	}
	echo_va = iau_tile_map(ctrlr, mp, IAU_TILE_MAP_ECHO, v.m_echo_iova, iau_tile_echo_bytes(&v), MAP_RW);
	d_va = echo_va == NULL ? NULL : iau_tile_map(ctrlr, mp, IAU_TILE_MAP_D, v.d_iova, iau_tile_d_bytes(&v), MAP_R);
	r_va = d_va == NULL ? NULL : iau_tile_map(ctrlr, mp, IAU_TILE_MAP_R, v.r_iova, iau_tile_r_bytes(&v), MAP_RW);
	if (r_va == NULL) {
		rc = -EFAULT;
		goto fail;
	}
	if (vt->eng6_spill) {
		/* P4.10b: the route's block in the GCB ctx region; refused cleanly (the GCB, echo, D and R maps taken so
		 * far are released at fail:) when the GCB publishes none, the region cannot hold this block alone (the
		 * RTL's sum check, iau_mmio.v:743, with nothing else held), or its mapping fails. P4.10e: s = the route's
		 * slots as iau_mmio's BIND takes them under its reset SLOT_POLICY, credits (iau_mmio.v:443). */
		uint64_t need;
		const char *why = NULL;

		ctx_s = iau_tile_ctx_slots(IAU_TILE_SLOT_POLICY_TARGET, v.m_credits, v.m_entries);
		need = iau_tile_ctx_block_bytes(ctx_s);
		iau_tile_gcb_ctx(gcb_va, &cr);
		rc = iau_tile_ctx_region_check(&cr, err, sizeof(err));
		if (rc != 0) {
			why = err;
		} else if (cr.bytes < need) {
			snprintf(err, sizeof(err), "the region (%u B) cannot hold one %u-slot route block (%" PRIu64 " B)",
				 cr.bytes, ctx_s, need);
			rc = -ENOSPC;
			why = err;
		} else if ((ctx_va = iau_tile_map(ctrlr, mp, IAU_TILE_MAP_CTX, cr.iova, cr.bytes, MAP_RW)) == NULL) {
			snprintf(err, sizeof(err), "mapping %#" PRIx64 " + %u failed", cr.iova, cr.bytes);
			rc = -EFAULT;
			why = err;
		} else {
			/* P4.10e (review I4): the bind's reference to the GCB's map, taken before the map is used; the
			 * block itself is allocated with the member add (section 2), and a failing bind drops only this
			 * reference, so it never frees the map under a concurrent bind of the same GCB */
			pthread_mutex_lock(&vt->eng_lock);
			cg = iau_tile_ctxgcb_get(&vt->ctxgcbs, q->gcb_iova, &cr, &rc);
			pthread_mutex_unlock(&vt->eng_lock);
			if (cg == NULL) {
				snprintf(err, sizeof(err), rc == -EINVAL ? "the GCB is bound with another ctx region" :
					 "no memory for the GCB's ctx map");
				why = err;
			}
		}
		if (why != NULL) {
			SPDK_ERRLOG("IAU_ENGINE6_CTX_SPILL_REFUSED ctrlr=%d qid=%u gid=%u route=%u rc=%d: %s\n", ctrlr->dev_idx,
				    qid, v.gid, q->route, rc, why);
			goto fail;
		}
	}
	if (posix_memalign((void **)&qb, 64, sizeof(*qb)) != 0) {
		qb = NULL;
		rc = -ENOMEM;
		goto fail;
	}
	memset(qb, 0, sizeof(*qb));
	iau_tile_member_cfg(&v, q, qid, ctrlr->dev_idx, &mc);
	mc.ssd_sq = q_addr(&sq->mapping);
	mc.ssd_cq = q_addr(&cq->mapping);
	mc.sq_entries = sq->size;
	mc.cq_entries = cq->size;
	mc.ssd_sq_db = sq->dbl_tailp;		/* the doorbells the SSD thread reads */
	mc.ssd_cq_db = cq->dbl_headp;
	mc.host_sink = iau_page_u32(ctrlr, IAU_CQDB_SINK_OFFSET, qid);
	mc.echo = echo_va;
	mc.fault = vt->eng6_fault;
	rc = iau_eng6_member_init(&qb->m, &mc);
	if (rc != 0) {
		SPDK_ERRLOG("%s: IAU tile registration refused for sqid:%u route %u (%d): credits=%u entries=%u "
			    "cq_entries=%u nsid=%u lba_shift=%u gen=%u\n", ctrlr_id(ctrlr), qid, q->route, rc, v.m_credits,
			    v.m_entries, cq->size, v.m_nsid, v.m_lba_shift, v.m_gen);
		free(qb);
		qb = NULL;
		goto fail;
	}
	qb->ctrlr = ctrlr;
	qb->sq = sq;
	qb->mp = mp;
	qb->reg = *q;
	qb->qid = qid;
	qb->cqid = sq->cqid;
	qb->mem[0] = q_addr(&sq->mapping);
	qb->mem[1] = q_addr(&cq->mapping);
	qb->mem[2] = echo_va;
	qb->mem[3] = gcb_va;
	qb->mem[4] = d_va;
	qb->mem[5] = r_va;
	qb->mem[6] = ctx_va;
	*iau_page_u32(ctrlr, IAU_CQDB_SINK_OFFSET, qid) = 0;	/* the host's CQ-head doorbell lands here */

	pthread_mutex_lock(&vt->eng_lock);
	TAILQ_FOREACH(gr, &vt->grings, link) {
		if (gr->gcb_iova == q->gcb_iova && gr->view.k == v.k) {
			break;
		}
	}
	if (gr != NULL && !iau_tile_gcb_same_ring(&gr->view, &v)) {
		SPDK_ERRLOG("%s: IAU tile: sqid:%u names ring %u of GCB %#" PRIx64 ", which is bound with another layout "
			    "(gid %u/%u, d %#" PRIx64 "x%u, r %#" PRIx64 "x%u)\n", ctrlr_id(ctrlr), qid, v.k, q->gcb_iova,
			    gr->view.gid, v.gid, gr->view.d_iova, gr->view.d_entries, gr->view.r_iova, gr->view.r_entries);
		rc = -EINVAL;
	} else if (gr == NULL) {
		eng = NULL;
		rc = iau_tile_gcb_fresh(gcb_va, &v, err, sizeof(err));
		if (rc != 0) {
			SPDK_ERRLOG("%s: IAU tile: sqid:%u: %s\n", ctrlr_id(ctrlr), qid, err);
		} else if ((eng = iau_tile_pick_engine(ctrlr, v.gid, v.k, &e)) == NULL) {
			rc = -EINVAL;
		} else if ((ng = calloc(1, sizeof(*ng))) == NULL) {
			rc = -ENOMEM;
		} else {
			TAILQ_INIT(&ng->qbs);
			ng->gcb_iova = q->gcb_iova;
			ng->view = v;
			ng->eng = eng;
			ng->src = qb;
			ng->cg = cg;
			iau_tile_ring_cfg(&v, gcb_va, d_va, r_va, &rcfg);
			rc = iau_eng6_ring_init(&ng->ring, &rcfg);
			if (rc == 0) {
				rc = iau_engine_call(eng, &ng->hs, iau_engine6_ring_bind_msg, ng, true, ctrlr->dev_idx, qid);
				if (rc != 0) {
					SPDK_ERRLOG("IAU_ENGINE6_BIND_TIMEOUT ctrlr=%d qid=%u engine=%u gid=%u ring=%u: no answer "
						    "in 1 s\n", ctrlr->dev_idx, qid, e, v.gid, v.k);
					ng = NULL;	/* abandoned: the engine thread frees it, unpolled */
				} else {
					rc = ng->rc;
				}
			}
			if (rc == 0) {
				ng->bound = ng->listed = true;
				TAILQ_INSERT_TAIL(&vt->grings, ng, link);
				gr = ng;
				new_ring = true;
			} else {
				SPDK_ERRLOG("%s: IAU tile: ring %u of GCB %#" PRIx64 " cannot be bound to engine %u (%d)\n",
					    ctrlr_id(ctrlr), v.k, q->gcb_iova, e, rc);
				free(ng);
			}
		}
	}
	if (rc == 0 && cg != NULL) {
		/* P4.10e: the route's block by iau_mmio's BIND step 7 (sum, reuse, first fit; iau_mmio.v:742-774), under the
		 * same lock hold as the member add below: the bind either commits its block with its member or leaves the
		 * map as it found it (ctx_undo), as a refused BIND changes nothing on the tile */
		rc = iau_tile_ctx_alloc(&cg->map, q->route, ctx_s, &ctx_off, &ctx_undo, &ctx_why);
		if (rc == 0 && iau_tile_ctx_lines_check(&cg->map, ctx_off, ctx_s) != 0) {
			rc = -ERANGE;
		}
		if (rc == 0) {
			rc = iau_eng6_member_set_ctx(&qb->m, (uint8_t *)ctx_va + ctx_off, (uint32_t)iau_tile_ctx_lines_bytes(ctx_s),
						     ctx_s);
		}
		if (rc != 0) {
			iau_tile_ctx_undo(&cg->map, &ctx_undo);
			SPDK_ERRLOG("IAU_ENGINE6_CTX_SPILL_REFUSED ctrlr=%d qid=%u gid=%u route=%u rc=%d: no room for the route's "
				    "%u-slot block (%" PRIu64 " B) in the %u B region (%s)\n", ctrlr->dev_idx, qid, v.gid, q->route,
				    rc, ctx_s, iau_tile_ctx_block_bytes(ctx_s), cr.bytes, ctx_why == IAU_TILE_CTX_R_SUM ? "CTX: the GCB's held "
				    "blocks plus this one exceed it" : ctx_why == IAU_TILE_CTX_R_FRAG ? "CTX_FRAG: no hole" : "range");
		}
	}
	if (rc == 0) {
		qb->gr = gr;
		rc = iau_engine_call(gr->eng, &qb->hs, iau_engine6_member_bind_msg, qb, true, ctrlr->dev_idx, qid);
		if (rc != 0) {
			SPDK_ERRLOG("IAU_ENGINE6_BIND_TIMEOUT ctrlr=%d qid=%u gid=%u route=%u: no answer in 1 s\n",
				    ctrlr->dev_idx, qid, v.gid, q->route);
			qb = NULL;	/* abandoned: the engine thread finishes and frees it, unpolled */
		} else {
			rc = qb->rc;
		}
		if (rc == 0) {
			qb->cg = cg;		/* the bind's reference is the member's now */
			qb->ctx_off = ctx_off;
			TAILQ_INSERT_TAIL(&gr->qbs, qb, glink);
			gr->n_qb++;
			iau_tile_note_bind(&gr->note[q->route], iau_tile_ctrlr_key(&ctrlr->tile_key, &vt->tile_key_next), sq->cqid,
				       q->gen);
			sq->eng6_qb = qb;
		} else {
			if (cg != NULL) {
				iau_tile_ctx_undo(&cg->map, &ctx_undo);	/* the member is not in the ring: nothing spilled */
			}
			if (qb != NULL) {
				SPDK_ERRLOG("IAU_ENGINE6_REFUSED ctrlr=%d qid=%u gid=%u route=%u rc=%d\n", ctrlr->dev_idx, qid,
					    v.gid, q->route, rc);
			}
		}
	}
	if (rc != 0 && new_ring && gr->n_qb == 0) {
		/* the ring was bound for this member only: it leaves before the maps it polls go */
		iau_engine_call(gr->eng, &gr->hs, iau_engine6_ring_unbind_msg, gr, false, ctrlr->dev_idx, qid);
		gring_unlist(vt, gr);
		free(gr);
	}
	pthread_mutex_unlock(&vt->eng_lock);
	if (rc == 0) {
		if (cg != NULL) {
			SPDK_NOTICELOG("IAU_ENGINE6_CTX_SPILL_BIND ctrlr=%d qid=%u gid=%u route=%u ring=%u off=%u slots=%u block=%" PRIu64
				       " region=%u\n", ctrlr->dev_idx, qid, v.gid, q->route, v.k, ctx_off, ctx_s,
				       iau_tile_ctx_block_bytes(ctx_s), cr.bytes);
		}
		return 0;
	}
fail:
	if (cg != NULL) {
		/* P4.10e (I4): drop this bind's reference only; the map stays while another bind or a member holds one */
		pthread_mutex_lock(&vt->eng_lock);
		iau_tile_ctxgcb_put(&vt->ctxgcbs, cg);
		pthread_mutex_unlock(&vt->eng_lock);
	}
	if (qb != NULL) {
		iau_eng6_member_fini(&qb->m);
		free(qb);
	}
	iau_tile_unmap(ctrlr, mp);
	return rc;
}

/* V9 phase 1 (controller disable, before the IAU page is zeroed): every tile member of the controller stops
 * issuing (the SSD threads keep serving its SQ until delete_sq_done()), and the controller's next tile bind gets a
 * new key, so no later Delete I/O CQ can release a route of this incarnation (review I2). Decided under eng_lock
 * (M2). Idempotent. */
static void VU_IAU_ENG_OOL
ctrlr_eng6_disable(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct iau_eng6_qb *qb;
	uint32_t i;

	pthread_mutex_lock(&vt->eng_lock);
	iau_tile_ctrlr_key_reset(&ctrlr->tile_key);
	for (i = 1; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		qb = ctrlr->sqs[i] != NULL ? ctrlr->sqs[i]->eng6_qb : NULL;
		if (qb != NULL && !qb->dropped && !qb->stopped) {
			iau_engine_call(qb->gr->eng, &qb->hs, iau_engine6_member_stop_msg, qb, false, ctrlr->dev_idx, i);
			qb->stopped = true;
		}
	}
	pthread_mutex_unlock(&vt->eng_lock);
}

static bool VU_IAU_ENG_OOL
iau_tile_in(const void *p, const void *s, const void *e)
{
	return p != NULL && p >= s && p <= e;
}

/* qb's maps of the ring memory (GCB, D, R) lie in the region [s, e] being removed (s == NULL: none) */
static bool VU_IAU_ENG_OOL
iau_tile_ring_maps_in(const struct iau_eng6_qb *qb, const void *s, const void *e)
{
	return s != NULL && (iau_tile_in(qb->mem[3], s, e) || iau_tile_in(qb->mem[4], s, e) || iau_tile_in(qb->mem[5], s, e));
}

/* Drop ring gr (eng_lock): every member detaches with emit=false, the ring leaves its engine and the list. */
static void VU_IAU_ENG_OOL
gring_drop(struct nvmf_vfio_user_ctrlr *ctrlr, struct iau_tile_gring *gr, uint32_t qid, const char *why)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;

	SPDK_NOTICELOG("IAU tile ring gid=%u ring=%u: %s; dropping the ring (%u members, emit=false)\n", gr->view.gid,
		       gr->view.k, why, gr->n_qb);
	iau_engine_call(gr->eng, &gr->hs, iau_engine6_ring_drop_msg, gr, false, ctrlr->dev_idx, qid);
	gr->bound = false;
	gring_unlist(vt, gr);	/* P4.10e: the RTL's RING_RESET frees the ring's ctx blocks */
}

/*
 * The source plan for ring gr (eng_lock; iau_tile_src_plan(), review I3): `leaving` (or NULL) detaches now, [s, e] is
 * the DMA region being removed (s == NULL: none). The ring keeps its source, moves it to another member's
 * mappings (ring_remap) or, when no member's mapping of the ring survives or a remap fails (M3: the old maps
 * are never released under an engine that may still poll them), is dropped. Returns true if dropped.
 */
static bool VU_IAU_ENG_OOL
gring_handover(struct nvmf_vfio_user_ctrlr *ctrlr, struct iau_tile_gring *gr, const void *s, const void *e,
	       struct iau_eng6_qb *leaving, uint32_t qid)
{
	struct iau_tile_rm m[IAU_TILE_MAX_ROUTES];
	struct iau_eng6_qb *qbs[IAU_TILE_MAX_ROUTES], *qb;
	uint32_t n = 0, src = UINT32_MAX, to = 0;
	int plan;

	if (!gr->bound) {
		return true;
	}
	TAILQ_FOREACH(qb, &gr->qbs, glink) {
		if (n == IAU_TILE_MAX_ROUTES) {
			break;
		}
		m[n].alive = !qb->dropped;
		m[n].ring_in = qb->ctrlr == ctrlr && iau_tile_ring_maps_in(qb, s, e);
		m[n].leaving = qb == leaving;
		if (qb == gr->src) {
			src = n;
		}
		qbs[n++] = qb;
	}
	plan = iau_tile_src_plan(m, n, src, &to);
	if (plan == IAU_TILE_SRC_KEEP) {
		return false;
	}
	if (plan == IAU_TILE_SRC_MOVE) {
		struct iau_eng6_qb *alt = qbs[to];

		/* owner loss: the same host memory through alt's maps (no cross-controller mapping) */
		gr->rm_d = alt->mem[4];
		gr->rm_r = (void *)(uintptr_t)alt->mem[5];
		iau_tile_ring_ptrs(&gr->view, (volatile struct iau_tile_gcb *)(uintptr_t)alt->mem[3], &gr->rm_db, &gr->rm_dh,
			       &gr->rm_rc);
		iau_engine_call(gr->eng, &gr->hs, iau_engine6_ring_remap_msg, gr, false, ctrlr->dev_idx, qid);
		if (gr->rc == 0) {
			gr->src = alt;
			return false;
		}
		SPDK_ERRLOG("IAU tile ring gid=%u ring=%u: remap to ctrlr=%d qid=%u failed (%d)\n", gr->view.gid,
			    gr->view.k, alt->m.ctrlr, alt->qid, gr->rc);
		gring_drop(ctrlr, gr, qid, "its source cannot move");
		return true;
	}
	gring_drop(ctrlr, gr, qid, "no member's mapping of its memory survives");
	return true;
}

/* DMA region [s, e] of ctrlr goes (memory_region_remove_cb(), before any member detaches): every listed ring
 * whose source mapping lies in it hands the source over to a surviving mapping, or is dropped (review I3). */
static void VU_IAU_ENG_OOL
ctrlr_eng6_region(struct nvmf_vfio_user_ctrlr *ctrlr, const void *s, const void *e)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct iau_tile_gring *gr, *tmp;

	pthread_mutex_lock(&vt->eng_lock);
	TAILQ_FOREACH_SAFE(gr, &vt->grings, link, tmp) {
		if (gr->bound && gr->src != NULL && gr->src->ctrlr == ctrlr && iau_tile_ring_maps_in(gr->src, s, e)) {
			gring_handover(ctrlr, gr, s, e, NULL, 0);
		}
	}
	pthread_mutex_unlock(&vt->eng_lock);
}

/*
 * V9 phase 2 (and every other unbind): sq's member leaves its ring. With s == NULL always, else only if one of
 * its maps (SSD SQ, SSD CQ, echo, GCB, D, R) starts inside [s, e], the DMA region being removed (whose rings were
 * handed over first, ctrlr_eng6_region()). If the ring polls through this member's maps, the source moves first
 * (gring_handover()); the member then detaches with emit (a ring that is not dropped always has a surviving
 * source mapping), is finished and its maps released. A member of a dropped ring is only freed. Returns only
 * once the engine let go (never abandoned).
 */
static void VU_IAU_ENG_OOL
sq_eng6_detach(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, const void *s, const void *e)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct iau_eng6_qb *qb;
	struct iau_tile_gring *gr;
	bool hit = (s == NULL), dropped;
	int i;

	pthread_mutex_lock(&vt->eng_lock);
	qb = sq->eng6_qb;
	for (i = 0; qb != NULL && !hit && i < 7; i++) {
		hit = iau_tile_in(qb->mem[i], s, e);
	}
	if (qb == NULL || !hit) {
		pthread_mutex_unlock(&vt->eng_lock);
		return;
	}
	gr = qb->gr;
	sq->eng6_qb = NULL;
	if (!qb->dropped) {
		dropped = gr->src == qb ? gring_handover(ctrlr, gr, s, e, qb, sq->qid) : !gr->bound;
		if (!dropped) {
			qb->emit = iau_tile_member_emit(false);
			if (gr->n_qb == 1 && gr->bound) {
				/* the ring's last member: detach with the final D fetch and the unbind in one message
				 * (P4.5 fix A: R_DETACHED is the ring's last record) */
				iau_engine_call(gr->eng, &qb->hs, iau_engine6_member_last_msg, qb, false, ctrlr->dev_idx,
						sq->qid);
				if (qb->rc >= 0) {
					gr->bound = false;	/* off its engine already */
				}
			} else {
				iau_engine_call(gr->eng, &qb->hs, iau_engine6_member_unbind_msg, qb, false, ctrlr->dev_idx,
						sq->qid);
			}
		}
	}
	TAILQ_REMOVE(&gr->qbs, qb, glink);
	gr->n_qb--;
	/* a controller that goes away (disconnect, reset / shutdown, a failed connect that frees it) forgets the
	 * note's key at once (review I2) */
	iau_tile_note_detach(&gr->note[qb->reg.route], ctrlr->disconnect || ctrlr->reset_shn ||
			 ctrlr->endpoint->ctrlr != ctrlr);
	if (gr->n_qb == 0) {
		if (gr->bound) {	/* a dropped ring, or a last member the one-message path did not take out */
			iau_engine_call(gr->eng, &gr->hs, iau_engine6_ring_unbind_msg, gr, false, ctrlr->dev_idx, sq->qid);
		}
		gring_unlist(vt, gr);	/* P4.10e: the unbind frees the ring's ctx blocks (qb's reference keeps cg) */
		free(gr);
	} else if (gr->src == qb) {
		gr->src = NULL;	/* dropped ring: nothing polls it any more */
	}
	iau_tile_ctxgcb_put(&vt->ctxgcbs, qb->cg);	/* P4.10e: the member's reference */
	pthread_mutex_unlock(&vt->eng_lock);
	iau_eng6_member_fini(&qb->m);
	iau_tile_unmap(ctrlr, qb->mp);
	free(qb);
}

/*
 * The host's own Admin Delete I/O CQ of cqid succeeded (handle_del_io_q(), host_delete_cq()): release the
 * reservation of the tile route that was bound to it, with the member gen of that bind (I2; H1). The only caller
 * of iau_eng6_route_release(): a target-internal CQ deletion (delete_sq_done() at reset, shutdown or
 * disconnect) does not come here.
 */
static void VU_IAU_ENG_OOL
sq_eng6_cq_deleted(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t cqid)
{
	struct nvmf_vfio_user_transport *vt = ctrlr->transport;
	struct iau_tile_gring *gr;
	uint32_t r;
	uint8_t gen;

	if (!iau_mode_engine(&ctrlr->iau)) {
		return;
	}
	pthread_mutex_lock(&vt->eng_lock);
	TAILQ_FOREACH(gr, &vt->grings, link) {
		for (r = 0; r < IAU_TILE_MAX_ROUTES; r++) {
			/* the current incarnation only (tile_key 0: none bound since the last disable) */
			if (!iau_tile_note_cq_deleted(&gr->note[r], ctrlr->tile_key, cqid, &gen)) {
				continue;
			}
			gr->arg_route = (uint16_t)r;
			gr->arg_gen = gen;
			iau_engine_call(gr->eng, &gr->hs, iau_engine6_route_release_msg, gr, false, ctrlr->dev_idx, cqid);
			SPDK_NOTICELOG("IAU_ENGINE6_RELEASE ctrlr=%d cqid=%u gid=%u ring=%u route=%u gen=%u rc=%d\n",
				       ctrlr->dev_idx, cqid, gr->view.gid, gr->view.k, r, gen, gr->rc);
			if (gr->rc == 0 && gr->cg != NULL) {
				iau_tile_ctx_free(&gr->cg->map, (uint16_t)r);	/* P4.10b: the reservation ended, so the block */
			}
		}
	}
	pthread_mutex_unlock(&vt->eng_lock);
}

/* handle_del_io_q() for a CQ the host deletes (one call in .text; the CQ is deleted, then the release). */
static void VU_IAU_ENG_OOL
host_delete_cq(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq)
{
	uint16_t cqid = (uint16_t)cq->qid;

	delete_cq_done(ctrlr, cq);
	sq_eng6_cq_deleted(ctrlr, cqid);
}

/* Transport destroy (under eng_lock, before the endpoints go): tile rings still bound are reported and dropped;
 * their members are finished and their maps released here. */
static void VU_IAU_ENG_OOL
iau_eng6_destroy_all(struct nvmf_vfio_user_transport *vt)
{
	struct iau_tile_gring *gr;
	struct iau_eng6_qb *qb;

	while ((gr = TAILQ_FIRST(&vt->grings)) != NULL) {
		SPDK_ERRLOG("IAU tile ring gid=%u ring=%u: %u members still bound at transport destroy; dropping it "
			    "(IAU_ENGINE6_STATS follow)\n", gr->view.gid, gr->view.k, gr->n_qb);
		if (gr->bound) {
			iau_engine_call(gr->eng, &gr->hs, iau_engine6_ring_drop_msg, gr, false, -1, 0);
		}
		gring_unlist(vt, gr);
		while ((qb = TAILQ_FIRST(&gr->qbs)) != NULL) {
			TAILQ_REMOVE(&gr->qbs, qb, glink);
			qb->sq->eng6_qb = NULL;
			iau_eng6_member_fini(&qb->m);
			iau_tile_unmap(qb->ctrlr, qb->mp);
			iau_tile_ctxgcb_put(&vt->ctxgcbs, qb->cg);	/* P4.10e: the member's reference */
			free(qb);
		}
		free(gr);
	}
}

/* fp_fault_post()'s interrupt, as post_completion() raises it */
static void VU_IAU_ENG_OOL
fp_fault_irq(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_cq *cq)
{
	if ((cq->qid == 0 || !ctrlr->adaptive_irqs_enabled) && cq->ien && ctrlr_interrupt_enabled(ctrlr)) {
		if (vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv) != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n", ctrlr_id(ctrlr));
		}
	}
}

/* M5: an injected completion in the device trace, as the Mode 0 loop records its own */
static void VU_IAU_ENG_OOL
fp_fault_trace(struct nvmf_vfio_user_sq *sq, const struct fp_entry *e)
{
	struct devtrace_rec rec = {
		.qid = sq->qid,
		.cid = e->cid,
		.mode = 0,
		.ts_db_seen = e->ts_db_seen,
		.ts_fetched = e->ts_fetched,
		.ts_exec = e->ts_fetched,
		.ts_done = e->due_tsc,
		.ts_posted = spdk_get_ticks(),
	};

	devtrace_add(&sq->dt, &rec);
}

/* fp_pop_due_fault()'s post callback (D19): the injected completion of a fast-path command, posted as
 * post_completion() would, but with the knob's full status word (SC, SCT, DNR). The SQ's poll-group thread. */
static void VU_IAU_ENG_OOL
fp_fault_post(void *arg, const struct fp_entry *e)
{
	struct nvmf_vfio_user_sq *sq = arg;
	struct nvmf_vfio_user_ctrlr *ctrlr = sq->ctrlr;
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	struct spdk_nvme_cpl *cpl;
	uint16_t st;

	if (spdk_unlikely(cq == NULL || q_addr(&cq->mapping) == NULL)) {
		return;
	}
	if (cq_is_full(cq)) {
		SPDK_ERRLOG("%s: cqid:%u full (tail=%u, head=%u)\n", ctrlr_id(ctrlr), cq->qid, *cq_tailp(cq),
			    *cq_dbl_headp(cq));
		return;
	}
	cpl = ((struct spdk_nvme_cpl *)q_addr(&cq->mapping)) + *cq_tailp(cq);
	cpl->sqhd = *sq_headp(sq);
	cpl->sqid = sq->qid;
	cpl->cid = e->cid;
	cpl->cdw0 = 0;
	st = (uint16_t)((e->fault_st & ~FP_FAULT_MARK) | (cq->phase ? 1u : 0u));
	memcpy(&cpl->status, &st, sizeof(st));	/* the whole status word at once, as post_completion() */
	cq->nr_outstanding--;
	spdk_wmb();
	cq_tail_advance(cq);
	fp_fault_irq(ctrlr, cq);
	if (spdk_unlikely(sq->dt.on)) {
		fp_fault_trace(sq, e);
	}
}

/*
 * Every I/O SQ when it connects (sq_eng_connect(), any mode): the device model's call pointers. The mode-0
 * error knob (IAU_DEV_FAULT_ERR_*[_C<k>], D19) selects the fault variants for this SQ once; off, the SQ calls
 * fp_schedule / fp_pop_due through them. DEV_FAULT ctrlr= every= status= once per controller. The knob needs
 * the fast path; transport create refused it in Mode D (iau_faults_init()). Returns the Create I/O SQ status.
 */
static uint16_t VU_IAU_ENG_OOL
sq_fp_connect(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct iau_tile_fault f;
	char err[256];

	sq->fp_sched = fp_schedule;
	sq->fp_pop = fp_pop_due;
	if (iau_tile_dev_fault_load(&f, ctrlr->dev_idx, getenv, err, sizeof(err)) != 0) {
		SPDK_ERRLOG("%s: invalid device fault knob: %s; failing sqid:%u\n", ctrlr_id(ctrlr), err, sq->qid);
		return SPDK_NVME_SC_INVALID_FIELD;
	}
	if (f.every == 0) {
		return SPDK_NVME_SC_SUCCESS;
	}
	if (sq->fp == NULL || iau_mode_d(&ctrlr->iau) ||
	    fp_sq_set_fault(sq->fp, f.every, f.status, fp_fault_post, sq) != 0) {
		SPDK_ERRLOG("%s: IAU_DEV_FAULT_ERR_EVERY=%u needs the fast path and the mode 0 completion path "
			    "(fastpath=%d mode=%d); failing sqid:%u\n", ctrlr_id(ctrlr), f.every, sq->fp != NULL,
			    ctrlr->iau.mode, sq->qid);
		return SPDK_NVME_SC_INVALID_FIELD;
	}
	sq->fp_sched = fp_schedule_fault;
	sq->fp_pop = fp_pop_due_fault;
	if (!ctrlr->dev_fault_noted) {
		SPDK_NOTICELOG("DEV_FAULT ctrlr=%d every=%u status=0x%04x\n", ctrlr->dev_idx, f.every, f.status);
		ctrlr->dev_fault_noted = true;
	}
	return SPDK_NVME_SC_SUCCESS;
}

/* Teardown of an SQ with the error knob on: what it injected, once (check_l NX_FAULT). */
static void VU_IAU_ENG_OOL
sq_fp_fault_report(struct nvmf_vfio_user_sq *sq)
{
	if (sq->qid == 0 || sq->fp == NULL || sq->fp->fault_every == 0 || sq->fp->fault_reported) {
		return;
	}
	sq->fp->fault_reported = true;
	SPDK_NOTICELOG("DEV_FAULT_STATS ctrlr=%d qid=%u every=%u marked=%" PRIu64 " posted=%" PRIu64 "\n",
		       sq->ctrlr->dev_idx, sq->qid, sq->fp->fault_every, sq->fp->fault_marked, sq->fp->fault_posted);
}

/*
 * Transport create, before the engines: the grings list, the engine clock and the fault knobs. The device knob
 * (IAU_DEV_FAULT_ERR_*) is checked for every controller it names and refused in Mode D (whose completion path
 * it does not reach); the engine knob (IAU_ENG_FAULT_ERR_*) only exists in mode 5. 0, or -EINVAL after an
 * ERRLOG (the transport create fails).
 */
static int VU_IAU_ENG_OOL
iau_faults_init(struct nvmf_vfio_user_transport *vt)
{
	struct iau_tile_fault f;
	struct iau_ctx ic;
	char err[256];
	bool dev_on, spill;
	int mode;

	TAILQ_INIT(&vt->grings);
	TAILQ_INIT(&vt->ctxgcbs);
	if (iau_tile_clock_init(&vt->eng6_clock, spdk_get_ticks_hz()) != 0) {
		SPDK_ERRLOG("IAU: tick rate %" PRIu64 " Hz is too low for the v6 engine clock\n", spdk_get_ticks_hz());
		return -EINVAL;
	}
	if (iau_ctx_init(&ic, NULL, spdk_get_ticks_hz()) != 0) {
		iau_ctx_fini(&ic);
		return 0;	/* a bad IAU_* environment fails every listener (vu_dev_cfg_check()) */
	}
	mode = ic.mode;
	iau_ctx_fini(&ic);
	if (iau_tile_dev_fault_scan(environ, getenv, &dev_on, err, sizeof(err)) != 0) {
		SPDK_ERRLOG("IAU: invalid device fault knob: %s\n", err);
		return -EINVAL;
	}
	if (dev_on && mode == IAU_MODE_D) {
		SPDK_ERRLOG("IAU: IAU_DEV_FAULT_ERR_EVERY needs the mode 0 completion path (IAU_UNCORE_MODE 0 or 5), not "
			    "Mode D\n");
		return -EINVAL;
	}
	if (iau_tile_eng_fault_load(&f, getenv, err, sizeof(err)) != 0) {
		SPDK_ERRLOG("IAU: invalid engine fault knob: %s\n", err);
		return -EINVAL;
	}
	if (f.every != 0 && mode != IAU_MODE_ENGINE) {
		SPDK_ERRLOG("IAU: IAU_ENG_FAULT_ERR_EVERY=%u needs IAU_UNCORE_MODE=5 (the v6 engine)\n", f.every);
		return -EINVAL;
	}
	if (iau_tile_spill_load(&spill, getenv, err, sizeof(err)) != 0) {
		SPDK_ERRLOG("IAU: invalid IAU_TILE_CTX_SPILL: %s\n", err);
		return -EINVAL;
	}
	if (spill && mode != IAU_MODE_ENGINE) {
		SPDK_ERRLOG("IAU: IAU_TILE_CTX_SPILL=1 needs IAU_UNCORE_MODE=5 (the v6 engine)\n");
		return -EINVAL;
	}
	vt->eng6_spill = spill;
	if (spill) {
		SPDK_NOTICELOG("IAU_ENGINE6_CTX_SPILL on=1 line=%u\n", IAU_ENG6_SPILL_LINE);
	}
	vt->eng6_fault.err_every = f.every;
	vt->eng6_fault.err_status = f.status;
	if (f.every != 0) {
		SPDK_NOTICELOG("IAU_ENG6_FAULT every=%u status=0x%04x\n", f.every, f.status);
	}
	return 0;
}

/* The last VU_IAU_ENG_OOL function is above this line. */
VU_IAU_ENG_OOL_END;

/*
 * IAU-next P3: the one validator of a controller's configuration. nvmf_vfio_user_listen() runs it so
 * every config error fails the listener RPC (start_target.sh) instead of hanging the host's first
 * connect; fp_load_cfg() runs it again at controller creation and then uses the result. Checks:
 *  - devcfg_load(): IAU_DEV_<KEY>_C<k> > IAU_DEV_<KEY> > default for k = dev_idx, and IAU_DEV_MODEL;
 *  - svc_ticks = hz / tmax >= 1 (hz = spdk_get_ticks_hz());
 *  - Mode D (IAU_UNCORE_MODE=4) and mode 5 need this controller's fast path (IAU_DEV_FASTPATH_C<k>=0 is
 *    refused);
 *  - mode 5 (P3.9) also needs IAU_CQ_BATCH_N=1 (the engine publishes every record at once) and the
 *    transport's IAU-engine threads (IAU_ENGINE_CPUS, parsed at transport create by devcfg_cpu_list(),
 *    which refuses a CPU listed twice) with no poll group on an engine core (iau_engines_check()); a
 *    build with IAU_ENGINE_AVAILABLE == 0 refuses mode 5.
 * ic = the IAU_* settings iau_ctx_init() parsed (mode, batch_n). Returns 0 with *c and *svc_ticks
 * filled, or -EINVAL after logging the reason prefixed with `who`.
 */
static int
vu_dev_cfg_check(struct nvmf_vfio_user_transport *vt, const char *who, int dev_idx, const struct iau_ctx *ic,
		 struct devcfg *c, uint64_t *svc_ticks)
{
	uint64_t hz = spdk_get_ticks_hz();

	if (devcfg_load(c, dev_idx, getenv) != 0) {
		SPDK_ERRLOG("%s: invalid device-model config: %s\n", who, c->err);
		return -EINVAL;
	}
	*svc_ticks = hz / c->tmax;	/* devcfg: tmax >= 1 */
	if (*svc_ticks < 1) {
		SPDK_ERRLOG("%s: IAU_DEV_TMAX=%" PRIu64 " exceeds the tick rate %" PRIu64 " Hz (svc_ticks 0)\n",
			    who, c->tmax, hz);
		return -EINVAL;
	}
	if (ic->mode == IAU_MODE_D && !c->fastpath) {
		SPDK_ERRLOG("%s: IAU Mode D (IAU_UNCORE_MODE=4) requires the fast path, but controller %d has "
			    "IAU_DEV_FASTPATH=0\n", who, dev_idx);
		return -EINVAL;
	}
	if (ic->mode == IAU_MODE_ENGINE) {
		return iau_engines_check(vt, who, dev_idx, ic, c);
	}
	return 0;
}

/*
 * Loads the controller's device model (IAU-next P3) through vu_dev_cfg_check(). Called at controller
 * creation (after iau_ctx_init(), whose mode it checks against); fp_cfg_loaded is set only on success.
 * Returns 0, or -EINVAL: an invalid value is fatal for the controller (no silent fast-path-off fallback).
 * Prints one DEV_MODEL line per controller, the default model included (check_l.py NX_DEVMODEL).
 */
static int
fp_load_cfg(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	uint64_t hz = spdk_get_ticks_hz();
	uint64_t svc_ticks;
	struct devcfg c;

	if (ctrlr->fp_cfg_loaded) {
		return 0;
	}
	if (vu_dev_cfg_check(ctrlr->transport, ctrlr_id(ctrlr), ctrlr->dev_idx, &ctrlr->iau, &c, &svc_ticks) != 0) {
		return -EINVAL;
	}
	ctrlr->fp_on = c.fastpath;
	ctrlr->fp_lmin_ns = c.lmin_ns;
	ctrlr->fp_tmax = c.tmax;
	ctrlr->fp_channels = c.channels;
	ctrlr->fp_model_ctrlr = c.model_ctrlr;
	ctrlr->fp_lmin_ticks = ctrlr->fp_lmin_ns * hz / 1000000000ULL;
	ctrlr->fp_svc_ticks = svc_ticks;
	if (ctrlr->fp_on && ctrlr->fp_model_ctrlr &&
	    fp_chan_init(&ctrlr->fp_chan, ctrlr->fp_channels, ctrlr->fp_svc_ticks) != 0) {
		SPDK_ERRLOG("%s: fp_chan_init(channels=%u) failed\n", ctrlr_id(ctrlr), ctrlr->fp_channels);
		return -EINVAL;
	}
	ctrlr->fp_cfg_loaded = true;
	if (ctrlr->fp_on) {
		SPDK_NOTICELOG("IAU fastpath on: lmin_ns=%lu tmax=%lu channels=%u\n",
			       ctrlr->fp_lmin_ns, ctrlr->fp_tmax, ctrlr->fp_channels);
	}
	/* One line per controller, the default model included (check_l NX_DEVMODEL). */
	SPDK_NOTICELOG("DEV_MODEL ctrlr=%d model=%s fastpath=%d channels=%u tmax=%" PRIu64 " lmin_ns=%" PRIu64
		       " svc_ticks=%" PRIu64 " lmin_ticks=%" PRIu64 " override=%s\n", ctrlr->dev_idx,
		       ctrlr->fp_model_ctrlr ? "ctrlr" : "sq", ctrlr->fp_on ? 1 : 0, ctrlr->fp_channels,
		       ctrlr->fp_tmax, ctrlr->fp_lmin_ns, ctrlr->fp_svc_ticks, ctrlr->fp_lmin_ticks, c.overrides);
	return 0;
}

/* Deletes a SQ, if this SQ is the last user of the associated CQ
 * and the controller is being shut down/reset or vfio-user client disconnects,
 * then the CQ is also deleted.
 */
static void
delete_sq_done(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_cq *cq;
	uint16_t cqid;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: delete sqid:%d=%p done\n", ctrlr_id(vu_ctrlr),
		      sq->qid, sq);

	/* P3.9, mode 5: the IAU engine lets go of this SQ (IAU_ENGINE_STATS) before its queues and rings
	 * are unmapped; the SSD side's OPC_STATS follows below. */
	sq_eng_detach(vu_ctrlr, sq, NULL, NULL);

	/* Free SQ resources */
	unmap_q(vu_ctrlr, &sq->mapping);

	free_sq_reqs(sq);

	if (sq->qid != 0) {
		sq_iau_detach(vu_ctrlr, sq, true);
		sq_opc_stats_dump(sq);
		devtrace_dump(&sq->dt, sq->qid);
		devtrace_fini(&sq->dt);
		sq_fp_free(sq);
	}

	sq->size = 0;

	sq->sq_state = VFIO_USER_SQ_DELETED;

	/* Controller RESET and SHUTDOWN are special cases,
	 * VM may not send DELETE IO SQ/CQ commands, NVMf library
	 * will disconnect IO queue pairs.
	 */
	if (vu_ctrlr->reset_shn || vu_ctrlr->disconnect) {
		cqid = sq->cqid;
		cq = vu_ctrlr->cqs[cqid];

		SPDK_DEBUGLOG(nvmf_vfio, "%s: try to delete cqid:%u=%p\n", ctrlr_id(vu_ctrlr),
			      cq->qid, cq);

		assert(cq->cq_ref > 0);
		if (--cq->cq_ref == 0) {
			delete_cq_done(vu_ctrlr, cq);
		}
	}
}

static void
free_qp(struct nvmf_vfio_user_ctrlr *ctrlr, uint16_t qid)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	if (ctrlr == NULL) {
		return;
	}

	sq = ctrlr->sqs[qid];
	if (sq) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: Free sqid:%u\n", ctrlr_id(ctrlr), qid);
		/* P3.9: every path that frees a controller has unbound its SQs (iau_engines_create()). */
		assert(sq->eng_qb == NULL);
		unmap_q(ctrlr, &sq->mapping);

		free_sq_reqs(sq);

		if (qid != 0) {
			/* No-op if delete_sq_done() already detached the IAU rings. */
			sq_iau_detach(ctrlr, sq, true);
			sq_opc_stats_dump(sq);	/* no-op if already printed */
			/* No-op if delete_sq_done() already dumped and freed the ring. */
			devtrace_dump(&sq->dt, qid);
			devtrace_fini(&sq->dt);
			sq_fp_free(sq);
		}

		free(sq->mapping.sg);
		free(sq);
		ctrlr->sqs[qid] = NULL;
	}

	cq = ctrlr->cqs[qid];
	if (cq) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: Free cqid:%u\n", ctrlr_id(ctrlr), qid);
		unmap_q(ctrlr, &cq->mapping);
		free(cq->mapping.sg);
		free(cq);
		ctrlr->cqs[qid] = NULL;
	}
}

static int
init_sq(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_transport *transport,
	const uint16_t id)
{
	struct nvmf_vfio_user_sq *sq;

	assert(ctrlr != NULL);
	assert(transport != NULL);
	assert(ctrlr->sqs[id] == NULL);

	sq = calloc(1, sizeof(*sq));
	if (sq == NULL) {
		return -ENOMEM;
	}
	sq->mapping.sg = calloc(1, dma_sg_size());
	if (sq->mapping.sg == NULL) {
		free(sq);
		return -ENOMEM;
	}

	sq->qid = id;
	sq->qpair.qid = id;
	sq->qpair.transport = transport;
	sq->ctrlr = ctrlr;
	ctrlr->sqs[id] = sq;

	TAILQ_INIT(&sq->free_reqs);

	return 0;
}

static int
init_cq(struct nvmf_vfio_user_ctrlr *vu_ctrlr, const uint16_t id)
{
	struct nvmf_vfio_user_cq *cq;

	assert(vu_ctrlr != NULL);
	assert(vu_ctrlr->cqs[id] == NULL);

	cq = calloc(1, sizeof(*cq));
	if (cq == NULL) {
		return -ENOMEM;
	}
	cq->mapping.sg = calloc(1, dma_sg_size());
	if (cq->mapping.sg == NULL) {
		free(cq);
		return -ENOMEM;
	}

	cq->qid = id;
	vu_ctrlr->cqs[id] = cq;

	return 0;
}

static int
alloc_sq_reqs(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_req *vu_req, *tmp;
	size_t req_size;
	uint32_t i;

	req_size = sizeof(struct nvmf_vfio_user_req) +
		   (dma_sg_size() * NVMF_VFIO_USER_MAX_IOVECS);

	for (i = 0; i < sq->size; i++) {
		struct spdk_nvmf_request *req;

		vu_req = calloc(1, req_size);
		if (vu_req == NULL) {
			goto err;
		}

		req = &vu_req->req;
		req->qpair = &sq->qpair;
		req->rsp = (union nvmf_c2h_msg *)&vu_req->rsp;
		req->cmd = (union nvmf_h2c_msg *)&vu_req->cmd;
		req->stripped_data = NULL;

		TAILQ_INSERT_TAIL(&sq->free_reqs, vu_req, link);
	}

	return 0;

err:
	TAILQ_FOREACH_SAFE(vu_req, &sq->free_reqs, link, tmp) {
		free(vu_req);
	}
	return -ENOMEM;
}

static volatile uint32_t *
ctrlr_doorbell_ptr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	return ctrlr->sdbl != NULL ?
	       ctrlr->sdbl->shadow_doorbells :
	       ctrlr->bar0_doorbells;
}

static uint16_t
handle_create_io_sq(struct nvmf_vfio_user_ctrlr *ctrlr,
		    struct spdk_nvme_cmd *cmd, uint16_t *sct)
{
	struct nvmf_vfio_user_transport *vu_transport = ctrlr->transport;
	struct nvmf_vfio_user_sq *sq;
	uint32_t qsize;
	uint16_t cqid;
	uint16_t qid;
	int err;

	qid = cmd->cdw10_bits.create_io_q.qid;
	cqid = cmd->cdw11_bits.create_io_sq.cqid;
	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	if (ctrlr->sqs[qid] == NULL) {
		err = init_sq(ctrlr, ctrlr->sqs[0]->qpair.transport, qid);
		if (err != 0) {
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	if (cqid == 0 || cqid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
		SPDK_ERRLOG("%s: invalid cqid:%u\n", ctrlr_id(ctrlr), cqid);
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
	}

	/* CQ must be created before SQ. */
	if (!io_q_exists(ctrlr, cqid, true)) {
		SPDK_ERRLOG("%s: cqid:%u does not exist\n", ctrlr_id(ctrlr), cqid);
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_COMPLETION_QUEUE_INVALID;
	}

	if (cmd->cdw11_bits.create_io_sq.pc != 0x1) {
		SPDK_ERRLOG("%s: non-PC SQ not supported\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	sq = ctrlr->sqs[qid];
	sq->size = qsize;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: sqid:%d cqid:%d\n", ctrlr_id(ctrlr),
		      qid, cqid);

	sq->mapping.prp1 = cmd->dptr.prp.prp1;
	sq->mapping.len = sq->size * sizeof(struct spdk_nvme_cmd);

	err = map_q(ctrlr, &sq->mapping, MAP_INITIALIZE);
	if (err) {
		SPDK_ERRLOG("%s: failed to map I/O queue: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: mapped sqid:%d IOVA=%#lx vaddr=%p\n",
		      ctrlr_id(ctrlr), qid, cmd->dptr.prp.prp1,
		      q_addr(&sq->mapping));

	err = alloc_sq_reqs(ctrlr, sq);
	if (err < 0) {
		SPDK_ERRLOG("%s: failed to allocate SQ requests: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	err = devtrace_init(&sq->dt, 0);
	if (err != 0) {
		SPDK_ERRLOG("%s: devtrace_init failed for sqid:%u: %d\n", ctrlr_id(ctrlr), qid, err);
	}

	/* No-op since P3: the device model is loaded at controller creation. The call stays so this
	 * function (inlined into the SQ poller via the admin path) compiles as in P1. With
	 * IAU_DEV_MODEL=ctrlr the SQ binds the controller's fp_chan when it connects, on its poll
	 * group's thread (sq_fp_bind()). sq_fp_free() finishes a previous fp_sq of this SQ (and its
	 * fp_chan binding) before the new one is initialised. */
	(void)fp_load_cfg(ctrlr);
	sq_fp_free(sq);
	if (ctrlr->fp_on) {
		sq->fp = calloc(1, sizeof(*sq->fp));
		if (sq->fp == NULL ||
		    fp_sq_init(sq->fp, sq->size, ctrlr->fp_channels,
			       ctrlr->fp_svc_ticks, ctrlr->fp_lmin_ticks) != 0) {
			SPDK_ERRLOG("%s: fastpath init failed for sqid:%u\n", ctrlr_id(ctrlr), qid);
			free(sq->fp);
			sq->fp = NULL;
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	if (iau_mode_d(&ctrlr->iau) && qid != 0) {
		uint64_t desc_iova = iau_desc_ring_iova(&ctrlr->iau, qid);
		uint64_t rec_iova = iau_rec_ring_iova(&ctrlr->iau, qid);
		void *desc_va = NULL, *rec_va = NULL;

		sq->iau_sg_desc = calloc(1, dma_sg_size());
		sq->iau_sg_rec = calloc(1, dma_sg_size());
		if (sq->iau_sg_desc == NULL || sq->iau_sg_rec == NULL) {
			err = -ENOMEM;
		} else if (desc_iova == 0 || rec_iova == 0) {
			SPDK_ERRLOG("%s: IAU Mode D: sqid:%u has no ring IOVAs (desc=%#lx rec=%#lx)\n",
				    ctrlr_id(ctrlr), qid, desc_iova, rec_iova);
			err = -EINVAL;
		} else {
			desc_va = map_one(ctrlr->endpoint->vfu_ctx, desc_iova,
					  (uint64_t)qsize * IAU_DESC_D_BYTES,
					  sq->iau_sg_desc, &sq->iau_iov_desc, MAP_R);
			rec_va = map_one(ctrlr->endpoint->vfu_ctx, rec_iova,
					 (uint64_t)qsize * IAU_REC_BYTES,
					 sq->iau_sg_rec, &sq->iau_iov_rec, MAP_RW);
			if (desc_va == NULL || rec_va == NULL) {
				SPDK_ERRLOG("%s: IAU Mode D: failed to map rings of sqid:%u\n",
					    ctrlr_id(ctrlr), qid);
				/* map_one() leaves iov_base unset on failure; put what succeeded */
				if (desc_va == NULL) {
					sq->iau_iov_desc.iov_base = NULL;
				}
				if (rec_va == NULL) {
					sq->iau_iov_rec.iov_base = NULL;
				}
				err = -EFAULT;
			} else {
				err = iau_sq_attach(&ctrlr->iau, qid, qsize, desc_va, rec_va);
			}
		}
		if (err != 0) {
			SPDK_ERRLOG("%s: IAU attach failed for sqid:%u: %d\n", ctrlr_id(ctrlr), qid, err);
			/* Unwind everything this command set up so a retry starts clean. */
			sq->iau_attached = true;	/* let the helper release the maps/sgs */
			sq_iau_detach(ctrlr, sq, false);
			sq_fp_free(sq);
			devtrace_fini(&sq->dt);
			free_sq_reqs(sq);
			unmap_q(ctrlr, &sq->mapping);
			sq->size = 0;
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INVALID_FIELD;
		}
		sq->iau_attached = true;
	}

	sq->cqid = cqid;
	ctrlr->cqs[sq->cqid]->cq_ref++;
	sq->sq_state = VFIO_USER_SQ_CREATED;
	memset(sq->opc_cnt, 0, sizeof(sq->opc_cnt));
	sq->opc_armed = true;
	*sq_headp(sq) = 0;

	sq->dbl_tailp = ctrlr_doorbell_ptr(ctrlr) + queue_index(qid, false);

	/*
	 * We should always reset the doorbells.
	 *
	 * The Specification prohibits the controller from writing to the shadow
	 * doorbell buffer, however older versions of the Linux NVMe driver
	 * don't reset the shadow doorbell buffer after a Queue-Level or
	 * Controller-Level reset, which means that we're left with garbage
	 * doorbell values.
	 */
	*sq_dbl_tailp(sq) = 0;

	if (ctrlr->sdbl != NULL) {
		sq->need_rearm = true;

		if (!set_sq_eventidx(sq)) {
			SPDK_ERRLOG("%s: host updated SQ tail doorbell before "
				    "sqid:%hu was initialized\n",
				    ctrlr_id(ctrlr), qid);
			fail_ctrlr(ctrlr);
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	/*
	 * Create our new I/O qpair. This asynchronously invokes, on a suitable
	 * poll group, the nvmf_vfio_user_poll_group_add() callback, which will
	 * call spdk_nvmf_request_exec() with a generated fabrics
	 * connect command. This command is then eventually completed via
	 * handle_queue_connect_rsp().
	 */
	sq->create_io_sq_cmd = *cmd;
	sq->post_create_io_sq_completion = true;

	spdk_nvmf_tgt_new_qpair(ctrlr->transport->transport.tgt,
				&sq->qpair);

	*sct = SPDK_NVME_SCT_GENERIC;
	return SPDK_NVME_SC_SUCCESS;
}

static uint16_t
handle_create_io_cq(struct nvmf_vfio_user_ctrlr *ctrlr,
		    struct spdk_nvme_cmd *cmd, uint16_t *sct)
{
	struct nvmf_vfio_user_cq *cq;
	uint32_t qsize;
	uint16_t qid;
	int err;

	qid = cmd->cdw10_bits.create_io_q.qid;
	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	if (ctrlr->cqs[qid] == NULL) {
		err = init_cq(ctrlr, qid);
		if (err != 0) {
			*sct = SPDK_NVME_SCT_GENERIC;
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	if (cmd->cdw11_bits.create_io_cq.pc != 0x1) {
		SPDK_ERRLOG("%s: non-PC CQ not supported\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	if (cmd->cdw11_bits.create_io_cq.iv > NVMF_VFIO_USER_MSIX_NUM - 1) {
		SPDK_ERRLOG("%s: IV is too big\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR;
	}

	cq = ctrlr->cqs[qid];
	cq->size = qsize;

	cq->mapping.prp1 = cmd->dptr.prp.prp1;
	cq->mapping.len = cq->size * sizeof(struct spdk_nvme_cpl);

	cq->dbl_headp = ctrlr_doorbell_ptr(ctrlr) + queue_index(qid, true);

	err = map_q(ctrlr, &cq->mapping, MAP_RW | MAP_INITIALIZE);
	if (err) {
		SPDK_ERRLOG("%s: failed to map I/O queue: %m\n", ctrlr_id(ctrlr));
		*sct = SPDK_NVME_SCT_GENERIC;
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: mapped cqid:%u IOVA=%#lx vaddr=%p\n",
		      ctrlr_id(ctrlr), qid, cmd->dptr.prp.prp1,
		      q_addr(&cq->mapping));

	cq->ien = cmd->cdw11_bits.create_io_cq.ien;
	cq->iv = cmd->cdw11_bits.create_io_cq.iv;
	cq->phase = true;
	cq->cq_state = VFIO_USER_CQ_CREATED;

	*cq_tailp(cq) = 0;

	/*
	 * We should always reset the doorbells.
	 *
	 * The Specification prohibits the controller from writing to the shadow
	 * doorbell buffer, however older versions of the Linux NVMe driver
	 * don't reset the shadow doorbell buffer after a Queue-Level or
	 * Controller-Level reset, which means that we're left with garbage
	 * doorbell values.
	 */
	*cq_dbl_headp(cq) = 0;

	*sct = SPDK_NVME_SCT_GENERIC;
	return SPDK_NVME_SC_SUCCESS;
}

/*
 * Creates a completion or submission I/O queue. Returns 0 on success, -errno
 * on error.
 */
static int
handle_create_io_q(struct nvmf_vfio_user_ctrlr *ctrlr,
		   struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	struct nvmf_vfio_user_transport *vu_transport = ctrlr->transport;
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_SUCCESS;
	uint32_t qsize;
	uint16_t qid;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	qid = cmd->cdw10_bits.create_io_q.qid;
	if (qid == 0 || qid >= vu_transport->transport.opts.max_qpairs_per_ctrlr) {
		SPDK_ERRLOG("%s: invalid qid=%d, max=%d\n", ctrlr_id(ctrlr),
			    qid, vu_transport->transport.opts.max_qpairs_per_ctrlr);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	if (io_q_exists(ctrlr, qid, is_cq)) {
		SPDK_ERRLOG("%s: %cqid:%d already exists\n", ctrlr_id(ctrlr),
			    is_cq ? 'c' : 's', qid);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	qsize = cmd->cdw10_bits.create_io_q.qsize + 1;
	if (qsize == 1 || qsize > max_queue_size(ctrlr)) {
		SPDK_ERRLOG("%s: invalid I/O queue size %u\n", ctrlr_id(ctrlr), qsize);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_SIZE;
		goto out;
	}

	if (is_cq) {
		sc = handle_create_io_cq(ctrlr, cmd, &sct);
	} else {
		sc = handle_create_io_sq(ctrlr, cmd, &sct);

		if (sct == SPDK_NVME_SCT_GENERIC &&
		    sc == SPDK_NVME_SC_SUCCESS) {
			/* Completion posted asynchronously. */
			return 0;
		}
	}

out:
	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/* For ADMIN I/O DELETE SUBMISSION QUEUE the NVMf library will disconnect and free
 * queue pair, so save the command id and controller in a context.
 */
struct vfio_user_delete_sq_ctx {
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	uint16_t cid;
};

static void
vfio_user_qpair_delete_cb(void *cb_arg)
{
	struct vfio_user_delete_sq_ctx *ctx = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx->vu_ctrlr;
	struct nvmf_vfio_user_cq *admin_cq = vu_ctrlr->cqs[0];

	assert(admin_cq != NULL);
	assert(admin_cq->group != NULL);
	assert(admin_cq->group->group->thread != NULL);
	if (admin_cq->group->group->thread != spdk_get_thread()) {
		spdk_thread_send_msg(admin_cq->group->group->thread,
				     vfio_user_qpair_delete_cb,
				     cb_arg);
	} else {
		post_completion(vu_ctrlr, admin_cq, 0, 0,
				ctx->cid,
				SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
		free(ctx);
	}
}

/*
 * Deletes a completion or submission I/O queue.
 */
static int
handle_del_io_q(struct nvmf_vfio_user_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd, const bool is_cq)
{
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_SUCCESS;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: delete I/O %cqid:%d\n",
		      ctrlr_id(ctrlr), is_cq ? 'c' : 's',
		      cmd->cdw10_bits.delete_io_q.qid);

	if (!io_q_exists(ctrlr, cmd->cdw10_bits.delete_io_q.qid, is_cq)) {
		SPDK_ERRLOG("%s: I/O %cqid:%d does not exist\n", ctrlr_id(ctrlr),
			    is_cq ? 'c' : 's', cmd->cdw10_bits.delete_io_q.qid);
		sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		goto out;
	}

	if (is_cq) {
		cq = ctrlr->cqs[cmd->cdw10_bits.delete_io_q.qid];
		if (cq->cq_ref) {
			SPDK_ERRLOG("%s: the associated SQ must be deleted first\n", ctrlr_id(ctrlr));
			sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			sc = SPDK_NVME_SC_INVALID_QUEUE_DELETION;
			goto out;
		}
		/* P4.4: the host's own Delete I/O CQ (the I2 reap trigger, host_delete_cq()) */
		host_delete_cq(ctrlr, cq);
	} else {
		/*
		 * Deletion of the CQ is only deferred to delete_sq_done() on
		 * VM reboot or CC.EN change, so we have to delete it in all
		 * other cases.
		 */
		sq = ctrlr->sqs[cmd->cdw10_bits.delete_io_q.qid];
		sq->delete_ctx = calloc(1, sizeof(*sq->delete_ctx));
		if (!sq->delete_ctx) {
			sct = SPDK_NVME_SCT_GENERIC;
			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		sq->delete_ctx->vu_ctrlr = ctrlr;
		sq->delete_ctx->cid = cmd->cid;
		sq->sq_state = VFIO_USER_SQ_DELETED;
		assert(ctrlr->cqs[sq->cqid]->cq_ref);
		ctrlr->cqs[sq->cqid]->cq_ref--;

		spdk_nvmf_qpair_disconnect(&sq->qpair);
		return 0;
	}

out:
	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/*
 * Configures Shadow Doorbells.
 */
static int
handle_doorbell_buffer_config(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	struct nvmf_vfio_user_shadow_doorbells *sdbl = NULL;
	uint32_t dstrd;
	uintptr_t page_size, page_mask;
	uint64_t prp1, prp2;
	uint16_t sct = SPDK_NVME_SCT_GENERIC;
	uint16_t sc = SPDK_NVME_SC_INVALID_FIELD;

	assert(ctrlr != NULL);
	assert(ctrlr->endpoint != NULL);
	assert(cmd != NULL);

	dstrd = doorbell_stride(ctrlr);
	page_size = memory_page_size(ctrlr);
	page_mask = memory_page_mask(ctrlr);

	/* FIXME: we don't check doorbell stride when setting queue doorbells. */
	if ((4u << dstrd) * NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR > page_size) {
		SPDK_ERRLOG("%s: doorbells do not fit in a single host page",
			    ctrlr_id(ctrlr));

		goto out;
	}

	/* Verify guest physical addresses passed as PRPs. */
	if (cmd->psdt != SPDK_NVME_PSDT_PRP) {
		SPDK_ERRLOG("%s: received Doorbell Buffer Config without PRPs",
			    ctrlr_id(ctrlr));

		goto out;
	}

	prp1 = cmd->dptr.prp.prp1;
	prp2 = cmd->dptr.prp.prp2;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: configuring shadow doorbells with PRP1=%#lx and PRP2=%#lx (GPAs)\n",
		      ctrlr_id(ctrlr), prp1, prp2);

	if (prp1 == prp2
	    || prp1 != (prp1 & page_mask)
	    || prp2 != (prp2 & page_mask)) {
		SPDK_ERRLOG("%s: invalid shadow doorbell GPAs\n",
			    ctrlr_id(ctrlr));

		goto out;
	}

	/* Map guest physical addresses to our virtual address space. */
	sdbl = map_sdbl(ctrlr->endpoint->vfu_ctx, prp1, prp2, page_size);
	if (sdbl == NULL) {
		SPDK_ERRLOG("%s: failed to map shadow doorbell buffers\n",
			    ctrlr_id(ctrlr));

		goto out;
	}

	ctrlr->shadow_doorbell_buffer = prp1;
	ctrlr->eventidx_buffer = prp2;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: mapped shadow doorbell buffers [%p, %p) and [%p, %p)\n",
		      ctrlr_id(ctrlr),
		      sdbl->iovs[0].iov_base,
		      sdbl->iovs[0].iov_base + sdbl->iovs[0].iov_len,
		      sdbl->iovs[1].iov_base,
		      sdbl->iovs[1].iov_base + sdbl->iovs[1].iov_len);


	/*
	 * Set all possible CQ head doorbells to polling mode now, such that we
	 * don't have to worry about it later if the host creates more queues.
	 *
	 * We only ever want interrupts for writes to the SQ tail doorbells
	 * (which are initialised in set_ctrlr_intr_mode() below).
	 */
	for (uint16_t i = 0; i < NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR; ++i) {
		sdbl->eventidxs[queue_index(i, true)] = NVMF_VFIO_USER_EVENTIDX_POLL;
	}

	/* Update controller. */
	SWAP(ctrlr->sdbl, sdbl);

	/*
	 * Copy doorbells from either the previous shadow doorbell buffer or the
	 * BAR0 doorbells and make I/O queue doorbells point to the new buffer.
	 *
	 * This needs to account for older versions of the Linux NVMe driver,
	 * which don't clear out the buffer after a controller reset.
	 */
	copy_doorbells(ctrlr, sdbl != NULL ?
		       sdbl->shadow_doorbells : ctrlr->bar0_doorbells,
		       ctrlr->sdbl->shadow_doorbells);

	vfio_user_ctrlr_switch_doorbells(ctrlr, true);

	ctrlr_kick(ctrlr);

	sc = SPDK_NVME_SC_SUCCESS;

out:
	/*
	 * Unmap existing buffers, in case Doorbell Buffer Config was sent
	 * more than once (pointless, but not prohibited by the spec), or
	 * in case of an error.
	 *
	 * If this is the first time Doorbell Buffer Config was processed,
	 * then we've just swapped a NULL from ctrlr->sdbl into sdbl, so
	 * free_sdbl() becomes a noop.
	 */
	free_sdbl(ctrlr->endpoint->vfu_ctx, sdbl);

	return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid, sc, sct);
}

/* Returns 0 on success and -errno on error. */
static int
consume_admin_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd)
{
	assert(ctrlr != NULL);
	assert(cmd != NULL);

	if (cmd->fuse != 0) {
		/* Fused admin commands are not supported. */
		return post_completion(ctrlr, ctrlr->cqs[0], 0, 0, cmd->cid,
				       SPDK_NVME_SC_INVALID_FIELD,
				       SPDK_NVME_SCT_GENERIC);
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		return handle_create_io_q(ctrlr, cmd,
					  cmd->opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		return handle_del_io_q(ctrlr, cmd,
				       cmd->opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		SPDK_NOTICELOG("%s: requested shadow doorbells (supported: %d)\n",
			       ctrlr_id(ctrlr),
			       !ctrlr->transport->transport_opts.disable_shadow_doorbells);
		if (!ctrlr->transport->transport_opts.disable_shadow_doorbells) {
			return handle_doorbell_buffer_config(ctrlr, cmd);
		}
	/* FALLTHROUGH */
	default:
		return handle_cmd_req(ctrlr, cmd, ctrlr->sqs[0]);
	}
}

static int
handle_cmd_rsp(struct nvmf_vfio_user_req *vu_req, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = sq->ctrlr;
	uint16_t sqid, cqid;
	int rc;

	assert(sq != NULL);
	assert(vu_req != NULL);
	assert(vu_ctrlr != NULL);

	if (spdk_unlikely(sq->dt.on)) {
		vu_req->ts_done = spdk_get_ticks();
	}

	if (spdk_likely(vu_req->iovcnt)) {
		vfu_sgl_put(vu_ctrlr->endpoint->vfu_ctx,
			    index_to_sg_t(vu_req->sg, 0),
			    vu_req->iov, vu_req->iovcnt);
	}
	sqid = sq->qid;
	cqid = sq->cqid;

	rc = post_completion(vu_ctrlr, vu_ctrlr->cqs[cqid],
			     vu_req->req.rsp->nvme_cpl.cdw0,
			     sqid,
			     vu_req->req.cmd->nvme_cmd.cid,
			     vu_req->req.rsp->nvme_cpl.status.sc,
			     vu_req->req.rsp->nvme_cpl.status.sct);

	if (spdk_unlikely(sq->dt.on) && sqid != 0) {
		struct devtrace_rec rec = {
			.qid = sqid,
			.cid = vu_req->req.cmd->nvme_cmd.cid,
			.mode = 0,
			.ts_db_seen = vu_req->ts_db_seen,
			.ts_fetched = vu_req->ts_fetched,
			.ts_exec = vu_req->ts_exec,
			.ts_done = vu_req->ts_done,
			.ts_posted = spdk_get_ticks(),
		};
		devtrace_add(&sq->dt, &rec);
	}

	return rc;
}

static int
consume_cmd(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq,
	    struct spdk_nvme_cmd *cmd)
{
	assert(sq != NULL);
	if (spdk_unlikely(nvmf_qpair_is_admin_queue(&sq->qpair))) {
		return consume_admin_cmd(ctrlr, cmd);
	}

	/* Mode 0 entry of every I/O command (fast path or not): count its opcode. */
	sq_count_opc(sq, cmd->opc);

	if (sq->fp != NULL &&
	    (cmd->opc == SPDK_NVME_OPC_READ || cmd->opc == SPDK_NVME_OPC_WRITE)) {
		/* Fast path: no nvmf request, no bdev, no payload movement (D1(a)). */
		struct fp_entry e;
		uint64_t now = spdk_get_ticks();
		uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;

		memset(&e, 0, sizeof(e));
		e.cid = cmd->cid;
		e.sq_head_after = *sq_headp(sq);
		if (spdk_unlikely(sq->dt.on)) {
			e.ts_db_seen = sq->ts_last_db_seen;
			e.ts_fetched = now;
		}
		if (spdk_unlikely(sq->fp_sched(sq->fp, now, slba, &e) == UINT64_MAX)) {
			SPDK_ERRLOG("%s: fastpath queue full on sqid:%u\n", ctrlr_id(ctrlr), sq->qid);
			return post_completion(ctrlr, ctrlr->cqs[sq->cqid], 0, sq->qid, cmd->cid,
					       SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, SPDK_NVME_SCT_GENERIC);
		}
		return 0;
	}

	return handle_cmd_req(ctrlr, cmd, sq);
}

/* Stage one Mode-D completion (no NVMe CQE, no interrupt). */
static void
iau_stage_cpl(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq, uint16_t cid,
	      uint64_t cb_fn, uint64_t cb_arg, uint8_t sct, uint8_t sc, uint64_t now)
{
	/* The cpl as its two words (cdw0 = cdw1 = 0, phase 0): the same bytes as the
	 * former zeroed struct + fields, without the narrow-store/wide-load stall. */
	iau_complete_words(&ctrlr->iau, sq->qid, cb_fn, cb_arg, 0,
			   iau_cpl_hi_word((uint16_t)ctrlr->iau.sq[sq->qid].dev_tail, sq->qid, cid, sct, sc),
			   now);
}

/* Posts every fast-path completion that is due. Returns the number posted. */
static int
fp_drain_due(struct nvmf_vfio_user_ctrlr *ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct fp_entry e;
	uint64_t now;
	int n = 0;

	if (sq->fp->n == 0) {
		return 0;
	}
	now = spdk_get_ticks();
	if (sq->iau_attached) {
		/* Mode D: stage 32 B callback records instead of NVMe CQEs. */
		while (fp_pop_due(sq->fp, now, &e)) {
			iau_stage_cpl(ctrlr, sq, e.cid, e.aux0, e.aux1,
				      SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, now);
			n++;
			if (spdk_unlikely(sq->dt.on)) {
				struct devtrace_rec rec = {
					.qid = sq->qid,
					.cid = e.cid,
					.mode = 4,
					.ts_db_seen = e.ts_db_seen,
					.ts_fetched = e.ts_fetched,
					.ts_exec = e.ts_fetched,
					.ts_done = e.due_tsc,
					.ts_posted = now,
				};
				devtrace_add(&sq->dt, &rec);
			}
		}
		return n;
	}
	/* P4.4 (D19): fp_pop is fp_pop_due, or with the error knob fp_pop_due_fault(), which posts an injected
	 * completion itself (fp_fault_post()) and returns only the entries this loop completes successfully. */
	while (sq->fp_pop(sq->fp, now, &e)) {
		/* post_completion() fills sqhd from the live SQ head, which is at or past
		 * e.sq_head_after; the host only uses it to free SQ slots, so this is safe. */
		post_completion(ctrlr, ctrlr->cqs[sq->cqid], 0, sq->qid, e.cid,
				SPDK_NVME_SC_SUCCESS, SPDK_NVME_SCT_GENERIC);
		n++;
		if (spdk_unlikely(sq->dt.on)) {
			struct devtrace_rec rec = {
				.qid = sq->qid,
				.cid = e.cid,
				.mode = 0,
				.ts_db_seen = e.ts_db_seen,
				.ts_fetched = e.ts_fetched,
				.ts_exec = e.ts_fetched,
				.ts_done = e.due_tsc,
				.ts_posted = now,
			};
			devtrace_add(&sq->dt, &rec);
		}
	}
	return n;
}

/* Returns the number of commands processed, or a negative value on error. */
static int
handle_sq_tdbl_write(struct nvmf_vfio_user_ctrlr *ctrlr, const uint32_t new_tail,
		     struct nvmf_vfio_user_sq *sq)
{
	struct spdk_nvme_cmd *queue;
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	int count = 0;
	uint32_t free_cq_slots;

	assert(ctrlr != NULL);
	assert(sq != NULL);

	if (spdk_unlikely(sq->dt.on)) {
		sq->ts_last_db_seen = spdk_get_ticks();
	}

	if (ctrlr->sdbl != NULL && sq->qid != 0) {
		/*
		 * Submission queue index has moved past the event index, so it
		 * needs to be re-armed before we go to sleep.
		 */
		sq->need_rearm = true;
	}

	if (sq->iau_attached && sq->qid != 0 && iau_mode_d(&ctrlr->iau)) {
		/*
		 * Mode D: fetch 40 B descriptors from the host descriptor ring
		 * (index space == SQ size). The NVMe SQ ring is not read and no
		 * CQ slot is consumed.
		 */
		struct iau_sq *isq = &ctrlr->iau.sq[sq->qid];

		if (spdk_unlikely(new_tail >= sq->size)) {
			return -1;
		}
		spdk_rmb();
		while (isq->dev_tail != new_tail) {
			struct spdk_nvme_cmd dcmd;
			uint64_t cb_fn, cb_arg;
			uint64_t now = spdk_get_ticks();

			iau_fetch_one(&ctrlr->iau, sq->qid, &dcmd, &cb_fn, &cb_arg);
			count++;
			sq_count_opc(sq, dcmd.opc);	/* Mode D entry of every I/O command */
			if (dcmd.opc == SPDK_NVME_OPC_READ || dcmd.opc == SPDK_NVME_OPC_WRITE) {
				struct fp_entry e;
				uint64_t slba = ((uint64_t)dcmd.cdw11 << 32) | dcmd.cdw10;

				memset(&e, 0, sizeof(e));
				e.cid = dcmd.cid;
				e.sq_head_after = isq->dev_tail;
				e.aux0 = cb_fn;
				e.aux1 = cb_arg;
				if (spdk_unlikely(sq->dt.on)) {
					e.ts_db_seen = sq->ts_last_db_seen;
					e.ts_fetched = now;
				}
				if (spdk_unlikely(fp_schedule(sq->fp, now, slba, &e) == UINT64_MAX)) {
					SPDK_ERRLOG("%s: fastpath queue full on sqid:%u\n", ctrlr_id(ctrlr), sq->qid);
					iau_stage_cpl(ctrlr, sq, dcmd.cid, cb_fn, cb_arg, SPDK_NVME_SCT_GENERIC,
						      SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, now);
				}
			} else {
				iau_stage_cpl(ctrlr, sq, dcmd.cid, cb_fn, cb_arg, SPDK_NVME_SCT_GENERIC,
					      SPDK_NVME_SC_INVALID_OPCODE, now);
			}
		}
		/* Keep the poller's emptiness check (head == doorbell) correct. */
		*sq_headp(sq) = isq->dev_tail;
		return count;
	}

	free_cq_slots = cq_free_slots(cq);
	queue = q_addr(&sq->mapping);
	while (*sq_headp(sq) != new_tail) {
		int err;
		struct spdk_nvme_cmd *cmd;

		/*
		 * At least the Linux nvme driver can submit more requests than
		 * our current view of the available free CQ slots, although it
		 * is not clear exactly why or how; it is relatively rare even
		 * under high load.
		 *
		 * As we need to make sure we have free CQ slots (see
		 * post_completion()), we implement flow control here: if the
		 * number of currently outstanding requests for this SQ would
		 * use all the available CQ slots, then we cannot submit this
		 * new request.
		 *
		 * Instead we back off until the driver has informed us that CQ
		 * slots are available.
		 */
		if ((free_cq_slots-- <= cq->nr_outstanding)) {
			struct nvmf_vfio_user_poll_group *vu_group;
			cq->last_head = *cq_dbl_headp(cq);

			free_cq_slots = cq_free_slots(cq);
			if (free_cq_slots > cq->nr_outstanding) {
				continue;
			}

			vu_group = sq_to_poll_group(sq);

			vu_group->stats.cq_full++;

			/*
			 * There are no free CQ slots, so stop processing
			 * submissions for this SQ until "a later time". In
			 * interrupt mode, we need to kick ourselves, so that we
			 * are guaranteed to wake up and come back here.
			 */
			if (in_interrupt_mode(ctrlr->transport)) {
				vu_group->need_kick = true;
			}
			break;
		}

		cmd = &queue[*sq_headp(sq)];
		count++;

		cq->nr_outstanding++;

		/*
		 * SQHD must contain the new head pointer, so we must increase
		 * it before we generate a completion.
		 */
		sq_head_advance(sq);

		err = consume_cmd(ctrlr, sq, cmd);
		if (spdk_unlikely(err != 0)) {
			return err;
		}
	}

	return count;
}

/* Checks whether endpoint is connected from the same process */
static bool
is_peer_same_process(struct nvmf_vfio_user_endpoint *endpoint)
{
	struct ucred ucred;
	socklen_t ucredlen = sizeof(ucred);

	if (endpoint == NULL) {
		return false;
	}

	if (getsockopt(vfu_get_poll_fd(endpoint->vfu_ctx), SOL_SOCKET, SO_PEERCRED, &ucred,
		       &ucredlen) < 0) {
		SPDK_ERRLOG("getsockopt(SO_PEERCRED): %s\n", strerror(errno));
		return false;
	}

	return ucred.pid == getpid();
}

static void
memory_region_add_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;
	void *map_start, *map_end;
	int ret;

	/*
	 * We're not interested in any DMA regions that aren't mappable (we don't
	 * support clients that don't share their memory).
	 */
	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	assert(endpoint != NULL);
	if (endpoint->ctrlr == NULL) {
		return;
	}
	ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: map IOVA %p-%p\n", endpoint_id(endpoint),
		      map_start, map_end);

	/* VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE are enabled when registering to VFIO, here we also
	 * check the protection bits before registering. When vfio client and server are run in same process
	 * there is no need to register the same memory again.
	 */
	if (info->prot == (PROT_WRITE | PROT_READ) && !is_peer_same_process(endpoint)) {
		ret = spdk_mem_register(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region register %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}

	pthread_mutex_lock(&endpoint->lock);
	TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
		if (sq->sq_state != VFIO_USER_SQ_INACTIVE) {
			continue;
		}

		cq = ctrlr->cqs[sq->cqid];

		/* For shared CQ case, we will use q_addr() to avoid mapping CQ multiple times */
		if (cq->size && q_addr(&cq->mapping) == NULL) {
			ret = map_q(ctrlr, &cq->mapping, MAP_RW | MAP_QUIET);
			if (ret) {
				SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap cqid:%d %#lx-%#lx\n",
					      cq->qid, cq->mapping.prp1,
					      cq->mapping.prp1 + cq->mapping.len);
				continue;
			}
		}

		if (sq->size) {
			ret = map_q(ctrlr, &sq->mapping, MAP_R | MAP_QUIET);
			if (ret) {
				SPDK_DEBUGLOG(nvmf_vfio, "Memory isn't ready to remap sqid:%d %#lx-%#lx\n",
					      sq->qid, sq->mapping.prp1,
					      sq->mapping.prp1 + sq->mapping.len);
				continue;
			}
		}
		sq->sq_state = VFIO_USER_SQ_ACTIVE;
		SPDK_DEBUGLOG(nvmf_vfio, "Remap sqid:%u successfully\n", sq->qid);
	}
	pthread_mutex_unlock(&endpoint->lock);
}

static void
memory_region_remove_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;
	void *map_start, *map_end;
	int ret = 0;

	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(nvmf_vfio, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	assert(endpoint != NULL);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: unmap IOVA %p-%p\n", endpoint_id(endpoint),
		      map_start, map_end);

	if (endpoint->ctrlr != NULL) {
		struct nvmf_vfio_user_ctrlr *ctrlr;
		ctrlr = endpoint->ctrlr;

		pthread_mutex_lock(&endpoint->lock);
		/* P3.9, mode 5: the engines let go of every queue whose memory is going away, first. */
		ctrlr_eng_detach_region(ctrlr, map_start, map_end);
		TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
			if (q_addr(&sq->mapping) >= map_start && q_addr(&sq->mapping) <= map_end) {
				unmap_q(ctrlr, &sq->mapping);
				sq->sq_state = VFIO_USER_SQ_INACTIVE;
			}

			if (sq->iau_attached &&
			    ((sq->iau_iov_desc.iov_base >= map_start && sq->iau_iov_desc.iov_base <= map_end) ||
			     (sq->iau_iov_rec.iov_base >= map_start && sq->iau_iov_rec.iov_base <= map_end))) {
				/* Rings are going away; drop them without writing. */
				sq_iau_detach(ctrlr, sq, false);
			}

			cq = ctrlr->cqs[sq->cqid];
			if (q_addr(&cq->mapping) >= map_start && q_addr(&cq->mapping) <= map_end) {
				unmap_q(ctrlr, &cq->mapping);
			}
		}

		if (ctrlr->sdbl != NULL) {
			size_t i;

			for (i = 0; i < NVMF_VFIO_USER_SHADOW_DOORBELLS_BUFFER_COUNT; i++) {
				const void *const iov_base = ctrlr->sdbl->iovs[i].iov_base;

				if (iov_base >= map_start && iov_base < map_end) {
					copy_doorbells(ctrlr,
						       ctrlr->sdbl->shadow_doorbells,
						       ctrlr->bar0_doorbells);
					vfio_user_ctrlr_switch_doorbells(ctrlr, false);
					free_sdbl(endpoint->vfu_ctx, ctrlr->sdbl);
					ctrlr->sdbl = NULL;
					break;
				}
			}
		}

		pthread_mutex_unlock(&endpoint->lock);
	}

	if (info->prot == (PROT_WRITE | PROT_READ) && !is_peer_same_process(endpoint)) {
		ret = spdk_mem_unregister(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region unregister %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}
}

/* Used to initiate a controller-level reset or a controller shutdown. */
static void
disable_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	SPDK_NOTICELOG("%s: disabling controller\n", ctrlr_id(vu_ctrlr));

	/* P3.9, mode 5: the IAU engines let go of this controller's queues before the page that holds their
	 * DESC_TAIL and CQ-sink words is zeroed (a zeroed DESC_TAIL would read as new descriptors). */
	ctrlr_eng_detach_all(vu_ctrlr);

	/* The host re-writes its IAU ring bases on the next enable. */
	if (vu_ctrlr->endpoint->iau_page != NULL) {
		memset((void *)vu_ctrlr->endpoint->iau_page, 0, IAU_BAR0_UNCORE_PAGE_SIZE);
	}

	/* Unmap Admin queue. */

	assert(vu_ctrlr->sqs[0] != NULL);
	assert(vu_ctrlr->cqs[0] != NULL);

	unmap_q(vu_ctrlr, &vu_ctrlr->sqs[0]->mapping);
	unmap_q(vu_ctrlr, &vu_ctrlr->cqs[0]->mapping);

	vu_ctrlr->sqs[0]->size = 0;
	*sq_headp(vu_ctrlr->sqs[0]) = 0;

	vu_ctrlr->sqs[0]->sq_state = VFIO_USER_SQ_INACTIVE;

	vu_ctrlr->cqs[0]->size = 0;
	*cq_tailp(vu_ctrlr->cqs[0]) = 0;

	/*
	 * For PCIe controller reset or shutdown, we will drop all AER
	 * responses.
	 */
	spdk_nvmf_ctrlr_abort_aer(vu_ctrlr->ctrlr);

	/* Free the shadow doorbell buffer. */
	vfio_user_ctrlr_switch_doorbells(vu_ctrlr, false);
	free_sdbl(vu_ctrlr->endpoint->vfu_ctx, vu_ctrlr->sdbl);
	vu_ctrlr->sdbl = NULL;
}

/* Used to re-enable the controller after a controller-level reset. */
static int
enable_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	int err;

	assert(vu_ctrlr != NULL);

	SPDK_NOTICELOG("%s: enabling controller\n", ctrlr_id(vu_ctrlr));

	err = acq_setup(vu_ctrlr);
	if (err != 0) {
		return err;
	}

	err = asq_setup(vu_ctrlr);
	if (err != 0) {
		return err;
	}

	vu_ctrlr->sqs[0]->sq_state = VFIO_USER_SQ_ACTIVE;

	return 0;
}

static int
nvmf_vfio_user_prop_req_rsp_set(struct nvmf_vfio_user_req *req,
				struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	union spdk_nvme_cc_register cc, diff;

	assert(req->req.cmd->prop_set_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET);
	assert(sq->ctrlr != NULL);
	vu_ctrlr = sq->ctrlr;

	if (req->req.cmd->prop_set_cmd.ofst != offsetof(struct spdk_nvme_registers, cc)) {
		return 0;
	}

	cc.raw = req->req.cmd->prop_set_cmd.value.u64;
	diff.raw = cc.raw ^ req->cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			int ret = enable_ctrlr(vu_ctrlr);
			if (ret) {
				SPDK_ERRLOG("%s: failed to enable ctrlr\n", ctrlr_id(vu_ctrlr));
				return ret;
			}
			vu_ctrlr->reset_shn = false;
		} else {
			vu_ctrlr->reset_shn = true;
		}
	}

	if (diff.bits.shn) {
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL || cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			vu_ctrlr->reset_shn = true;
		}
	}

	if (vu_ctrlr->reset_shn) {
		disable_ctrlr(vu_ctrlr);
	}
	return 0;
}

static int
nvmf_vfio_user_prop_req_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq = cb_arg;

	assert(sq != NULL);
	assert(req != NULL);

	if (req->req.cmd->prop_get_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET) {
		assert(sq->ctrlr != NULL);
		assert(req != NULL);

		memcpy(req->req.iov[0].iov_base,
		       &req->req.rsp->prop_get_rsp.value.u64,
		       req->req.length);
		return 0;
	}

	return nvmf_vfio_user_prop_req_rsp_set(req, sq);
}

/*
 * Handles a write at offset 0x1000 or more; this is the non-mapped path when a
 * doorbell is written via access_bar0_fn().
 *
 * DSTRD is set to fixed value 0 for NVMf.
 *
 */
static int
handle_dbl_access(struct nvmf_vfio_user_ctrlr *ctrlr, uint32_t *buf,
		  const size_t count, loff_t pos, const bool is_write)
{
	struct nvmf_vfio_user_poll_group *group;

	assert(ctrlr != NULL);
	assert(buf != NULL);

	if (spdk_unlikely(!is_write)) {
		SPDK_WARNLOG("%s: host tried to read BAR0 doorbell %#lx\n",
			     ctrlr_id(ctrlr), pos);
		errno = EPERM;
		return -1;
	}

	if (spdk_unlikely(count != sizeof(uint32_t))) {
		SPDK_ERRLOG("%s: bad doorbell buffer size %ld\n",
			    ctrlr_id(ctrlr), count);
		errno = EINVAL;
		return -1;
	}

	pos -= NVME_DOORBELLS_OFFSET;

	/* pos must be dword aligned */
	if (spdk_unlikely((pos & 0x3) != 0)) {
		SPDK_ERRLOG("%s: bad doorbell offset %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	/* convert byte offset to array index */
	pos >>= 2;

	if (spdk_unlikely(pos >= NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR * 2)) {
		SPDK_ERRLOG("%s: bad doorbell index %#lx\n", ctrlr_id(ctrlr), pos);
		errno = EINVAL;
		return -1;
	}

	ctrlr->bar0_doorbells[pos] = *buf;
	spdk_wmb();

	group = ctrlr_to_poll_group(ctrlr);
	if (pos == 1) {
		group->stats.cqh_admin_writes++;
	} else if (pos & 1) {
		group->stats.cqh_io_writes++;
	}

	SPDK_DEBUGLOG(vfio_user_db, "%s: updating BAR0 doorbell %s:%ld to %u\n",
		      ctrlr_id(ctrlr), (pos & 1) ? "cqid" : "sqid",
		      pos / 2, *buf);


	return 0;
}

static size_t
vfio_user_property_access(struct nvmf_vfio_user_ctrlr *vu_ctrlr,
			  char *buf, size_t count, loff_t pos,
			  bool is_write)
{
	struct nvmf_vfio_user_req *req;
	const struct spdk_nvmf_registers *regs;

	if ((count != 4) && (count != 8)) {
		errno = EINVAL;
		return -1;
	}

	/* Construct a Fabric Property Get/Set command and send it */
	req = get_nvmf_vfio_user_req(vu_ctrlr->sqs[0]);
	if (req == NULL) {
		errno = ENOBUFS;
		return -1;
	}
	regs = spdk_nvmf_ctrlr_get_regs(vu_ctrlr->ctrlr);
	req->cc.raw = regs->cc.raw;

	req->cb_fn = nvmf_vfio_user_prop_req_rsp;
	req->cb_arg = vu_ctrlr->sqs[0];
	req->req.cmd->prop_set_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->req.cmd->prop_set_cmd.cid = 0;
	if (count == 4) {
		req->req.cmd->prop_set_cmd.attrib.size = 0;
	} else {
		req->req.cmd->prop_set_cmd.attrib.size = 1;
	}
	req->req.cmd->prop_set_cmd.ofst = pos;
	if (is_write) {
		req->req.cmd->prop_set_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
		if (req->req.cmd->prop_set_cmd.attrib.size) {
			req->req.cmd->prop_set_cmd.value.u64 = *(uint64_t *)buf;
		} else {
			req->req.cmd->prop_set_cmd.value.u32.high = 0;
			req->req.cmd->prop_set_cmd.value.u32.low = *(uint32_t *)buf;
		}
	} else {
		req->req.cmd->prop_get_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	}
	req->req.length = count;
	SPDK_IOV_ONE(req->req.iov, &req->req.iovcnt, buf, req->req.length);

	spdk_nvmf_request_exec(&req->req);

	return count;
}

static ssize_t
access_bar0_fn(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t pos,
	       bool is_write)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr;
	int ret;

	ctrlr = endpoint->ctrlr;
	if (spdk_unlikely(endpoint->need_async_destroy || !ctrlr)) {
		errno = EIO;
		return -1;
	}

	if (pos >= IAU_BAR0_UNCORE_PAGE_OFFSET &&
	    pos < IAU_BAR0_UNCORE_PAGE_OFFSET + IAU_BAR0_UNCORE_PAGE_SIZE) {
		/* IAU uncore page over the socket (non-mmap path). */
		loff_t off = pos - IAU_BAR0_UNCORE_PAGE_OFFSET;

		if (endpoint->iau_page == NULL || off + (loff_t)count > IAU_BAR0_UNCORE_PAGE_SIZE) {
			errno = EINVAL;
			return -1;
		}
		if (is_write) {
			memcpy((void *)(endpoint->iau_page + off), buf, count);
		} else {
			memcpy(buf, (const void *)(endpoint->iau_page + off), count);
		}
		return count;
	}

	if (pos >= NVME_DOORBELLS_OFFSET) {
		/*
		 * The fact that the doorbells can be memory mapped doesn't mean
		 * that the client (VFIO in QEMU) is obliged to memory map them,
		 * it might still elect to access them via regular read/write;
		 * we might also have had disable_mappable_bar0 set.
		 */
		ret = handle_dbl_access(ctrlr, (uint32_t *)buf, count,
					pos, is_write);
		if (ret == 0) {
			return count;
		}
		return ret;
	}

	return vfio_user_property_access(ctrlr, buf, count, pos, is_write);
}

static ssize_t
access_pci_config(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t offset,
		  bool is_write)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);

	if (is_write) {
		SPDK_ERRLOG("%s: write %#lx-%#lx not supported\n",
			    endpoint_id(endpoint), offset, offset + count);
		errno = EINVAL;
		return -1;
	}

	if (offset + count > NVME_REG_CFG_SIZE) {
		SPDK_ERRLOG("%s: access past end of extended PCI configuration space, want=%ld+%ld, max=%d\n",
			    endpoint_id(endpoint), offset, count,
			    NVME_REG_CFG_SIZE);
		errno = ERANGE;
		return -1;
	}

	memcpy(buf, ((unsigned char *)endpoint->pci_config_space) + offset, count);

	return count;
}

static void
vfio_user_log(vfu_ctx_t *vfu_ctx, int level, char const *msg)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);

	if (level >= LOG_DEBUG) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_INFO) {
		SPDK_INFOLOG(nvmf_vfio, "%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_NOTICE) {
		SPDK_NOTICELOG("%s: %s\n", endpoint_id(endpoint), msg);
	} else if (level >= LOG_WARNING) {
		SPDK_WARNLOG("%s: %s\n", endpoint_id(endpoint), msg);
	} else {
		SPDK_ERRLOG("%s: %s\n", endpoint_id(endpoint), msg);
	}
}

static int
vfio_user_get_log_level(void)
{
	int level;

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf_vfio")) {
		return LOG_DEBUG;
	}

	level = spdk_log_to_syslog_level(spdk_log_get_level());
	if (level < 0) {
		return LOG_ERR;
	}

	return level;
}

static void
init_pci_config_space(vfu_pci_config_space_t *p)
{
	/* MLBAR */
	p->hdr.bars[0].raw = 0x0;
	/* MUBAR */
	p->hdr.bars[1].raw = 0x0;

	/* vendor specific, let's set them to zero for now */
	p->hdr.bars[3].raw = 0x0;
	p->hdr.bars[4].raw = 0x0;
	p->hdr.bars[5].raw = 0x0;

	/* enable INTx */
	p->hdr.intr.ipin = 0x1;
}

struct ctrlr_quiesce_ctx {
	struct nvmf_vfio_user_endpoint *endpoint;
	struct nvmf_vfio_user_poll_group *group;
	int status;
};

static void ctrlr_quiesce(struct nvmf_vfio_user_ctrlr *vu_ctrlr);

static void
_vfio_user_endpoint_resume_done_msg(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {
		return;
	}

	if (!vu_ctrlr->queued_quiesce) {
		vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;

		/*
		 * We might have ignored new SQ entries while we were quiesced:
		 * kick ourselves so we'll definitely check again while in
		 * VFIO_USER_CTRLR_RUNNING state.
		 */
		if (in_interrupt_mode(endpoint->transport)) {
			ctrlr_kick(vu_ctrlr);
		}
		return;
	}


	/*
	 * Basically, once we call `vfu_device_quiesced` the device is
	 * unquiesced from libvfio-user's perspective so from the moment
	 * `vfio_user_quiesce_done` returns libvfio-user might quiesce the device
	 * again. However, because the NVMf subsystem is an asynchronous
	 * operation, this quiesce might come _before_ the NVMf subsystem has
	 * been resumed, so in the callback of `spdk_nvmf_subsystem_resume` we
	 * need to check whether a quiesce was requested.
	 */
	SPDK_DEBUGLOG(nvmf_vfio, "%s has queued quiesce event, quiesce again\n",
		      ctrlr_id(vu_ctrlr));
	ctrlr_quiesce(vu_ctrlr);
}

static void
vfio_user_endpoint_resume_done(struct spdk_nvmf_subsystem *subsystem,
			       void *cb_arg, int status)
{
	struct nvmf_vfio_user_endpoint *endpoint = cb_arg;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s resumed done with status %d\n", endpoint_id(endpoint), status);

	if (!vu_ctrlr) {
		return;
	}

	spdk_thread_send_msg(vu_ctrlr->thread, _vfio_user_endpoint_resume_done_msg, endpoint);
}

static void
vfio_user_quiesce_done(void *ctx)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;
	int ret;

	if (!vu_ctrlr) {
		free(quiesce_ctx);
		return;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s device quiesced\n", ctrlr_id(vu_ctrlr));

	assert(vu_ctrlr->state == VFIO_USER_CTRLR_PAUSING);
	vu_ctrlr->state = VFIO_USER_CTRLR_PAUSED;
	vfu_device_quiesced(endpoint->vfu_ctx, quiesce_ctx->status);
	vu_ctrlr->queued_quiesce = false;
	free(quiesce_ctx);

	SPDK_DEBUGLOG(nvmf_vfio, "%s start to resume\n", ctrlr_id(vu_ctrlr));
	vu_ctrlr->state = VFIO_USER_CTRLR_RESUMING;
	ret = spdk_nvmf_subsystem_resume((struct spdk_nvmf_subsystem *)endpoint->subsystem,
					 vfio_user_endpoint_resume_done, endpoint);
	if (ret < 0) {
		vu_ctrlr->state = VFIO_USER_CTRLR_PAUSED;
		SPDK_ERRLOG("%s: failed to resume, ret=%d\n", endpoint_id(endpoint), ret);
	}
}

static void
vfio_user_pause_done(struct spdk_nvmf_subsystem *subsystem,
		     void *ctx, int status)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {
		free(quiesce_ctx);
		return;
	}

	quiesce_ctx->status = status;

	SPDK_DEBUGLOG(nvmf_vfio, "%s pause done with status %d\n",
		      ctrlr_id(vu_ctrlr), status);

	spdk_thread_send_msg(vu_ctrlr->thread,
			     vfio_user_quiesce_done, ctx);
}

/*
 * Ensure that, for this PG, we've stopped running in nvmf_vfio_user_sq_poll();
 * we've already set ctrlr->state, so we won't process new entries, but we need
 * to ensure that this PG is quiesced. This only works because there's no
 * callback context set up between polling the SQ and spdk_nvmf_request_exec().
 *
 * Once we've walked all PGs, we need to pause any submitted I/O via
 * spdk_nvmf_subsystem_pause(SPDK_NVME_GLOBAL_NS_TAG).
 */
static void
vfio_user_quiesce_pg(void *ctx)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = quiesce_ctx->endpoint;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;
	struct nvmf_vfio_user_poll_group *vu_group = quiesce_ctx->group;
	struct spdk_nvmf_subsystem *subsystem = endpoint->subsystem;
	int ret;

	SPDK_DEBUGLOG(nvmf_vfio, "quiesced pg:%p\n", vu_group);

	if (!vu_ctrlr) {
		free(quiesce_ctx);
		return;
	}

	quiesce_ctx->group = TAILQ_NEXT(vu_group, link);
	if (quiesce_ctx->group != NULL)  {
		spdk_thread_send_msg(poll_group_to_thread(quiesce_ctx->group),
				     vfio_user_quiesce_pg, quiesce_ctx);
		return;
	}

	ret = spdk_nvmf_subsystem_pause(subsystem, SPDK_NVME_GLOBAL_NS_TAG,
					vfio_user_pause_done, quiesce_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("%s: failed to pause, ret=%d\n",
			    endpoint_id(endpoint), ret);
		vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;
		fail_ctrlr(vu_ctrlr);
		free(quiesce_ctx);
	}
}

static void
ctrlr_quiesce(struct nvmf_vfio_user_ctrlr *vu_ctrlr)
{
	struct ctrlr_quiesce_ctx *quiesce_ctx;

	vu_ctrlr->state = VFIO_USER_CTRLR_PAUSING;

	quiesce_ctx = calloc(1, sizeof(*quiesce_ctx));
	if (!quiesce_ctx) {
		SPDK_ERRLOG("Failed to allocate subsystem pause context\n");
		assert(false);
		return;
	}

	quiesce_ctx->endpoint = vu_ctrlr->endpoint;
	quiesce_ctx->status = 0;
	quiesce_ctx->group = TAILQ_FIRST(&vu_ctrlr->transport->poll_groups);

	spdk_thread_send_msg(poll_group_to_thread(quiesce_ctx->group),
			     vfio_user_quiesce_pg, quiesce_ctx);
}

static int
vfio_user_dev_quiesce_cb(vfu_ctx_t *vfu_ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct spdk_nvmf_subsystem *subsystem = endpoint->subsystem;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = endpoint->ctrlr;

	if (!vu_ctrlr) {
		return 0;
	}

	/* NVMf library will destruct controller when no
	 * connected queue pairs.
	 */
	if (!nvmf_subsystem_get_ctrlr(subsystem, vu_ctrlr->cntlid)) {
		return 0;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s starts to quiesce\n", ctrlr_id(vu_ctrlr));

	/* There is no race condition here as device quiesce callback
	 * and nvmf_prop_set_cc() are running in the same thread context.
	 */
	if (!vu_ctrlr->ctrlr->vcprop.cc.bits.en) {
		return 0;
	} else if (!vu_ctrlr->ctrlr->vcprop.csts.bits.rdy) {
		return 0;
	} else if (vu_ctrlr->ctrlr->vcprop.csts.bits.shst == SPDK_NVME_SHST_COMPLETE) {
		return 0;
	}

	switch (vu_ctrlr->state) {
	case VFIO_USER_CTRLR_PAUSED:
		return 0;
	case VFIO_USER_CTRLR_RUNNING:
		ctrlr_quiesce(vu_ctrlr);
		break;
	case VFIO_USER_CTRLR_RESUMING:
		vu_ctrlr->queued_quiesce = true;
		SPDK_DEBUGLOG(nvmf_vfio, "%s is busy to quiesce, current state %u\n", ctrlr_id(vu_ctrlr),
			      vu_ctrlr->state);
		break;
	default:
		assert(vu_ctrlr->state != VFIO_USER_CTRLR_PAUSING);
		break;
	}

	errno = EBUSY;
	return -1;
}

/*
 * If we are about to close the connection, we need to unregister the interrupt,
 * as the library will subsequently close the file descriptor we registered.
 */
static int
vfio_user_device_reset(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
	struct nvmf_vfio_user_endpoint *endpoint = vfu_get_private(vfu_ctx);
	struct nvmf_vfio_user_ctrlr *ctrlr = endpoint->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "Device reset type %u\n", type);

	if (type == VFU_RESET_LOST_CONN) {
		if (ctrlr != NULL) {
			spdk_interrupt_unregister(&ctrlr->intr);
			ctrlr->intr_fd = -1;
		}
		return 0;
	}

	/* FIXME: LOST_CONN case ? */
	if (ctrlr->sdbl != NULL) {
		vfio_user_ctrlr_switch_doorbells(ctrlr, false);
		free_sdbl(vfu_ctx, ctrlr->sdbl);
		ctrlr->sdbl = NULL;
	}

	/* FIXME: much more needed here. */

	return 0;
}

static int
vfio_user_dev_info_fill(struct nvmf_vfio_user_transport *vu_transport,
			struct nvmf_vfio_user_endpoint *endpoint)
{
	int ret;
	ssize_t cap_offset;
	vfu_ctx_t *vfu_ctx = endpoint->vfu_ctx;

	struct pmcap pmcap = { .hdr.id = PCI_CAP_ID_PM, .pmcs.nsfrst = 0x1 };
	struct pxcap pxcap = {
		.hdr.id = PCI_CAP_ID_EXP,
		.pxcaps.ver = 0x2,
		.pxdcap = {.rer = 0x1, .flrc = 0x1},
		.pxdcap2.ctds = 0x1
	};

	struct msixcap msixcap = {
		.hdr.id = PCI_CAP_ID_MSIX,
		.mxc.ts = NVMF_VFIO_USER_MSIX_NUM - 1,
		.mtab = {.tbir = NVMF_VFIO_USER_MSIX_TABLE_BIR, .to = 0x0},
		.mpba = {.pbir = NVMF_VFIO_USER_MSIX_PBA_BIR, .pbao = 0x0}
	};

	struct iovec sparse_mmap[] = {
		{
			.iov_base = (void *)NVME_DOORBELLS_OFFSET,
			/*
			 * One area spanning doorbells .. IAU page: libvfio-user sends a
			 * single fd per region and the SPDK host indexes fds[i] per area,
			 * so a second area would get an invalid fd.
			 */
			.iov_len = IAU_BAR0_UNCORE_PAGE_OFFSET + IAU_BAR0_UNCORE_PAGE_SIZE - NVME_DOORBELLS_OFFSET,
		},
	};

	ret = vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to initialize PCI\n", vfu_ctx);
		return ret;
	}
	vfu_pci_set_id(vfu_ctx, SPDK_PCI_VID_NUTANIX, 0x0001, SPDK_PCI_VID_NUTANIX, 0);
	/*
	 * 0x02, controller uses the NVM Express programming interface
	 * 0x08, non-volatile memory controller
	 * 0x01, mass storage controller
	 */
	vfu_pci_set_class(vfu_ctx, 0x01, 0x08, 0x02);

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &pmcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pmcap\n", vfu_ctx);
		return ret;
	}

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &pxcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pxcap\n", vfu_ctx);
		return ret;
	}

	cap_offset = vfu_pci_add_capability(vfu_ctx, 0, 0, &msixcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add msixcap\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_CFG_REGION_IDX, NVME_REG_CFG_SIZE,
			       access_pci_config, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup cfg\n", vfu_ctx);
		return ret;
	}

	if (vu_transport->transport_opts.disable_mappable_bar0) {
		ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, NVME_REG_BAR0_SIZE,
				       access_bar0_fn, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
				       NULL, 0, -1, 0);
	} else {
		ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, NVME_REG_BAR0_SIZE,
				       access_bar0_fn, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
				       sparse_mmap, SPDK_COUNTOF(sparse_mmap), endpoint->devmem_fd, 0);
	}

	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 0\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR4_REGION_IDX, NVMF_VFIO_USER_BAR4_SIZE,
			       NULL, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 4\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR5_REGION_IDX, NVMF_VFIO_USER_BAR5_SIZE,
			       NULL, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup bar 5\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_dma(vfu_ctx, memory_region_add_cb, memory_region_remove_cb);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup dma callback\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_reset_cb(vfu_ctx, vfio_user_device_reset);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup reset callback\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_INTX_IRQ, 1);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup INTX\n", vfu_ctx);
		return ret;
	}

	ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSIX_IRQ, NVMF_VFIO_USER_MSIX_NUM);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup MSIX\n", vfu_ctx);
		return ret;
	}

	vfu_setup_device_quiesce_cb(vfu_ctx, vfio_user_dev_quiesce_cb);

	ret = vfu_realize_ctx(vfu_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to realize\n", vfu_ctx);
		return ret;
	}

	endpoint->pci_config_space = vfu_pci_get_config_space(endpoint->vfu_ctx);
	assert(endpoint->pci_config_space != NULL);
	init_pci_config_space(endpoint->pci_config_space);

	assert(cap_offset != 0);
	endpoint->msix = (struct msixcap *)((uint8_t *)endpoint->pci_config_space + cap_offset);

	return 0;
}

static int nvmf_vfio_user_accept(void *ctx);

/*
 * Register an "accept" poller: this is polling for incoming vfio-user socket
 * connections (on the listening socket).
 *
 * We need to do this on first listening, and also after destroying a
 * controller, so we can accept another connection.
 */
static int
vfio_user_register_accept_poller(struct nvmf_vfio_user_endpoint *endpoint)
{
	uint64_t poll_rate_us = endpoint->transport->transport.opts.acceptor_poll_rate;

	SPDK_DEBUGLOG(nvmf_vfio, "registering accept poller\n");

	endpoint->accept_poller = SPDK_POLLER_REGISTER(nvmf_vfio_user_accept,
				  endpoint, poll_rate_us);

	if (!endpoint->accept_poller) {
		return -1;
	}

	endpoint->accept_thread = spdk_get_thread();
	endpoint->need_relisten = false;

	if (!spdk_interrupt_mode_is_enabled()) {
		return 0;
	}

	endpoint->accept_intr_fd = vfu_get_poll_fd(endpoint->vfu_ctx);
	assert(endpoint->accept_intr_fd != -1);

	endpoint->accept_intr = SPDK_INTERRUPT_REGISTER(endpoint->accept_intr_fd,
				nvmf_vfio_user_accept, endpoint);

	assert(endpoint->accept_intr != NULL);

	spdk_poller_register_interrupt(endpoint->accept_poller, NULL, NULL);
	return 0;
}

static void
_vfio_user_relisten(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;

	vfio_user_register_accept_poller(endpoint);
}

static void
_free_ctrlr(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	struct nvmf_vfio_user_endpoint *endpoint = ctrlr->endpoint;

	/* free_ctrlr() finished every SQ (free_qp()), so no fp_sq is bound to the embedded fp_chan. */
	if (ctrlr->fp_chan.users != 0) {
		SPDK_ERRLOG("%s: fp_chan freed with %u SQs bound\n", ctrlr_id(ctrlr), ctrlr->fp_chan.users);
	}
	free_sdbl(endpoint->vfu_ctx, ctrlr->sdbl);
	iau_ctx_fini(&ctrlr->iau);

	spdk_interrupt_unregister(&ctrlr->intr);
	ctrlr->intr_fd = -1;
	spdk_poller_unregister(&ctrlr->vfu_ctx_poller);

	free(ctrlr);

	if (endpoint->need_async_destroy) {
		nvmf_vfio_user_destroy_endpoint(endpoint);
	} else if (endpoint->need_relisten) {
		spdk_thread_send_msg(endpoint->accept_thread,
				     _vfio_user_relisten, endpoint);
	}
}

static void
free_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct spdk_thread *thread;
	int i;

	assert(ctrlr != NULL);
	thread = ctrlr->thread ? ctrlr->thread : spdk_get_thread();

	SPDK_DEBUGLOG(nvmf_vfio, "free %s\n", ctrlr_id(ctrlr));

	for (i = 0; i < NVMF_VFIO_USER_MAX_QPAIRS_PER_CTRLR; i++) {
		free_qp(ctrlr, i);
	}

	spdk_thread_exec_msg(thread, _free_ctrlr, ctrlr);
}

static int
nvmf_vfio_user_create_ctrlr(struct nvmf_vfio_user_transport *transport,
			    struct nvmf_vfio_user_endpoint *endpoint)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	int err = 0;

	SPDK_DEBUGLOG(nvmf_vfio, "%s\n", endpoint_id(endpoint));

	/* First, construct a vfio-user CUSTOM transport controller */
	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		err = -ENOMEM;
		goto out;
	}
	/*
	 * We can only support one connection for now, but generate a unique cntlid in case vfio-user
	 * transport is used together with RDMA or TCP transports in the same target
	 */
	ctrlr->cntlid = nvmf_subsystem_gen_cntlid(endpoint->subsystem);
	ctrlr->intr_fd = -1;
	ctrlr->transport = transport;
	ctrlr->endpoint = endpoint;
	/* P3: the endpoint's number (start_target.sh: $RC_RUNTIME/vfu<k>) selects IAU_DEV_*_C<k>. */
	ctrlr->dev_idx = devcfg_ctrlr_index(endpoint->trid.traddr);
	ctrlr->bar0_doorbells = endpoint->bar0_doorbells;
	TAILQ_INIT(&ctrlr->connected_sqs);

	/* Fresh controller: clear the host-written IAU ring-base registers. */
	memset((void *)endpoint->iau_page, 0, IAU_BAR0_UNCORE_PAGE_SIZE);
	if (iau_ctx_init(&ctrlr->iau, endpoint->iau_page, spdk_get_ticks_hz()) != 0) {
		SPDK_ERRLOG("IAU: bad IAU_* environment\n");
		free(ctrlr);
		err = -EINVAL;
		goto out;
	}
	/* P3: the device model is per controller and loaded here, in every mode, through the validator
	 * the listener already ran (vu_dev_cfg_check(): devcfg, svc_ticks, Mode D needs the fast path,
	 * mode 5 needs the engine); any error fails the controller instead of silently turning the fast
	 * path off. */
	if (fp_load_cfg(ctrlr) != 0) {
		iau_ctx_fini(&ctrlr->iau);
		free(ctrlr);
		err = -EINVAL;
		goto out;
	}
	SPDK_NOTICELOG("IAU mode=%d batch_n=%u batch_t_ticks=%" PRIu64 "\n",
		       ctrlr->iau.mode, ctrlr->iau.batch_n, ctrlr->iau.batch_t_ticks);

	ctrlr->adaptive_irqs_enabled =
		!transport->transport_opts.disable_adaptive_irq;

	/* Then, construct an admin queue pair */
	err = init_sq(ctrlr, &transport->transport, 0);
	if (err != 0) {
		iau_ctx_fini(&ctrlr->iau);
		free(ctrlr);
		goto out;
	}

	err = init_cq(ctrlr, 0);
	if (err != 0) {
		iau_ctx_fini(&ctrlr->iau);
		free(ctrlr);
		goto out;
	}

	ctrlr->sqs[0]->size = NVMF_VFIO_USER_DEFAULT_AQ_DEPTH;

	err = alloc_sq_reqs(ctrlr, ctrlr->sqs[0]);
	if (err != 0) {
		iau_ctx_fini(&ctrlr->iau);
		free(ctrlr);
		goto out;
	}
	endpoint->ctrlr = ctrlr;

	/* Notify the generic layer about the new admin queue pair */
	spdk_nvmf_tgt_new_qpair(transport->transport.tgt, &ctrlr->sqs[0]->qpair);

out:
	if (err != 0) {
		SPDK_ERRLOG("%s: failed to create vfio-user controller: %s\n",
			    endpoint_id(endpoint), strerror(-err));
	}

	return err;
}

static int
nvmf_vfio_user_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvmf_listen_opts *listen_opts)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;
	char path[PATH_MAX] = {};
	char uuid[PATH_MAX] = {};
	int ret;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		/* Only compare traddr */
		if (strncmp(endpoint->trid.traddr, trid->traddr, sizeof(endpoint->trid.traddr)) == 0) {
			pthread_mutex_unlock(&vu_transport->lock);
			return -EEXIST;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);

	/* P3: validate this endpoint's configuration now (IAU_* through iau_ctx_init(), then
	 * vu_dev_cfg_check()), so every config error fails the listener RPC (start_target.sh) instead
	 * of hanging the host's first connect. Controller creation runs the same checks again. */
	{
		struct iau_ctx ic;
		struct devcfg c;
		uint64_t svc_ticks;

		if (iau_ctx_init(&ic, NULL, spdk_get_ticks_hz()) != 0) {
			iau_ctx_fini(&ic);
			SPDK_ERRLOG("%s: IAU: bad IAU_* environment\n", trid->traddr);
			return -EINVAL;
		}
		ret = vu_dev_cfg_check(vu_transport, trid->traddr, devcfg_ctrlr_index(trid->traddr), &ic, &c,
				       &svc_ticks);
		iau_ctx_fini(&ic);
		if (ret != 0) {
			return ret;
		}
	}

	endpoint = calloc(1, sizeof(*endpoint));
	if (!endpoint) {
		return -ENOMEM;
	}

	pthread_mutex_init(&endpoint->lock, NULL);
	endpoint->devmem_fd = -1;
	memcpy(&endpoint->trid, trid, sizeof(endpoint->trid));
	endpoint->transport = vu_transport;

	ret = snprintf(path, PATH_MAX, "%s/bar0", endpoint_id(endpoint));
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get socket path: %s.\n", endpoint_id(endpoint), spdk_strerror(errno));
		ret = -1;
		goto out;
	}

	ret = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (ret == -1) {
		SPDK_ERRLOG("%s: failed to open device memory at %s: %s.\n",
			    endpoint_id(endpoint), path, spdk_strerror(errno));
		goto out;
	}
	unlink(path);

	endpoint->devmem_fd = ret;
	ret = ftruncate(endpoint->devmem_fd, NVME_REG_BAR0_SIZE);
	if (ret != 0) {
		SPDK_ERRLOG("%s: error to ftruncate file %s: %s.\n", endpoint_id(endpoint), path,
			    spdk_strerror(errno));
		goto out;
	}

	/*
	 * The doorbell offset is fixed at 0x1000.
	 * But mmap requires the offset must be a multiple of the page size as returned by
	 * sysconf(_SC_PAGE_SIZE).
	 * In order to avoid the mmap failure in non-4K page size kernel,
	 * set 0 to mmap's offset, and then change to doorbell offset manually.
	 */
	endpoint->bar0 = mmap(NULL, NVME_REG_BAR0_SIZE, PROT_READ | PROT_WRITE,
			      MAP_SHARED, endpoint->devmem_fd, 0);
	if (endpoint->bar0 == MAP_FAILED) {
		SPDK_ERRLOG("%s: error to mmap file %s: %s.\n", endpoint_id(endpoint), path, spdk_strerror(errno));
		endpoint->bar0 = NULL;
		ret = -1;
		goto out;
	}
	endpoint->bar0_doorbells = (uint32_t *)(((unsigned long)endpoint->bar0) + NVME_DOORBELLS_OFFSET);
	/*
	 * IAU uncore page: same MAP_SHARED mapping of devmem_fd, so host mmap of
	 * BAR0+0x5000 (fd offset 0x5000) and this view alias the same pages.
	 * ftruncate() zero-fills it.
	 */
	endpoint->iau_page = (volatile uint8_t *)endpoint->bar0 + IAU_BAR0_UNCORE_PAGE_OFFSET;

	ret = snprintf(uuid, PATH_MAX, "%s/cntrl", endpoint_id(endpoint));
	if (ret < 0 || ret >= PATH_MAX) {
		SPDK_ERRLOG("%s: error to get ctrlr file path: %s\n", endpoint_id(endpoint), spdk_strerror(errno));
		ret = -1;
		goto out;
	}

	endpoint->vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, uuid, LIBVFIO_USER_FLAG_ATTACH_NB,
					   endpoint, VFU_DEV_TYPE_PCI);
	if (endpoint->vfu_ctx == NULL) {
		SPDK_ERRLOG("%s: error creating libmuser context: %m\n",
			    endpoint_id(endpoint));
		ret = -1;
		goto out;
	}

	ret = vfu_setup_log(endpoint->vfu_ctx, vfio_user_log,
			    vfio_user_get_log_level());
	if (ret < 0) {
		goto out;
	}


	ret = vfio_user_dev_info_fill(vu_transport, endpoint);
	if (ret < 0) {
		goto out;
	}

	ret = vfio_user_register_accept_poller(endpoint);

	if (ret != 0) {
		goto out;
	}

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_INSERT_TAIL(&vu_transport->endpoints, endpoint, link);
	pthread_mutex_unlock(&vu_transport->lock);

out:
	if (ret != 0) {
		nvmf_vfio_user_destroy_endpoint(endpoint);
	}

	return ret;
}

static void
nvmf_vfio_user_stop_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint, *tmp;

	assert(trid != NULL);
	assert(trid->traddr != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: stop listen\n", trid->traddr);

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_FOREACH_SAFE(endpoint, &vu_transport->endpoints, link, tmp) {
		if (strcmp(trid->traddr, endpoint->trid.traddr) == 0) {
			TAILQ_REMOVE(&vu_transport->endpoints, endpoint, link);
			/* Defer to free endpoint resources until the controller
			 * is freed.  There are two cases when running here:
			 * 1. kill nvmf target while VM is connected
			 * 2. remove listener via RPC call
			 * nvmf library will disconnect all queue paris.
			 */
			if (endpoint->ctrlr) {
				assert(!endpoint->need_async_destroy);
				endpoint->need_async_destroy = true;
				pthread_mutex_unlock(&vu_transport->lock);
				return;
			}

			nvmf_vfio_user_destroy_endpoint(endpoint);
			pthread_mutex_unlock(&vu_transport->lock);
			return;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: not found\n", trid->traddr);
}

static void
nvmf_vfio_user_cdata_init(struct spdk_nvmf_transport *transport,
			  struct spdk_nvmf_subsystem *subsystem,
			  struct spdk_nvmf_ctrlr_data *cdata)
{
	struct nvmf_vfio_user_transport *vu_transport;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport, transport);

	cdata->vid = SPDK_PCI_VID_NUTANIX;
	cdata->ssvid = SPDK_PCI_VID_NUTANIX;
	cdata->ieee[0] = 0x8d;
	cdata->ieee[1] = 0x6b;
	cdata->ieee[2] = 0x50;
	memset(&cdata->sgls, 0, sizeof(struct spdk_nvme_cdata_sgls));
	cdata->sgls.supported = SPDK_NVME_SGLS_SUPPORTED_DWORD_ALIGNED;
	cdata->oncs.nvmcmps = !vu_transport->transport_opts.disable_compare &&
			      vu_transport->transport.opts.oncs.nvmcmps;
	/* libvfio-user can only support 1 connection for now */
	cdata->oncs.reservs = 0;
	cdata->oacs.dbcs = !vu_transport->transport_opts.disable_shadow_doorbells;
	cdata->fuses.fcws = !vu_transport->transport_opts.disable_compare &&
			    vu_transport->transport.opts.oncs.nvmcmps && vu_transport->transport.opts.fuses.fcws;
}

static int
nvmf_vfio_user_listen_associate(struct spdk_nvmf_transport *transport,
				const struct spdk_nvmf_subsystem *subsystem,
				const struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_endpoint *endpoint;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport, transport);

	pthread_mutex_lock(&vu_transport->lock);
	TAILQ_FOREACH(endpoint, &vu_transport->endpoints, link) {
		if (strncmp(endpoint->trid.traddr, trid->traddr, sizeof(endpoint->trid.traddr)) == 0) {
			break;
		}
	}
	pthread_mutex_unlock(&vu_transport->lock);

	if (endpoint == NULL) {
		return -ENOENT;
	}

	/* Drop const - we will later need to pause/unpause. */
	endpoint->subsystem = (struct spdk_nvmf_subsystem *)subsystem;

	return 0;
}

/*
 * Executed periodically at a default SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US
 * frequency.
 *
 * For this endpoint (which at the libvfio-user level corresponds to a socket),
 * if we don't currently have a controller set up, peek to see if the socket is
 * able to accept a new connection.
 */
static int
nvmf_vfio_user_accept(void *ctx)
{
	struct nvmf_vfio_user_endpoint *endpoint = ctx;
	struct nvmf_vfio_user_transport *vu_transport;
	int err;

	vu_transport = endpoint->transport;

	if (endpoint->ctrlr != NULL) {
		return SPDK_POLLER_IDLE;
	}

	err = vfu_attach_ctx(endpoint->vfu_ctx);
	if (err == 0) {
		SPDK_DEBUGLOG(nvmf_vfio, "attach succeeded\n");
		err = nvmf_vfio_user_create_ctrlr(vu_transport, endpoint);
		if (err == 0) {
			/*
			 * Unregister ourselves: now we've accepted a
			 * connection, there is nothing for us to poll for, and
			 * we will poll the connection via vfu_run_ctx()
			 * instead.
			 */
			spdk_interrupt_unregister(&endpoint->accept_intr);
			spdk_poller_unregister(&endpoint->accept_poller);
		}
		return SPDK_POLLER_BUSY;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

static void
nvmf_vfio_user_discover(struct spdk_nvmf_transport *transport,
			struct spdk_nvme_transport_id *trid,
			struct spdk_nvmf_discovery_log_page_entry *entry)
{ }

static int vfio_user_poll_group_intr(void *ctx);

static void
vfio_user_poll_group_add_intr(struct nvmf_vfio_user_poll_group *vu_group,
			      struct spdk_nvmf_poll_group *group)
{
	vu_group->intr_fd = eventfd(0, EFD_NONBLOCK);
	assert(vu_group->intr_fd != -1);

	vu_group->intr = SPDK_INTERRUPT_REGISTER(vu_group->intr_fd,
			 vfio_user_poll_group_intr, vu_group);
	assert(vu_group->intr != NULL);
}

static struct spdk_nvmf_transport_poll_group *
nvmf_vfio_user_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_poll_group *vu_group;

	vu_transport = SPDK_CONTAINEROF(transport, struct nvmf_vfio_user_transport,
					transport);

	SPDK_DEBUGLOG(nvmf_vfio, "create poll group\n");

	vu_group = calloc(1, sizeof(*vu_group));
	if (vu_group == NULL) {
		SPDK_ERRLOG("Error allocating poll group: %m");
		return NULL;
	}

	if (in_interrupt_mode(vu_transport)) {
		vfio_user_poll_group_add_intr(vu_group, group);
	}

	TAILQ_INIT(&vu_group->sqs);
	/* Runs on the poll group's own reactor thread (nvmf_tgt_create_poll_group message). */
	vu_group->lcore = spdk_env_get_current_core();

	pthread_mutex_lock(&vu_transport->pg_lock);
	TAILQ_INSERT_TAIL(&vu_transport->poll_groups, vu_group, link);
	if (vu_transport->next_pg == NULL) {
		vu_transport->next_pg = vu_group;
	}
	pthread_mutex_unlock(&vu_transport->pg_lock);

	return &vu_group->group;
}

/* The poll group whose lcore has rank `want` among all poll groups (ascending lcore), NULL if none has
 * (lcores not distinct). Caller holds pg_lock. */
static struct nvmf_vfio_user_poll_group *
pg_by_lcore_rank(struct nvmf_vfio_user_transport *vu_transport, uint32_t want)
{
	struct nvmf_vfio_user_poll_group *pg, *o;

	TAILQ_FOREACH(pg, &vu_transport->poll_groups, link) {
		uint32_t rank = 0;

		TAILQ_FOREACH(o, &vu_transport->poll_groups, link) {
			rank += o->lcore < pg->lcore;
		}
		if (rank == want) {
			return pg;
		}
	}
	return NULL;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_vfio_user_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_transport *vu_transport;
	struct nvmf_vfio_user_poll_group **vu_group;
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_cq *cq;

	struct spdk_nvmf_transport_poll_group *result = NULL;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	cq = sq->ctrlr->cqs[sq->cqid];
	assert(cq != NULL);
	vu_transport = SPDK_CONTAINEROF(qpair->transport, struct nvmf_vfio_user_transport, transport);

	pthread_mutex_lock(&vu_transport->pg_lock);
	if (TAILQ_EMPTY(&vu_transport->poll_groups)) {
		goto out;
	}

	if (!nvmf_qpair_is_admin_queue(qpair)) {
		/*
		 * If this is shared IO CQ case, just return the used CQ's poll
		 * group, so I/O completions don't have to use
		 * spdk_thread_send_msg().
		 */
		if (cq->group != NULL) {
			result = cq->group;
			goto out;
		}

		/*
		 * If we're in interrupt mode, align all qpairs for a controller
		 * on the same poll group by default, unless requested. This can
		 * be lower in performance than running on a single poll group,
		 * so we disable spreading by default.
		 */
		if (in_interrupt_mode(vu_transport) &&
		    !vu_transport->transport_opts.enable_intr_mode_sq_spreading) {
			result = sq->ctrlr->sqs[0]->group;
			goto out;
		}

		/* IAU-next P3, IAU_DEV_MODEL=ctrlr: all I/O SQs of a controller on one poll group (the owner of
		 * the controller's shared channel state): rank dev_idx mod n, sticky per controller. Without an
		 * endpoint number (dev_idx -1), or if no group has that rank, the round-robin group, also sticky.
		 * IAU_DEV_MODEL=sq (the default) skips this and keeps PG_BY_QID / round robin unchanged. */
		if (sq->ctrlr->fp_model_ctrlr) {
			struct nvmf_vfio_user_poll_group *pg, *pick = sq->ctrlr->io_pg;
			uint32_t n = 0;

			TAILQ_FOREACH(pg, &vu_transport->poll_groups, link) {
				n++;
			}
			if (pick == NULL) {
				if (sq->ctrlr->dev_idx >= 0) {
					pick = pg_by_lcore_rank(vu_transport, (uint32_t)sq->ctrlr->dev_idx % n);
				}
				if (pick == NULL) {
					pick = vu_transport->next_pg;
				}
				sq->ctrlr->io_pg = pick;
			}
			assert(pick != NULL);
			SPDK_NOTICELOG("PG_BY_CTRLR ctrlr=%d qid=%u lcore=%u n=%u\n", sq->ctrlr->dev_idx, sq->qid,
				       pick->lcore, n);
			result = &pick->group;
			goto out;
		}

		if (vu_transport->pg_by_qid) {
			struct nvmf_vfio_user_poll_group *pg, *pick = NULL;
			uint32_t n = 0, want, rank, last = 0;

			TAILQ_FOREACH(pg, &vu_transport->poll_groups, link) {
				n++;
			}
			want = (sq->qid - 1) % n;
			/* Select the group whose lcore has rank `want` in ascending order. */
			TAILQ_FOREACH(pg, &vu_transport->poll_groups, link) {
				rank = 0;
				last = pg->lcore;
				{
					struct nvmf_vfio_user_poll_group *o;

					TAILQ_FOREACH(o, &vu_transport->poll_groups, link) {
						if (o->lcore < last) {
							rank++;
						}
					}
				}
				if (rank == want) {
					pick = pg;
					break;
				}
			}
			if (pick != NULL) {
				SPDK_NOTICELOG("PG_BY_QID qid=%u lcore=%u n=%u\n", sq->qid, pick->lcore, n);
				result = &pick->group;
				goto out;
			}
		}
	}

	vu_group = &vu_transport->next_pg;
	assert(*vu_group != NULL);

	result = &(*vu_group)->group;
	*vu_group = TAILQ_NEXT(*vu_group, link);
	if (*vu_group == NULL) {
		*vu_group = TAILQ_FIRST(&vu_transport->poll_groups);
	}

out:
	if (cq->group == NULL) {
		cq->group = result;
	}

	pthread_mutex_unlock(&vu_transport->pg_lock);
	return result;
}

static void
vfio_user_poll_group_del_intr(struct nvmf_vfio_user_poll_group *vu_group)
{
	assert(vu_group->intr_fd != -1);

	spdk_interrupt_unregister(&vu_group->intr);

	close(vu_group->intr_fd);
	vu_group->intr_fd = -1;
}

/* called when process exits */
static void
nvmf_vfio_user_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group, *next_tgroup;
	struct nvmf_vfio_user_transport *vu_transport;

	SPDK_DEBUGLOG(nvmf_vfio, "destroy poll group\n");

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);
	vu_transport = SPDK_CONTAINEROF(vu_group->group.transport, struct nvmf_vfio_user_transport,
					transport);

	if (in_interrupt_mode(vu_transport)) {
		vfio_user_poll_group_del_intr(vu_group);
	}

	pthread_mutex_lock(&vu_transport->pg_lock);
	next_tgroup = TAILQ_NEXT(vu_group, link);
	TAILQ_REMOVE(&vu_transport->poll_groups, vu_group, link);
	if (next_tgroup == NULL) {
		next_tgroup = TAILQ_FIRST(&vu_transport->poll_groups);
	}
	if (vu_transport->next_pg == vu_group) {
		vu_transport->next_pg = next_tgroup;
	}
	pthread_mutex_unlock(&vu_transport->pg_lock);

	free(vu_group);
}

static void
_vfio_user_qpair_disconnect(void *ctx)
{
	struct nvmf_vfio_user_sq *sq = ctx;

	spdk_nvmf_qpair_disconnect(&sq->qpair);
}

/* The function is used when socket connection is destroyed */
static int
vfio_user_destroy_ctrlr(struct nvmf_vfio_user_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_endpoint *endpoint;

	SPDK_DEBUGLOG(nvmf_vfio, "%s stop processing\n", ctrlr_id(ctrlr));

	endpoint = ctrlr->endpoint;
	assert(endpoint != NULL);

	pthread_mutex_lock(&endpoint->lock);
	endpoint->need_relisten = true;
	ctrlr->disconnect = true;
	if (TAILQ_EMPTY(&ctrlr->connected_sqs)) {
		endpoint->ctrlr = NULL;
		free_ctrlr(ctrlr);
		pthread_mutex_unlock(&endpoint->lock);
		return 0;
	}

	TAILQ_FOREACH(sq, &ctrlr->connected_sqs, tailq) {
		/* add another round thread poll to avoid recursive endpoint lock */
		spdk_thread_send_msg(ctrlr->thread, _vfio_user_qpair_disconnect, sq);
	}
	pthread_mutex_unlock(&endpoint->lock);

	return 0;
}

/*
 * Poll for and process any incoming vfio-user messages.
 */
static int
vfio_user_poll_vfu_ctx(void *ctx)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	int ret;

	assert(ctrlr != NULL);

	/* This will call access_bar0_fn() if there are any writes
	 * to the portion of the BAR that is not mmap'd */
	ret = vfu_run_ctx(ctrlr->endpoint->vfu_ctx);
	if (spdk_unlikely(ret == -1)) {
		if (errno == EBUSY) {
			return SPDK_POLLER_IDLE;
		}

		spdk_poller_unregister(&ctrlr->vfu_ctx_poller);

		/*
		 * We lost the client; the reset callback will already have
		 * unregistered the interrupt.
		 */
		if (errno == ENOTCONN) {
			vfio_user_destroy_ctrlr(ctrlr);
			return SPDK_POLLER_BUSY;
		}

		/*
		 * We might not have got a reset callback in this case, so
		 * explicitly unregister the interrupt here.
		 */
		spdk_interrupt_unregister(&ctrlr->intr);
		ctrlr->intr_fd = -1;
		fail_ctrlr(ctrlr);
	}

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

struct vfio_user_post_cpl_ctx {
	struct nvmf_vfio_user_ctrlr	*ctrlr;
	struct nvmf_vfio_user_cq	*cq;
	struct spdk_nvme_cpl		cpl;
};

static void
_post_completion_msg(void *ctx)
{
	struct vfio_user_post_cpl_ctx *cpl_ctx = ctx;

	post_completion(cpl_ctx->ctrlr, cpl_ctx->cq, cpl_ctx->cpl.cdw0, cpl_ctx->cpl.sqid,
			cpl_ctx->cpl.cid, cpl_ctx->cpl.status.sc, cpl_ctx->cpl.status.sct);
	free(cpl_ctx);
}

static int nvmf_vfio_user_poll_group_poll(struct spdk_nvmf_transport_poll_group *group);

static int
vfio_user_poll_group_process(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_group = ctx;
	int ret = 0;

	SPDK_DEBUGLOG(vfio_user_db, "pg:%p got intr\n", vu_group);

	ret |= nvmf_vfio_user_poll_group_poll(&vu_group->group);

	/*
	 * Re-arm the event indexes. NB: this also could rearm other
	 * controller's SQs.
	 */
	ret |= vfio_user_poll_group_rearm(vu_group);

	vu_group->stats.pg_process_count++;
	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
vfio_user_poll_group_intr(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_group = ctx;
	eventfd_t val;

	eventfd_read(vu_group->intr_fd, &val);

	vu_group->stats.intr++;

	return vfio_user_poll_group_process(ctx);
}

/*
 * Handle an interrupt for the given controller: we must poll the vfu_ctx, and
 * the SQs assigned to our own poll group. Other poll groups are handled via
 * vfio_user_poll_group_intr().
 */
static int
vfio_user_ctrlr_intr(void *ctx)
{
	struct nvmf_vfio_user_poll_group *vu_ctrlr_group;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr = ctx;
	struct nvmf_vfio_user_poll_group *vu_group;
	int ret = SPDK_POLLER_IDLE;

	vu_ctrlr_group = ctrlr_to_poll_group(vu_ctrlr);

	SPDK_DEBUGLOG(vfio_user_db, "ctrlr pg:%p got intr\n", vu_ctrlr_group);

	vu_ctrlr_group->stats.ctrlr_intr++;

	/*
	 * Poll vfio-user for this controller. We need to do this before polling
	 * any SQs, as this is where doorbell writes may be handled.
	 */
	ret = vfio_user_poll_vfu_ctx(vu_ctrlr);

	/*
	 * `sqs[0]` could be set to NULL in vfio_user_poll_vfu_ctx() context,
	 * just return for this case.
	 */
	if (vu_ctrlr->sqs[0] == NULL) {
		return ret;
	}

	if (vu_ctrlr->transport->transport_opts.enable_intr_mode_sq_spreading) {
		/*
		 * We may have just written to a doorbell owned by another
		 * reactor: we need to prod them to make sure its SQs are polled
		 * *after* the doorbell value is updated.
		 */
		TAILQ_FOREACH(vu_group, &vu_ctrlr->transport->poll_groups, link) {
			if (vu_group != vu_ctrlr_group) {
				SPDK_DEBUGLOG(vfio_user_db, "prodding pg:%p\n", vu_group);
				eventfd_write(vu_group->intr_fd, 1);
			}
		}
	}

	ret |= vfio_user_poll_group_process(vu_ctrlr_group);

	return ret;
}

static void
vfio_user_ctrlr_set_intr_mode(struct spdk_poller *poller, void *ctx,
			      bool interrupt_mode)
{
	struct nvmf_vfio_user_ctrlr *ctrlr = ctx;
	assert(ctrlr != NULL);
	assert(ctrlr->endpoint != NULL);

	SPDK_DEBUGLOG(nvmf_vfio, "%s: setting interrupt mode to %d\n",
		      ctrlr_id(ctrlr), interrupt_mode);

	/*
	 * interrupt_mode needs to persist across controller resets, so store
	 * it in the endpoint instead.
	 */
	ctrlr->endpoint->interrupt_mode = interrupt_mode;

	vfio_user_poll_group_rearm(ctrlr_to_poll_group(ctrlr));
}

/*
 * In response to the nvmf_vfio_user_create_ctrlr() path, the admin queue is now
 * set up and we can start operating on this controller.
 */
static void
start_ctrlr(struct nvmf_vfio_user_ctrlr *vu_ctrlr,
	    struct spdk_nvmf_ctrlr *ctrlr)
{
	struct nvmf_vfio_user_endpoint *endpoint = vu_ctrlr->endpoint;

	vu_ctrlr->ctrlr = ctrlr;
	vu_ctrlr->cntlid = ctrlr->cntlid;
	vu_ctrlr->thread = spdk_get_thread();
	vu_ctrlr->state = VFIO_USER_CTRLR_RUNNING;

	if (!in_interrupt_mode(endpoint->transport)) {
		vu_ctrlr->vfu_ctx_poller = SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx,
					   vu_ctrlr, 1000);
		return;
	}

	vu_ctrlr->vfu_ctx_poller = SPDK_POLLER_REGISTER(vfio_user_poll_vfu_ctx,
				   vu_ctrlr, 0);

	vu_ctrlr->intr_fd = vfu_get_poll_fd(vu_ctrlr->endpoint->vfu_ctx);
	assert(vu_ctrlr->intr_fd != -1);

	vu_ctrlr->intr = SPDK_INTERRUPT_REGISTER(vu_ctrlr->intr_fd,
			 vfio_user_ctrlr_intr, vu_ctrlr);

	assert(vu_ctrlr->intr != NULL);

	spdk_poller_register_interrupt(vu_ctrlr->vfu_ctx_poller,
				       vfio_user_ctrlr_set_intr_mode,
				       vu_ctrlr);
}

/*
 * IAU-next P3, IAU_DEV_MODEL=ctrlr: binds an I/O SQ to its controller's shared channel state
 * (fp_sq_share: every SQ of the controller uses the same channels, one drive) and the calling thread,
 * the SQ's poll group, which drives its fast path from now on, claims it (fp_chan_claim). Runs when
 * the SQ connects, before it is ACTIVE, so its fp_sq is still empty. 0 if the model is sq or the
 * fast path is off (nothing to do), or the bind holds; -EPERM if another thread already drives the
 * controller's channels, a PG_BY_CTRLR placement violation (an fp_chan is never shared across
 * threads); another negative errno if the share fails. Caller holds endpoint->lock, which orders
 * users++ and the claim against fp_sq_fini() of the controller's other SQs (close_qpair).
 *
 * Reset semantics: when no SQ is bound (users 0, so no owner: the last fp_sq_fini() cleared it) the
 * channels are re-initialised (fp_chan_init), so after a CC.EN 0->1 reset, or any delete and re-create
 * of all I/O SQs, the drive starts idle, as each SQ's own channels do in the sq model.
 * Evidence on success: FP_SHARE ctrlr=<k> qid=<q> lcore=<l> users=<n> (check_l: every qid of a
 * controller on one lcore).
 *
 * Known gaps, not reachable with spdk_nvme_perf (it creates its I/O queues once, never retries a
 * failed Create I/O SQ and resets nothing while creating them):
 *  - on a bind failure the Create I/O SQ error completion is posted before the deferred disconnect
 *    has torn the SQ down; a host that re-issued Create I/O SQ for the same qid at once could race
 *    that teardown;
 *  - the failure path releases cq_ref as handle_del_io_q() does; if a controller reset/shutdown
 *    (reset_shn) or disconnect starts before the deferred disconnect runs, delete_sq_done() releases
 *    it a second time, the same window upstream handle_del_io_q() has;
 *  - pre-existing upstream races are left as they are: cq_ref is updated by the admin thread and by
 *    poll-group threads without one lock, and the -ENOMEM return in handle_queue_connect_rsp()
 *    leaves endpoint->lock held.
 */
static int
sq_fp_bind(struct nvmf_vfio_user_ctrlr *vu_ctrlr, struct nvmf_vfio_user_sq *sq)
{
	struct fp_chan *chan = &vu_ctrlr->fp_chan;
	int rc;

	if (sq->fp == NULL || !vu_ctrlr->fp_model_ctrlr) {
		return 0;
	}
	if (chan->users == 0) {
		rc = fp_chan_init(chan, vu_ctrlr->fp_channels, vu_ctrlr->fp_svc_ticks);
		if (rc != 0) {
			SPDK_ERRLOG("%s: fp_chan_init(channels=%u) failed for sqid:%u: %d; failing the SQ\n",
				    ctrlr_id(vu_ctrlr), vu_ctrlr->fp_channels, sq->qid, rc);
			return rc;
		}
	}
	rc = fp_sq_share(sq->fp, chan);
	if (rc != 0) {
		SPDK_ERRLOG("%s: fastpath channel share failed for sqid:%u: %d; failing the SQ\n",
			    ctrlr_id(vu_ctrlr), sq->qid, rc);
		return rc;
	}
	rc = fp_chan_claim(chan, spdk_get_thread());
	if (rc != 0) {
		SPDK_ERRLOG("PG_BY_CTRLR violation: ctrlr=%d qid=%u connected on thread %s (lcore %u), but the "
			    "controller's fast-path channels are driven by another thread (fp_chan_claim %d); "
			    "failing the SQ\n", vu_ctrlr->dev_idx, sq->qid, spdk_thread_get_name(spdk_get_thread()),
			    spdk_env_get_current_core(), rc);
		return rc;
	}
	SPDK_NOTICELOG("FP_SHARE ctrlr=%d qid=%u lcore=%u users=%u\n", vu_ctrlr->dev_idx, sq->qid,
		       spdk_env_get_current_core(), chan->users);
	return 0;
}

static int
handle_queue_connect_rsp(struct nvmf_vfio_user_req *req, void *cb_arg)
{
	struct nvmf_vfio_user_poll_group *vu_group;
	struct nvmf_vfio_user_sq *sq = cb_arg;
	struct nvmf_vfio_user_cq *admin_cq;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	struct nvmf_vfio_user_endpoint *endpoint;
	bool create_failed = false;

	assert(sq != NULL);
	assert(req != NULL);

	vu_ctrlr = sq->ctrlr;
	assert(vu_ctrlr != NULL);
	endpoint = vu_ctrlr->endpoint;
	assert(endpoint != NULL);

	if (spdk_nvme_cpl_is_error(&req->req.rsp->nvme_cpl)) {
		SPDK_ERRLOG("SC %u, SCT %u\n", req->req.rsp->nvme_cpl.status.sc, req->req.rsp->nvme_cpl.status.sct);
		endpoint->ctrlr = NULL;
		/* P3.9, mode 5: free_ctrlr() frees every SQ, bound ones included. */
		ctrlr_eng_detach_all(vu_ctrlr);
		free_ctrlr(vu_ctrlr);
		return -1;
	}

	vu_group = SPDK_CONTAINEROF(sq->group, struct nvmf_vfio_user_poll_group, group);
	TAILQ_INSERT_TAIL(&vu_group->sqs, sq, link);

	admin_cq = vu_ctrlr->cqs[0];
	assert(admin_cq != NULL);
	assert(admin_cq->group != NULL);
	assert(admin_cq->group->group->thread != NULL);

	pthread_mutex_lock(&endpoint->lock);
	if (nvmf_qpair_is_admin_queue(&sq->qpair)) {
		assert(admin_cq->group->group->thread == spdk_get_thread());
		/*
		 * The admin queue is special as SQ0 and CQ0 are created
		 * together.
		 */
		admin_cq->cq_ref = 1;
		start_ctrlr(vu_ctrlr, sq->qpair.ctrlr);
	} else {
		/* P3: this thread drives the SQ from now on; with IAU_DEV_MODEL=ctrlr the SQ binds the
		 * controller's channels here and this must be the only thread driving them, else the
		 * Create I/O SQ fails below. P3.9: then QCFG[q] against the mode (sq_eng_connect()): mode 5
		 * binds the SQ to an IAU engine; a mismatch fails the Create I/O SQ with INVALID_FIELD. */
		uint16_t create_sc = SPDK_NVME_SC_SUCCESS;

		if (sq_fp_bind(vu_ctrlr, sq) != 0) {
			/* As handle_del_io_q(): the SQ is deleted and releases its CQ; its qpair is
			 * disconnected below, outside endpoint->lock (close_qpair takes it), which tears
			 * the SQ down (delete_sq_done()) and drops its fp_chan binding. */
			create_failed = true;
			create_sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			sq->sq_state = VFIO_USER_SQ_DELETED;
			assert(vu_ctrlr->cqs[sq->cqid]->cq_ref);
			vu_ctrlr->cqs[sq->cqid]->cq_ref--;
		} else if ((create_sc = sq_eng_connect(vu_ctrlr, sq)) != SPDK_NVME_SC_SUCCESS) {
			/* The same for a QCFG mismatch or a failed IAU-engine bind (P3.9). */
			create_failed = true;
			sq->sq_state = VFIO_USER_SQ_DELETED;
			assert(vu_ctrlr->cqs[sq->cqid]->cq_ref);
			vu_ctrlr->cqs[sq->cqid]->cq_ref--;
		}
		/* For I/O queues this command was generated in response to an
		 * ADMIN I/O CREATE SUBMISSION QUEUE command which has not yet
		 * been completed. Complete it now.
		 */
		if (sq->post_create_io_sq_completion) {
			if (admin_cq->group->group->thread != spdk_get_thread()) {
				struct vfio_user_post_cpl_ctx *cpl_ctx;

				cpl_ctx = calloc(1, sizeof(*cpl_ctx));
				if (!cpl_ctx) {
					return -ENOMEM;
				}
				cpl_ctx->ctrlr = vu_ctrlr;
				cpl_ctx->cq = admin_cq;
				cpl_ctx->cpl.sqid = 0;
				cpl_ctx->cpl.cdw0 = 0;
				cpl_ctx->cpl.cid = sq->create_io_sq_cmd.cid;
				cpl_ctx->cpl.status.sc = create_sc;
				cpl_ctx->cpl.status.sct = SPDK_NVME_SCT_GENERIC;

				spdk_thread_send_msg(admin_cq->group->group->thread,
						     _post_completion_msg,
						     cpl_ctx);
			} else {
				post_completion(vu_ctrlr, admin_cq, 0, 0,
						sq->create_io_sq_cmd.cid, create_sc, SPDK_NVME_SCT_GENERIC);
			}
			sq->post_create_io_sq_completion = false;
		} else if (in_interrupt_mode(endpoint->transport)) {
			/*
			 * If we're live migrating a guest, there is a window
			 * where the I/O queues haven't been set up but the
			 * device is in running state, during which the guest
			 * might write to a doorbell. This doorbell write will
			 * go unnoticed, so let's poll the whole controller to
			 * pick that up.
			 */
			ctrlr_kick(vu_ctrlr);
		}
		if (!create_failed) {
			sq->sq_state = VFIO_USER_SQ_ACTIVE;
		}
	}

	TAILQ_INSERT_TAIL(&vu_ctrlr->connected_sqs, sq, tailq);
	pthread_mutex_unlock(&endpoint->lock);

	free(req->req.iov[0].iov_base);
	req->req.iov[0].iov_base = NULL;
	req->req.iovcnt = 0;

	if (create_failed) {
		/* After this connect completes; the host saw its Create I/O SQ fail. */
		spdk_thread_send_msg(spdk_get_thread(), _vfio_user_qpair_disconnect, sq);
	}

	return 0;
}

static void
_nvmf_vfio_user_poll_group_add(void *req)
{
	spdk_nvmf_request_exec(req);
}

/*
 * Add the given qpair to the given poll group. New qpairs are added via
 * spdk_nvmf_tgt_new_qpair(), which picks a poll group via
 * nvmf_vfio_user_get_optimal_poll_group(), then calls back here via
 * nvmf_transport_poll_group_add().
 */
static int
nvmf_vfio_user_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_req *vu_req;
	struct nvmf_vfio_user_ctrlr *ctrlr;
	struct spdk_nvmf_request *req;
	struct spdk_nvmf_fabric_connect_data *data;
	bool admin;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	sq->group = group;
	ctrlr = sq->ctrlr;

	SPDK_DEBUGLOG(nvmf_vfio, "%s: add QP%d=%p(%p) to poll_group=%p\n",
		      ctrlr_id(ctrlr), sq->qpair.qid,
		      sq, qpair, group);

	admin = nvmf_qpair_is_admin_queue(&sq->qpair);

	vu_req = get_nvmf_vfio_user_req(sq);
	if (vu_req == NULL) {
		return -1;
	}

	req = &vu_req->req;
	req->cmd->connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	req->cmd->connect_cmd.cid = 0;
	req->cmd->connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	req->cmd->connect_cmd.recfmt = 0;
	req->cmd->connect_cmd.sqsize = sq->size - 1;
	req->cmd->connect_cmd.qid = admin ? 0 : qpair->qid;

	req->length = sizeof(struct spdk_nvmf_fabric_connect_data);

	data = calloc(1, req->length);
	if (data == NULL) {
		nvmf_vfio_user_req_free(req);
		return -ENOMEM;
	}

	SPDK_IOV_ONE(req->iov, &req->iovcnt, data, req->length);

	data->cntlid = ctrlr->cntlid;
	snprintf(data->subnqn, sizeof(data->subnqn), "%s",
		 spdk_nvmf_subsystem_get_nqn(ctrlr->endpoint->subsystem));

	vu_req->cb_fn = handle_queue_connect_rsp;
	vu_req->cb_arg = sq;

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: sending connect fabrics command for qid:%#x cntlid=%#x\n",
		      ctrlr_id(ctrlr), qpair->qid, data->cntlid);

	/*
	 * By the time transport's poll_group_add() callback is executed, the
	 * qpair isn't in the ACTIVE state yet, so spdk_nvmf_request_exec()
	 * would fail.  The state changes to ACTIVE immediately after the
	 * callback finishes, so delay spdk_nvmf_request_exec() by sending a
	 * message.
	 */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_vfio_user_poll_group_add, req);
	return 0;
}

static int
nvmf_vfio_user_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_poll_group *vu_group;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);

	SPDK_DEBUGLOG(nvmf_vfio,
		      "%s: remove NVMf QP%d=%p from NVMf poll_group=%p\n",
		      ctrlr_id(sq->ctrlr), qpair->qid, qpair, group);


	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);
	TAILQ_REMOVE(&vu_group->sqs, sq, link);

	return 0;
}

static void
_nvmf_vfio_user_req_free(struct nvmf_vfio_user_sq *sq, struct nvmf_vfio_user_req *vu_req)
{
	memset(&vu_req->cmd, 0, sizeof(vu_req->cmd));
	memset(&vu_req->rsp, 0, sizeof(vu_req->rsp));
	vu_req->iovcnt = 0;
	vu_req->req.iovcnt = 0;
	vu_req->req.length = 0;
	vu_req->req.raw = 0; /* clear all flags */
	vu_req->req.cmd_cb_fn = NULL;
	vu_req->state = VFIO_USER_REQUEST_STATE_FREE;

	TAILQ_INSERT_TAIL(&sq->free_reqs, vu_req, link);
}

static void
nvmf_vfio_user_req_free(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_req *vu_req;

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	sq = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);

	_nvmf_vfio_user_req_free(sq, vu_req);
}

static void
nvmf_vfio_user_req_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_req *vu_req;

	assert(req != NULL);

	vu_req = SPDK_CONTAINEROF(req, struct nvmf_vfio_user_req, req);
	sq = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);

	if (vu_req->cb_fn != NULL) {
		if (vu_req->cb_fn(vu_req, vu_req->cb_arg) != 0) {
			fail_ctrlr(sq->ctrlr);
		}
	}

	_nvmf_vfio_user_req_free(sq, vu_req);
}

static void
nvmf_vfio_user_close_qpair(struct spdk_nvmf_qpair *qpair,
			   spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_ctrlr *vu_ctrlr;
	struct nvmf_vfio_user_endpoint *endpoint;
	struct vfio_user_delete_sq_ctx *del_ctx;

	assert(qpair != NULL);
	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	vu_ctrlr = sq->ctrlr;
	endpoint = vu_ctrlr->endpoint;
	del_ctx = sq->delete_ctx;
	sq->delete_ctx = NULL;

	pthread_mutex_lock(&endpoint->lock);
	TAILQ_REMOVE(&vu_ctrlr->connected_sqs, sq, tailq);
	delete_sq_done(vu_ctrlr, sq);
	if (TAILQ_EMPTY(&vu_ctrlr->connected_sqs)) {
		endpoint->ctrlr = NULL;
		free_ctrlr(vu_ctrlr);
	}
	pthread_mutex_unlock(&endpoint->lock);

	if (del_ctx) {
		vfio_user_qpair_delete_cb(del_ctx);
	}

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

/**
 * Returns a preallocated request, or NULL if there isn't one available.
 */
static struct nvmf_vfio_user_req *
get_nvmf_vfio_user_req(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_req *req;

	if (sq == NULL) {
		return NULL;
	}

	req = TAILQ_FIRST(&sq->free_reqs);
	if (req == NULL) {
		return NULL;
	}

	TAILQ_REMOVE(&sq->free_reqs, req, link);

	return req;
}

static int
get_nvmf_io_req_length(struct spdk_nvmf_request *req)
{
	uint16_t nr;
	uint32_t nlb, nsid;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_ns *ns;

	nsid = cmd->nsid;
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		SPDK_ERRLOG("unsuccessful query for nsid %u\n", cmd->nsid);
		return -EINVAL;
	}

	if (cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
		nr = cmd->cdw10_bits.dsm.nr + 1;
		return nr * sizeof(struct spdk_nvme_dsm_range);
	}

	if (cmd->opc == SPDK_NVME_OPC_COPY) {
		nr = (cmd->cdw12 & 0x000000ffu) + 1;
		return nr * sizeof(struct spdk_nvme_scc_source_range);
	}

	nlb = (cmd->cdw12 & 0x0000ffffu) + 1;
	return nlb * spdk_bdev_desc_get_block_size(ns->desc);
}

static int
map_admin_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint32_t len = 0, numdw = 0;
	uint8_t fid;
	int iovcnt;

	req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

	if (req->xfer == SPDK_NVME_DATA_NONE) {
		return 0;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		len = 4096;
		break;
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		numdw = ((((uint32_t)cmd->cdw11_bits.get_log_page.numdu << 16) |
			  cmd->cdw10_bits.get_log_page.numdl) + 1);
		if (numdw > UINT32_MAX / 4) {
			return -EINVAL;
		}
		len = numdw * 4;
		break;
	case SPDK_NVME_OPC_GET_FEATURES:
	case SPDK_NVME_OPC_SET_FEATURES:
		fid = cmd->cdw10_bits.set_features.fid;
		switch (fid) {
		case SPDK_NVME_FEAT_LBA_RANGE_TYPE:
			len = 4096;
			break;
		case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION:
			len = 256;
			break;
		case SPDK_NVME_FEAT_TIMESTAMP:
			len = 8;
			break;
		case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
			len = 512;
			break;
		case SPDK_NVME_FEAT_HOST_IDENTIFIER:
			if (cmd->cdw11_bits.feat_host_identifier.bits.exhid) {
				len = 16;
			} else {
				len = 8;
			}
			break;
		default:
			return 0;
		}
		break;
	case SPDK_NVME_OPC_FABRIC:
		return -ENOTSUP;
	default:
		return 0;
	}

	/* ADMIN command will not use SGL */
	if (cmd->psdt != 0) {
		return -EINVAL;
	}

	iovcnt = vfio_user_map_cmd(ctrlr, req, req->iov, len);
	if (iovcnt < 0) {
		SPDK_ERRLOG("%s: map Admin Opc %x failed\n",
			    ctrlr_id(ctrlr), cmd->opc);
		return -1;
	}
	req->length = len;
	req->iovcnt = iovcnt;

	return 0;
}

/*
 * Map an I/O command's buffers.
 *
 * Returns 0 on success and -errno on failure.
 */
static int
map_io_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvmf_request *req)
{
	int len, iovcnt;
	struct spdk_nvme_cmd *cmd;

	assert(ctrlr != NULL);
	assert(req != NULL);

	cmd = &req->cmd->nvme_cmd;
	req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

	if (spdk_unlikely(req->xfer == SPDK_NVME_DATA_NONE)) {
		return 0;
	}

	len = get_nvmf_io_req_length(req);
	if (len < 0) {
		return -EINVAL;
	}
	req->length = len;

	iovcnt = vfio_user_map_cmd(ctrlr, req, req->iov, req->length);
	if (iovcnt < 0) {
		SPDK_ERRLOG("%s: failed to map IO OPC %u\n", ctrlr_id(ctrlr), cmd->opc);
		return -EFAULT;
	}
	req->iovcnt = iovcnt;

	return 0;
}

static int
handle_cmd_req(struct nvmf_vfio_user_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
	       struct nvmf_vfio_user_sq *sq)
{
	int err;
	struct nvmf_vfio_user_req *vu_req;
	struct spdk_nvmf_request *req;

	assert(ctrlr != NULL);
	assert(cmd != NULL);

	vu_req = get_nvmf_vfio_user_req(sq);
	if (spdk_unlikely(vu_req == NULL)) {
		SPDK_ERRLOG("%s: no request for NVMe command opc 0x%x\n", ctrlr_id(ctrlr), cmd->opc);
		return post_completion(ctrlr, ctrlr->cqs[sq->cqid], 0, 0, cmd->cid,
				       SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, SPDK_NVME_SCT_GENERIC);

	}
	req = &vu_req->req;

	if (spdk_unlikely(sq->dt.on)) {
		vu_req->ts_db_seen = sq->ts_last_db_seen;
		vu_req->ts_fetched = spdk_get_ticks();
		vu_req->ts_exec = vu_req->ts_fetched;	/* overwritten just before exec */
		vu_req->ts_done = 0;
	}

	assert(req->qpair != NULL);
	SPDK_DEBUGLOG(nvmf_vfio, "%s: handle sqid:%u, req opc=%#x cid=%d\n",
		      ctrlr_id(ctrlr), req->qpair->qid, cmd->opc, cmd->cid);

	vu_req->cb_fn = handle_cmd_rsp;
	vu_req->cb_arg = SPDK_CONTAINEROF(req->qpair, struct nvmf_vfio_user_sq, qpair);
	req->cmd->nvme_cmd = *cmd;

	if (nvmf_qpair_is_admin_queue(req->qpair)) {
		err = map_admin_cmd_req(ctrlr, req);
	} else {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
		case SPDK_NVME_OPC_FABRIC:
			err = -ENOTSUP;
			break;
		default:
			err = map_io_cmd_req(ctrlr, req);
			break;
		}
	}

	if (spdk_unlikely(err < 0)) {
		SPDK_ERRLOG("%s: process NVMe command opc 0x%x failed\n",
			    ctrlr_id(ctrlr), cmd->opc);
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = err == -ENOTSUP ?
					       SPDK_NVME_SC_INVALID_OPCODE :
					       SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		err = handle_cmd_rsp(vu_req, vu_req->cb_arg);
		_nvmf_vfio_user_req_free(sq, vu_req);
		return err;
	}

	vu_req->state = VFIO_USER_REQUEST_STATE_EXECUTING;
	if (spdk_unlikely(sq->dt.on)) {
		vu_req->ts_exec = spdk_get_ticks();
	}
	spdk_nvmf_request_exec(req);

	return 0;
}

/*
 * If we suppressed an IRQ in post_completion(), check if it needs to be fired
 * here: if the host isn't up to date, and is apparently not actively processing
 * the queue (i.e. ->last_head isn't changing), we need an IRQ.
 */
static void
handle_suppressed_irq(struct nvmf_vfio_user_ctrlr *ctrlr,
		      struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_cq *cq = ctrlr->cqs[sq->cqid];
	uint32_t cq_head;
	uint32_t cq_tail;

	if (!cq->ien || cq->qid == 0 || !ctrlr_interrupt_enabled(ctrlr)) {
		return;
	}

	cq_tail = *cq_tailp(cq);

	/* Already sent? */
	if (cq_tail == cq->last_trigger_irq_tail) {
		return;
	}

	spdk_ivdt_dcache(cq_dbl_headp(cq));
	cq_head = *cq_dbl_headp(cq);

	if (cq_head != cq_tail && cq_head == cq->last_head) {
		int err = vfu_irq_trigger(ctrlr->endpoint->vfu_ctx, cq->iv);
		if (err != 0) {
			SPDK_ERRLOG("%s: failed to trigger interrupt: %m\n",
				    ctrlr_id(ctrlr));
		} else {
			cq->last_trigger_irq_tail = cq_tail;
		}
	}

	cq->last_head = cq_head;
}

static int nvmf_vfio_user_sq_poll_inner(struct nvmf_vfio_user_sq *sq);

/* Returns the number of commands processed, or a negative value on error. */
static int
nvmf_vfio_user_sq_poll(struct nvmf_vfio_user_sq *sq)
{
	int count = nvmf_vfio_user_sq_poll_inner(sq);

	if (sq->fp != NULL && sq->ctrlr->state == VFIO_USER_CTRLR_RUNNING) {
		int done = fp_drain_due(sq->ctrlr, sq);

		if (count >= 0) {
			count += done;
		}
		if (sq->iau_attached && iau_sq_has_staged(&sq->ctrlr->iau, sq->qid)) {
			/* Time-based flush, checked on every poll including empty ones. The
			 * tick is read only while a completion is staged, the only case in
			 * which iau_sq_poll_timer() uses it (never at IAU_CQ_BATCH_N=1). */
			iau_sq_poll_timer(&sq->ctrlr->iau, sq->qid, spdk_get_ticks());
		}
	}
	return count;
}

static int
nvmf_vfio_user_sq_poll_inner(struct nvmf_vfio_user_sq *sq)
{
	struct nvmf_vfio_user_ctrlr *ctrlr;
	uint32_t new_tail;
	int count = 0;

	assert(sq != NULL);

	ctrlr = sq->ctrlr;

	/*
	 * A quiesced, or migrating, controller should never process new
	 * commands.
	 */
	if (ctrlr->state != VFIO_USER_CTRLR_RUNNING) {
		return SPDK_POLLER_IDLE;
	}

	if (ctrlr->adaptive_irqs_enabled) {
		handle_suppressed_irq(ctrlr, sq);
	}

	/* On aarch64 platforms, doorbells update from guest VM may not be seen
	 * on SPDK target side. This is because there is memory type mismatch
	 * situation here. That is on guest VM side, the doorbells are treated as
	 * device memory while on SPDK target side, it is treated as normal
	 * memory. And this situation cause problem on ARM platform.
	 * Refer to "https://developer.arm.com/documentation/102376/0100/
	 * Memory-aliasing-and-mismatched-memory-types". Only using spdk_mb()
	 * cannot fix this. Use "dc civac" to invalidate cache may solve
	 * this.
	 */
	spdk_ivdt_dcache(sq_dbl_tailp(sq));

	/* Load-Acquire. */
	new_tail = *sq_dbl_tailp(sq);

	new_tail = new_tail & 0xffffu;
	if (spdk_unlikely(new_tail >= sq->size)) {
		SPDK_DEBUGLOG(nvmf_vfio, "%s: invalid sqid:%u doorbell value %u\n", ctrlr_id(ctrlr), sq->qid,
			      new_tail);
		spdk_nvmf_ctrlr_async_event_error_event(ctrlr->ctrlr, SPDK_NVME_ASYNC_EVENT_INVALID_DB_WRITE);

		return -1;
	}

	if (*sq_headp(sq) == new_tail) {
		return 0;
	}

	SPDK_DEBUGLOG(nvmf_vfio, "%s: sqid:%u doorbell old=%u new=%u\n",
		      ctrlr_id(ctrlr), sq->qid, *sq_headp(sq), new_tail);
	if (ctrlr->sdbl != NULL) {
		SPDK_DEBUGLOG(nvmf_vfio,
			      "%s: sqid:%u bar0_doorbell=%u shadow_doorbell=%u eventidx=%u\n",
			      ctrlr_id(ctrlr), sq->qid,
			      ctrlr->bar0_doorbells[queue_index(sq->qid, false)],
			      ctrlr->sdbl->shadow_doorbells[queue_index(sq->qid, false)],
			      ctrlr->sdbl->eventidxs[queue_index(sq->qid, false)]);
	}

	/*
	 * Ensure that changes to the queue are visible to us.
	 * The host driver should write the queue first, do a wmb(), and then
	 * update the SQ tail doorbell (their Store-Release).
	 */
	spdk_rmb();

	count = handle_sq_tdbl_write(ctrlr, new_tail, sq);
	if (spdk_unlikely(count < 0)) {
		fail_ctrlr(ctrlr);
	}

	return count;
}

/*
 * vfio-user transport poll handler. Note that the library context is polled in
 * a separate poller (->vfu_ctx_poller), so this poller only needs to poll the
 * active SQs.
 *
 * Returns the number of commands processed, or a negative value on error.
 */
static int
nvmf_vfio_user_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_vfio_user_poll_group *vu_group;
	struct nvmf_vfio_user_sq *sq, *tmp;
	int count = 0;

	assert(group != NULL);

	vu_group = SPDK_CONTAINEROF(group, struct nvmf_vfio_user_poll_group, group);

	SPDK_DEBUGLOG(vfio_user_db, "polling all SQs\n");

	TAILQ_FOREACH_SAFE(sq, &vu_group->sqs, link, tmp) {
		int ret;

		if (spdk_unlikely(sq->sq_state != VFIO_USER_SQ_ACTIVE || !sq->size)) {
			continue;
		}

		ret = nvmf_vfio_user_sq_poll(sq);

		if (spdk_unlikely(ret < 0)) {
			return ret;
		}

		count += ret;
	}

	vu_group->stats.polls++;
	vu_group->stats.poll_reqs += count;
	vu_group->stats.poll_reqs_squared += count * count;
	if (count == 0) {
		vu_group->stats.polls_spurious++;
	}

	if (vu_group->need_kick) {
		poll_group_kick(vu_group);
	}

	return count;
}

static int
nvmf_vfio_user_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_ctrlr *ctrlr;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	ctrlr = sq->ctrlr;

	memcpy(trid, &ctrlr->endpoint->trid, sizeof(*trid));
	return 0;
}

static int
nvmf_vfio_user_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	return 0;
}

static int
nvmf_vfio_user_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid)
{
	struct nvmf_vfio_user_sq *sq;
	struct nvmf_vfio_user_ctrlr *ctrlr;

	sq = SPDK_CONTAINEROF(qpair, struct nvmf_vfio_user_sq, qpair);
	ctrlr = sq->ctrlr;

	memcpy(trid, &ctrlr->endpoint->trid, sizeof(*trid));
	return 0;
}

static void
nvmf_vfio_user_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_request *req_to_abort = NULL;
	struct spdk_nvmf_request *temp_req = NULL;
	uint16_t cid;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;

	TAILQ_FOREACH(temp_req, &qpair->outstanding, link) {
		struct nvmf_vfio_user_req *vu_req;

		vu_req = SPDK_CONTAINEROF(temp_req, struct nvmf_vfio_user_req, req);

		if (vu_req->state == VFIO_USER_REQUEST_STATE_EXECUTING && vu_req->cmd.cid == cid) {
			req_to_abort = temp_req;
			break;
		}
	}

	if (req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = req_to_abort;
	nvmf_ctrlr_abort_request(req);
}

static void
nvmf_vfio_user_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
				    struct spdk_json_write_ctx *w)
{
	struct nvmf_vfio_user_poll_group *vu_group = SPDK_CONTAINEROF(group,
			struct nvmf_vfio_user_poll_group, group);
	uint64_t polls_denom;

	spdk_json_write_named_uint64(w, "ctrlr_intr", vu_group->stats.ctrlr_intr);
	spdk_json_write_named_uint64(w, "ctrlr_kicks", vu_group->stats.ctrlr_kicks);
	spdk_json_write_named_uint64(w, "pg_kicks", vu_group->stats.pg_kicks);
	spdk_json_write_named_uint64(w, "won", vu_group->stats.won);
	spdk_json_write_named_uint64(w, "lost", vu_group->stats.lost);
	spdk_json_write_named_uint64(w, "lost_count", vu_group->stats.lost_count);
	spdk_json_write_named_uint64(w, "rearms", vu_group->stats.rearms);
	spdk_json_write_named_uint64(w, "cq_full", vu_group->stats.cq_full);
	spdk_json_write_named_uint64(w, "pg_process_count", vu_group->stats.pg_process_count);
	spdk_json_write_named_uint64(w, "intr", vu_group->stats.intr);
	spdk_json_write_named_uint64(w, "polls", vu_group->stats.polls);
	spdk_json_write_named_uint64(w, "polls_spurious", vu_group->stats.polls_spurious);
	spdk_json_write_named_uint64(w, "poll_reqs", vu_group->stats.poll_reqs);
	polls_denom = vu_group->stats.polls * (vu_group->stats.polls - 1);
	if (polls_denom) {
		uint64_t n = vu_group->stats.polls * vu_group->stats.poll_reqs_squared - vu_group->stats.poll_reqs *
			     vu_group->stats.poll_reqs;
		spdk_json_write_named_double(w, "poll_reqs_variance", sqrt(n / polls_denom));
	}

	spdk_json_write_named_uint64(w, "cqh_admin_writes", vu_group->stats.cqh_admin_writes);
	spdk_json_write_named_uint64(w, "cqh_io_writes", vu_group->stats.cqh_io_writes);
}

static void
nvmf_vfio_user_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		NVMF_VFIO_USER_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	NVMF_VFIO_USER_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	0;
	opts->max_io_size =		NVMF_VFIO_USER_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		NVMF_VFIO_USER_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =		NVMF_VFIO_USER_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	0;
	opts->buf_cache_size =		0;
	opts->association_timeout =	0;
	opts->transport_specific =      NULL;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_vfio_user = {
	.name = "VFIOUSER",
	.type = SPDK_NVME_TRANSPORT_VFIOUSER,
	.opts_init = nvmf_vfio_user_opts_init,
	.create = nvmf_vfio_user_create,
	.destroy = nvmf_vfio_user_destroy,

	.listen = nvmf_vfio_user_listen,
	.stop_listen = nvmf_vfio_user_stop_listen,
	.cdata_init = nvmf_vfio_user_cdata_init,
	.listen_associate = nvmf_vfio_user_listen_associate,

	.listener_discover = nvmf_vfio_user_discover,

	.poll_group_create = nvmf_vfio_user_poll_group_create,
	.get_optimal_poll_group = nvmf_vfio_user_get_optimal_poll_group,
	.poll_group_destroy = nvmf_vfio_user_poll_group_destroy,
	.poll_group_add = nvmf_vfio_user_poll_group_add,
	.poll_group_remove = nvmf_vfio_user_poll_group_remove,
	.poll_group_poll = nvmf_vfio_user_poll_group_poll,

	.req_free = nvmf_vfio_user_req_free,
	.req_complete = nvmf_vfio_user_req_complete,

	.qpair_fini = nvmf_vfio_user_close_qpair,
	.qpair_get_local_trid = nvmf_vfio_user_qpair_get_local_trid,
	.qpair_get_peer_trid = nvmf_vfio_user_qpair_get_peer_trid,
	.qpair_get_listen_trid = nvmf_vfio_user_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_vfio_user_qpair_abort_request,

	.poll_group_dump_stat = nvmf_vfio_user_poll_group_dump_stat,
};

SPDK_NVMF_TRANSPORT_REGISTER(muser, &spdk_nvmf_transport_vfio_user);
SPDK_LOG_REGISTER_COMPONENT(nvmf_vfio)
SPDK_LOG_REGISTER_COMPONENT(vfio_user_db)
