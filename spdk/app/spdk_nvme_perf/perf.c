/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 *
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk_internal/nvme_util.h"
#include "spdk/vmd.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"
#include "spdk/histogram_data.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/sock.h"
#include "spdk/zipf.h"
#include "spdk/nvmf.h"
#include "spdk/keyring.h"
#include "spdk/module/keyring/file.h"
#include "spdk/event.h"
#include "spdk/trace.h"

#ifdef SPDK_CONFIG_URING
#include <liburing.h>
#endif

#if HAVE_LIBAIO
#include <libaio.h>
#endif

/* IAU-next P4.12, the fault mode (SPDK_PERF_FAULT=1): its state, declared first so the existing globals keep their
 * .bss offsets (GCC lays .bss out in reverse order of declaration), and its functions, placed with the cold code
 * (.text.unlikely.perf_fault) and defined after work_fn() (perf.o's .text, work_fn() and the default per-IO path keep
 * their code and their 64 B phases) */
struct ns_worker_ctx;
struct perf_task;
struct worker_thread;
struct perf_fault;
struct perf_fault_ns;
static struct perf_fault *g_fault;	/* NULL: the fault mode is off */
static int perf_fault_init(void);
static int perf_fault_setup(void);
static int perf_fault_attach(void);
static void perf_fault_print_end(void);
static void *perf_fault_poll_ctrlrs(void *arg);
static void perf_idle_time_print(struct worker_thread *worker, uint64_t idle_tsc, uint64_t window_tsc);

static bool g_worker_group;	/* SPDK_PERF_WORKER_GROUP=1 (IAU-next P3), parsed once in main(); first, so the
				 * existing globals keep their .bss offsets */
static bool g_perf_trace_fast;		/* SPDK_PERF_TRACE_FAST=1, parsed once in main() */
static bool g_perf_trace_fast_check;	/* SPDK_PERF_TRACE_FAST_CHECK=1 (implies fast), parsed once in main() */

#define HELP_RETURN_CODE UINT16_MAX

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	enum spdk_nvme_transport_type		trtype;
	struct spdk_nvme_intel_rw_latency_page	*latency_page;

	struct spdk_nvme_qpair			**unused_qpairs;

	TAILQ_ENTRY(ctrlr_entry)		link;
	char					name[1024];
};

enum entry_type {
	ENTRY_TYPE_NVME_NS,
	ENTRY_TYPE_AIO_FILE,
	ENTRY_TYPE_URING_FILE,
};

struct ns_fn_table;

struct ns_entry {
	enum entry_type		type;
	const struct ns_fn_table	*fn_table;

	union {
		struct {
			struct spdk_nvme_ctrlr	*ctrlr;
			struct spdk_nvme_ns	*ns;
		} nvme;
#ifdef SPDK_CONFIG_URING
		struct {
			int			fd;
		} uring;
#endif
#if HAVE_LIBAIO
		struct {
			int                     fd;
		} aio;
#endif
	} u;

	TAILQ_ENTRY(ns_entry)	link;
	uint32_t		io_size_blocks;
	uint32_t		num_io_requests;
	uint64_t		size_in_ios;
	uint32_t		block_size;
	uint32_t		md_size;
	bool			md_interleave;
	struct spdk_zipf	*zipf;
	bool			pi_loc;
	enum spdk_nvme_pi_type	pi_type;
	uint32_t		io_flags;
	char			name[1024];
};

static const double g_latency_cutoffs[] = {
	0.01,
	0.10,
	0.25,
	0.50,
	0.75,
	0.90,
	0.95,
	0.98,
	0.99,
	0.995,
	0.999,
	0.9999,
	0.99999,
	0.999999,
	0.9999999,
	-1,
};

struct ns_worker_stats {
	uint64_t		io_submitted;
	uint64_t		io_completed;
	uint64_t		last_io_completed;
	uint64_t		total_tsc;
	uint64_t		min_tsc;
	uint64_t		max_tsc;
	uint64_t		last_tsc;
	uint64_t		busy_tsc;
	uint64_t		idle_tsc;
	uint64_t		last_busy_tsc;
	uint64_t		last_idle_tsc;
};

struct worker_thread;

struct ns_worker_ctx {
	struct ns_entry		*entry;
	struct ns_worker_stats	stats;
	uint64_t		current_queue_depth;
	uint64_t		number_ios;
	uint64_t		offset_in_ios;
	uint64_t		trace_cursor;	/* per-worker trace replay cursor */
	bool			is_draining;

	union {
		struct {
			int				num_active_qpairs;
			int				num_all_qpairs;
			struct spdk_nvme_qpair		**qpair;
			struct spdk_nvme_poll_group	*group;
			int				last_qpair;
		} nvme;

#ifdef SPDK_CONFIG_URING
		struct {
			struct io_uring		ring;
			uint64_t		io_inflight;
			uint64_t		io_pending;
			struct io_uring_cqe	**cqes;

		} uring;
#endif
#if HAVE_LIBAIO
		struct {
			struct io_event		*events;
			io_context_t		ctx;
		} aio;
#endif
	} u;

	TAILQ_ENTRY(ns_worker_ctx)	link;

	TAILQ_HEAD(, perf_task)		queued_tasks;

	struct spdk_histogram_data	*histogram;
	int				status;
	uint64_t		trace_check_cursor;	/* SPDK_PERF_TRACE_FAST_CHECK: stock-path shadow cursor */
	uint64_t		*trace_fast_tbl;	/* SPDK_PERF_TRACE_FAST: (offset_in_ios << 1 | is_read) x g_trace_count */
	struct worker_thread	*worker;	/* owner (SPDK_PERF_WORKER_GROUP) */
	struct perf_fault_ns	*fault;		/* SPDK_PERF_FAULT=1 (P4.12): its state; last, no field moves */
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	struct iovec		*iovs; /* array of iovecs to transfer. */
	int			iovcnt; /* Number of iovecs in iovs array. */
	int			iovpos; /* Current iovec position. */
	uint32_t		iov_offset; /* Offset in current iovec. */
	struct iovec		md_iov;
	uint64_t		submit_tsc;
	bool			is_read;
	uint8_t			f_inflight; /* SPDK_PERF_FAULT=1 (P4.12): submitted, no callback yet; in a hole */
	struct spdk_dif_ctx	dif_ctx;
#if HAVE_LIBAIO
	struct iocb		iocb;
#endif
	TAILQ_ENTRY(perf_task)	link;
};

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx)	ns_ctx;
	TAILQ_ENTRY(worker_thread)	link;
	unsigned			lcore;
	uint32_t			idx;	/* ascending-lcore registration order */
	/* SPDK_PERF_WORKER_GROUP=1: one spdk_nvme_poll_group holding the qpairs of every ns_ctx of this worker. */
	struct spdk_nvme_poll_group	*group;
	uint32_t			group_ctx;	/* ns_ctx whose qpairs are in group */
};

/* PERF_QPAIR_ORDERED: round r, worker w allocates qid r*W+w+1 (startup only). */
static uint32_t g_qpair_turn;

/* ---- Co-resident DiskANN-style compute kernel (SPDK_COMPUTE_PER_IO) --------
 * Models a graph-ANN search that SHARES the SPDK poll core: on each completed
 * read the same thread computes L2 distances between a fixed query vector and
 * the fetched node's vectors, then issues the next hop.  g_compute_per_io is
 * the number of 128-dim float32 distance computations per completed IO
 * (0 = pure I/O, the stock behaviour).  This converts the IAU's freed host CPU
 * into a measurable application throughput at iso-IOPS.  Scalar SSE2 math only
 * (the guest binary omits SSSE3, which crashes the sim CPU). */
#define ANN_DIM 128
#define ANN_MAX_THREADS 256
/* P8.H2-fix: no write-shared global on the per-IO path.  The read-only kernel inputs (compute_per_io and the
 * query vector) live on their own 64 B lines, written only at lazy init.  The sink and the op/call counters are
 * per thread, each thread's block on its own 64 B-aligned heap line, summed only for the end-of-run
 * compute_stats line (heap blocks outlive the worker threads). */
struct ann_ro {
	int		per_io;			/* -1 = uninitialised (lazy init); 0 = pure I/O */
	char		pad[60];
	float		query[ANN_DIM];
} __attribute__((aligned(64)));
struct ann_tls {
	volatile double	sink;			/* volatile: defeat dead-code elim */
	uint64_t	ops;			/* distance kernels executed by this thread */
	uint64_t	calls;			/* completions that ran the kernel on this thread */
	char		pad[40];
} __attribute__((aligned(64)));
static struct ann_ro g_ann_ro = { .per_io = -1 };
#define g_compute_per_io g_ann_ro.per_io
#define g_ann_query g_ann_ro.query
static struct ann_tls *g_ann_tls[ANN_MAX_THREADS];
static uint32_t g_ann_tls_n;
static __thread struct ann_tls *t_ann;

struct ns_fn_table {
	void	(*setup_payload)(struct perf_task *task, uint8_t pattern);

	int	(*submit_io)(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
			     struct ns_entry *entry, uint64_t offset_in_ios);

	int64_t	(*check_io)(struct ns_worker_ctx *ns_ctx);

	void	(*verify_io)(struct perf_task *task, struct ns_entry *entry);

	int	(*init_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);

	void	(*cleanup_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);
	void	(*dump_transport_stats)(uint32_t lcore, struct ns_worker_ctx *ns_ctx);
};

static uint32_t g_io_unit_size = (UINT32_MAX & (~0x03));

static int g_outstanding_commands;

static bool g_latency_ssd_tracking_enable;
static int g_latency_sw_tracking_level;

static bool g_vmd;
static const char *g_workload_type;

/*
 * BigANN/DiskANN trace replay support (paper §4.1 workload-realism evidence).
 * When --trace-file <path>.bin is passed, submit_single_io() pulls the next
 * (offset, size, op) tuple from the in-memory trace array instead of using
 * the random/zipf/sequential generator. The .bin format is produced by
 * scripts/bigann/trace_to_binary.py from the canonical trace CSV.
 *
 * Layout of one entry in the .bin file (16 bytes, little-endian):
 *   uint64_t offset_bytes;
 *   uint32_t size_bytes;
 *   uint8_t  op;          // 'R' (read) or 'W' (write)
 *   uint8_t  pad[3];
 */
struct perf_trace_entry {
	uint64_t offset;
	uint32_t size;
	uint8_t  op;
	uint8_t  pad[3];
} __attribute__((packed));

static const char *g_trace_path = NULL;
static struct perf_trace_entry *g_trace_entries = NULL;
static uint64_t g_trace_count = 0;

static int
load_trace_file(const char *path)
{
	FILE *fp;
	long fsize;

	fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "[trace] cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	fsize = ftell(fp);
	rewind(fp);

	if (fsize <= 0 || (fsize % (long)sizeof(struct perf_trace_entry)) != 0) {
		fprintf(stderr, "[trace] %s: bad size %ld (must be multiple of %zu bytes)\n",
			path, fsize, sizeof(struct perf_trace_entry));
		fclose(fp);
		return -1;
	}

	g_trace_count = (uint64_t)fsize / sizeof(struct perf_trace_entry);
	g_trace_entries = malloc((size_t)fsize);
	if (!g_trace_entries) {
		fprintf(stderr, "[trace] malloc(%ld) failed\n", fsize);
		fclose(fp);
		return -1;
	}

	if (fread(g_trace_entries, sizeof(struct perf_trace_entry), g_trace_count, fp) !=
	    g_trace_count) {
		fprintf(stderr, "[trace] short read from %s\n", path);
		free(g_trace_entries);
		g_trace_entries = NULL;
		fclose(fp);
		return -1;
	}
	fclose(fp);

	fprintf(stdout, "[trace] loaded %lu entries (%ld bytes) from %s\n",
		g_trace_count, fsize, path);
	return 0;
}
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static uint32_t g_num_namespaces = 0;
static uint32_t g_num_async_devs = 0;
static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);
static uint32_t g_num_workers = 0;
static bool g_use_every_core = false;
static uint32_t g_main_core;
static pthread_barrier_t g_worker_sync_barrier;

static uint64_t g_tsc_rate;

static bool g_monitor_perf_cores = false;

static uint32_t g_io_align = 0x200;
static bool g_io_align_specified;
static uint32_t g_io_size_bytes;
static uint32_t g_max_io_md_size;
static uint32_t g_max_io_size_blocks;
static uint32_t g_metacfg_pract_flag;
static uint32_t g_metacfg_prchk_flags;
static int g_rw_percentage = -1;
static int g_is_random;
static uint32_t g_queue_depth;
static int g_nr_io_queues_per_ns = 1;
static int g_nr_unused_io_queues;
static int g_time_in_sec;
static uint64_t g_number_ios;
static int g_number_ios_percent;
static uint64_t g_elapsed_time_in_usec;
static int g_warmup_time_in_sec;
static uint32_t g_max_completions;
static uint32_t g_disable_sq_cmb;
static bool g_enable_interrupt;
static bool g_use_uring;
static bool g_warn;
static bool g_header_digest;
static bool g_data_digest;
static bool g_no_shn_notification;
static bool g_mix_specified;
static char *g_tpoint_group_mask = NULL;
/* The flag is used to exit the program while keep alive fails on the transport */
static bool g_exit;
/* Default to 10 seconds for the keep alive value. This value is arbitrary. */
static uint32_t g_keep_alive_timeout_in_ms = 10000;
static bool g_continue_on_error = false;
static uint32_t g_quiet_count = 1;
static double g_zipf_theta;
/* Set default io_queue_size to UINT16_MAX, NVMe driver will then reduce this
 * to MQES to maximize the io_queue_size as much as possible.
 */
static uint32_t g_io_queue_size = UINT16_MAX;

static enum spdk_log_level g_log_level = SPDK_LOG_ERROR;

static uint32_t g_sock_zcopy_threshold;
static char *g_sock_threshold_impl;

static uint8_t g_transport_tos = 0;

static uint32_t g_rdma_srq_size;
static struct spdk_key *g_psk = NULL, *g_dhchap = NULL, *g_dhchap_ctrlr = NULL;
static char *g_vf_token = NULL;

/* When user specifies -Q, some error messages are rate limited.  When rate
 * limited, we only print the error message every g_quiet_count times the
 * error occurs.
 *
 * Note: the __count is not thread safe, meaning the rate limiting will not
 * be exact when running perf with multiple thread with lots of errors.
 * Thread-local __count would mean rate-limiting per thread which doesn't
 * seem as useful.
 */
#define RATELIMIT_LOG(...) \
	{								\
		static uint64_t __count = 0;				\
		if ((__count % g_quiet_count) == 0) {			\
			if (__count > 0 && g_quiet_count > 1) {		\
				fprintf(stderr, "Message suppressed %" PRIu32 " times: ",	\
					g_quiet_count - 1);		\
			}						\
			fprintf(stderr, __VA_ARGS__);			\
		}							\
		__count++;						\
	}

static bool g_dump_transport_stats;
static pthread_mutex_t g_stats_mutex;

#define MAX_ALLOWED_PCI_DEVICE_NUM 128
static struct spdk_pci_addr g_allowed_pci_addr[MAX_ALLOWED_PCI_DEVICE_NUM];

struct _trid_entry {
	struct spdk_nvme_trid_entry entry;
	TAILQ_ENTRY(_trid_entry) tailq;
};

SPDK_LOG_DEPRECATION_REGISTER(perf_g_option, "perf -G option", "v26.05", 0);

#define MAX_TRID_ENTRY 256
static struct _trid_entry g_trids[MAX_TRID_ENTRY];
static TAILQ_HEAD(, _trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static int g_file_optind; /* Index of first filename in argv */

static inline void task_complete(struct perf_task *task);

static void
perf_set_sock_opts(const char *impl_name, const char *field, uint32_t val, const char *valstr)
{
	struct spdk_sock_impl_opts sock_opts = {};
	size_t opts_size = sizeof(sock_opts);
	int rc;

	rc = spdk_sock_impl_get_opts(impl_name, &sock_opts, &opts_size);
	if (rc < 0) {
		if (rc == -EINVAL) {
			fprintf(stderr, "Unknown sock impl %s\n", impl_name);
		} else {
			fprintf(stderr, "spdk_sock_impl_get_opts() failed, sock impl %s, %d: %s\n", impl_name, rc,
				spdk_strerror(-rc));
		}
		return;
	}

	if (opts_size != sizeof(sock_opts)) {
		fprintf(stderr, "Warning: sock_opts size mismatch. Expected %zu, received %zu\n",
			sizeof(sock_opts), opts_size);
		opts_size = sizeof(sock_opts);
	}

	if (!field) {
		fprintf(stderr, "Warning: no socket opts field specified\n");
		return;
	} else if (strcmp(field, "enable_zerocopy_send_client") == 0) {
		sock_opts.enable_zerocopy_send_client = val;
	} else if (strcmp(field, "tls_version") == 0) {
		sock_opts.tls_version = val;
	} else if (strcmp(field, "ktls") == 0) {
		sock_opts.enable_ktls = val;
	} else if (strcmp(field, "zerocopy_threshold") == 0) {
		sock_opts.zerocopy_threshold = val;
	} else {
		fprintf(stderr, "Warning: invalid or unprocessed socket opts field: %s\n", field);
		return;
	}

	rc = spdk_sock_impl_set_opts(impl_name, &sock_opts, opts_size);
	if (rc < 0) {
		fprintf(stderr, "spdk_sock_impl_set_opts() failed to set %s: %d for sock impl %s, rc %d: %s\n",
			field, val, impl_name, rc, strerror(-rc));
	}
}

static void
nvme_perf_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct iovec *iov;
	struct perf_task *task = (struct perf_task *)ref;

	task->iov_offset = sgl_offset;
	for (task->iovpos = 0; task->iovpos < task->iovcnt; task->iovpos++) {
		iov = &task->iovs[task->iovpos];
		if (task->iov_offset < iov->iov_len) {
			break;
		}

		task->iov_offset -= iov->iov_len;
	}
}

static int
nvme_perf_next_sge(void *ref, void **address, uint32_t *length)
{
	struct iovec *iov;
	struct perf_task *task = (struct perf_task *)ref;

	assert(task->iovpos < task->iovcnt);

	iov = &task->iovs[task->iovpos];
	assert(task->iov_offset <= iov->iov_len);

	*address = iov->iov_base + task->iov_offset;
	*length = iov->iov_len - task->iov_offset;
	task->iovpos++;
	task->iov_offset = 0;

	return 0;
}

static int
nvme_perf_allocate_iovs(struct perf_task *task, void *buf, uint32_t length)
{
	int iovpos = 0;
	struct iovec *iov;
	uint32_t offset = 0;

	task->iovcnt = SPDK_CEIL_DIV(length, (uint64_t)g_io_unit_size);
	task->iovs = calloc(task->iovcnt, sizeof(struct iovec));
	if (!task->iovs) {
		return -1;
	}

	while (length > 0) {
		iov = &task->iovs[iovpos];
		iov->iov_len = spdk_min(length, g_io_unit_size);
		iov->iov_base = buf + offset;
		length -= iov->iov_len;
		offset += iov->iov_len;
		iovpos++;
	}

	return 0;
}

#ifdef SPDK_CONFIG_URING

static void
uring_setup_payload(struct perf_task *task, uint8_t pattern)
{
	struct iovec *iov;

	task->iovs = calloc(1, sizeof(struct iovec));
	if (!task->iovs) {
		fprintf(stderr, "perf task failed to allocate iovs\n");
		exit(1);
	}
	task->iovcnt = 1;

	iov = &task->iovs[0];
	iov->iov_base = spdk_dma_zmalloc(g_io_size_bytes, g_io_align, NULL);
	iov->iov_len = g_io_size_bytes;
	if (iov->iov_base == NULL) {
		fprintf(stderr, "spdk_dma_zmalloc() for task->iovs[0].iov_base failed\n");
		free(task->iovs);
		exit(1);
	}
	memset(iov->iov_base, pattern, iov->iov_len);
}

static int
uring_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
		struct ns_entry *entry, uint64_t offset_in_ios)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&ns_ctx->u.uring.ring);
	if (!sqe) {
		fprintf(stderr, "Cannot get sqe\n");
		return -1;
	}

	if (task->is_read) {
		io_uring_prep_readv(sqe, entry->u.uring.fd, task->iovs, 1, offset_in_ios * task->iovs[0].iov_len);
	} else {
		io_uring_prep_writev(sqe, entry->u.uring.fd, task->iovs, 1, offset_in_ios * task->iovs[0].iov_len);
	}

	io_uring_sqe_set_data(sqe, task);
	ns_ctx->u.uring.io_pending++;

	return 0;
}

static int64_t
uring_check_io(struct ns_worker_ctx *ns_ctx)
{
	int i, to_complete, to_submit, count = 0, ret = 0;
	struct perf_task *task;

	to_submit = ns_ctx->u.uring.io_pending;

	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call spdk_io_uring_enter appropriately. */
		ret = io_uring_submit(&ns_ctx->u.uring.ring);
		if (ret < 0) {
			ns_ctx->status = 1;
			return -1;
		}
		ns_ctx->u.uring.io_pending = 0;
		ns_ctx->u.uring.io_inflight += to_submit;
	}

	to_complete = ns_ctx->u.uring.io_inflight;
	if (to_complete > 0) {
		count = io_uring_peek_batch_cqe(&ns_ctx->u.uring.ring, ns_ctx->u.uring.cqes, to_complete);
		ns_ctx->u.uring.io_inflight -= count;
		for (i = 0; i < count; i++) {
			int res;

			assert(ns_ctx->u.uring.cqes[i] != NULL);
			task = (struct perf_task *)ns_ctx->u.uring.cqes[i]->user_data;
			res = ns_ctx->u.uring.cqes[i]->res;
			if (res != (int)task->iovs[0].iov_len) {
				fprintf(stderr, "cqe->status=%d, iov_len=%d\n", res,
					(int)task->iovs[0].iov_len);
				ns_ctx->status = 1;
				if (res == -EIO) {
					/* The block device has been removed.
					 * Stop trying to send I/O to it.
					 */
					ns_ctx->is_draining = true;
				}
			}
			io_uring_cqe_seen(&ns_ctx->u.uring.ring, ns_ctx->u.uring.cqes[i]);
			task_complete(task);
		}
	}
	return count;
}

static void
uring_verify_io(struct perf_task *task, struct ns_entry *entry)
{
}

static int
uring_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	if (io_uring_queue_init(g_queue_depth, &ns_ctx->u.uring.ring, 0) < 0) {
		SPDK_ERRLOG("uring I/O context setup failure\n");
		return -1;
	}

	ns_ctx->u.uring.cqes = calloc(g_queue_depth, sizeof(struct io_uring_cqe *));
	if (!ns_ctx->u.uring.cqes) {
		io_uring_queue_exit(&ns_ctx->u.uring.ring);
		return -1;
	}

	return 0;
}

static void
uring_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	io_uring_queue_exit(&ns_ctx->u.uring.ring);
	free(ns_ctx->u.uring.cqes);
}

static const struct ns_fn_table uring_fn_table = {
	.setup_payload          = uring_setup_payload,
	.submit_io              = uring_submit_io,
	.check_io               = uring_check_io,
	.verify_io              = uring_verify_io,
	.init_ns_worker_ctx     = uring_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx  = uring_cleanup_ns_worker_ctx,
};

#endif

#ifdef HAVE_LIBAIO
static void
aio_setup_payload(struct perf_task *task, uint8_t pattern)
{
	struct iovec *iov;

	task->iovs = calloc(1, sizeof(struct iovec));
	if (!task->iovs) {
		fprintf(stderr, "perf task failed to allocate iovs\n");
		exit(1);
	}
	task->iovcnt = 1;

	iov = &task->iovs[0];
	iov->iov_base = spdk_dma_zmalloc(g_io_size_bytes, g_io_align, NULL);
	iov->iov_len = g_io_size_bytes;
	if (iov->iov_base == NULL) {
		fprintf(stderr, "spdk_dma_zmalloc() for task->iovs[0].iov_base failed\n");
		free(task->iovs);
		exit(1);
	}
	memset(iov->iov_base, pattern, iov->iov_len);
}

static int
aio_submit(io_context_t aio_ctx, struct iocb *iocb, int fd, enum io_iocb_cmd cmd,
	   struct iovec *iov, uint64_t offset, void *cb_ctx)
{
	iocb->aio_fildes = fd;
	iocb->aio_reqprio = 0;
	iocb->aio_lio_opcode = cmd;
	iocb->u.c.buf = iov->iov_base;
	iocb->u.c.nbytes = iov->iov_len;
	iocb->u.c.offset = offset * iov->iov_len;
	iocb->data = cb_ctx;

	if (io_submit(aio_ctx, 1, &iocb) < 0) {
		printf("io_submit");
		return -1;
	}

	return 0;
}

static int
aio_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
	      struct ns_entry *entry, uint64_t offset_in_ios)
{
	if (task->is_read) {
		return aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PREAD,
				  task->iovs, offset_in_ios, task);
	} else {
		return aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PWRITE,
				  task->iovs, offset_in_ios, task);
	}
}

static int64_t
aio_check_io(struct ns_worker_ctx *ns_ctx)
{
	int count, i;
	struct timespec timeout;
	struct perf_task *task;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	count = io_getevents(ns_ctx->u.aio.ctx, 1, g_queue_depth, ns_ctx->u.aio.events, &timeout);
	if (count < 0) {
		fprintf(stderr, "io_getevents error\n");
		ns_ctx->status = 1;
		return -1;
	}

	for (i = 0; i < count; i++) {
		unsigned long res;

		task = (struct perf_task *)ns_ctx->u.aio.events[i].data;
		res = ns_ctx->u.aio.events[i].res;
		if (res != (uint64_t)task->iovs[0].iov_len) {
			fprintf(stderr, "event->res=%ld, iov_len=%lu\n", (long)res,
				(uint64_t)task->iovs[0].iov_len);
			ns_ctx->status = 1;
			if ((long)res == -EIO) {
				/* The block device has been removed.  Stop trying to send I/O to it. */
				ns_ctx->is_draining = true;
			}
		}
		task_complete(ns_ctx->u.aio.events[i].data);
	}
	return count;
}

static void
aio_verify_io(struct perf_task *task, struct ns_entry *entry)
{
}

static int
aio_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->u.aio.events = calloc(g_queue_depth, sizeof(struct io_event));
	if (!ns_ctx->u.aio.events) {
		return -1;
	}
	ns_ctx->u.aio.ctx = 0;
	if (io_setup(g_queue_depth, &ns_ctx->u.aio.ctx) < 0) {
		free(ns_ctx->u.aio.events);
		perror("io_setup");
		return -1;
	}
	return 0;
}

static void
aio_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	io_destroy(ns_ctx->u.aio.ctx);
	free(ns_ctx->u.aio.events);
}

static const struct ns_fn_table aio_fn_table = {
	.setup_payload		= aio_setup_payload,
	.submit_io		= aio_submit_io,
	.check_io		= aio_check_io,
	.verify_io		= aio_verify_io,
	.init_ns_worker_ctx	= aio_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx	= aio_cleanup_ns_worker_ctx,
};

#endif /* HAVE_LIBAIO */

#if defined(HAVE_LIBAIO) || defined(SPDK_CONFIG_URING)

static int
register_file(const char *path)
{
	struct ns_entry *entry;

	int flags, fd;
	uint64_t size;
	uint32_t blklen;

	if (g_rw_percentage == 100) {
		flags = O_RDONLY;
	} else if (g_rw_percentage == 0) {
		flags = O_WRONLY;
	} else {
		flags = O_RDWR;
	}

	flags |= O_DIRECT;

	fd = open(path, flags);
	if (fd < 0) {
		fprintf(stderr, "Could not open device %s: %s\n", path, strerror(errno));
		return -1;
	}

	size = spdk_fd_get_size(fd);
	if (size == 0) {
		fprintf(stderr, "Could not determine size of device %s\n", path);
		close(fd);
		return -1;
	}

	blklen = spdk_fd_get_blocklen(fd);
	if (blklen == 0) {
		fprintf(stderr, "Could not determine block size of device %s\n", path);
		close(fd);
		return -1;
	}

	/*
	 * TODO: This should really calculate the LCM of the current g_io_align and blklen.
	 * For now, it's fairly safe to just assume all block sizes are powers of 2.
	 */
	if (g_io_align < blklen) {
		if (g_io_align_specified) {
			fprintf(stderr, "Wrong IO alignment (%u). aio requires block-sized alignment (%u)\n", g_io_align,
				blklen);
			close(fd);
			return -1;
		}

		g_io_align = blklen;
	}

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		close(fd);
		perror("ns_entry malloc");
		return -1;
	}

	if (g_use_uring) {
#ifdef SPDK_CONFIG_URING
		entry->type = ENTRY_TYPE_URING_FILE;
		entry->fn_table = &uring_fn_table;
		entry->u.uring.fd = fd;
#endif
	} else {
#if HAVE_LIBAIO
		entry->type = ENTRY_TYPE_AIO_FILE;
		entry->fn_table = &aio_fn_table;
		entry->u.aio.fd = fd;
#endif
	}
	entry->size_in_ios = size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / blklen;

	if (g_is_random) {
		if (g_zipf_theta > 0) {
			entry->zipf = spdk_zipf_create(entry->size_in_ios, g_zipf_theta, 0);
		}
	}

	snprintf(entry->name, sizeof(entry->name), "%s", path);

	g_num_namespaces++;
	g_num_async_devs++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	return 0;
}

static int
register_files(int argc, char **argv)
{
	int i;

	/* Treat everything after the options as files for AIO/URING */
	for (i = g_file_optind; i < argc; i++) {
		if (register_file(argv[i]) != 0) {
			return 1;
		}
	}

	return 0;
}
#endif

static void io_complete(void *ctx, const struct spdk_nvme_cpl *cpl);

static void
nvme_setup_payload(struct perf_task *task, uint8_t pattern)
{
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t max_io_size_bytes, max_io_md_size;
	int32_t numa_id;
	void *buf;
	int rc;

	ctrlr = task->ns_ctx->entry->u.nvme.ctrlr;
	numa_id = spdk_nvme_ctrlr_get_numa_id(ctrlr);

	/* maximum extended lba format size from all active namespace,
	 * it's same with g_io_size_bytes for namespace without metadata.
	 */
	max_io_size_bytes = g_io_size_bytes + g_max_io_md_size * g_max_io_size_blocks;
	buf = spdk_dma_zmalloc_socket(max_io_size_bytes, g_io_align, NULL, numa_id);
	if (buf == NULL) {
		fprintf(stderr, "task->buf spdk_dma_zmalloc failed\n");
		exit(1);
	}
	memset(buf, pattern, max_io_size_bytes);

	rc = nvme_perf_allocate_iovs(task, buf, max_io_size_bytes);
	if (rc < 0) {
		fprintf(stderr, "perf task failed to allocate iovs\n");
		spdk_dma_free(buf);
		exit(1);
	}

	max_io_md_size = g_max_io_md_size * g_max_io_size_blocks;
	if (max_io_md_size != 0) {
		task->md_iov.iov_base = spdk_dma_zmalloc(max_io_md_size, g_io_align, NULL);
		task->md_iov.iov_len = max_io_md_size;
		if (task->md_iov.iov_base == NULL) {
			fprintf(stderr, "task->md_buf spdk_dma_zmalloc failed\n");
			spdk_dma_free(task->iovs[0].iov_base);
			free(task->iovs);
			exit(1);
		}
	}
}

static int
nvme_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
	       struct ns_entry *entry, uint64_t offset_in_ios)
{
	uint64_t lba;
	int rc;
	int qp_num;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	enum dif_mode {
		DIF_MODE_NONE = 0,
		DIF_MODE_DIF = 1,
		DIF_MODE_DIX = 2,
	}  mode = DIF_MODE_NONE;

	lba = offset_in_ios * entry->io_size_blocks;

	if (entry->md_size != 0 && !(entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT)) {
		if (entry->md_interleave) {
			mode = DIF_MODE_DIF;
		} else {
			mode = DIF_MODE_DIX;
		}
	}

	qp_num = ns_ctx->u.nvme.last_qpair;
	ns_ctx->u.nvme.last_qpair++;
	if (ns_ctx->u.nvme.last_qpair == ns_ctx->u.nvme.num_active_qpairs) {
		ns_ctx->u.nvme.last_qpair = 0;
	}

	if (mode != DIF_MODE_NONE) {
		dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
		dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
		rc = spdk_dif_ctx_init(&task->dif_ctx, entry->block_size, entry->md_size,
				       entry->md_interleave, entry->pi_loc,
				       (enum spdk_dif_type)entry->pi_type, entry->io_flags,
				       lba, 0xFFFF, (uint16_t)entry->io_size_blocks, 0, 0, &dif_opts);
		if (rc != 0) {
			fprintf(stderr, "Initialization of DIF context failed\n");
			exit(1);
		}
	}

	if (task->is_read) {
		if (task->iovcnt == 1) {
			return spdk_nvme_ns_cmd_read_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							     task->iovs[0].iov_base, task->md_iov.iov_base,
							     lba,
							     entry->io_size_blocks, io_complete,
							     task, entry->io_flags,
							     task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		} else {
			return spdk_nvme_ns_cmd_readv_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							      lba, entry->io_size_blocks,
							      io_complete, task, entry->io_flags,
							      nvme_perf_reset_sgl, nvme_perf_next_sge,
							      task->md_iov.iov_base,
							      task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		}
	} else {
		switch (mode) {
		case DIF_MODE_DIF:
			rc = spdk_dif_generate(task->iovs, task->iovcnt, entry->io_size_blocks, &task->dif_ctx);
			if (rc != 0) {
				fprintf(stderr, "Generation of DIF failed\n");
				return rc;
			}
			break;
		case DIF_MODE_DIX:
			rc = spdk_dix_generate(task->iovs, task->iovcnt, &task->md_iov, entry->io_size_blocks,
					       &task->dif_ctx);
			if (rc != 0) {
				fprintf(stderr, "Generation of DIX failed\n");
				return rc;
			}
			break;
		default:
			break;
		}

		if (task->iovcnt == 1) {
			return spdk_nvme_ns_cmd_write_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							      task->iovs[0].iov_base, task->md_iov.iov_base,
							      lba,
							      entry->io_size_blocks, io_complete,
							      task, entry->io_flags,
							      task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		} else {
			return spdk_nvme_ns_cmd_writev_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							       lba, entry->io_size_blocks,
							       io_complete, task, entry->io_flags,
							       nvme_perf_reset_sgl, nvme_perf_next_sge,
							       task->md_iov.iov_base,
							       task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		}
	}
}

static void
perf_disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{
	struct ns_worker_ctx *ns_ctx = ctx;

	ns_ctx->is_draining = true;
	ns_ctx->status = 1;
}

/* SPDK_PERF_WORKER_GROUP: the group's ctx is the worker; mark the ns_ctx that owns the qpair.
 * Marks only: qpairs are freed by cleanup after the drain, never from a completion context. */
static void
perf_worker_disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{
	struct worker_thread *worker = ctx;
	struct ns_worker_ctx *ns_ctx;
	int i;

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (ns_ctx->entry->type != ENTRY_TYPE_NVME_NS || ns_ctx->u.nvme.qpair == NULL) {
			continue;
		}
		for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
			if (ns_ctx->u.nvme.qpair[i] == qpair) {
				ns_ctx->is_draining = true;
				ns_ctx->status = 1;
				return;
			}
		}
	}
}

static int64_t
nvme_check_io(struct ns_worker_ctx *ns_ctx)
{
	int64_t rc;

	if (g_enable_interrupt) {
		rc = spdk_nvme_poll_group_wait(ns_ctx->u.nvme.group, perf_disconnect_cb);
	} else {
		rc = spdk_nvme_poll_group_process_completions(ns_ctx->u.nvme.group, g_max_completions,
				perf_disconnect_cb);
	}
	if (rc < 0) {
		fprintf(stderr, "NVMe io qpair process completion error\n");
		ns_ctx->status = 1;
		return -1;
	}
	return rc;
}

static void
nvme_verify_io(struct perf_task *task, struct ns_entry *entry)
{
	struct spdk_dif_error err_blk = {};
	int rc;

	if (!task->is_read || (entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT)) {
		return;
	}

	if (entry->md_interleave) {
		rc = spdk_dif_verify(task->iovs, task->iovcnt, entry->io_size_blocks, &task->dif_ctx,
				     &err_blk);
		if (rc != 0) {
			fprintf(stderr, "DIF error detected. type=%d, offset=%" PRIu32 "\n",
				err_blk.err_type, err_blk.err_offset);
		}
	} else {
		rc = spdk_dix_verify(task->iovs, task->iovcnt, &task->md_iov, entry->io_size_blocks,
				     &task->dif_ctx, &err_blk);
		if (rc != 0) {
			fprintf(stderr, "DIX error detected. type=%d, offset=%" PRIu32 "\n",
				err_blk.err_type, err_blk.err_offset);
		}
	}
}

/* SPDK_PERF_WORKER_GROUP: drop ns_ctx's reference on its worker's group after ns_ctx freed its qpairs; the
 * worker's last ns_ctx destroys the group (spdk_nvme_poll_group_destroy is -EBUSY while any member remains). */
static void
perf_worker_group_put(struct ns_worker_ctx *ns_ctx)
{
	struct worker_thread *w = ns_ctx->worker;
	int rc;

	ns_ctx->u.nvme.group = NULL;
	if (--w->group_ctx != 0) {
		return;
	}
	rc = spdk_nvme_poll_group_destroy(w->group);
	if (rc != 0) {
		fprintf(stderr, "PERF_WORKER_GROUP lcore=%u group destroy failed: %d\n", w->lcore, rc);
		return;
	}
	w->group = NULL;
}

/*
 * TODO: If a controller has multiple namespaces, they could all use the same queue.
 *  For now, give each namespace/thread combination its own queue.
 */
static int
nvme_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	const struct spdk_nvme_ctrlr_opts *ctrlr_opts;
	struct spdk_nvme_io_qpair_opts opts;
	struct ns_entry *entry = ns_ctx->entry;
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_qpair *qpair;
	uint64_t poll_timeout_tsc;
	int i, rc;
	bool ordered;
	uint32_t worker_idx;

	ns_ctx->u.nvme.num_active_qpairs = g_nr_io_queues_per_ns;
	ns_ctx->u.nvme.num_all_qpairs = g_nr_io_queues_per_ns + g_nr_unused_io_queues;
	ns_ctx->u.nvme.qpair = calloc(ns_ctx->u.nvme.num_all_qpairs, sizeof(struct spdk_nvme_qpair *));
	if (!ns_ctx->u.nvme.qpair) {
		return -1;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(entry->u.nvme.ctrlr, &opts, sizeof(opts));
	/* Use the exact number of requests needed for the queue depth to reduce memory usage */
	opts.io_queue_requests = entry->num_io_requests;

	opts.delay_cmd_submit = !g_enable_interrupt;
	opts.create_only = true;

	ctrlr_opts = spdk_nvme_ctrlr_get_opts(entry->u.nvme.ctrlr);
	opts.async_mode = !(spdk_nvme_ctrlr_get_transport_id(entry->u.nvme.ctrlr)->trtype ==
			    SPDK_NVME_TRANSPORT_PCIE
			    && ns_ctx->u.nvme.num_all_qpairs > ctrlr_opts->admin_queue_size);

	if (g_worker_group) {
		struct worker_thread *w = ns_ctx->worker;

		if (w->group == NULL) {
			w->group = spdk_nvme_poll_group_create(w, NULL);
			if (w->group == NULL) {
				goto poll_group_failed;
			}
			printf("PERF_WORKER_GROUP lcore=%u group=created\n", w->lcore);
		}
		w->group_ctx++;
		ns_ctx->u.nvme.group = w->group;
	} else {
		ns_ctx->u.nvme.group = spdk_nvme_poll_group_create(ns_ctx, NULL);
		if (ns_ctx->u.nvme.group == NULL) {
			goto poll_group_failed;
		}
	}

	group = ns_ctx->u.nvme.group;
	{
		const char *ord = getenv("PERF_QPAIR_ORDERED");
		struct worker_thread *w;
		uint32_t my_idx = 0, nctx = 0;
		bool found = false;
		struct ns_worker_ctx *c;

		ordered = (ord != NULL && ord[0] != '\0' && ord[0] != '0');
		if (ordered) {
			TAILQ_FOREACH(w, &g_workers, link) {
				if (w->lcore == spdk_env_get_current_core()) {
					found = true;
					my_idx = w->idx;
					TAILQ_FOREACH(c, &w->ns_ctx, link) {
						nctx++;
					}
					break;
				}
			}
			if (!found || nctx != 1) {
				static bool warned;

				if (!__atomic_exchange_n(&warned, true, __ATOMIC_RELAXED)) {
					printf("WARNING: PERF_QPAIR_ORDERED needs exactly one ns_ctx per worker; unordered\n");
				}
				ordered = false;
			}
		}
		worker_idx = my_idx;
	}
	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		bool timed_out = false;

		if (ordered) {
			uint64_t t_end = spdk_get_ticks() + 10 * spdk_get_ticks_hz();
			uint32_t want = (uint32_t)i * g_num_workers + worker_idx;

			while (__atomic_load_n(&g_qpair_turn, __ATOMIC_ACQUIRE) != want) {
				if (spdk_get_ticks() > t_end) {
					printf("WARNING: PERF_QPAIR_ORDERED turn timeout\n");
					timed_out = true;
					break;
				}
				spdk_pause();
			}
		}
		ns_ctx->u.nvme.qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(entry->u.nvme.ctrlr, &opts,
					  sizeof(opts));
		if (ordered && !timed_out) {
			__atomic_fetch_add(&g_qpair_turn, 1, __ATOMIC_RELEASE);
		}
		qpair = ns_ctx->u.nvme.qpair[i];
		if (!qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			goto qpair_failed;
		}

		if (spdk_nvme_poll_group_add(group, qpair)) {
			printf("ERROR: unable to add I/O qpair to poll group.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}

		if (spdk_nvme_ctrlr_connect_io_qpair(entry->u.nvme.ctrlr, qpair)) {
			printf("ERROR: unable to connect I/O qpair.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}
		printf("QPAIR_MAP lcore=%u qid=%u\n", spdk_env_get_current_core(),
		       (unsigned)spdk_nvme_qpair_get_id(qpair));
	}

	/* Busy poll here until all qpairs are connected - this ensures once we start
	 * I/O we aren't still waiting for some qpairs to connect. Limit the poll to
	 * 10 seconds though.
	 */
	poll_timeout_tsc = spdk_get_ticks() + 10 * spdk_get_ticks_hz();
	rc = -EAGAIN;
	while (spdk_get_ticks() < poll_timeout_tsc && rc == -EAGAIN) {
		spdk_nvme_poll_group_process_completions(group, 0,
				g_worker_group ? perf_worker_disconnect_cb : perf_disconnect_cb);
		rc = spdk_nvme_poll_group_all_connected(group);
		if (rc == 0) {
			return 0;
		}
	}

	/* If we reach here, it means we either timed out, or some connection failed. */
	assert(spdk_get_ticks() > poll_timeout_tsc || rc == -EIO);

qpair_failed:
	for (; i > 0; --i) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i - 1]);
	}

	if (g_worker_group) {
		perf_worker_group_put(ns_ctx);
	} else {
		spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
	}
poll_group_failed:
	free(ns_ctx->u.nvme.qpair);
	return -1;
}

static void
nvme_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	int i;

	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i]);
	}

	if (g_worker_group) {
		perf_worker_group_put(ns_ctx);
	} else {
		spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
	}
	free(ns_ctx->u.nvme.qpair);
}

static void
nvme_dump_rdma_statistics(struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_rdma_device_stat *device_stats;
	uint32_t i;

	printf("RDMA transport:\n");
	for (i = 0; i < stat->rdma.num_devices; i++) {
		device_stats = &stat->rdma.device_stats[i];
		printf("\tdev name:              %s\n", device_stats->name);
		printf("\tpolls:                 %"PRIu64"\n", device_stats->polls);
		printf("\tidle_polls:            %"PRIu64"\n", device_stats->idle_polls);
		printf("\tcompletions:           %"PRIu64"\n", device_stats->completions);
		printf("\tqueued_requests:       %"PRIu64"\n", device_stats->queued_requests);
		printf("\ttotal_send_wrs:        %"PRIu64"\n", device_stats->total_send_wrs);
		printf("\tsend_doorbell_updates: %"PRIu64"\n", device_stats->send_doorbell_updates);
		printf("\ttotal_recv_wrs:        %"PRIu64"\n", device_stats->total_recv_wrs);
		printf("\trecv_doorbell_updates: %"PRIu64"\n", device_stats->recv_doorbell_updates);
		printf("\t---------------------------------\n");
	}
}

static void
nvme_dump_pcie_statistics(struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_pcie_stat *pcie_stat;
#ifndef SPDK_NVME_IO_CYCLE_OFF
	uint32_t i;
#endif

	pcie_stat = &stat->pcie;

	printf("PCIE transport:\n");
	printf("\tpolls:               %"PRIu64"\n", pcie_stat->polls);
	printf("\tidle_polls:          %"PRIu64"\n", pcie_stat->idle_polls);
	printf("\tcompletions:         %"PRIu64"\n", pcie_stat->completions);
#ifndef SPDK_NVME_IO_CYCLE_OFF
	printf("\tcompletion_calls:    %"PRIu64"\n", pcie_stat->completion_calls);
#endif
	printf("\tcq_mmio_doorbell_updates: %"PRIu64"\n", pcie_stat->cq_mmio_doorbell_updates);
	printf("\tcq_shadow_doorbell_updates: %"PRIu64"\n", pcie_stat->cq_shadow_doorbell_updates);
	printf("\tsubmitted_requests:  %"PRIu64"\n", pcie_stat->submitted_requests);
	printf("\tsq_mmio_doorbell_updates:  %"PRIu64"\n", pcie_stat->sq_mmio_doorbell_updates);
	printf("\tsq_shadow_doorbell_updates:  %"PRIu64"\n", pcie_stat->sq_shadow_doorbell_updates);
	printf("\tqueued_requests:     %"PRIu64"\n", pcie_stat->queued_requests);
#ifndef SPDK_NVME_IO_CYCLE_OFF
	printf("\tcompletions_per_poll_hist: ");
	for (i = 0; i < 33; i++) {
		if (i == 32) {
			printf("%u+:%"PRIu64, i, pcie_stat->completion_calls_hist[i]);
		} else {
			printf("%u:%"PRIu64, i, pcie_stat->completion_calls_hist[i]);
		}
		if (i + 1 < 33) {
			printf(", ");
		}
	}
	printf("\n");
#endif
	printf("\tpcie_stats: polls=%"PRIu64" completions=%"PRIu64" sq_mmio=%"PRIu64" cq_mmio=%"PRIu64"\n",
	       pcie_stat->polls, pcie_stat->completions,
	       pcie_stat->sq_mmio_doorbell_updates, pcie_stat->cq_mmio_doorbell_updates);
}

static void
nvme_dump_tcp_statistics(struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_tcp_stat *tcp_stat;

	tcp_stat = &stat->tcp;

	printf("TCP transport:\n");
	printf("\tpolls:              %"PRIu64"\n", tcp_stat->polls);
	printf("\tidle_polls:         %"PRIu64"\n", tcp_stat->idle_polls);
	printf("\tsock_completions:   %"PRIu64"\n", tcp_stat->socket_completions);
	printf("\tnvme_completions:   %"PRIu64"\n", tcp_stat->nvme_completions);
	printf("\tsubmitted_requests: %"PRIu64"\n", tcp_stat->submitted_requests);
	printf("\tqueued_requests:    %"PRIu64"\n", tcp_stat->queued_requests);
}

static void
nvme_dump_transport_stats(uint32_t lcore, struct ns_worker_ctx *ns_ctx)
{
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_poll_group_stat *stat = NULL;
	uint32_t i;
	int rc;

	group = ns_ctx->u.nvme.group;
	if (group == NULL) {
		return;
	}

	rc = spdk_nvme_poll_group_get_stats(group, &stat);
	if (rc) {
		fprintf(stderr, "Can't get transport stats, error %d\n", rc);
		return;
	}

	printf("\n====================\n");
	printf("lcore %u, ns %s statistics:\n", lcore, ns_ctx->entry->name);

	/* WG-SYNC:transport-switch begin */
	for (i = 0; i < stat->num_transports; i++) {
		switch (stat->transport_stat[i]->trtype) {
		case SPDK_NVME_TRANSPORT_RDMA:
			nvme_dump_rdma_statistics(stat->transport_stat[i]);
			break;
		case SPDK_NVME_TRANSPORT_PCIE:
			nvme_dump_pcie_statistics(stat->transport_stat[i]);
			break;
		case SPDK_NVME_TRANSPORT_TCP:
			nvme_dump_tcp_statistics(stat->transport_stat[i]);
			break;
		default:
			fprintf(stderr, "Unknown transport statistics %d %s\n", stat->transport_stat[i]->trtype,
				spdk_nvme_transport_id_trtype_str(stat->transport_stat[i]->trtype));
		}
	}
	/* WG-SYNC:transport-switch end */

	spdk_nvme_poll_group_free_stats(group, stat);
}

static const struct ns_fn_table nvme_fn_table = {
	.setup_payload		= nvme_setup_payload,
	.submit_io		= nvme_submit_io,
	.check_io		= nvme_check_io,
	.verify_io		= nvme_verify_io,
	.init_ns_worker_ctx	= nvme_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx	= nvme_cleanup_ns_worker_ctx,
	.dump_transport_stats	= nvme_dump_transport_stats
};

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t max_xfer_size, entries, sector_size;
	uint64_t ns_size;
	struct spdk_nvme_io_qpair_opts opts;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		g_warn = true;
		return;
	}

	ns_size = spdk_nvme_ns_get_size(ns);
	sector_size = spdk_nvme_ns_get_sector_size(ns);

	if (ns_size < g_io_size_bytes || sector_size > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       ns_size, spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		g_warn = true;
		return;
	}

	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	/* NVMe driver may add additional entries based on
	 * stripe size and maximum transfer size, we assume
	 * 1 more entry be used for stripe.
	 */
	entries = (g_io_size_bytes - 1) / max_xfer_size + 2;
	if ((g_queue_depth * entries) > opts.io_queue_size) {
		printf("Controller IO queue size %u, less than required.\n",
		       opts.io_queue_size);
		printf("Consider using lower queue depth or smaller IO size, because "
		       "IO requests may be queued at the NVMe driver.\n");
	}
	/* For requests which have children requests, parent request itself
	 * will also occupy 1 entry.
	 */
	entries += 1;

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->type = ENTRY_TYPE_NVME_NS;
	entry->fn_table = &nvme_fn_table;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;
	entry->num_io_requests = entries * spdk_divide_round_up(g_queue_depth, g_nr_io_queues_per_ns);

	entry->size_in_ios = ns_size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / sector_size;

	if (g_is_random) {
		if (g_zipf_theta > 0) {
			entry->zipf = spdk_zipf_create(entry->size_in_ios, g_zipf_theta, 0);
		}
	}

	entry->block_size = spdk_nvme_ns_get_extended_sector_size(ns);
	entry->md_size = spdk_nvme_ns_get_md_size(ns);
	entry->md_interleave = spdk_nvme_ns_supports_extended_lba(ns);
	entry->pi_loc = spdk_nvme_ns_get_data(ns)->dps.md_start;
	entry->pi_type = spdk_nvme_ns_get_pi_type(ns);

	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		entry->io_flags = g_metacfg_pract_flag | g_metacfg_prchk_flags;
	}

	/* If metadata size = 8 bytes, PI is stripped (read) or inserted (write),
	 *  and so reduce metadata size from block size.  (If metadata size > 8 bytes,
	 *  PI is passed (read) or replaced (write).  So block size is not necessary
	 *  to change.)
	 */
	if ((entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT) && (entry->md_size == 8)) {
		entry->block_size = spdk_nvme_ns_get_sector_size(ns);
	}

	if (g_io_size_bytes % entry->block_size != 0) {
		printf("WARNING: IO size %u (-o) is not a multiple of nsid %u sector size %u."
		       " Removing this ns from test\n", g_io_size_bytes, spdk_nvme_ns_get_id(ns), entry->block_size);
		g_warn = true;
		spdk_zipf_free(&entry->zipf);
		free(entry);
		return;
	}

	if (g_max_io_md_size < entry->md_size) {
		g_max_io_md_size = entry->md_size;
	}

	if (g_max_io_size_blocks < entry->io_size_blocks) {
		g_max_io_size_blocks = entry->io_size_blocks;
	}

	spdk_nvme_build_name(entry->name, sizeof(entry->name), ctrlr, ns);

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_namespaces, link, tmp) {
		TAILQ_REMOVE(&g_namespaces, entry, link);
		spdk_zipf_free(&entry->zipf);
		if (g_use_uring) {
#ifdef SPDK_CONFIG_URING
			close(entry->u.uring.fd);
#endif
		} else {
#if HAVE_LIBAIO
			close(entry->u.aio.fd);
#endif
		}
		free(entry);
	}
}

static void
enable_latency_tracking_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("enable_latency_tracking_complete failed\n");
	}
	g_outstanding_commands--;
}

static void
set_latency_tracking_feature(struct spdk_nvme_ctrlr *ctrlr, bool enable)
{
	int res;
	union spdk_nvme_intel_feat_latency_tracking latency_tracking;

	if (enable) {
		latency_tracking.bits.enable = 0x01;
	} else {
		latency_tracking.bits.enable = 0x00;
	}

	res = spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING,
					      latency_tracking.raw, 0, NULL, 0, enable_latency_tracking_complete, NULL);
	if (res) {
		printf("fail to allocate nvme request.\n");
		return;
	}
	g_outstanding_commands++;

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_trid_entry *trid_entry)
{
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	uint32_t nsid;

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	entry->latency_page = spdk_dma_zmalloc(sizeof(struct spdk_nvme_intel_rw_latency_page),
					       4096, NULL);
	if (entry->latency_page == NULL) {
		printf("Allocation error (latency page)\n");
		exit(1);
	}

	spdk_nvme_build_name(entry->name, sizeof(entry->name), ctrlr, NULL);
	printf("Attached to NVMe%s Controller at %s\n",
	       trid_entry->trid.trtype != SPDK_NVME_TRANSPORT_PCIE ? "oF" : "", entry->name);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	if (g_latency_ssd_tracking_enable &&
	    spdk_nvme_ctrlr_is_feature_supported(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
		set_latency_tracking_feature(ctrlr, true);
	}

	if (trid_entry->nsid == 0) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (ns == NULL) {
				continue;
			}
			register_ns(ctrlr, ns);
		}
	} else {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, trid_entry->nsid);
		if (!ns) {
			perror("Namespace does not exist.");
			exit(1);
		}

		register_ns(ctrlr, ns);
	}
}

static __thread uint64_t seed = 0;

/* SPDK_PERF_TRACE_FAST: precompute (offset_in_ios << 1 | is_read) for exactly g_trace_count trace
 * entries so the per-IO path does no 64-bit division; the cursor wraps at g_trace_count like stock.
 * Cold path, built on the first IO of each ns_ctx. */
static __attribute__((noinline)) void
perf_trace_fast_init(struct ns_worker_ctx *ns_ctx, struct ns_entry *entry)
{
	uint64_t j;

	ns_ctx->trace_fast_tbl = malloc(g_trace_count * sizeof(uint64_t));
	if (ns_ctx->trace_fast_tbl == NULL) {
		fprintf(stderr, "PERF_TRACE_FAST: trace table allocation failed\n");
		exit(1);
	}
	for (j = 0; j < g_trace_count; j++) {
		const struct perf_trace_entry *e = &g_trace_entries[j];
		uint64_t off = e->offset / g_io_size_bytes;

		if (off >= entry->size_in_ios) {
			off = off % entry->size_in_ios;
		}
		ns_ctx->trace_fast_tbl[j] = (off << 1) | (uint64_t)(e->op == 'R' || e->op == 'r');
	}
	ns_ctx->trace_cursor %= g_trace_count;
	fprintf(stderr, "PERF_TRACE_FAST entries=%" PRIu64 "\n", g_trace_count);
}

static inline void
submit_single_io(struct perf_task *task)
{
	uint64_t		rand_value, offset_in_ios;
	int			rc;
	struct ns_worker_ctx	*ns_ctx = task->ns_ctx;
	struct ns_entry		*entry = ns_ctx->entry;

	assert(!ns_ctx->is_draining);

	if (g_trace_entries) {
		/* Trace replay: pull next entry, wraparound on exhaustion.
		 * The trace's offset/size are in bytes; offset_in_ios is in
		 * units of g_io_size_bytes. Our trace is all 4 KB-aligned, so
		 * dividing by g_io_size_bytes is exact when -o 4096. */
		/* Per-worker cursor: no cross-core atomic on the submit path (realcore T1.3).
		 * Workers start at evenly spaced offsets and wrap independently. */
		if (spdk_unlikely(g_perf_trace_fast)) {
			uint64_t v, c;

			if (spdk_unlikely(ns_ctx->trace_fast_tbl == NULL)) {
				perf_trace_fast_init(ns_ctx, entry);
			}
			c = ns_ctx->trace_cursor;
			v = ns_ctx->trace_fast_tbl[c];
			ns_ctx->trace_cursor = (c + 1 == g_trace_count) ? 0 : c + 1;
			offset_in_ios = v >> 1;
			task->is_read = (v & 1) != 0;
			if (spdk_unlikely(g_perf_trace_fast_check)) {
				uint64_t i = ns_ctx->trace_check_cursor++ % g_trace_count;
				uint64_t want = g_trace_entries[i].offset / g_io_size_bytes;
				bool want_rd = (g_trace_entries[i].op == 'R' || g_trace_entries[i].op == 'r');

				if (want >= entry->size_in_ios) {
					want %= entry->size_in_ios;
				}
				if (want != offset_in_ios || want_rd != task->is_read) {
					fprintf(stderr, "PERF_TRACE_FAST mismatch at entry %" PRIu64 ": want=%" PRIu64 " got=%" PRIu64
						" want_rd=%d got_rd=%d\n", i, want, offset_in_ios,
						(int)want_rd, (int)task->is_read);
					abort();
				}
			}
		} else {
			uint64_t i = ns_ctx->trace_cursor++ % g_trace_count;
			offset_in_ios = g_trace_entries[i].offset / g_io_size_bytes;
			if (spdk_unlikely(offset_in_ios >= entry->size_in_ios)) {
				offset_in_ios = offset_in_ios % entry->size_in_ios;
			}
			task->is_read = (g_trace_entries[i].op == 'R' || g_trace_entries[i].op == 'r');
		}
	} else if (entry->zipf) {
		offset_in_ios = spdk_zipf_generate(entry->zipf);
	} else if (g_is_random) {
		rand_value = spdk_rand_xorshift64(&seed);
		offset_in_ios = rand_value % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	task->submit_tsc = spdk_get_ticks();

	if (!g_trace_entries) {
		if ((g_rw_percentage == 100) ||
		    (g_rw_percentage != 0 &&
		     ((spdk_rand_xorshift64(&seed) % 100) < (uint64_t)g_rw_percentage))) {
			task->is_read = true;
		} else {
			task->is_read = false;
		}
	}
	/* In trace-replay mode, task->is_read was set above from the trace entry. */

	rc = entry->fn_table->submit_io(task, ns_ctx, entry, offset_in_ios);

	if (spdk_unlikely(rc != 0)) {
		if (g_continue_on_error) {
			/* We can't just resubmit here or we can get in a loop that
			 * stack overflows. */
			TAILQ_INSERT_TAIL(&ns_ctx->queued_tasks, task, link);
		} else {
			RATELIMIT_LOG("starting I/O failed: %d\n", rc);
			spdk_dma_free(task->iovs[0].iov_base);
			free(task->iovs);
			spdk_dma_free(task->md_iov.iov_base);
			task->ns_ctx->status = 1;
			free(task);
		}
	} else {
		ns_ctx->current_queue_depth++;
		ns_ctx->stats.io_submitted++;
	}

	if (spdk_unlikely(ns_ctx->number_ios && ns_ctx->stats.io_submitted >= ns_ctx->number_ios)) {
		ns_ctx->is_draining = true;
	}
}

static void
ann_compute_init(void)
{
	const char *e = getenv("SPDK_COMPUTE_PER_IO");
	int i;

	int per_io = (e != NULL) ? atoi(e) : 0;

	if (per_io < 0) {
		per_io = 0;
	}
	for (i = 0; i < ANN_DIM; i++) {
		g_ann_query[i] = (float)((unsigned)(i * 2654435761u) & 0xFFFFu) / 65536.0f;
	}
	g_compute_per_io = per_io;
	printf("PHASE1_TRACE_INFO: SPDK_COMPUTE_PER_IO=%d (co-resident distance kernels/IO)\n",
	       per_io);
}

/* Cold: first kernel call on a thread allocates its 64 B-aligned block and registers it. */
static struct ann_tls *
ann_tls_get(void)
{
	struct ann_tls *a = NULL;
	uint32_t idx;

	if (posix_memalign((void **)&a, 64, sizeof(*a)) != 0) {
		fprintf(stderr, "ann_tls alloc failed\n");
		exit(1);
	}
	memset(a, 0, sizeof(*a));
	idx = __atomic_fetch_add(&g_ann_tls_n, 1, __ATOMIC_ACQ_REL);
	if (idx >= ANN_MAX_THREADS) {
		fprintf(stderr, "ann_tls: more than %d threads\n", ANN_MAX_THREADS);
		exit(1);
	}
	g_ann_tls[idx] = a;
	/* publish: the count above may be read before the slot is stored only by the end-of-run sum, after join */
	t_ann = a;
	return a;
}

static inline void
ann_compute_kernel(struct perf_task *task)
{
	const float *base;
	uint32_t nvec;
	double acc = 0.0;
	struct ann_tls *a;
	int r, i;

	if (spdk_unlikely(g_compute_per_io < 0)) {
		ann_compute_init();
	}
	if (g_compute_per_io == 0) {
		return;
	}
	base = (const float *)task->iovs[0].iov_base;
	if (spdk_unlikely(base == NULL)) {
		return;
	}
	nvec = (uint32_t)(task->iovs[0].iov_len / (ANN_DIM * sizeof(float)));
	if (nvec == 0) {
		nvec = 1;
	}
	for (r = 0; r < g_compute_per_io; r++) {
		const float *v = base + (uint32_t)(r % nvec) * ANN_DIM;
		double d = 0.0;
		for (i = 0; i < ANN_DIM; i++) {
			double diff = (double)v[i] - (double)g_ann_query[i];
			d += diff * diff;
		}
		acc += d;
	}
	a = t_ann;
	if (spdk_unlikely(a == NULL)) {
		a = ann_tls_get();
	}
	a->sink += acc;
	a->ops += (uint64_t)g_compute_per_io;
	a->calls++;
}

static inline void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx;
	uint64_t		tsc_diff;
	struct ns_entry		*entry;

	ns_ctx = task->ns_ctx;
	entry = ns_ctx->entry;
	ns_ctx->current_queue_depth--;
	ns_ctx->stats.io_completed++;
	tsc_diff = spdk_get_ticks() - task->submit_tsc;
	ns_ctx->stats.total_tsc += tsc_diff;
	if (spdk_unlikely(ns_ctx->stats.min_tsc > tsc_diff)) {
		ns_ctx->stats.min_tsc = tsc_diff;
	}
	if (spdk_unlikely(ns_ctx->stats.max_tsc < tsc_diff)) {
		ns_ctx->stats.max_tsc = tsc_diff;
	}
	if (spdk_unlikely(g_latency_sw_tracking_level > 0)) {
		spdk_histogram_data_tally(ns_ctx->histogram, tsc_diff);
	}

	if (spdk_unlikely(entry->md_size > 0)) {
		/* add application level verification for end-to-end data protection */
		entry->fn_table->verify_io(task, entry);
	}

	/*
	 * is_draining indicates when time has expired or io_submitted exceeded
	 * g_number_ios for the test run and we are just waiting for the previously
	 * submitted I/O to complete. In this case, do not submit a new I/O to
	 * replace the one just completed.
	 */
	if (spdk_unlikely(ns_ctx->is_draining)) {
		spdk_dma_free(task->iovs[0].iov_base);
		free(task->iovs);
		spdk_dma_free(task->md_iov.iov_base);
		free(task);
	} else {
		/* Co-resident compute: process the fetched node before the next hop. */
		ann_compute_kernel(task);
		submit_single_io(task);
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct perf_task *task = ctx;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		if (task->is_read) {
			RATELIMIT_LOG("Read completed with error (sct=%d, sc=%d)\n",
				      cpl->status.sct, cpl->status.sc);
		} else {
			RATELIMIT_LOG("Write completed with error (sct=%d, sc=%d)\n",
				      cpl->status.sct, cpl->status.sc);
		}
		if (!g_continue_on_error) {
			if (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
			    cpl->status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT) {
				/* The namespace was hotplugged.  Stop trying to send I/O to it. */
				task->ns_ctx->is_draining = true;
			}

			task->ns_ctx->status = 1;
		}
	}

	task_complete(task);
}

static struct perf_task *
allocate_task(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Out of memory allocating tasks\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;
	ns_ctx->entry->fn_table->setup_payload(task, queue_depth % 8 + 1);

	return task;
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	while (queue_depth-- > 0) {
		task = allocate_task(ns_ctx, queue_depth);
		submit_single_io(task);
	}
}

static int
init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	TAILQ_INIT(&ns_ctx->queued_tasks);
	return ns_ctx->entry->fn_table->init_ns_worker_ctx(ns_ctx);
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task *task, *ttask;

	TAILQ_FOREACH_SAFE(task, &ns_ctx->queued_tasks, link, ttask) {
		TAILQ_REMOVE(&ns_ctx->queued_tasks, task, link);
		task_complete(task);
	}
	ns_ctx->entry->fn_table->cleanup_ns_worker_ctx(ns_ctx);
}

static void
print_periodic_performance(bool warmup)
{
	uint64_t io_this_second;
	double mb_this_second;
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	uint64_t busy_tsc;
	uint64_t idle_tsc;
	uint64_t core_busy_tsc = 0;
	uint64_t core_idle_tsc = 0;
	double core_busy_perc = 0;

	if (!isatty(STDOUT_FILENO)) {
		/* Don't print periodic stats if output is not going
		 * to a terminal.
		 */
		return;
	}
	io_this_second = 0;
	TAILQ_FOREACH(worker, &g_workers, link) {
		busy_tsc = 0;
		idle_tsc = 0;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			io_this_second += ns_ctx->stats.io_completed - ns_ctx->stats.last_io_completed;
			ns_ctx->stats.last_io_completed = ns_ctx->stats.io_completed;

			if (g_monitor_perf_cores) {
				busy_tsc += ns_ctx->stats.busy_tsc - ns_ctx->stats.last_busy_tsc;
				idle_tsc += ns_ctx->stats.idle_tsc - ns_ctx->stats.last_idle_tsc;
				ns_ctx->stats.last_busy_tsc = ns_ctx->stats.busy_tsc;
				ns_ctx->stats.last_idle_tsc = ns_ctx->stats.idle_tsc;
			}
		}
		if (g_monitor_perf_cores) {
			core_busy_tsc += busy_tsc;
			core_idle_tsc += idle_tsc;
		}
	}
	mb_this_second = (double)io_this_second * g_io_size_bytes / (1024 * 1024);

	printf("%s%9ju IOPS, %8.2f MiB/s", warmup ? "[warmup] " : "", io_this_second, mb_this_second);
	if (g_monitor_perf_cores) {
		core_busy_perc = (double)core_busy_tsc / (core_idle_tsc + core_busy_tsc) * 100;
		printf("%3d Core(s): %6.2f%% Busy", g_num_workers, core_busy_perc);
	}
	printf("\r");
	fflush(stdout);
}

static void
perf_dump_transport_statistics(struct worker_thread *worker)
{
	struct ns_worker_ctx *ns_ctx;

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (ns_ctx->entry->fn_table->dump_transport_stats) {
			ns_ctx->entry->fn_table->dump_transport_stats(worker->lcore, ns_ctx);
		}
	}
}

/* SPDK_PERF_WORKER_GROUP: one shared group, so one stats block, per worker: the evidence line, then the group's
 * stats once, under a header naming the whole group (nvme_dump_transport_stats() would credit the group's totals
 * to one namespace). NVMe only: work_fn_worker_group() refuses other entries. */
static void
perf_worker_dump_transport_statistics(struct worker_thread *worker)
{
	struct spdk_nvme_poll_group_stat *stat = NULL;
	struct ns_worker_ctx *ns_ctx, *first;
	uint32_t n = 0, q = 0, i;
	int rc;

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		n++;
		q += (uint32_t)ns_ctx->u.nvme.num_all_qpairs;
	}
	printf("PERF_WORKER_GROUP lcore=%u ns_ctx=%u qpairs=%u\n", worker->lcore, n, q);
	first = TAILQ_FIRST(&worker->ns_ctx);
	if (first == NULL || worker->group == NULL) {
		return;
	}

	rc = spdk_nvme_poll_group_get_stats(worker->group, &stat);
	if (rc) {
		fprintf(stderr, "Can't get transport stats, error %d\n", rc);
		return;
	}

	printf("\n====================\n");
	printf("lcore %u, worker group (ns %s +%u more) statistics:\n", worker->lcore, first->entry->name, n - 1);

	/* WG-SYNC:transport-switch begin */
	for (i = 0; i < stat->num_transports; i++) {
		switch (stat->transport_stat[i]->trtype) {
		case SPDK_NVME_TRANSPORT_RDMA:
			nvme_dump_rdma_statistics(stat->transport_stat[i]);
			break;
		case SPDK_NVME_TRANSPORT_PCIE:
			nvme_dump_pcie_statistics(stat->transport_stat[i]);
			break;
		case SPDK_NVME_TRANSPORT_TCP:
			nvme_dump_tcp_statistics(stat->transport_stat[i]);
			break;
		default:
			fprintf(stderr, "Unknown transport statistics %d %s\n", stat->transport_stat[i]->trtype,
				spdk_nvme_transport_id_trtype_str(stat->transport_stat[i]->trtype));
		}
	}
	/* WG-SYNC:transport-switch end */

	spdk_nvme_poll_group_free_stats(worker->group, stat);
}

/*
 * SPDK_PERF_WORKER_GROUP copies of allocate_task(), submit_io(), cleanup_ns_worker_ctx() and
 * print_periodic_performance(), called only by work_fn_worker_group(). A second caller of an original would end
 * GCC's called-once inlining of it into work_fn() and so change work_fn()'s code; PERF_WG_NO_ICF stops GCC folding
 * a copy back into its original (-fipa-icf). KEEP IN STEP with the originals.
 */
#if defined(__GNUC__) && !defined(__clang__)
#define PERF_WG_NO_ICF __attribute__((no_icf))
#else
#define PERF_WG_NO_ICF
#endif
/* P4.12: NOP bytes at work_fn_worker_group()'s exit (run once) so that its P4.12 growth is a multiple of 64 B: work_fn(),
 * after it in perf.o's .text, keeps its 64 B phase and the objects after perf.o keep theirs (NM_PHASE, measured on the
 * linked lean binary; re-measure after any change to work_fn_worker_group()) */
#define PERF_WG_LAYOUT_PAD "48"

static PERF_WG_NO_ICF struct perf_task *
perf_wg_allocate_task(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Out of memory allocating tasks\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;
	ns_ctx->entry->fn_table->setup_payload(task, queue_depth % 8 + 1);

	return task;
}

static PERF_WG_NO_ICF void
perf_wg_submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	while (queue_depth-- > 0) {
		task = perf_wg_allocate_task(ns_ctx, queue_depth);
		submit_single_io(task);
	}
}

static PERF_WG_NO_ICF void
perf_wg_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task *task, *ttask;

	TAILQ_FOREACH_SAFE(task, &ns_ctx->queued_tasks, link, ttask) {
		TAILQ_REMOVE(&ns_ctx->queued_tasks, task, link);
		task_complete(task);
	}
	ns_ctx->entry->fn_table->cleanup_ns_worker_ctx(ns_ctx);
}

static PERF_WG_NO_ICF void
perf_wg_print_periodic_performance(bool warmup)
{
	uint64_t io_this_second;
	double mb_this_second;
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	uint64_t busy_tsc;
	uint64_t idle_tsc;
	uint64_t core_busy_tsc = 0;
	uint64_t core_idle_tsc = 0;
	double core_busy_perc = 0;

	if (!isatty(STDOUT_FILENO)) {
		/* Don't print periodic stats if output is not going
		 * to a terminal.
		 */
		return;
	}
	io_this_second = 0;
	TAILQ_FOREACH(worker, &g_workers, link) {
		busy_tsc = 0;
		idle_tsc = 0;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			io_this_second += ns_ctx->stats.io_completed - ns_ctx->stats.last_io_completed;
			ns_ctx->stats.last_io_completed = ns_ctx->stats.io_completed;

			if (g_monitor_perf_cores) {
				busy_tsc += ns_ctx->stats.busy_tsc - ns_ctx->stats.last_busy_tsc;
				idle_tsc += ns_ctx->stats.idle_tsc - ns_ctx->stats.last_idle_tsc;
				ns_ctx->stats.last_busy_tsc = ns_ctx->stats.busy_tsc;
				ns_ctx->stats.last_idle_tsc = ns_ctx->stats.idle_tsc;
			}
		}
		if (g_monitor_perf_cores) {
			core_busy_tsc += busy_tsc;
			core_idle_tsc += idle_tsc;
		}
	}
	mb_this_second = (double)io_this_second * g_io_size_bytes / (1024 * 1024);

	printf("%s%9ju IOPS, %8.2f MiB/s", warmup ? "[warmup] " : "", io_this_second, mb_this_second);
	if (g_monitor_perf_cores) {
		core_busy_perc = (double)core_busy_tsc / (core_idle_tsc + core_busy_tsc) * 100;
		printf("%3d Core(s): %6.2f%% Busy", g_num_workers, core_busy_perc);
	}
	printf("\r");
	fflush(stdout);
}

static int64_t
perf_worker_check(struct worker_thread *worker)
{
	int64_t rc = spdk_nvme_poll_group_process_completions(worker->group, g_max_completions,
			perf_worker_disconnect_cb);

	if (rc < 0) {
		struct ns_worker_ctx *first = TAILQ_FIRST(&worker->ns_ctx);

		/* As nvme_check_io(), a failed poll fails the run; perf_worker_disconnect_cb has already marked
		 * (draining, status) the ns_ctx that owns the failed qpair. */
		fprintf(stderr, "NVMe io qpair process completion error\n");
		if (first != NULL) {
			first->status = 1;
		}
		return -1;
	}
	return rc;
}

/*
 * SPDK_PERF_WORKER_GROUP=1 variant of work_fn(): one poll group per worker holds the qpairs of all its ns_ctx,
 * so each loop iteration polls the group once instead of once per ns_ctx. Submission, warmup, timing,
 * draining and the stats dump follow work_fn(); the group's busy/idle time is booked on the worker's first
 * ns_ctx. Polling only (main() refuses -E). main() starts the workers here instead of work_fn() (one branch
 * at launch) and the helpers it shares with work_fn() are perf_wg_* copies, so work_fn()'s code is unchanged;
 * noinline keeps this out of main(). KEEP IN STEP with work_fn().
 */
static __attribute__((noinline)) int
work_fn_worker_group(void *arg)
{
	uint64_t tsc_start, tsc_end, tsc_current, tsc_next_print, check_now, tsc_last, idle_tsc = 0;
	struct worker_thread *worker = (struct worker_thread *) arg;
	struct ns_worker_ctx *ns_ctx = NULL, *first;
	uint32_t unfinished_ns_ctx;
	bool warmup = false;
	int rc;
	int64_t check_rc;
	TAILQ_HEAD(, perf_task)	swap;
	struct perf_task *task;

	/* Initialize thread local seed. */
	seed = spdk_rand_xorshift64_seed();

	/* The group holds NVMe qpairs only: refuse AIO/URING entries before any ns_ctx is initialized. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (ns_ctx->entry->type != ENTRY_TYPE_NVME_NS) {
			printf("ERROR: SPDK_PERF_WORKER_GROUP=1 needs NVMe namespaces (%s)\n", ns_ctx->entry->name);
			pthread_barrier_wait(&g_worker_sync_barrier);
			ns_ctx->status = 1;
			return 1;
		}
	}

	/* Allocate queue pairs for each namespace, all in the worker's group. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			/* Wait on barrier to avoid blocking of successful workers */
			pthread_barrier_wait(&g_worker_sync_barrier);
			ns_ctx->status = 1;
			return 1;
		}
	}
	first = TAILQ_FIRST(&worker->ns_ctx);

	rc = pthread_barrier_wait(&g_worker_sync_barrier);
	if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
		printf("ERROR: failed to wait on thread sync barrier\n");
		if (first != NULL) {
			first->status = 1;
		}
		return 1;
	}

	tsc_start = spdk_get_ticks();
	tsc_current = tsc_start;
	tsc_next_print = tsc_current + g_tsc_rate;

	if (g_warmup_time_in_sec) {
		warmup = true;
		tsc_end = tsc_current + g_warmup_time_in_sec * g_tsc_rate;
	} else {
		tsc_end = tsc_current + g_time_in_sec * g_tsc_rate;
	}

	/* Submit initial I/O for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		perf_wg_submit_io(ns_ctx, g_queue_depth);
	}

	while (spdk_likely(!g_exit) && first != NULL) {
		bool all_draining = true;

		if (g_continue_on_error) {
			TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
				if (ns_ctx->is_draining) {
					continue;
				}
				/* Submit any I/O that is queued up */
				TAILQ_INIT(&swap);
				TAILQ_SWAP(&swap, &ns_ctx->queued_tasks, perf_task, link);
				while (!TAILQ_EMPTY(&swap)) {
					task = TAILQ_FIRST(&swap);
					TAILQ_REMOVE(&swap, task, link);
					if (ns_ctx->is_draining) {
						TAILQ_INSERT_TAIL(&ns_ctx->queued_tasks, task, link);
						continue;
					}
					submit_single_io(task);
				}
			}
		}

		/* One group poll covers every qpair of every ns_ctx of this worker. */
		check_now = spdk_get_ticks();
		check_rc = perf_worker_check(worker);

		if (check_rc > 0) {
			first->stats.busy_tsc += check_now - first->stats.last_tsc;
		} else {
			first->stats.idle_tsc += check_now - first->stats.last_tsc;
		}
		first->stats.last_tsc = check_now;

		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (!ns_ctx->is_draining) {
				all_draining = false;
				break;
			}
		}

		if (spdk_unlikely(all_draining)) {
			break;
		}

		tsc_last = tsc_current;
		tsc_current = spdk_get_ticks();

		/* IAU-next P4.12 (A16, the host-bound rule's time source): after the warmup, the TSC ticks of every
		 * iteration whose group poll completed nothing, from the previous iteration's reading to this one (the
		 * poll and the rest of an idle iteration: the -Q queued-task scan, the print and warmup tests);
		 * PERF_IDLE_TIME at the end (perf_idle_time_print()). Two existing tick reads, no new one. */
		if (check_rc <= 0 && !warmup) {
			idle_tsc += tsc_current - tsc_last;
		}

		/* WG-SYNC:periodic-print begin */
		if (worker->lcore == g_main_core && tsc_current > tsc_next_print) {
			tsc_next_print += g_tsc_rate;
			perf_wg_print_periodic_performance(warmup);
		}
		/* WG-SYNC:periodic-print end */

		/* WG-SYNC:warmup-reset begin */
		if (tsc_current > tsc_end) {
			if (warmup) {
				/* Update test start and end time, clear statistics */
				tsc_start = spdk_get_ticks();
				tsc_end = tsc_start + g_time_in_sec * g_tsc_rate;

				TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
					memset(&ns_ctx->stats, 0, sizeof(ns_ctx->stats));
					ns_ctx->stats.min_tsc = UINT64_MAX;
					spdk_histogram_data_reset(ns_ctx->histogram);
				}

				if (worker->lcore == g_main_core && isatty(STDOUT_FILENO)) {
					/* warmup stage prints a longer string to stdout, need to erase it */
					printf("%c[2K", 27);
				}

				warmup = false;
			} else {
				break;
			}
		}
		/* WG-SYNC:warmup-reset end */
	}

	/* Capture the actual elapsed time when we break out of the main loop (see work_fn()). */
	/* WG-SYNC:elapsed-time begin */
	if (worker->lcore == g_main_core) {
		g_elapsed_time_in_usec = (tsc_current - tsc_start) * SPDK_SEC_TO_USEC / g_tsc_rate;
	}
	/* WG-SYNC:elapsed-time end */
	perf_idle_time_print(worker, idle_tsc, tsc_current - tsc_start);
	/* P4.12 layout pad: keeps work_fn() (next in .text) at its 64 B phase and perf.o's .text size mod 64 (NM_PHASE) */
	__asm__ volatile(".nops " PERF_WG_LAYOUT_PAD);

	/* drain: the group poll serves every ns_ctx at once */
	do {
		unfinished_ns_ctx = 0;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			ns_ctx->is_draining = true;
			if (ns_ctx->current_queue_depth > 0) {
				unfinished_ns_ctx++;
			}
		}
		if (unfinished_ns_ctx > 0) {
			perf_worker_check(worker);
		}
	} while (unfinished_ns_ctx > 0);

	if (g_dump_transport_stats) {
		pthread_mutex_lock(&g_stats_mutex);
		perf_worker_dump_transport_statistics(worker);
		pthread_mutex_unlock(&g_stats_mutex);
	}

	/* Outside any completion context: each ns_ctx frees its qpairs, the last one destroys the group. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		perf_wg_cleanup_ns_worker_ctx(ns_ctx);
	}

	return 0;
}

static int
work_fn(void *arg)
{
	uint64_t tsc_start, tsc_end, tsc_current, tsc_next_print;
	struct worker_thread *worker = (struct worker_thread *) arg;
	struct ns_worker_ctx *ns_ctx = NULL;
	uint32_t unfinished_ns_ctx;
	bool warmup = false;
	int rc;
	int64_t check_rc;
	uint64_t check_now;
	TAILQ_HEAD(, perf_task)	swap;
	struct perf_task *task;

	/* Initialize thread local seed. */
	seed = spdk_rand_xorshift64_seed();

	/* Allocate queue pairs for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			/* Wait on barrier to avoid blocking of successful workers */
			pthread_barrier_wait(&g_worker_sync_barrier);
			ns_ctx->status = 1;
			return 1;
		}
	}

	rc = pthread_barrier_wait(&g_worker_sync_barrier);
	if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
		printf("ERROR: failed to wait on thread sync barrier\n");
		ns_ctx->status = 1;
		return 1;
	}

	tsc_start = spdk_get_ticks();
	tsc_current = tsc_start;
	tsc_next_print = tsc_current + g_tsc_rate;

	if (g_warmup_time_in_sec) {
		warmup = true;
		tsc_end = tsc_current + g_warmup_time_in_sec * g_tsc_rate;
	} else {
		tsc_end = tsc_current + g_time_in_sec * g_tsc_rate;
	}

	/* Submit initial I/O for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		submit_io(ns_ctx, g_queue_depth);
	}

	while (spdk_likely(!g_exit)) {
		bool all_draining = true;

		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (g_continue_on_error && !ns_ctx->is_draining) {
				/* Submit any I/O that is queued up */
				TAILQ_INIT(&swap);
				TAILQ_SWAP(&swap, &ns_ctx->queued_tasks, perf_task, link);
				while (!TAILQ_EMPTY(&swap)) {
					task = TAILQ_FIRST(&swap);
					TAILQ_REMOVE(&swap, task, link);
					if (ns_ctx->is_draining) {
						TAILQ_INSERT_TAIL(&ns_ctx->queued_tasks,
								  task, link);
						continue;
					}
					submit_single_io(task);
				}
			}

			check_now = spdk_get_ticks();
			check_rc = ns_ctx->entry->fn_table->check_io(ns_ctx);

			if (check_rc > 0) {
				ns_ctx->stats.busy_tsc += check_now - ns_ctx->stats.last_tsc;
			} else {
				ns_ctx->stats.idle_tsc += check_now - ns_ctx->stats.last_tsc;
			}
			ns_ctx->stats.last_tsc = check_now;

			if (!ns_ctx->is_draining) {
				all_draining = false;
			}
		}

		if (spdk_unlikely(all_draining)) {
			break;
		}

		tsc_current = spdk_get_ticks();

		/* WG-SYNC:periodic-print begin */
		if (worker->lcore == g_main_core && tsc_current > tsc_next_print) {
			tsc_next_print += g_tsc_rate;
			print_periodic_performance(warmup);
		}
		/* WG-SYNC:periodic-print end */

		/* WG-SYNC:warmup-reset begin */
		if (tsc_current > tsc_end) {
			if (warmup) {
				/* Update test start and end time, clear statistics */
				tsc_start = spdk_get_ticks();
				tsc_end = tsc_start + g_time_in_sec * g_tsc_rate;

				TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
					memset(&ns_ctx->stats, 0, sizeof(ns_ctx->stats));
					ns_ctx->stats.min_tsc = UINT64_MAX;
					spdk_histogram_data_reset(ns_ctx->histogram);
				}

				if (worker->lcore == g_main_core && isatty(STDOUT_FILENO)) {
					/* warmup stage prints a longer string to stdout, need to erase it */
					printf("%c[2K", 27);
				}

				warmup = false;
			} else {
				break;
			}
		}
		/* WG-SYNC:warmup-reset end */
	}

	/* Capture the actual elapsed time when we break out of the main loop. This will account
	 * for cases where we exit prematurely due to a signal. We only need to capture it on
	 * one core, so use the main core.
	 */
	/* WG-SYNC:elapsed-time begin */
	if (worker->lcore == g_main_core) {
		g_elapsed_time_in_usec = (tsc_current - tsc_start) * SPDK_SEC_TO_USEC / g_tsc_rate;
	}
	/* WG-SYNC:elapsed-time end */

	/* drain the io of each ns_ctx in round robin to make the fairness */
	do {
		unfinished_ns_ctx = 0;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			/* first time will enter into this if case */
			if (!ns_ctx->is_draining) {
				ns_ctx->is_draining = true;
			}

			if (ns_ctx->current_queue_depth > 0) {
				ns_ctx->entry->fn_table->check_io(ns_ctx);
				if (ns_ctx->current_queue_depth > 0) {
					unfinished_ns_ctx++;
				}
			}
		}
	} while (unfinished_ns_ctx > 0);

	if (g_dump_transport_stats) {
		pthread_mutex_lock(&g_stats_mutex);
		perf_dump_transport_statistics(worker);
		pthread_mutex_unlock(&g_stats_mutex);
	}

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		cleanup_ns_worker_ctx(ns_ctx);
	}

	return 0;
}

/*
 * IAU-next P4.12: the fault mode, SPDK_PERF_FAULT=1 (P4 plan task P4.12 rev 2 with its rev 1 code; amendments A6 (c),
 * A7, A20). Opt-in, set up once in main() before the workers start; refused (perf exits) with -E (interrupt mode),
 * with SPDK_PERF_WORKER_GROUP=1, without -Q (continue on error: injected errors must not end the run), with a
 * metadata / PI namespace, with more than one iovec per IO (-O), with a non-NVMe entry, and with
 * SPDK_PERF_FAULT_RESET_MS and more than one worker (spdk_nvme_ctrlr_reset() is not safe against other threads' IO).
 * Knobs, decimal, 0 or unset = off; any of them set without SPDK_PERF_FAULT=1 is refused:
 *   SPDK_PERF_FAULT_ABORT_EVERY=N       every N-th successful submit of an ns_ctx is aborted by cookie at once
 *                                       (spdk_nvme_ctrlr_cmd_abort_ext(), cmd_cb_arg = the task)
 *   SPDK_PERF_FAULT_TIMEOUT_US=U        spdk_nvme_ctrlr_register_timeout_callback(ctrlr, U, U) on every controller
 *   SPDK_PERF_FAULT_ABORT_ON_TIMEOUT=1  an IO's timeout callback aborts it by cid (needs TIMEOUT_US)
 *   SPDK_PERF_FAULT_DISCONNECT_MS=T     T ms after its first poll, the first ns_ctx to get there disconnects its qpair[0],
 *                                       once per process (spdk_nvme_ctrlr_disconnect_io_qpair())
 *   SPDK_PERF_FAULT_RESET_MS=T          T ms after its first poll, spdk_nvme_ctrlr_reset() of that ns_ctx's controller,
 *                                       once per process
 * (No RAW_INVAL_EVERY knob: P4.12b removed it. spdk_nvme_ns_cmd_readv()'s SGL split ignores next_sge_fn's return, so a
 * failing callback never reaches the payload builder; DV29 is covered by the harness and the C gate vectors.)
 * Every NVMe namespace gets fn_table = the fault table: nvme_fn_table with
 *   submit_io = perf_fault_submit_io(): nvme_submit_io() for one iovec without metadata, completing in
 *     perf_fault_io_complete(), task->f_inflight set around it; its round robin skips a qpair the disconnect tick
 *     freed (A7);
 *   check_io = perf_fault_check_io(): nvme_check_io() (polling only) with perf_fault_disconnect_cb() and, outside any
 *     completion context, the disconnect and reset ticks, the free of the disconnected qpair once its disconnect
 *     callback ran (A7), a failed poll tolerated (counted) once a tick fired (A6 (c), C4), and the exit drain bound
 *     (A20): PERF_FAULT_DRAIN_BOUND_MS after an ns_ctx starts draining with IO outstanding, PERF_FAULT_DRAIN_BOUND names
 *     it and its outstanding IO is abandoned (current_queue_depth = 0 ends work_fn()'s drain; a later callback is counted
 *     late and its task freed, never resubmitted); check_l NX_FAULT then FAILs the point.
 * perf_fault_io_complete(): a callback with f_inflight clear is a second callback of one IO (dup, not passed on); else
 * the status is counted (ok, err = an error with dnr 0, err_dnr, aborted_req, aborted_sqdel, other_err = a path or
 * vendor status; inval also counts each INVALID_FIELD) and io_complete() runs. The admin poller is
 * perf_fault_poll_ctrlrs(): nvme_poll_ctrlrs() that tolerates a failed admin poll once the reset tick fired.
 * Evidence (stderr, one line each): PERF_FAULT_CONFIG at setup; PERF_FAULT after the workers joined (inflight_end =
 * submitted - completed); PERF_FAULT_DRAIN_BOUND per abandoned ns_ctx. Every function below sits in
 * .text.unlikely.perf_fault (PERF_FAULT_COLD), so work_fn(), nvme_submit_io() and nvme_check_io() keep their code and
 * perf.o's .text its size (diff=0, A20).
 */
#define PERF_FAULT_DRAIN_BOUND_MS	10000u	/* A20: > the host library's 1 s drain bound plus a delete; < run_point's
						 * 60 s grace after warmup + time */
/* placed with the cold code (.text.unlikely.*, the linker's unlikely group, before .text), but not __attribute__((cold)):
 * a call to a cold function makes GCC split its caller into hot and cold parts (main(), work_fn_worker_group()) */
#define PERF_FAULT_COLD			__attribute__((noinline, section(".text.unlikely.perf_fault")))

enum perf_fault_disc {
	PERF_FAULT_DISC_NONE = 0,	/* no disconnect tick on this ns_ctx */
	PERF_FAULT_DISC_TICKED,		/* qpair[0] disconnected, its disconnect callback not yet run */
	PERF_FAULT_DISC_SEEN,		/* its callback ran (the ns_ctx drains): free it at the poll's end */
	PERF_FAULT_DISC_FREED,		/* freed, its slot NULL */
};

struct perf_fault {
	/* the knobs */
	uint64_t		abort_every, timeout_us, abort_on_timeout, disconnect_ms, reset_ms;
	/* the once-per-process ticks (any worker) */
	int			disc_done, reset_started, reset_rc;
	/* the counters (workers, timeout callbacks on the admin poller: atomics) */
	uint64_t		submitted, completed, dup, ok, err, err_dnr, aborted_req, aborted_sqdel, other_err,
				timeouts, abort_calls, abort_cbs, abort_rc_err, disconnects, resets, inval, late,
				bound_hits, admin_timeouts, poll_err, admin_err;
	struct ns_fn_table	fn_table;
};

struct perf_fault_ns {
	uint64_t		t0;		/* the first fault poll (the ticks' clock) */
	uint64_t		drain_t0;	/* the first fault poll that saw is_draining (the drain bound's clock) */
	uint64_t		n_try, n_ok;	/* submit attempts / successful submits (the every-N knobs) */
	struct spdk_nvme_qpair	*disc_qpair;
	enum perf_fault_disc	disc_state;
	bool			abandoned;	/* the drain bound hit */
};

static inline void
perf_fault_inc(uint64_t *c)
{
	__atomic_fetch_add(c, 1, __ATOMIC_RELAXED);
}

static PERF_FAULT_COLD void
perf_fault_classify(struct perf_fault *g, const struct spdk_nvme_cpl *cpl)
{
	if (!spdk_nvme_cpl_is_error(cpl)) {
		perf_fault_inc(&g->ok);
		return;
	}
	if (cpl->status.sct == SPDK_NVME_SCT_GENERIC && cpl->status.sc == SPDK_NVME_SC_INVALID_FIELD) {
		perf_fault_inc(&g->inval);
	}
	if (cpl->status.sct == SPDK_NVME_SCT_GENERIC && cpl->status.sc == SPDK_NVME_SC_ABORTED_BY_REQUEST) {
		perf_fault_inc(&g->aborted_req);
	} else if (cpl->status.sct == SPDK_NVME_SCT_GENERIC && cpl->status.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION) {
		perf_fault_inc(&g->aborted_sqdel);
	} else if (cpl->status.sct == SPDK_NVME_SCT_GENERIC || cpl->status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC ||
		   cpl->status.sct == SPDK_NVME_SCT_MEDIA_ERROR) {
		perf_fault_inc(cpl->status.dnr ? &g->err_dnr : &g->err);
	} else {
		perf_fault_inc(&g->other_err);
	}
}

static PERF_FAULT_COLD void
perf_fault_io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct perf_task *task = ctx;
	struct perf_fault *g = g_fault;

	if (task->f_inflight == 0) {
		perf_fault_inc(&g->dup);	/* a second callback of one IO: counted, not passed on */
		return;
	}
	task->f_inflight = 0;
	perf_fault_inc(&g->completed);
	perf_fault_classify(g, cpl);
	if (spdk_unlikely(task->ns_ctx->fault->abandoned)) {
		/* after the drain bound (A20): its ns_ctx's current_queue_depth is 0, so no io_complete(); the task is freed
		 * as task_complete() frees a draining one */
		perf_fault_inc(&g->late);
		spdk_dma_free(task->iovs[0].iov_base);
		free(task->iovs);
		spdk_dma_free(task->md_iov.iov_base);
		free(task);
		return;
	}
	io_complete(ctx, cpl);
}

static PERF_FAULT_COLD void
perf_fault_abort_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	perf_fault_inc(&g_fault->abort_cbs);
}

static PERF_FAULT_COLD void
perf_fault_timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	struct perf_fault *g = g_fault;
	int rc;

	if (qpair == NULL) {
		perf_fault_inc(&g->admin_timeouts);	/* an admin command (the admin poller's thread) */
		return;
	}
	perf_fault_inc(&g->timeouts);
	if (g->abort_on_timeout) {
		rc = spdk_nvme_ctrlr_cmd_abort(ctrlr, qpair, cid, perf_fault_abort_cb, NULL);
		perf_fault_inc(rc == 0 ? &g->abort_calls : &g->abort_rc_err);
	}
}

static PERF_FAULT_COLD int
perf_fault_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx, struct ns_entry *entry,
		     uint64_t offset_in_ios)
{
	struct perf_fault *g = g_fault;
	struct perf_fault_ns *f = ns_ctx->fault;
	struct spdk_nvme_qpair *qpair = NULL;
	uint64_t lba;
	int i, qp_num, rc;

	lba = offset_in_ios * entry->io_size_blocks;
	/* nvme_submit_io()'s round robin, skipping a slot the disconnect tick freed (A7) */
	for (i = 0; i < ns_ctx->u.nvme.num_active_qpairs && qpair == NULL; i++) {
		qp_num = ns_ctx->u.nvme.last_qpair;
		ns_ctx->u.nvme.last_qpair++;
		if (ns_ctx->u.nvme.last_qpair == ns_ctx->u.nvme.num_active_qpairs) {
			ns_ctx->u.nvme.last_qpair = 0;
		}
		qpair = ns_ctx->u.nvme.qpair[qp_num];
	}
	if (qpair == NULL) {
		return -ENXIO;
	}
	f->n_try++;
	task->f_inflight = 1;	/* before the call: a callback may run inside it (A22's note) */
	if (task->is_read) {
		/* FAULT-SYNC: nvme_submit_io()'s one-iovec read */
		rc = spdk_nvme_ns_cmd_read_with_md(entry->u.nvme.ns, qpair,
						   task->iovs[0].iov_base, task->md_iov.iov_base,
						   lba,
						   entry->io_size_blocks, perf_fault_io_complete,
						   task, entry->io_flags,
						   task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
	} else {
		/* FAULT-SYNC: nvme_submit_io()'s one-iovec write */
		rc = spdk_nvme_ns_cmd_write_with_md(entry->u.nvme.ns, qpair,
						    task->iovs[0].iov_base, task->md_iov.iov_base,
						    lba,
						    entry->io_size_blocks, perf_fault_io_complete,
						    task, entry->io_flags,
						    task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
	}
	if (rc != 0) {
		task->f_inflight = 0;
		return rc;
	}
	perf_fault_inc(&g->submitted);
	f->n_ok++;
	if (g->abort_every != 0 && f->n_ok % g->abort_every == 0) {
		rc = spdk_nvme_ctrlr_cmd_abort_ext(entry->u.nvme.ctrlr, qpair, task, perf_fault_abort_cb, NULL);
		perf_fault_inc(rc == 0 ? &g->abort_calls : &g->abort_rc_err);
	}
	return 0;
}

/* C4: only the qpair the disconnect tick disconnected drains its ns_ctx; any other disconnected qpair (a reset's, the
 * target's) is left to the library (a reset reconnects it), with no status: IO that never ends FAILs the point at the
 * drain bound */
static PERF_FAULT_COLD void
perf_fault_disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{
	struct ns_worker_ctx *ns_ctx = ctx;
	struct perf_fault_ns *f = ns_ctx->fault;

	if (f->disc_state == PERF_FAULT_DISC_TICKED && qpair == f->disc_qpair) {
		ns_ctx->is_draining = true;
		f->disc_state = PERF_FAULT_DISC_SEEN;
	}
}

static PERF_FAULT_COLD void
perf_fault_ticks(struct ns_worker_ctx *ns_ctx, struct perf_fault_ns *f, uint64_t now)
{
	struct perf_fault *g = g_fault;

	if (g->disconnect_ms != 0 && f->disc_state == PERF_FAULT_DISC_NONE && !ns_ctx->is_draining &&
	    ns_ctx->u.nvme.qpair[0] != NULL && now - f->t0 >= g->disconnect_ms * g_tsc_rate / 1000 &&
	    __atomic_exchange_n(&g->disc_done, 1, __ATOMIC_ACQ_REL) == 0) {
		f->disc_qpair = ns_ctx->u.nvme.qpair[0];
		f->disc_state = PERF_FAULT_DISC_TICKED;
		spdk_nvme_ctrlr_disconnect_io_qpair(f->disc_qpair);
		perf_fault_inc(&g->disconnects);
	}
	if (g->reset_ms != 0 && now - f->t0 >= g->reset_ms * g_tsc_rate / 1000 &&
	    __atomic_load_n(&g->reset_started, __ATOMIC_ACQUIRE) == 0 &&
	    __atomic_exchange_n(&g->reset_started, 1, __ATOMIC_ACQ_REL) == 0) {
		g->reset_rc = spdk_nvme_ctrlr_reset(ns_ctx->entry->u.nvme.ctrlr);
		perf_fault_inc(&g->resets);
	}
}

/* A20: the exit drain is bounded */
static PERF_FAULT_COLD void
perf_fault_drain_bound(struct ns_worker_ctx *ns_ctx, struct perf_fault_ns *f, uint64_t now)
{
	if (f->abandoned) {
		return;
	}
	if (f->drain_t0 == 0) {
		f->drain_t0 = now;
		return;
	}
	if (ns_ctx->current_queue_depth == 0 || now - f->drain_t0 < PERF_FAULT_DRAIN_BOUND_MS * g_tsc_rate / 1000) {
		return;
	}
	f->abandoned = true;
	perf_fault_inc(&g_fault->bound_hits);
	fprintf(stderr, "PERF_FAULT_DRAIN_BOUND lcore=%u inflight=%" PRIu64 " bound_ms=%u ns=%s\n",
		spdk_env_get_current_core(), ns_ctx->current_queue_depth, PERF_FAULT_DRAIN_BOUND_MS, ns_ctx->entry->name);
	ns_ctx->current_queue_depth = 0;
}

static PERF_FAULT_COLD int64_t
perf_fault_check_io(struct ns_worker_ctx *ns_ctx)
{
	struct perf_fault *g = g_fault;
	struct perf_fault_ns *f = ns_ctx->fault;
	uint64_t now = spdk_get_ticks();
	int64_t rc;
	int i;

	if (f->t0 == 0) {
		f->t0 = now;
	}
	perf_fault_ticks(ns_ctx, f, now);
	/* FAULT-SYNC: nvme_check_io()'s poll */
	rc = spdk_nvme_poll_group_process_completions(ns_ctx->u.nvme.group, g_max_completions,
			perf_fault_disconnect_cb);
	if (f->disc_state == PERF_FAULT_DISC_SEEN) {
		/* A7: the disconnected qpair is freed here, outside any completion context; its IO ends through the delete
		 * path (for stock, stock's), its slot NULL (submit skips it, cleanup frees NULL) */
		for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
			if (ns_ctx->u.nvme.qpair[i] == f->disc_qpair) {
				ns_ctx->u.nvme.qpair[i] = NULL;
			}
		}
		spdk_nvme_ctrlr_free_io_qpair(f->disc_qpair);
		f->disc_state = PERF_FAULT_DISC_FREED;
	}
	if (rc < 0) {
		if (__atomic_load_n(&g->disc_done, __ATOMIC_ACQUIRE) == 0 &&
		    __atomic_load_n(&g->reset_started, __ATOMIC_ACQUIRE) == 0) {
			fprintf(stderr, "NVMe io qpair process completion error\n");	/* nvme_check_io()'s */
			ns_ctx->status = 1;
			return -1;
		}
		perf_fault_inc(&g->poll_err);	/* A6 (c), C4: after a tick, a failed poll is the tick's (counted) */
		rc = 0;
	}
	if (ns_ctx->is_draining) {
		perf_fault_drain_bound(ns_ctx, f, now);
	}
	return rc;
}

static PERF_FAULT_COLD bool
perf_fault_admin_tolerated(void)
{
	if (__atomic_load_n(&g_fault->reset_started, __ATOMIC_ACQUIRE) == 0) {
		return false;
	}
	perf_fault_inc(&g_fault->admin_err);	/* the reset tick disconnects the admin queue meanwhile */
	return true;
}

/* nvme_poll_ctrlrs(), but a failed admin poll ends the run only before the reset tick fired */
static PERF_FAULT_COLD void *
perf_fault_poll_ctrlrs(void *arg)
{
	struct ctrlr_entry *entry;
	int oldstate;
	int rc;

	spdk_unaffinitize_thread();

	if (g_tpoint_group_mask) {
		spdk_trace_register_user_thread();
	}

	while (true) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

		TAILQ_FOREACH(entry, &g_controllers, link) {
			if (entry->trtype != SPDK_NVME_TRANSPORT_PCIE) {
				rc = spdk_nvme_ctrlr_process_admin_completions(entry->ctrlr);
				if (spdk_unlikely(rc < 0 && !g_exit) && !perf_fault_admin_tolerated()) {
					g_exit = true;
				}
			}
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

static PERF_FAULT_COLD int
perf_fault_knob(const char *name, uint64_t max, uint64_t *out)
{
	const char *v = getenv(name);
	char *end = NULL;
	unsigned long long x;

	*out = 0;
	if (v == NULL || v[0] == '\0') {
		return 0;
	}
	errno = 0;
	x = strtoull(v, &end, 10);
	if (errno != 0 || end == v || *end != '\0' || v[0] == '-' || v[0] == '+' || x > max) {
		fprintf(stderr, "SPDK_PERF_FAULT: %s=%s is not a decimal number in 0..%" PRIu64 "\n", name, v, max);
		return -1;
	}
	*out = x;
	return 0;
}

static const char *const g_perf_fault_knobs[] = {
	"SPDK_PERF_FAULT_ABORT_EVERY", "SPDK_PERF_FAULT_TIMEOUT_US", "SPDK_PERF_FAULT_ABORT_ON_TIMEOUT",
	"SPDK_PERF_FAULT_DISCONNECT_MS", "SPDK_PERF_FAULT_RESET_MS",
};

/* main(), with the options parsed: SPDK_PERF_FAULT and its knobs; 0 = off or set up, -1 = refused */
static PERF_FAULT_COLD int
perf_fault_init(void)
{
	const char *on = getenv("SPDK_PERF_FAULT");
	struct perf_fault *g;
	uint64_t *dst[5];
	uint64_t max[5] = { UINT32_MAX, UINT32_MAX, 1, UINT32_MAX, UINT32_MAX };
	size_t i;

	if (on == NULL || on[0] == '\0' || strcmp(on, "0") == 0) {
		for (i = 0; i < SPDK_COUNTOF(g_perf_fault_knobs); i++) {
			const char *v = getenv(g_perf_fault_knobs[i]);

			if (v != NULL && v[0] != '\0') {
				fprintf(stderr, "%s=%s needs SPDK_PERF_FAULT=1\n", g_perf_fault_knobs[i], v);
				return -1;
			}
		}
		return 0;
	}
	if (strcmp(on, "1") != 0) {
		fprintf(stderr, "SPDK_PERF_FAULT=%s is not 0 or 1\n", on);
		return -1;
	}
	if (g_enable_interrupt || g_worker_group || !g_continue_on_error) {
		fprintf(stderr, "SPDK_PERF_FAULT=1 needs polling mode (no -E), no SPDK_PERF_WORKER_GROUP=1 and -Q "
			"(continue on error)\n");
		return -1;
	}
	g = calloc(1, sizeof(*g));
	if (g == NULL) {
		return -1;
	}
	dst[0] = &g->abort_every;
	dst[1] = &g->timeout_us;
	dst[2] = &g->abort_on_timeout;
	dst[3] = &g->disconnect_ms;
	dst[4] = &g->reset_ms;
	for (i = 0; i < SPDK_COUNTOF(g_perf_fault_knobs); i++) {
		if (perf_fault_knob(g_perf_fault_knobs[i], max[i], dst[i]) != 0) {
			free(g);
			return -1;
		}
	}
	if (g->abort_on_timeout && g->timeout_us == 0) {
		fprintf(stderr, "SPDK_PERF_FAULT_ABORT_ON_TIMEOUT=1 needs SPDK_PERF_FAULT_TIMEOUT_US\n");
		free(g);
		return -1;
	}
	g_fault = g;
	return 0;
}

/* main(), the controllers and namespaces registered: the checks that need them, the fault table, the timeouts, the
 * PERF_FAULT_CONFIG line; -1 = refused */
static PERF_FAULT_COLD int
perf_fault_setup(void)
{
	struct perf_fault *g = g_fault;
	struct ns_entry *entry;
	struct ctrlr_entry *c;

	if (g_io_unit_size < g_io_size_bytes) {
		fprintf(stderr, "SPDK_PERF_FAULT=1 needs one iovec per IO (-O %u < -o %u)\n", g_io_unit_size, g_io_size_bytes);
		return -1;
	}
	if (g->reset_ms != 0 && g_num_workers > 1) {
		fprintf(stderr, "SPDK_PERF_FAULT_RESET_MS needs one worker (%u)\n", g_num_workers);
		return -1;
	}
	TAILQ_FOREACH(entry, &g_namespaces, link) {
		if (entry->type != ENTRY_TYPE_NVME_NS || entry->md_size != 0) {
			fprintf(stderr, "SPDK_PERF_FAULT=1 needs NVMe namespaces without metadata (%s)\n", entry->name);
			return -1;
		}
	}
	g->fn_table = nvme_fn_table;
	g->fn_table.submit_io = perf_fault_submit_io;
	g->fn_table.check_io = perf_fault_check_io;
	TAILQ_FOREACH(entry, &g_namespaces, link) {
		entry->fn_table = &g->fn_table;
	}
	if (g->timeout_us != 0) {
		TAILQ_FOREACH(c, &g_controllers, link) {
			spdk_nvme_ctrlr_register_timeout_callback(c->ctrlr, g->timeout_us, g->timeout_us,
					perf_fault_timeout_cb, NULL);
		}
	}
	fprintf(stderr, "PERF_FAULT_CONFIG abort_every=%" PRIu64 " timeout_us=%" PRIu64 " abort_on_timeout=%" PRIu64
		" disconnect_ms=%" PRIu64 " reset_ms=%" PRIu64 " drain_bound_ms=%u\n",
		g->abort_every, g->timeout_us, g->abort_on_timeout, g->disconnect_ms, g->reset_ms,
		PERF_FAULT_DRAIN_BOUND_MS);
	return 0;
}

/* main(), the ns_ctx associated: each one's fault state */
static PERF_FAULT_COLD int
perf_fault_attach(void)
{
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			ns_ctx->fault = calloc(1, sizeof(*ns_ctx->fault));
			if (ns_ctx->fault == NULL) {
				return -1;
			}
		}
	}
	return 0;
}

/* main(), the workers joined (their cleanup freed the qpairs, so every callback that will come has come) */
static PERF_FAULT_COLD void
perf_fault_print_end(void)
{
	struct perf_fault *g = g_fault;

	fprintf(stderr, "PERF_FAULT lcore=%u submitted=%" PRIu64 " completed=%" PRIu64 " dup=%" PRIu64 " ok=%" PRIu64
		" err=%" PRIu64 " err_dnr=%" PRIu64 " aborted_req=%" PRIu64 " aborted_sqdel=%" PRIu64 " other_err=%"
		PRIu64 " timeouts=%" PRIu64 " abort_calls=%" PRIu64 " abort_cbs=%" PRIu64 " abort_rc_err=%" PRIu64
		" disconnects=%" PRIu64 " inflight_end=%" PRIu64 " resets=%" PRIu64 " reset_rc=%d inval=%" PRIu64
		" late=%" PRIu64 " bound_hits=%" PRIu64 " admin_timeouts=%" PRIu64 " poll_err=%" PRIu64 " admin_err=%"
		PRIu64 "\n", g_main_core, g->submitted, g->completed, g->dup, g->ok, g->err, g->err_dnr,
		g->aborted_req, g->aborted_sqdel, g->other_err, g->timeouts, g->abort_calls, g->abort_cbs,
		g->abort_rc_err, g->disconnects, g->submitted >= g->completed ? g->submitted - g->completed : 0,
		g->resets, g->reset_rc, g->inval, g->late, g->bound_hits, g->admin_timeouts, g->poll_err,
		g->admin_err);
}

/* IAU-next P4.12 (A16): work_fn_worker_group()'s time-based idle, one line per worker: the ticks of the iterations
 * whose group poll completed nothing over the measured window (after the warmup) */
static PERF_FAULT_COLD void
perf_idle_time_print(struct worker_thread *worker, uint64_t idle_tsc, uint64_t window_tsc)
{
	fprintf(stderr, "PERF_IDLE_TIME lcore=%u idle_tsc=%" PRIu64 " window_tsc=%" PRIu64 " idle_time_frac=%.4f\n",
		worker->lcore, idle_tsc, window_tsc, window_tsc != 0 ? (double)idle_tsc / (double)window_tsc : 0.0);
}

static void
usage_basic(char *program_name)
{
	printf("%s - NVMe Performance Testing Tool\n\n", program_name);
	printf("ESSENTIAL OPTIONS:\n");
	printf("  -q, --io-depth <val>     Queue depth (required)\n");
	printf("  -o, --io-size <val>      I/O size in bytes (required)\n");
	printf("  -w, --io-pattern <type>  Workload: read, write, randread, randwrite, rw, randrw (required)\n");
	printf("  -t, --time <sec>         Test duration in seconds (required)\n");
	printf("  -r, --transport <fmt>    Target device/transport (required)\n");
	printf("  -c, --core-mask <mask>   CPU core mask (default: 1)\n");
	printf("\n");
	printf("EXAMPLES:\n");
	printf("  Local NVMe:  %s -q 64 -o 4k -w randread -t 60 -r 'trtype:PCIe traddr:0000:01:00.0'\n",
	       program_name);
	printf("  NVMe-oF TCP: %s -q 64 -o 4K -w randread -t 60 -r 'trtype:TCP adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420'\n",
	       program_name);
	printf("\n");
	printf("For complete options: %s --help-full or -v\n", program_name);
	printf("For documentation: see app/spdk_nvme_perf/README.md\n");
}

static void
usage(char *program_name)
{
	printf("%s options", program_name);
#if defined(SPDK_CONFIG_URING) || defined(HAVE_LIBAIO)
	printf(" [Kernel device(s)]...");
#endif
	printf("\n\n");
	printf("==== BASIC OPTIONS ====\n\n");
	printf("\t-q, --io-depth <val> io depth\n");
	printf("\t-o, --io-size <val> io size in bytes\n");
	printf("\t-w, --io-pattern <pattern> io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)\n");
	printf("\t-M, --rwmixread <0-100> rwmixread (100 for reads, 0 for writes)\n");
	printf("\t-t, --time <sec> time in seconds\n");
	printf("\t-a, --warmup-time <sec> warmup time in seconds\n");
	printf("\t-c, --core-mask <mask> core mask for I/O submission/completion.\n");
	printf("\t\t(default: 1)\n");
	spdk_nvme_transport_id_usage(stdout,
				     SPDK_NVME_TRID_USAGE_OPT_LONGOPT | SPDK_NVME_TRID_USAGE_OPT_MULTI | SPDK_NVME_TRID_USAGE_OPT_NS |
				     SPDK_NVME_TRID_USAGE_OPT_HOSTNQN);
	printf("\n");

	printf("==== ADVANCED OPTIONS ====\n\n");
	printf("\t--use-every-core for each namespace, I/Os are submitted from all cores\n");
	printf("\t--io-queue-size <val> size of NVMe IO queue. Default: maximum allowed by controller\n");
	printf("\t-O, --io-unit-size io unit size in bytes (4-byte aligned) for SPDK driver. default: same as io size\n");
	printf("\t-P, --num-qpairs <val> number of io queues per namespace. default: 1\n");
	printf("\t-U, --num-unused-qpairs <val> number of unused io queues per controller. default: 0\n");
	printf("\t-A, --buffer-alignment IO buffer alignment. Must be power of 2 and not less than cache line (%u)\n",
	       SPDK_CACHE_LINE_SIZE);
	printf("\t-s, --hugemem-size <MB> DPDK huge memory size in MB.\n");
	printf("\t-g, --mem-single-seg use single file descriptor for DPDK memory segments\n");
	printf("\t-C, --max-completion-per-poll <val> max completions per poll\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t-i, --shmem-grp-id <id> shared memory group ID\n");
	printf("\t-d, --number-ios <val> number of I/O to perform per thread on each namespace. Note: this is additional exit criteria.\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t\tYou may also specify an integer percent of the namespace size as <N>%% (e.g. -d 50%% for 50%% of the namespace size)\n");
	printf("\t-e, --metadata <fmt> metadata configuration\n");
	printf("\t\t Keys:\n");
	printf("\t\t  PRACT      Protection Information Action bit (PRACT=1 or PRACT=0)\n");
	printf("\t\t  PRCHK      Control of Protection Information Checking (PRCHK=GUARD|REFTAG|APPTAG)\n");
	printf("\t\t Example: -e 'PRACT=0,PRCHK=GUARD|REFTAG|APPTAG'\n");
	printf("\t\t          -e 'PRACT=1,PRCHK=GUARD'\n");
	printf("\t-F, --zipf <theta> use zipf distribution for random I/O\n");
#ifdef SPDK_CONFIG_URING
	printf("\t-R, --enable-uring enable using liburing to drive kernel devices (Default: libaio)\n");
#endif
	printf("\t--iova-mode <mode> specify DPDK IOVA mode: va|pa\n");
	printf("\t--no-huge, SPDK is run without hugepages\n");
	printf("\t--enforce-numa, SPDK is run with enforce-numa environment flag, useful to enforce NUMA restrictions on huge page allocations\n");
	printf("\t--trace-file <path> replay a packed binary I/O trace (16-byte entries: u64 offset, u32 size, u8 op, 3-byte pad) -- overrides the random/zipf/sequential workload generator. See scripts/bigann/trace_to_binary.py to produce the binary from a canonical CSV.\n");
	printf("\t--vfio-vf-token <token> VF token (UUID) shared between SR-IOV PF and VFs for vfio_pci driver\n");
	spdk_trace_mask_usage(stdout, "-y");
	printf("\n");

	printf("==== PCIe OPTIONS ====\n\n");
	printf("\t-b, --allowed-pci-addr <addr> allowed local PCIe device address\n");
	printf("\t\t Example: -b 0000:d8:00.0 -b 0000:d9:00.0\n");
	printf("\t-V, --enable-vmd enable VMD enumeration\n");
	printf("\t-D, --disable-sq-cmb disable submission queue in controller memory buffer, default: enabled\n");
	printf("\t-E, --enable-interrupt enable interrupts on completion queue, default: disabled\n");
	printf("\n");

	printf("==== TCP OPTIONS ====\n\n");
	printf("\t-S, --default-sock-impl <impl> set the default sock impl, e.g. \"posix\"\n");
	printf("\t--disable-ktls disable Kernel TLS. Only valid for ssl impl. Default for ssl impl\n");
	printf("\t--enable-ktls enable Kernel TLS. Only valid for ssl impl\n");
	printf("\t--tls-version <val> TLS version to use. Only valid for ssl impl. Default: 0 (auto-negotiation)\n");
	printf("\t--psk-path <val> Path to PSK file (only applies when sock_impl == ssl)\n");
	printf("\t--psk-identity <val> Default PSK ID, e.g. psk.spdk.io (only applies when sock_impl == ssl)\n");
	printf("\t--dhchap-key <val> Path to DH-HMAC-CHAP key file (required if controller key is specified)\n");
	printf("\t--dhchap-ctrlr-key <val> Path to DH-HMAC-CHAP controller key file\n");
	printf("\t--zerocopy-threshold <val> data is sent with MSG_ZEROCOPY if size is greater than this val. Default: 0 to disable it\n");
	printf("\t--zerocopy-threshold-sock-impl <impl> specify the sock implementation to set zerocopy_threshold\n");
	printf("\t-z, --disable-zcopy <impl> disable zero copy send for the given sock implementation. Default for posix impl\n");
	printf("\t-Z, --enable-zcopy <impl> enable zero copy send for the given sock implementation\n");
	printf("\t-k, --keepalive <ms> keep alive timeout period in millisecond\n");
	printf("\t-H, --enable-tcp-hdgst enable header digest for TCP transport, default: disabled\n");
	printf("\t-I, --enable-tcp-ddgst enable data digest for TCP transport, default: disabled\n");
	printf("\n");

	printf("==== RDMA OPTIONS ====\n\n");
	printf("\t--transport-tos <val> specify the type of service for RDMA transport. Default: 0 (disabled)\n");
	printf("\t--rdma-srq-size <val> The size of a shared rdma receive queue. Default: 0 (disabled)\n");
	printf("\t-k, --keepalive <ms> keep alive timeout period in millisecond\n");
	printf("\n");

	printf("==== LOGGING ====\n\n");
	printf("\t--log-level <level>   set log level (error, warning, notice, info, debug)\n");
	printf("\t                   (Note: use -T to enable component-specific logs)\n");
	printf("\t-L, --enable-sw-latency-tracking enable latency tracking via sw, default: disabled\n");
	printf("\t\t-L for latency summary, -LL for detailed histogram\n");
	printf("\t-l, --enable-ssd-latency-tracking enable latency tracking via ssd (if supported), default: disabled\n");
	printf("\t-N, --no-shst-notification no shutdown notification process for controllers, default: disabled\n");
	printf("\t-Q, --continue-on-error <val> Do not stop on error. Log I/O errors every N times (default: 1)\n");
	spdk_log_usage(stdout, "\t-T");
	printf("\t-m, --cpu-usage display real-time overall cpu usage on used cores\n");
#ifdef DEBUG
	printf("\t-G, --enable-debug enable debug logging\n");
#else
	printf("\t-G, --enable-debug enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
	printf("\t--transport-stats dump transport statistics\n");
	printf("\n\n");
}

static void
check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	double **cutoff = ctx;

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far / total;
	while (so_far_pct >= **cutoff && **cutoff > 0) {
		printf("%9.5f%% : %9.3fus\n", **cutoff * 100, (double)end * 1000 * 1000 / g_tsc_rate);
		(*cutoff)++;
	}
}

static void
print_bucket(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far * 100 / total;
	printf("%9.3f - %9.3f: %9.4f%%  (%9ju)\n",
	       (double)start * 1000 * 1000 / g_tsc_rate,
	       (double)end * 1000 * 1000 / g_tsc_rate,
	       so_far_pct, count);
}

static void
print_performance(void)
{
	uint64_t total_io_completed, total_io_tsc;
	double io_per_second, mb_per_second, average_latency, min_latency, max_latency;
	double sum_ave_latency, min_latency_so_far, max_latency_so_far;
	double total_io_per_second, total_mb_per_second;
	int ns_count;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;
	uint32_t max_strlen;

	total_io_per_second = 0;
	total_mb_per_second = 0;
	total_io_completed = 0;
	total_io_tsc = 0;
	min_latency_so_far = (double)UINT64_MAX;
	max_latency_so_far = 0;
	ns_count = 0;

	max_strlen = 0;
	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			max_strlen = spdk_max(strlen(ns_ctx->entry->name), max_strlen);
		}
	}

	printf("========================================================\n");
	printf("%*s\n", max_strlen + 60, "Latency(us)");
	printf("%-*s: %10s %10s %10s %10s %10s\n",
	       max_strlen + 13, "Device Information", "IOPS", "MiB/s", "Average", "min", "max");

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (ns_ctx->stats.io_completed != 0) {
				io_per_second = (double)ns_ctx->stats.io_completed * 1000 * 1000 / g_elapsed_time_in_usec;
				mb_per_second = io_per_second * g_io_size_bytes / (1024 * 1024);
				average_latency = ((double)ns_ctx->stats.total_tsc / ns_ctx->stats.io_completed) * 1000 * 1000 /
						  g_tsc_rate;
				min_latency = (double)ns_ctx->stats.min_tsc * 1000 * 1000 / g_tsc_rate;
				if (min_latency < min_latency_so_far) {
					min_latency_so_far = min_latency;
				}

				max_latency = (double)ns_ctx->stats.max_tsc * 1000 * 1000 / g_tsc_rate;
				if (max_latency > max_latency_so_far) {
					max_latency_so_far = max_latency;
				}

				printf("%-*.*s from core %2u: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
				       max_strlen, max_strlen, ns_ctx->entry->name, worker->lcore,
				       io_per_second, mb_per_second,
				       average_latency, min_latency, max_latency);
				total_io_per_second += io_per_second;
				total_mb_per_second += mb_per_second;
				total_io_completed += ns_ctx->stats.io_completed;
				total_io_tsc += ns_ctx->stats.total_tsc;
				ns_count++;
			}
		}
	}

	if (ns_count != 0 && total_io_completed) {
		sum_ave_latency = ((double)total_io_tsc / total_io_completed) * 1000 * 1000 / g_tsc_rate;
		printf("========================================================\n");
		printf("%-*s: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
		       max_strlen + 13, "Total", total_io_per_second, total_mb_per_second,
		       sum_ave_latency, min_latency_so_far, max_latency_so_far);
		printf("\n");
	}

	if (g_latency_sw_tracking_level == 0 || total_io_completed == 0) {
		return;
	}

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			const double *cutoff = g_latency_cutoffs;

			printf("Summary latency data for %-43.43s from core %u:\n", ns_ctx->entry->name, worker->lcore);
			printf("=================================================================================\n");

			spdk_histogram_data_iterate(ns_ctx->histogram, check_cutoff, &cutoff);

			printf("\n");
		}
	}

	if (g_latency_sw_tracking_level == 1) {
		return;
	}

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			printf("Latency histogram for %-43.43s from core %u:\n", ns_ctx->entry->name, worker->lcore);
			printf("==============================================================================\n");
			printf("       Range in us     Cumulative    IO count\n");

			spdk_histogram_data_iterate(ns_ctx->histogram, print_bucket, NULL);
			printf("\n");
		}
	}

}

static void
print_latency_page(struct ctrlr_entry *entry)
{
	int i;

	printf("\n");
	printf("%s\n", entry->name);
	printf("--------------------------------------------------------\n");

	for (i = 0; i < 32; i++) {
		if (entry->latency_page->buckets_32us[i]) {
			printf("Bucket %dus - %dus: %d\n", i * 32, (i + 1) * 32, entry->latency_page->buckets_32us[i]);
		}
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_1ms[i]) {
			printf("Bucket %dms - %dms: %d\n", i + 1, i + 2, entry->latency_page->buckets_1ms[i]);
		}
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_32ms[i])
			printf("Bucket %dms - %dms: %d\n", (i + 1) * 32, (i + 2) * 32,
			       entry->latency_page->buckets_32ms[i]);
	}
}

static void
print_latency_statistics(const char *op_name, enum spdk_nvme_intel_log_page log_page)
{
	struct ctrlr_entry	*ctrlr;

	printf("%s Latency Statistics:\n", op_name);
	printf("========================================================\n");
	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr->ctrlr, log_page, SPDK_NVME_GLOBAL_NS_TAG,
							     ctrlr->latency_page, sizeof(struct spdk_nvme_intel_rw_latency_page), 0,
							     enable_latency_tracking_complete,
							     NULL)) {
				printf("nvme_ctrlr_cmd_get_log_page() failed\n");
				exit(1);
			}

			g_outstanding_commands++;
		} else {
			printf("Controller %s: %s latency statistics not supported\n", ctrlr->name, op_name);
		}
	}

	while (g_outstanding_commands) {
		TAILQ_FOREACH(ctrlr, &g_controllers, link) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
		}
	}

	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			print_latency_page(ctrlr);
		}
	}
	printf("\n");
}

/* End-of-run compute totals (cold): called from the main thread after every worker has joined, so the per-thread
 * blocks are quiescent and compute_ops == compute_per_io x compute_calls holds exactly (check_l). */
static void
ann_compute_print_stats(void)
{
	uint64_t ann_ops = 0, ann_calls = 0;
	uint32_t ann_i, ann_n = __atomic_load_n(&g_ann_tls_n, __ATOMIC_ACQUIRE);

	for (ann_i = 0; ann_i < ann_n && ann_i < ANN_MAX_THREADS; ann_i++) {
		ann_ops += g_ann_tls[ann_i]->ops;
		ann_calls += g_ann_tls[ann_i]->calls;
	}
	printf("\tcompute_stats: compute_ops=%"PRIu64" compute_per_io=%d compute_calls=%"PRIu64"\n",
	       ann_ops, g_compute_per_io < 0 ? 0 : g_compute_per_io, ann_calls);
}

static void
print_stats(void)
{
	ann_compute_print_stats();
	print_performance();
	if (g_latency_ssd_tracking_enable) {
		if (g_rw_percentage != 0) {
			print_latency_statistics("Read", SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
		}
		if (g_rw_percentage != 100) {
			print_latency_statistics("Write", SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY);
		}
	}
}

static int
add_allowed_pci_device(const char *bdf_str, struct spdk_env_opts *env_opts)
{
	int rc;

	if (env_opts->num_pci_addr >= MAX_ALLOWED_PCI_DEVICE_NUM) {
		fprintf(stderr, "Currently we only support allowed PCI device num=%d\n",
			MAX_ALLOWED_PCI_DEVICE_NUM);
		return -1;
	}

	rc = spdk_pci_addr_parse(&env_opts->pci_allowed[env_opts->num_pci_addr], bdf_str);
	if (rc < 0) {
		fprintf(stderr, "Failed to parse the given bdf_str=%s\n", bdf_str);
		return -1;
	}

	env_opts->num_pci_addr++;
	return 0;
}

static size_t
parse_next_key(const char **str, char *key, char *val, size_t key_buf_size,
	       size_t val_buf_size)
{
	const char *sep;
	const char *separator = ", \t\n";
	size_t key_len, val_len;

	*str += strspn(*str, separator);

	sep = strchr(*str, '=');
	if (!sep) {
		fprintf(stderr, "Key without '=' separator\n");
		return 0;
	}

	key_len = sep - *str;
	if (key_len >= key_buf_size) {
		fprintf(stderr, "Key length %zu is greater than maximum allowed %zu\n",
			key_len, key_buf_size - 1);
		return 0;
	}

	memcpy(key, *str, key_len);
	key[key_len] = '\0';

	*str += key_len + 1;	/* Skip key */
	val_len = strcspn(*str, separator);
	if (val_len == 0) {
		fprintf(stderr, "Key without value\n");
		return 0;
	}

	if (val_len >= val_buf_size) {
		fprintf(stderr, "Value length %zu is greater than maximum allowed %zu\n",
			val_len, val_buf_size - 1);
		return 0;
	}

	memcpy(val, *str, val_len);
	val[val_len] = '\0';

	*str += val_len;

	return val_len;
}

static int
parse_metadata(const char *metacfg_str)
{
	const char *str;
	size_t val_len;
	char key[32];
	char val[1024];

	if (metacfg_str == NULL) {
		return -EINVAL;
	}

	str = metacfg_str;

	while (*str != '\0') {
		val_len = parse_next_key(&str, key, val, sizeof(key), sizeof(val));
		if (val_len == 0) {
			fprintf(stderr, "Failed to parse metadata\n");
			return -EINVAL;
		}

		if (strcmp(key, "PRACT") == 0) {
			if (*val == '1') {
				g_metacfg_pract_flag = SPDK_NVME_IO_FLAGS_PRACT;
			}
		} else if (strcmp(key, "PRCHK") == 0) {
			if (strstr(val, "GUARD") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
			}
			if (strstr(val, "REFTAG") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
			}
			if (strstr(val, "APPTAG") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_APPTAG;
			}
		} else {
			fprintf(stderr, "Unknown key '%s'\n", key);
		}
	}

	return 0;
}

static void
free_key(struct spdk_key **key)
{
	if (*key == NULL) {
		return;
	}

	spdk_keyring_put_key(*key);
	spdk_keyring_file_remove_key(spdk_key_get_name(*key));
	*key = NULL;
}

static struct spdk_key *
alloc_key(const char *name, const char *path)
{
	struct spdk_key *key;
	int rc;

	rc = spdk_keyring_file_add_key(name, path);
	if (rc != 0) {
		return NULL;
	}

	key = spdk_keyring_get_key(name);
	if (key == NULL) {
		return NULL;
	}

	return key;
}

#define PERF_GETOPT_SHORT "a:b:c:d:e:ghi:lmo:q:r:k:s:t:v:w:y:z:A:C:DEF:GHILM:NO:P:Q:RS:T:U:VZ:"

static const struct option g_perf_cmdline_opts[] = {
#define PERF_WARMUP_TIME	'a'
	{"warmup-time",			required_argument,	NULL, PERF_WARMUP_TIME},
#define PERF_ALLOWED_PCI_ADDR	'b'
	{"allowed-pci-addr",			required_argument,	NULL, PERF_ALLOWED_PCI_ADDR},
#define PERF_CORE_MASK	'c'
	{"core-mask",			required_argument,	NULL, PERF_CORE_MASK},
#define PERF_METADATA	'e'
	{"metadata",			required_argument,	NULL, PERF_METADATA},
#define PERF_MEM_SINGL_SEG	'g'
	{"mem-single-seg", no_argument, NULL, PERF_MEM_SINGL_SEG},
#define PERF_HELP		'h'
	{"help", no_argument, NULL, PERF_HELP},
#define PERF_SHMEM_GROUP_ID	'i'
	{"shmem-grp-id",			required_argument,	NULL, PERF_SHMEM_GROUP_ID},
#define PERF_ENABLE_SSD_LATENCY_TRACING	'l'
	{"enable-ssd-latency-tracking", no_argument, NULL, PERF_ENABLE_SSD_LATENCY_TRACING},
#define PERF_CPU_USAGE	'm'
	{"cpu-usage", no_argument, NULL, PERF_CPU_USAGE},
#define PERF_IO_SIZE	'o'
	{"io-size",			required_argument,	NULL, PERF_IO_SIZE},
#define PERF_IO_DEPTH	'q'
	{"io-depth",			required_argument,	NULL, PERF_IO_DEPTH},
#define PERF_TRANSPORT	'r'
	{"transport",			required_argument,	NULL, PERF_TRANSPORT},
#define PERF_KEEPALIVE	'k'
	{"keepalive",			required_argument,	NULL, PERF_KEEPALIVE},
#define PERF_HUGEMEM_SIZE	's'
	{"hugemem-size",			required_argument,	NULL, PERF_HUGEMEM_SIZE},
#define PERF_TIME	't'
	{"time",			required_argument,	NULL, PERF_TIME},
#define PERF_NUMBER_IOS	'd'
	{"number-ios",			required_argument,	NULL, PERF_NUMBER_IOS},
#define PERF_IO_PATTERN	'w'
	{"io-pattern",			required_argument,	NULL, PERF_IO_PATTERN},
#define PERF_DISABLE_ZCOPY	'z'
	{"disable-zcopy",			required_argument,	NULL, PERF_DISABLE_ZCOPY},
#define PERF_BUFFER_ALIGNMENT	'A'
	{"buffer-alignment",			required_argument,	NULL, PERF_BUFFER_ALIGNMENT},
#define PERF_MAX_COMPLETIONS_PER_POLL	'C'
	{"max-completion-per-poll",			required_argument,	NULL, PERF_MAX_COMPLETIONS_PER_POLL},
#define PERF_DISABLE_SQ_CMB	'D'
	{"disable-sq-cmb",			no_argument,	NULL, PERF_DISABLE_SQ_CMB},
#define PERF_ENABLE_INTERRUPT	'E'
	{"enable-interrupt",			no_argument,	NULL, PERF_ENABLE_INTERRUPT},
#define PERF_ZIPF		'F'
	{"zipf",				required_argument,	NULL, PERF_ZIPF},
#define PERF_ENABLE_DEBUG	'G'
	{"enable-debug",			no_argument,	NULL, PERF_ENABLE_DEBUG},
#define PERF_ENABLE_TCP_HDGST	'H'
	{"enable-tcp-hdgst",			no_argument,	NULL, PERF_ENABLE_TCP_HDGST},
#define PERF_ENABLE_TCP_DDGST	'I'
	{"enable-tcp-ddgst",			no_argument,	NULL, PERF_ENABLE_TCP_DDGST},
#define PERF_ENABLE_SW_LATENCY_TRACING	'L'
	{"enable-sw-latency-tracking", no_argument, NULL, PERF_ENABLE_SW_LATENCY_TRACING},
#define PERF_RW_MIXREAD	'M'
	{"rwmixread", required_argument, NULL, PERF_RW_MIXREAD},
#define PERF_NO_SHST_NOTIFICATION	'N'
	{"no-shst-notification", no_argument, NULL, PERF_NO_SHST_NOTIFICATION},
#define PERF_IO_UNIT_SIZE	'O'
	{"io-unit-size",			required_argument,	NULL, PERF_IO_UNIT_SIZE},
#define PERF_IO_QUEUES_PER_NS	'P'
	{"num-qpairs", required_argument, NULL, PERF_IO_QUEUES_PER_NS},
#define PERF_CONTINUE_ON_ERROR	'Q'
	{"continue-on-error",			required_argument,	NULL, PERF_CONTINUE_ON_ERROR},
#define PERF_ENABLE_URING	'R'
	{"enable-uring", no_argument, NULL, PERF_ENABLE_URING},
#define PERF_DEFAULT_SOCK_IMPL	'S'
	{"default-sock-impl", required_argument, NULL, PERF_DEFAULT_SOCK_IMPL},
#define PERF_LOG_FLAG	'T'
	{"logflag", required_argument, NULL, PERF_LOG_FLAG},
#define PERF_NUM_UNUSED_IO_QPAIRS	'U'
	{"num-unused-qpairs", required_argument, NULL, PERF_NUM_UNUSED_IO_QPAIRS},
#define PERF_ENABLE_VMD	'V'
	{"enable-vmd", no_argument, NULL, PERF_ENABLE_VMD},
#define PERF_ENABLE_ZCOPY	'Z'
	{"enable-zcopy",			required_argument,	NULL, PERF_ENABLE_ZCOPY},
#define PERF_TRACING_MASK 'y'
	{ "tpoint-group-mask", required_argument, NULL, PERF_TRACING_MASK},
#define PERF_TRANSPORT_STATISTICS	257
	{"transport-stats", no_argument, NULL, PERF_TRANSPORT_STATISTICS},
#define PERF_IOVA_MODE		258
	{"iova-mode", required_argument, NULL, PERF_IOVA_MODE},
#define PERF_IO_QUEUE_SIZE	259
	{"io-queue-size", required_argument, NULL, PERF_IO_QUEUE_SIZE},
#define PERF_DISABLE_KTLS	260
	{"disable-ktls", no_argument, NULL, PERF_DISABLE_KTLS},
#define PERF_ENABLE_KTLS	261
	{"enable-ktls", no_argument, NULL, PERF_ENABLE_KTLS},
#define PERF_TLS_VERSION	262
	{"tls-version", required_argument, NULL, PERF_TLS_VERSION},
#define PERF_PSK_PATH		263
	{"psk-path", required_argument, NULL, PERF_PSK_PATH},
#define PERF_PSK_IDENTITY	264
	{"psk-identity ", required_argument, NULL, PERF_PSK_IDENTITY},
#define PERF_ZEROCOPY_THRESHOLD		265
	{"zerocopy-threshold", required_argument, NULL, PERF_ZEROCOPY_THRESHOLD},
#define PERF_SOCK_IMPL		266
	{"zerocopy-threshold-sock-impl", required_argument, NULL, PERF_SOCK_IMPL},
#define PERF_TRANSPORT_TOS		267
	{"transport-tos", required_argument, NULL, PERF_TRANSPORT_TOS},
#define PERF_RDMA_SRQ_SIZE	268
	{"rdma-srq-size", required_argument, NULL, PERF_RDMA_SRQ_SIZE},
#define PERF_USE_EVERY_CORE	269
	{"use-every-core", no_argument, NULL, PERF_USE_EVERY_CORE},
#define PERF_NO_HUGE		270
	{"no-huge", no_argument, NULL, PERF_NO_HUGE},
#define PERF_DHCHAP_PATH		271
	{"dhchap-key", required_argument, NULL, PERF_DHCHAP_PATH},
#define PERF_DHCHAP_CTRLR_PATH		272
	{"dhchap-ctrlr-key", required_argument, NULL, PERF_DHCHAP_CTRLR_PATH},
#define PERF_VFIO_VF_TOKEN  273
	{"vfio-vf-token", required_argument, NULL, PERF_VFIO_VF_TOKEN},
#define PERF_LOG_LEVEL		274
	{"log-level", required_argument, NULL, PERF_LOG_LEVEL},
#define PERF_ENFORCE_NUMA   275
	{"enforce-numa",			no_argument,	NULL, PERF_ENFORCE_NUMA},
#define PERF_TRACE_FILE		276
	{"trace-file",			required_argument,	NULL, PERF_TRACE_FILE},
#define PERF_HELP_FULL 'v'
	{"help-full", no_argument, NULL, PERF_HELP_FULL},
	/* Should be the last element */
	{0, 0, 0, 0}
};

static int
parse_percent(const char *arg)
{
	char *endptr;
	int percent = strtol(arg, &endptr, 10);

	/* Allow percent values greater than 100 for preconditioning and extended testing */
	if (endptr == arg || percent < 1 || *endptr != '%') {
		return -1;
	}

	return percent;
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, long_idx;
	long int val;
	uint64_t val_u64;
	int rc;
	char *endptr;
	bool ssl_used = false;
	char *sock_impl = "posix";
	uint32_t trid_count = 0;
	bool log_level_set = false;
	bool debug_implied = false;

	while ((op = getopt_long(argc, argv, PERF_GETOPT_SHORT, g_perf_cmdline_opts, &long_idx)) != -1) {
		switch (op) {
		case PERF_WARMUP_TIME:
		case PERF_SHMEM_GROUP_ID:
		case PERF_MAX_COMPLETIONS_PER_POLL:
		case PERF_IO_QUEUES_PER_NS:
		case PERF_KEEPALIVE:
		case PERF_TIME:
		case PERF_RW_MIXREAD:
		case PERF_NUM_UNUSED_IO_QPAIRS:
		case PERF_CONTINUE_ON_ERROR:
		case PERF_RDMA_SRQ_SIZE:
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case PERF_WARMUP_TIME:
				g_warmup_time_in_sec = val;
				break;
			case PERF_SHMEM_GROUP_ID:
				env_opts->shm_id = val;
				break;
			case PERF_MAX_COMPLETIONS_PER_POLL:
				g_max_completions = val;
				break;
			case PERF_IO_QUEUES_PER_NS:
				g_nr_io_queues_per_ns = val;
				break;
			case PERF_KEEPALIVE:
				g_keep_alive_timeout_in_ms = val;
				break;
			case PERF_TIME:
				g_time_in_sec = val;
				break;
			case PERF_RW_MIXREAD:
				g_rw_percentage = val;
				g_mix_specified = true;
				break;
			case PERF_CONTINUE_ON_ERROR:
				g_quiet_count = val;
				g_continue_on_error = true;
				break;
			case PERF_NUM_UNUSED_IO_QPAIRS:
				g_nr_unused_io_queues = val;
				break;
			case PERF_RDMA_SRQ_SIZE:
				g_rdma_srq_size = val;
				break;
			}
			break;
		case PERF_IO_SIZE:
		case PERF_IO_UNIT_SIZE:
		case PERF_ZEROCOPY_THRESHOLD:
		case PERF_BUFFER_ALIGNMENT:
		case PERF_HUGEMEM_SIZE:
		case PERF_NUMBER_IOS:
		case PERF_IO_DEPTH:
		case PERF_IO_QUEUE_SIZE:
			if (op == PERF_NUMBER_IOS && strchr(optarg, '%')) {
				/* Handle percent input for number-ios */
				g_number_ios_percent = parse_percent(optarg);
				if (g_number_ios_percent < 0) {
					fprintf(stderr, "Invalid percent value for --number-ios: %s\n", optarg);
					return 1;
				}
				break;
			}
			rc = spdk_parse_capacity(optarg, &val_u64, NULL);
			if (rc != 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return 1;
			}
			switch (op) {
			case PERF_IO_SIZE:
				g_io_size_bytes = (uint32_t)val_u64;
				break;
			case PERF_IO_UNIT_SIZE:
				g_io_unit_size = (uint32_t)val_u64;
				break;
			case PERF_ZEROCOPY_THRESHOLD:
				g_sock_zcopy_threshold = (uint32_t)val_u64;
				break;
			case PERF_IO_DEPTH:
				g_queue_depth = (uint32_t)val_u64;
				break;
			case PERF_IO_QUEUE_SIZE:
				g_io_queue_size = (uint32_t)val_u64;
				break;
			case PERF_BUFFER_ALIGNMENT:
				g_io_align = (uint32_t)val_u64;
				if (!spdk_u32_is_pow2(g_io_align) || g_io_align < SPDK_CACHE_LINE_SIZE) {
					fprintf(stderr, "Wrong alignment %u. Must be power of 2 and not less than cache lize (%u)\n",
						g_io_align, SPDK_CACHE_LINE_SIZE);
					usage(argv[0]);
					return 1;
				}
				g_io_align_specified = true;
				break;
			case PERF_HUGEMEM_SIZE:
				env_opts->mem_size = (int)val_u64;
				break;
			case PERF_NUMBER_IOS:
				g_number_ios = val_u64;
				break;
			}
			break;
		case PERF_ZIPF:
			errno = 0;
			g_zipf_theta = strtod(optarg, &endptr);
			if (errno || optarg == endptr || g_zipf_theta < 0) {
				fprintf(stderr, "Illegal zipf theta value %s\n", optarg);
				return 1;
			}
			break;
		case PERF_ALLOWED_PCI_ADDR:
			if (add_allowed_pci_device(optarg, env_opts)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case PERF_CORE_MASK:
			env_opts->core_mask = optarg;
			break;
		case PERF_METADATA:
			if (parse_metadata(optarg)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case PERF_MEM_SINGL_SEG:
			env_opts->hugepage_single_segments = true;
			break;
		case PERF_ENABLE_SSD_LATENCY_TRACING:
			g_latency_ssd_tracking_enable = true;
			break;
		case PERF_CPU_USAGE:
			g_monitor_perf_cores = true;
			break;
		case PERF_TRANSPORT:
			if (trid_count == MAX_TRID_ENTRY) {
				fprintf(stderr, "Number of Transport ID specified with -r is limited to %u\n", MAX_TRID_ENTRY);
				return 1;
			}

			rc = spdk_nvme_trid_entry_parse(&g_trids[trid_count].entry, optarg);
			if (rc < 0) {
				usage(argv[0]);
				return 1;
			}

			TAILQ_INSERT_TAIL(&g_trid_list, &g_trids[trid_count], tailq);
			++trid_count;
			break;
		case PERF_IO_PATTERN:
			g_workload_type = optarg;
			break;
		case PERF_DISABLE_SQ_CMB:
			g_disable_sq_cmb = 1;
			break;
		case PERF_ENABLE_INTERRUPT:
			g_enable_interrupt = 1;
			break;
		case PERF_ENABLE_DEBUG:
#ifndef DEBUG
			fprintf(stderr, "%s must be configured with --enable-debug for -G flag\n",
				argv[0]);
			usage(argv[0]);
			return 1;
#else
			SPDK_LOG_DEPRECATED(perf_g_option);
			spdk_log_set_flag("nvme");
			debug_implied = true;
			break;
#endif
		case PERF_ENABLE_TCP_HDGST:
			g_header_digest = 1;
			break;
		case PERF_ENABLE_TCP_DDGST:
			g_data_digest = 1;
			break;
		case PERF_ENABLE_SW_LATENCY_TRACING:
			g_latency_sw_tracking_level++;
			break;
		case PERF_NO_SHST_NOTIFICATION:
			g_no_shn_notification = true;
			break;
		case PERF_ENABLE_URING:
#ifndef SPDK_CONFIG_URING
			fprintf(stderr, "%s must be rebuilt with CONFIG_URING=y for -R flag.\n",
				argv[0]);
			usage(argv[0]);
			return 0;
#endif
			g_use_uring = true;
			break;
		case PERF_LOG_FLAG:
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			debug_implied = true;
#endif
			break;
		case PERF_ENABLE_VMD:
			g_vmd = true;
			break;
		case PERF_DISABLE_KTLS:
			ssl_used = true;
			perf_set_sock_opts("ssl", "ktls", 0, NULL);
			break;
		case PERF_ENABLE_KTLS:
			ssl_used = true;
			perf_set_sock_opts("ssl", "ktls", 1, NULL);
			break;
		case PERF_TLS_VERSION:
			ssl_used = true;
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Illegal tls version value %s\n", optarg);
				return val;
			}
			perf_set_sock_opts("ssl", "tls_version", val, NULL);
			break;
		case PERF_PSK_PATH:
			ssl_used = true;
			free_key(&g_psk);
			g_psk = alloc_key("perf-psk", optarg);
			if (g_psk == NULL) {
				fprintf(stderr, "Unable to set PSK at %s\n", optarg);
				return 1;
			}
			break;
		case PERF_PSK_IDENTITY:
			ssl_used = true;
			perf_set_sock_opts("ssl", "psk_identity", 0, optarg);
			break;
		case PERF_DHCHAP_PATH:
			free_key(&g_dhchap);
			g_dhchap = alloc_key("perf-dhchap", optarg);
			if (g_dhchap == NULL) {
				fprintf(stderr, "Unable to set dhchap at %s\n", optarg);
				return 1;
			}
			break;
		case PERF_DHCHAP_CTRLR_PATH:
			free_key(&g_dhchap_ctrlr);
			g_dhchap_ctrlr = alloc_key("perf-dhchap-ctrlr", optarg);
			if (g_dhchap_ctrlr == NULL) {
				fprintf(stderr, "Unable to set dhchap-ctrl at %s\n", optarg);
				return 1;
			}
			break;
		case PERF_DISABLE_ZCOPY:
			perf_set_sock_opts(optarg, "enable_zerocopy_send_client", 0, NULL);
			break;
		case PERF_ENABLE_ZCOPY:
			perf_set_sock_opts(optarg, "enable_zerocopy_send_client", 1, NULL);
			break;
		case PERF_USE_EVERY_CORE:
			g_use_every_core = true;
			break;
		case PERF_DEFAULT_SOCK_IMPL:
			sock_impl = optarg;
			rc = spdk_sock_set_default_impl(optarg);
			if (rc) {
				fprintf(stderr, "spdk_sock_set_default_impl() failed to set sock impl %s, rc %d: %s\n", optarg, rc,
					spdk_strerror(-rc));
				return 1;
			}
			break;
		case PERF_TRANSPORT_STATISTICS:
			g_dump_transport_stats = true;
			break;
		case PERF_IOVA_MODE:
			env_opts->iova_mode = optarg;
			break;
		case PERF_SOCK_IMPL:
			g_sock_threshold_impl = optarg;
			break;
		case PERF_TRANSPORT_TOS:
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Invalid TOS value\n");
				return 1;
			}
			g_transport_tos = val;
			break;
		case PERF_NO_HUGE:
			env_opts->no_huge = true;
			break;
		case PERF_ENFORCE_NUMA:
			env_opts->enforce_numa = true;
			break;
		case PERF_VFIO_VF_TOKEN:
			g_vf_token = strdup(optarg);
			break;
		case PERF_TRACING_MASK:
			g_tpoint_group_mask = strdup(optarg);
			break;
		case PERF_LOG_LEVEL:
			if (!strcmp(optarg, "error")) {
				g_log_level = SPDK_LOG_ERROR;
			} else if (!strcmp(optarg, "warning")) {
				g_log_level = SPDK_LOG_WARN;
			} else if (!strcmp(optarg, "notice")) {
				g_log_level = SPDK_LOG_NOTICE;
			} else if (!strcmp(optarg, "info")) {
				g_log_level = SPDK_LOG_INFO;
			} else if (!strcmp(optarg, "debug")) {
				g_log_level = SPDK_LOG_DEBUG;
			} else {
				fprintf(stderr, "unknown log level %s\n", optarg);
				return 1;
			}
			log_level_set = true;
			break;
		case PERF_TRACE_FILE:
			g_trace_path = optarg;
			break;
		case PERF_HELP:
			usage_basic(argv[0]);
			return HELP_RETURN_CODE;
		case PERF_HELP_FULL:
			usage(argv[0]);
			return HELP_RETURN_CODE;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_nr_io_queues_per_ns) {
		usage(argv[0]);
		return 1;
	}

	if (!g_queue_depth) {
		fprintf(stderr, "missing -q (--io-depth) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		fprintf(stderr, "missing -o (--io-size) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_io_unit_size || g_io_unit_size % 4) {
		fprintf(stderr, "io unit size can not be 0 or non 4-byte aligned\n");
		return 1;
	}
	if (!g_workload_type) {
		fprintf(stderr, "missing -w (--io-pattern) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		fprintf(stderr, "missing -t (--time) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_quiet_count) {
		fprintf(stderr, "-Q (--continue-on-error) value must be greater than 0\n");
		usage(argv[0]);
		return 1;
	}

	if (strncmp(g_workload_type, "rand", 4) == 0) {
		g_is_random = 1;
		g_workload_type = &g_workload_type[4];
	}

	if (ssl_used && strncmp(sock_impl, "ssl", 3) != 0) {
		fprintf(stderr, "sock impl is not SSL but tried to use one of the SSL only options\n");
		usage(argv[0]);
		return 1;
	}


	if (strcmp(g_workload_type, "read") == 0 || strcmp(g_workload_type, "write") == 0) {
		g_rw_percentage = strcmp(g_workload_type, "read") == 0 ? 100 : 0;
		if (g_mix_specified) {
			fprintf(stderr, "Ignoring -M (--rwmixread) option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	} else if (strcmp(g_workload_type, "rw") == 0) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M (--rwmixread) must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	} else {
		fprintf(stderr,
			"-w (--io-pattern) io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}

	if (g_sock_zcopy_threshold > 0) {
		if (!g_sock_threshold_impl) {
			fprintf(stderr,
				"--zerocopy-threshold must be set with sock implementation specified(--zerocopy-threshold-sock-impl <impl>)\n");
			return 1;
		}

		perf_set_sock_opts(g_sock_threshold_impl, "zerocopy_threshold", g_sock_zcopy_threshold, NULL);
	}

	if ((g_number_ios || g_number_ios_percent) && g_warmup_time_in_sec) {
		fprintf(stderr,
			"-d (--number-ios) with -a (--warmup-time) is not supported\n");
		return 1;
	}

	if (g_number_ios && g_number_ios < g_queue_depth) {
		fprintf(stderr, "-d (--number-ios) less than -q (--io-depth) is not supported\n");
		return 1;
	}

	if (g_rdma_srq_size != 0) {
		struct spdk_nvme_transport_opts opts;

		spdk_nvme_transport_get_opts(&opts, sizeof(opts));
		opts.rdma_srq_size = g_rdma_srq_size;

		rc = spdk_nvme_transport_set_opts(&opts, sizeof(opts));
		if (rc != 0) {
			fprintf(stderr, "Failed to set NVMe transport options.\n");
			return 1;
		}
	}

	if (TAILQ_EMPTY(&g_trid_list)) {
		/* If no transport IDs specified, default to enumerating all local PCIe devices */
		rc = spdk_nvme_trid_entry_parse(&g_trids[trid_count].entry, "trtype:PCIe");
		if (rc < 0) {
			return 1;
		}

		TAILQ_INSERT_TAIL(&g_trid_list, &g_trids[trid_count], tailq);
	} else {
		struct _trid_entry *trid_entry;

		env_opts->no_pci = true;
		/* check whether there is local PCIe type */
		TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
			if (trid_entry->entry.trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
				env_opts->no_pci = false;
				break;
			}
		}
	}

	/*
	 * If the user didn't explicitly set a log level, but used -G or -T,
	 * default to DEBUG to preserve legacy behavior.
	 */
	if (!log_level_set && debug_implied) {
		g_log_level = SPDK_LOG_DEBUG;
	}

	g_file_optind = optind;

	/* Trace replay (paper §4.1 workload-realism evidence). Load once,
	 * after argument parsing has validated the rest of the options. The
	 * trace file is the .bin produced by scripts/bigann/trace_to_binary.py. */
	if (g_trace_path) {
		if (load_trace_file(g_trace_path) != 0) {
			return 1;
		}
		/* Trace is read-only; force the read-percentage knobs to a
		 * consistent state so the random/zipf paths are inert. */
		g_rw_percentage = 100;
	}

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		TAILQ_INIT(&worker->ns_ctx);
		worker->lcore = i;
		worker->idx = g_num_workers;
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *tmp_worker;
	struct ns_worker_ctx *ns_ctx, *tmp_ns_ctx;

	/* Free namespace context and worker thread */
	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);

		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx) {
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			spdk_histogram_data_free(ns_ctx->histogram);
			free(ns_ctx->trace_fast_tbl);
			free(ns_ctx);
		}

		free(worker);
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_trid_entry *trid_entry = cb_ctx;

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		if (g_disable_sq_cmb) {
			opts->use_cmb_sqs = false;
		}
		if (g_no_shn_notification) {
			opts->no_shn_notification = true;
		}
		if (g_enable_interrupt) {
			opts->enable_interrupts = true;
		}
	}

	if (trid->trtype != trid_entry->trid.trtype &&
	    strcasecmp(trid->trstring, trid_entry->trid.trstring)) {
		return false;
	}

	opts->io_queue_size = g_io_queue_size;

	/* Set the header and data_digest */
	opts->header_digest = g_header_digest;
	opts->data_digest = g_data_digest;
	opts->keep_alive_timeout_ms = g_keep_alive_timeout_in_ms;
	opts->tls_psk = g_psk;
	opts->dhchap_key = g_dhchap;
	opts->dhchap_ctrlr_key = g_dhchap_ctrlr;
	memcpy(opts->hostnqn, trid_entry->hostnqn, sizeof(opts->hostnqn));

	opts->transport_tos = g_transport_tos;
	if (opts->num_io_queues < g_num_workers * g_nr_io_queues_per_ns) {
		opts->num_io_queues = g_num_workers * g_nr_io_queues_per_ns;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		if (g_enable_interrupt && !opts->enable_interrupts) {
			fprintf(stderr, "Couldn't enable interrupts on NVMe controller at %s\n", trid->traddr);
			return;
		}
	}

	register_ctrlr(ctrlr, cb_ctx);
}

static int
register_controllers(void)
{
	struct _trid_entry *trid_entry;

	printf("Initializing NVMe Controllers\n");

	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid_entry->entry.trid, &trid_entry->entry, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid_entry->entry.trid.traddr);
			return -1;
		}
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, entry, link);

		spdk_dma_free(entry->latency_page);
		if (g_latency_ssd_tracking_enable &&
		    spdk_nvme_ctrlr_is_feature_supported(entry->ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
			set_latency_tracking_feature(entry->ctrlr, false);
		}

		if (g_nr_unused_io_queues) {
			int i;

			for (i = 0; i < g_nr_unused_io_queues; i++) {
				spdk_nvme_ctrlr_free_io_qpair(entry->unused_qpairs[i]);
			}

			free(entry->unused_qpairs);
		}

		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	if (g_vmd) {
		spdk_vmd_fini();
	}
}

static int
allocate_ns_worker(struct ns_entry *entry, struct worker_thread *worker)
{
	struct ns_worker_ctx	*ns_ctx;

	ns_ctx = calloc(1, sizeof(struct ns_worker_ctx));
	if (!ns_ctx) {
		return -1;
	}

	printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
	ns_ctx->stats.min_tsc = UINT64_MAX;
	ns_ctx->entry = entry;
	ns_ctx->worker = worker;
	if (g_trace_entries && g_num_workers > 0) {
		/* Trace is loaded and g_num_workers is final here. */
		uint64_t worker_index = 0;
		struct worker_thread *w;

		TAILQ_FOREACH(w, &g_workers, link) {
			if (w == worker) {
				break;
			}
			worker_index++;
		}
		ns_ctx->trace_cursor = worker_index * (g_trace_count / g_num_workers);
		ns_ctx->trace_check_cursor = worker_index * (g_trace_count / g_num_workers);
	}
	ns_ctx->histogram = spdk_histogram_data_alloc();
	if (g_number_ios_percent > 0) {
		ns_ctx->number_ios = entry->size_in_ios * g_number_ios_percent / 100;
		printf("number_ios for namespace %s set to %lu (%d%% of namespace size)\n",
		       entry->name, ns_ctx->number_ios, g_number_ios_percent);
	} else {
		ns_ctx->number_ios = g_number_ios;
	}
	TAILQ_INSERT_TAIL(&worker->ns_ctx, ns_ctx, link);

	return 0;
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = TAILQ_FIRST(&g_namespaces);
	struct worker_thread	*worker = TAILQ_FIRST(&g_workers);
	int			i, count;

	/* Each core contains single worker, and namespaces are associated as follows:
	 * --use-every-core not specified (default):
	 * 2) equal workers and namespaces - each worker associated with single namespace
	 * 3) more workers than namespaces - each namespace is associated with one or more workers
	 * 4) more namespaces than workers - each worker is associated with one or more namespaces
	 * --use-every-core option enabled - every worker is associated with all namespaces
	 */
	if (g_use_every_core) {
		TAILQ_FOREACH(worker, &g_workers, link) {
			TAILQ_FOREACH(entry, &g_namespaces, link) {
				if (allocate_ns_worker(entry, worker) != 0) {
					return -1;
				}
			}
		}
		return 0;
	}

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		if (allocate_ns_worker(entry, worker) != 0) {
			return -1;
		}

		worker = TAILQ_NEXT(worker, link);
		if (worker == NULL) {
			worker = TAILQ_FIRST(&g_workers);
		}

		entry = TAILQ_NEXT(entry, link);
		if (entry == NULL) {
			entry = TAILQ_FIRST(&g_namespaces);
		}

	}

	return 0;
}

static void *
nvme_poll_ctrlrs(void *arg)
{
	struct ctrlr_entry *entry;
	int oldstate;
	int rc;

	spdk_unaffinitize_thread();

	if (g_tpoint_group_mask) {
		spdk_trace_register_user_thread();
	}

	while (true) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

		TAILQ_FOREACH(entry, &g_controllers, link) {
			if (entry->trtype != SPDK_NVME_TRANSPORT_PCIE) {
				rc = spdk_nvme_ctrlr_process_admin_completions(entry->ctrlr);
				if (spdk_unlikely(rc < 0 && !g_exit)) {
					g_exit = true;
				}
			}
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

static void
sig_handler(int signo)
{
	g_exit = true;
}

static int
setup_sig_handlers(void)
{
	struct sigaction sigact = {};
	int rc;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_handler = sig_handler;
	rc = sigaction(SIGINT, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGINT) failed, errno %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGTERM) failed, errno %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	return 0;
}

static int
setup_spdk_tracing(const char *app_name, const char *tpoint_group_mask, int num_threads)
{
	struct spdk_app_opts app_opts;

	if (!tpoint_group_mask) {
		return 0;
	}

	spdk_app_opts_init(&app_opts, sizeof(app_opts));
	app_opts.name = app_name;
	app_opts.num_trace_threads = num_threads;
	app_opts.tpoint_group_mask = tpoint_group_mask;

	return spdk_app_setup_trace(&app_opts);
}

static void
free_globals(void)
{
	free(g_vf_token);
	free(g_tpoint_group_mask);

	free_key(&g_psk);
	free_key(&g_dhchap);
	free_key(&g_dhchap_ctrlr);
}

int
main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker, *main_worker;
	struct ns_worker_ctx *ns_ctx;
	struct spdk_env_opts opts;
	pthread_t thread_id = 0;

	/* Use the runtime PID to set the random seed */
	srand(getpid());

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "perf";
	opts.pci_allowed = g_allowed_pci_addr;
	rc = parse_args(argc, argv, &opts);
	if (rc != 0 || rc == HELP_RETURN_CODE) {
		free_globals();
		if (rc == HELP_RETURN_CODE) {
			return 0;
		}

		goto out;
	}

	/* Parse once, here, so the per-IO path only tests a bool. */
	{
		const char *tf_env = getenv("SPDK_PERF_TRACE_FAST");
		const char *tfc_env = getenv("SPDK_PERF_TRACE_FAST_CHECK");

		g_perf_trace_fast = tf_env != NULL && tf_env[0] != '\0' &&
				    strtoll(tf_env, NULL, 10) == 1;
		g_perf_trace_fast_check = tfc_env != NULL && tfc_env[0] != '\0' &&
					  strtoll(tfc_env, NULL, 10) == 1;
		if (g_perf_trace_fast_check) {
			g_perf_trace_fast = true;
		}
	}

	/* SPDK_PERF_WORKER_GROUP=1 (IAU-next P3): one poll group per worker for all of its ns_ctx. Polling only:
	 * the vfio-user transport has no poll_group_check_disconnected_qpairs for spdk_nvme_poll_group_wait(). */
	{
		const char *wg_env = getenv("SPDK_PERF_WORKER_GROUP");

		g_worker_group = wg_env != NULL && wg_env[0] != '\0' && strtoll(wg_env, NULL, 10) == 1;
		if (g_worker_group && g_enable_interrupt) {
			fprintf(stderr, "SPDK_PERF_WORKER_GROUP=1 needs polling mode (no -E)\n");
			free_globals();
			rc = -1;
			goto out;
		}
		if (g_worker_group) {
			printf("PERF_WORKER_GROUP enabled\n");
		}
	}

	/* SPDK_PERF_FAULT=1 (IAU-next P4.12): the fault mode's knobs; refused combinations end here */
	if (perf_fault_init() != 0) {
		free_globals();
		rc = -1;
		goto out;
	}

	/* Transport statistics are printed from each thread.
	 * To avoid mess in terminal, init and use mutex */
	rc = pthread_mutex_init(&g_stats_mutex, NULL);
	if (rc != 0) {
		fprintf(stderr, "Failed to init mutex\n");
		free_globals();
		goto out;
	}

	opts.vf_token = g_vf_token;
	rc = spdk_env_init(&opts);
	if (rc < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		pthread_mutex_destroy(&g_stats_mutex);
		free_globals();
		goto out;
	}

	rc = spdk_keyring_init();
	if (rc != 0) {
		fprintf(stderr, "Unable to initialize keyring: %s\n", spdk_strerror(-rc));
		pthread_mutex_destroy(&g_stats_mutex);
		free_globals();
		spdk_env_fini();
		goto out;
	}

	spdk_log_open(NULL);
	spdk_log_set_print_level(g_log_level);
	spdk_log_set_level(g_log_level);

	rc = setup_sig_handlers();
	if (rc != 0) {
		goto cleanup;
	}

	g_tsc_rate = spdk_get_ticks_hz();

	rc = register_workers();
	if (rc != 0) {
		goto cleanup;
	}

#if defined(HAVE_LIBAIO) || defined(SPDK_CONFIG_URING)
	rc = register_files(argc, argv);
	if (rc != 0) {
		goto cleanup;
	}
#endif

	rc = register_controllers();
	if (rc != 0) {
		goto cleanup;
	}

	if (g_warn) {
		printf("WARNING: Some requested NVMe devices were skipped\n");
	}

	/* check controllers; no controllers is ok when running with AIO/URING devices only */
	if ((g_num_async_devs == 0) && (TAILQ_EMPTY(&g_controllers))) {
		fprintf(stderr, "No valid NVMe controllers found\n");
		goto cleanup;
	}

	if (g_num_namespaces == 0) {
		if (g_num_async_devs == 0) {
			fprintf(stderr, "No valid AIO or URING devices found\n");
		} else {
			fprintf(stderr, "No active namespaces found\n");
		}

		goto cleanup;
	}

	if (g_fault != NULL && perf_fault_setup() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_num_workers > 1 && g_quiet_count > 1) {
		fprintf(stderr, "Error message rate-limiting enabled across multiple threads.\n");
		fprintf(stderr, "Error suppression count may not be exact.\n");
	}

	rc = setup_spdk_tracing("spdk_nvme_perf", g_tpoint_group_mask, g_num_workers);
	if (rc != 0) {
		goto cleanup;
	}

	rc = pthread_create(&thread_id, NULL, g_fault != NULL ? &perf_fault_poll_ctrlrs : &nvme_poll_ctrlrs, NULL);
	if (rc != 0) {
		fprintf(stderr, "Unable to spawn a thread to poll admin queues.\n");
		goto cleanup;
	}

	rc = associate_workers_with_ns();
	if (rc != 0) {
		goto cleanup;
	}

	if (g_fault != NULL && perf_fault_attach() != 0) {
		rc = -1;
		goto cleanup;
	}

	rc = pthread_barrier_init(&g_worker_sync_barrier, NULL, g_num_workers);
	if (rc != 0) {
		fprintf(stderr, "Unable to initialize thread sync barrier\n");
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the secondary workers */
	g_main_core = spdk_env_get_current_core();
	main_worker = NULL;
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore != g_main_core) {
			spdk_env_thread_launch_pinned(worker->lcore, g_worker_group ? work_fn_worker_group : work_fn,
						      worker);
		} else {
			assert(main_worker == NULL);
			main_worker = worker;
		}
	}

	assert(main_worker != NULL);
	if (g_worker_group) {
		work_fn_worker_group(main_worker);
	} else {
		work_fn(main_worker);
	}

	spdk_env_thread_wait_all();

	if (g_fault != NULL) {
		perf_fault_print_end();
	}

	print_stats();

	pthread_barrier_destroy(&g_worker_sync_barrier);

cleanup:
	fflush(stdout);

	if (thread_id && pthread_cancel(thread_id) == 0) {
		pthread_join(thread_id, NULL);
	}

	/* Collect errors from all workers and namespaces */
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (rc != 0) {
			break;
		}

		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (ns_ctx->status != 0) {
				rc = ns_ctx->status;
				break;
			}
		}
	}

	unregister_namespaces();
	unregister_controllers();
	unregister_workers();

	free_globals();

	spdk_keyring_cleanup();
	spdk_log_close();
	spdk_env_fini();

	pthread_mutex_destroy(&g_stats_mutex);

out:
	if (rc != 0) {
		fprintf(stderr, "%s: errors occurred\n", argv[0]);
	}

	return rc;
}
