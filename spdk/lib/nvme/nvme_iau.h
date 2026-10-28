/*   SPDX-License-Identifier: BSD-3-Clause
 *   IAU host helpers shared by the IAU completion paths in lib/nvme: the F3 tracker-free Mode D
 *   poll (nvme_pcie_md_poll(), nvme_pcie_common.c) and the IAU host interface tile (nvme_iau.c, IAU-next
 *   P4; its declarations are at the end of this file).
 */

#ifndef __NVME_IAU_H__
#define __NVME_IAU_H__

#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include "spdk_internal/iau_tile_abi.h"

/* What an IAU completion path does with a completion whose status is an error. */
enum nvme_iau_cpl_action {
	NVME_IAU_CPL_DELIVER = 0,	/* complete the request: nvme_complete_request() */
	NVME_IAU_CPL_RETRY = 1,		/* re-issue the command (caller: retries++, re-issue); no callback */
};

/*
 * The completion-error policy of nvme_pcie_qpair_complete_tracker() (the stock PCIe completion)
 * for a completion an IAU path received from the device, whatever its status (call it for every
 * completion the path checks, as complete_tracker runs it for every CQE):
 * - print exactly when it prints: the command and the completion for an error when print_on_error
 *   is set and the controller's error logging is on; the completion alone, success or error, when
 *   only the "nvme" debug log flag is on;
 * - retry exactly when it retries: an error nvme_completion_is_retry() accepts (e.g. NAMESPACE NOT
 *   READY with DNR = 0) while retries < retry_count (the qpair's transport_retry_count).
 * A success always returns DELIVER.  The caller acts on the result: RETRY = increment
 * req->retries and re-issue on the same slot; DELIVER = nvme_complete_request(), which also
 * applies completion-side error injection (stock does that after this decision, so an injected
 * error is never retried or printed).  The policy has no transport knowledge, so the IAU tile path
 * (P4) shares it.
 */
static inline enum nvme_iau_cpl_action
nvme_iau_cpl_error_policy(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd,
			  uint8_t retries, uint8_t retry_count, struct spdk_nvme_cpl *cpl,
			  bool print_on_error)
{
	bool error = spdk_nvme_cpl_is_error(cpl);
	bool retry = error && nvme_completion_is_retry(cpl) && retries < retry_count;
	bool print_error = error && print_on_error && !qpair->ctrlr->opts.disable_error_logging;

	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, cmd);
	}
	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		spdk_nvme_qpair_print_completion(qpair, cpl);
	}
	return retry ? NVME_IAU_CPL_RETRY : NVME_IAU_CPL_DELIVER;
}

/*
 * Order a completion record's poll-word load before the loads of its other words on weakly
 * ordered ISAs, exactly as the stock CQ loop orders the CQE phase load before its tracker and cid
 * loads (nvme_pcie_qpair_process_completions_stock()).  The record words are volatile and x86
 * keeps load order (TSO), so this is empty on x86: the x86 code does not change.
 */
static inline __attribute__((always_inline)) void
nvme_iau_rec_rmb(void)
{
#if defined(__PPC64__) || defined(__riscv) || defined(__loongarch__)
	spdk_mb();
#elif defined(__aarch64__)
	__asm volatile("dmb oshld" ::: "memory");
#endif
}

/*
 * ------------------------------------------------------------------------------------------------------------
 * IAU host interface tile (IAU-next P4; nvme_iau.c).  Plan 2026-10-17-iau-next-p4.md rev 2 D2, D4, D6, D8, D9,
 * D11, D13, D18 as amended by A1, A4, A6, A15, A18; the wire formats are spdk/include/spdk_internal/iau_tile_abi.h
 * (the one host == target definition); the target validates what is described here in
 * spdk/lib/nvmf/vfio_user_iau_tile_bind.c (iau_tile_queue_decide(), iau_tile_gcb_check(), iau_tile_gcb_fresh()).
 *
 * Selection (D2).  The vfio-user transport registers the table that nvme_iau_select_ops() fills once per
 * process, before the registration copies it (nvme_vfio_user.c): vfio_ops byte for byte unless SPDK_IAU_TILE=1,
 * else vfio_ops overridden by nvme_iau_override_ops().  An S1 / I1 / I1e process runs the stock functions; a
 * tile process reaches the tile code through the transport ops only (no test in a stock entry point).
 *
 * Configuration (nvme_iau_cfg_load(), once per process; switches are exactly "1"):
 *   SPDK_IAU_TILE=1             IAU tile on every IO qpair (harness mode 6 = target IAU_UNCORE_MODE=5 + this)
 *   SPDK_IAU_DESC_STRIDE      64 (default) | 48 (lever I1v6n48: the six host words packed; RAW refused)
 *   SPDK_IAU_RCONS=1          keep GCB ring[k].r_consumed (the engine's r_overflow tripwire; tests and fault
 *                             points), stored before each d_tail store
 *   SPDK_IAU_GROUP=1          group rings (P4.8, below): needs SPDK_IAU_TILE=1, refused with the 48 B stride
 *   SPDK_IAU_DB_BATCH 0..65535 (default NVME_IAU_DB_BATCH_DEFAULT; 0: off): the group pass's doorbell policy
 *                             (P4.8e, below); a non-zero value needs SPDK_IAU_TILE=1 and SPDK_IAU_GROUP=1, 0 is
 *                             accepted anywhere (without the group the knob is 0: IAU_TILE_OPS prints db_batch=0)
 *   SPDK_IAU_RINGS 1..4, SPDK_IAU_RING_ENTRIES 64..65535 (default NVME_IAU_RING_ENTRIES_DEFAULT): the K ring
 *                             pairs of a group and their entries
 *   SPDK_IAU_FAST=1           the request-free fast path (P4.9, below): needs SPDK_IAU_TILE=1, refused with the 48 B
 *                             stride
 *   SPDK_IAU_S2=1 (P4.11: the S2 ablation, nvme_iau_s2.c, below) excludes SPDK_IAU_TILE=1; refused with tile:
 *                             SPDK_UNCORE_MODE_C / _D / _E=1 and SPDK_IAU_ENGINE=1 (D1: mode 6 sets none of them;
 *                             host Mode E's table 0x5800 + 8 q borders QCFG2)
 *   A bad value or combination aborts the process at registration (IAU_TILE_CONFIG_ERROR).
 *
 * P4.5: a tile IO qpair outside a tile poll group owns a PRIVATE group of one (D4): its own GCB (K = 1, n_routes = 1,
 * route 0), one D ring and one R ring of iau_tile_private_entries(credits) = round64(3c + 4) entries each (A1), and
 * its echo slab.
 *
 * P4.8 group rings (SPDK_IAU_GROUP=1: the poll-group ops are overridden). A poll group owns one SHARED group: one
 * GCB, K ring pairs of SPDK_IAU_RING_ENTRIES entries, 256 routes. A tile qpair joins at poll_group_add (before its
 * first connect; a non-tile qpair is refused -EINVAL): the lowest route that is free and not held, ring route mod K,
 * the next member gen of (GCB, route), the D13 invariant per ring (else IAU_TILE_JOIN_REFUSED, -ENOSPC). P4.10f (A23,
 * the equal-credit join rule): the group has one credit count, its first member's, fixed at its creation; a member
 * whose own credits (min(entries - max_completions_cap, io_queue_requests)) are more takes the group's (credits=
 * in IAU_TILE_QPAIR), one whose credits are fewer is refused (IAU_TILE_JOIN_REFUSED reason=credits, -EINVAL), so every
 * route block of the GCB's ctx region has one size and first fit never fragments it. The
 * target binds the ring at the first member's Create I/O SQ (fresh positions) and unbinds it at the last one's
 * detach (CLOSED R_DETACHED); the host resets a closed ring for its next member.
 *   group poll   one pass over the K rings with the tgroup's in_completion_context set (route demux by
 *                route_qp[route]; no per-member loop), the sweep every 64 group or direct polls (members that
 *                need generic duties go on the attention list), the attention pass when that list is not
 *                empty, then <= K doorbells on the poll group's stats. Returns the pass's deliveries, or -ENXIO.
 *                P4.8e: the pass runs in rounds; a round takes at most db_batch records from every ring with
 *                budget left (ring order); after a round in which a ring returned a full chunk the dirty
 *                doorbells are rung (the caller's stats), so the descriptors callbacks write during a long pass
 *                reach the device then, not at its end (the convoy, p4a3_dq.md), and no ring runs ahead of the
 *                others (fix round, I1): a ready record waits <= (K - 1) x db_batch records of the other rings, a
 *                written descriptor < K x db_batch records for its doorbell. Doorbells per busy pass:
 *                <= K + K x floor(records / db_batch). db_batch 0: one round, the doorbells at the pass end only.
 *   attention pass  the list is taken as a local list (A3: a delete during the pass unlinks from the list that
 *                holds the member); per member, from the head: a delete deferred by a nested delete op (I1) runs
 *                now; a free a callback asked for (delete_after_completion_context) is carried out with
 *                spdk_nvme_ctrlr_free_io_qpair(); members not CONNECTING / CONNECTED / ENABLING / ENABLED are
 *                skipped (C4: stock's disconnected loop serves them); the others get one generic call
 *                (spdk_nvme_qpair_process_completions(m, 0): check_enabled, the connect state machine,
 *                resubmission of queued requests with the free credits as budget). Nothing of a member is touched
 *                after its generic call deleted it or after disconnected_qpair_cb; a delete inside the pass defers
 *                nvme_pcie_qpair_destroy() to the pass end (A2).
 *   R3           a direct poll of a member consumes the group's rings (every member's records), runs the sweep
 *                counter and the attention pass, and returns the member's own deliveries; nested inside a pass of
 *                its group it returns 0.
 *   R4           a called member whose generic call fails (a transport failure while ENABLED, a failed
 *                asynchronous connect) goes to disconnected_qpair_cb once and that group poll returns -ENXIO; a
 *                failure seen by a direct poll is reported by the next group poll only while the member is still on
 *                the connected list (I2); members removed from the poll group are never reported by it.
 *   deferred delete  a free inside a ring pass is deferred (nvme_ctrlr.c tests the poll group's
 *                in_completion_context; a member removed from its poll group runs its cold-path callbacks in its
 *                own completion context); it is found per record after its callback, after every cold-path
 *                callback, or by the sweep (another member's free: DV22, <= 64 polls) and carried out by the
 *                attention pass. A delete op nested in a ring pass defers its drain, Delete I/O SQ / CQ and destroy
 *                (DV23): they, and frees a flag deferred (delete_after_completion_context), run under the
 *                controller lock at the end of the enclosing top-level shared delete or drain if there is one,
 *                else in the next attention pass (the window left: a free nested in a group or direct poll
 *                completes at that poll's end, after spdk_nvme_ctrlr_free_io_qpair() returned and freed the qid;
 *                the qpair's outstanding commands complete, aborted, then). A deferred delete of another
 *                controller's member may complete at the next attention pass when that controller's lock is
 *                contended (its lock is only tried there: no two controller locks nest in a blocking order). A
 *                reconnect nested in a pass of its group returns -EBUSY before any drain (never LOST, n2).
 *   leave        drain until GONE, nothing in flight and GCB d_head past the route's last written slot (or its
 *                ring closed); Delete I/O CQ releases the route's reservation, else the route stays held until the
 *                ring resets. A shared ring's CLOSED R_DETACHED ends the leftovers of every route still on it and
 *                closes the ring (also when its member already left); a LOST member taints its ring (the group's
 *                memory is kept) only while the ring may still be bound, until that CLOSED.
 *   evidence     IAU_TILE_GROUP gid= k= entries= stride= private=0 (at the first join), IAU_TILE_QPAIR ... private=0,
 *                IAU_TILE_GROUP_END gid= records= stray= polls= doorbells= kept= idle_polls= sweeps= attn_calls= at
 *                every member leave with the group's cumulative counters (the last line per gid holds the totals),
 *                IAU_TILE_JOIN_REFUSED (reason connected / routes / not_tile / credits, or the invariant's need_d /
 *                need_r; gid=none: the first member alone does not fit a ring, no group is created),
 *                IAU_TILE_RESET_REFUSED reason=thread | nested, IAU_TILE_RESET_DEFERRED (n4, below).
 *   trtype       tile is vfio-user only (IAU_TILE_REFUSED at create otherwise): A5 (qpair_abort_reqs only for a removed
 *                controller) and R4 / I2 (a failed member moves to the disconnected list) rely on stock's
 *                check_enabled() for a non-PCIe trtype; for PCIe it aborts at CONNECTED -> ENABLING
 *                (nvme_qpair.c:951) and never disconnects.
 *   reset (n4)   stock's reset reinitializes every IO qpair from the resetting context
 *                (spdk_nvme_ctrlr_reconnect_poll_async()); a shared member's reconnect there (is_resetting) is
 *                deferred to its own poll as stock defers a foreign process's qpair: FAILURE_RESET, CONNECTED, rc 0
 *                (IAU_TILE_RESET_DEFERRED); its generic poll (the sweep's attention) then enables, disconnects and
 *                reinitializes it on the group's thread. P4.10d (P4.10a review I1): that reinitialize, reached nested
 *                in a pass of the group after the reset (a callback resubmits on the member: the generic submit's
 *                check_enabled()), is deferred again (IAU_TILE_RESET_DEFERRED ... nested=1, rc 0, CONNECTED with
 *                FAILURE_RESET, the attention list) instead of the nested -EBUSY below; iq->reset_deferred marks the
 *                member until its rejoin. The owner-thread -EBUSY (DV26) stays for a reconnect
 *                outside a reset: pthread_self() is the identity of the hazard it guards (two OS threads consuming
 *                one group's rings); lib/nvme has no spdk_thread (no lib/thread dependency; perf has none), and a
 *                poll group whose spdk_thread migrated between reactors gets a false -EBUSY without side effects.
 *
 * Registration (before Create I/O CQ / SQ, at every (re)connect): the GCB member line (valid last: ring =
 * route mod K, qid, credits, nsid, timeout_us, entries = the SSD SQ size, echo IOVA (64 B-aligned), lba_shift,
 * member gen), QCFG[qid] = nvme_pcie_iau_qcfg_pack(credits, nsid), QCFG2[qid] = iau_tile_qcfg2_pack(GCB IOVA,
 * route, gen); a new or re-registered ring has GCB d_head = db d_tail = 0 (and r_consumed = 0 with RCONS).
 * One SQ per CQ (the qpair's own).  sq_tdbl and cq_hdbl point at the CQ sink (IAU page 0x5300 + 4 q): the
 * host never writes the SSD doorbells (and never the sink: the engine flags a non-zero sink, host_ssd_db).
 *
 * Credits (D13).  IO credits = min(num_trackers, io_queue_requests), counted in qpair->queue_depth (engine
 * slots in use == host IO credits in use); control credits (ABORT_*, P4.10a) in ctrl_inflight.
 * Request-backed IO (every IO in P4.5): d5 = nvme_iau_req_done, d6 = the request, req->cmd.cid = the host's
 * in-flight index (inflight[], idx_stack); the record's cpl carries sqid = route and cid = the engine's tag.
 * IO kind iff the payload is contiguous, has no metadata buffer and iau_tile_cmd_is_io() (nvme_iau_req_is_io());
 * anything else is RAW (two slots: a header and the host-built SQE).  P4.7e: the submit tests an unfused READ /
 * WRITE with nvme_iau_cmd_rw_fast() first (a subset of iau_tile_cmd_is_io(), so the same classification and the
 * same descriptor) and runs the full test only when that fails.  queue_depth < credits implies a free index
 * (every index in use is a command counted in queue_depth), so the submit tests the credit alone.  P4.5's RAW
 * covers contiguous (<= 128 KiB, PRP list in the in-flight index's tracker list page) and data-less commands;
 * SGL payloads and metadata return -ENOTSUP until P4.10.
 *
 * Records (D6).  The consume loop loads w3 then w2 with iau_tile_rec_peek() and takes a record on the fast path
 * only if iau_tile_rec_fast() holds (phase, SC = SCT = 0, cdw1 = 0).  P4.7e: a fast request-backed record completes
 * inline (nvme_pcie_md_poll()'s sequence, stock's free-then-callback order; err_cmd_head tested once per pass,
 * nvme_iau_req_done() while an injection is armed; see consume()).  Everything else goes to the cold path, which
 * dispatches on kind and rflags only (never on status == MARK) and masks cdw1 (iau_tile_rec_cpl()):
 *   ACK              ctrl_inflight--, the next queued control goes out, the ack callback (P4.10a)
 *   TIMEOUT          timeout_cb_fn(arg, ctrlr, qpair, tag) if the controller is READY (D9)
 *   DETACHED         route GONE; every pending entry delivered as ABORTED_SQ_DELETION (D8); if the route was
 *                    ACTIVE (a target-initiated detach) the qpair is FAILED
 *   ABORTED / final  pending entry cleared, delivered (an error prints by the error policy's print rule)
 *   CPL + PARKED     route flushed: pending entry, no disposition (FLUSH's final record delivers it, D8);
 *                    else nvme_iau_cpl_error_policy(): IO kind RETRY = REISSUE + pending entry, no callback;
 *                    RAW RETRY = RELEASE + a new RAW from req->cmd (req->retries++); DELIVER = RELEASE, then
 *                    the callback.  RELEASE always precedes the callback in D.  Fused commands never park
 *                    (DV21: the engine delivers their errors as final).
 * The echo slab (entries x 64 B, the parked IO-kind SQE the engine rebuilt) is for request-free IO (P4.9);
 * its PRP2 beyond two pages is an ENGINE address (the engine's per-slot PRP list): the host never uses it.
 *
 * Route states (iq->state): IDLE (created) -> JOINED (registration published) -> ACTIVE (the target bound
 * it: Create I/O SQ succeeded) -> FLUSHING (the host wrote its one FLUSH) -> GONE (R_DETACHED consumed);
 * LOST (a 1 s drain timeout, IAU_TILE_LOST) is terminal: no rejoin, no Admin Delete I/O CQ. P4.10d: the route ends at once
 * (route_lost()): its records stop (route_qp cleared; slow_record() counts a LOST route's records as strays, a late
 * fetch of a descriptor it ended included), its request-free IO never fetched ends by the cold D walk (GCB d_head to
 * d_written, its route and gen: ABORTED_SQ_DELETION, dnr), its requests, pending entries and aborts end; only the
 * request-free IO the engine fetched is left, counted (DV19 case 2).
 * Teardown (D11 as amended by A4, A18):
 *   disconnect   FLUSH once (an ACTIVE route), then the stock disconnect; in-flight IO completes normally
 *   delete       FLUSH once -> Admin Delete I/O SQ, WAIT for its completion -> drain until state == GONE &&
 *                queue_depth == 0 && ctrl_inflight == 0 (1 s, else LOST) -> every abort_q entry completed
 *                (cdw0 bit0 = 1 unless it marked something) -> Admin Delete I/O CQ (the target's only
 *                reservation-release trigger, never before the Delete SQ completion) -> IAU_TILE_STATS -> leave,
 *                destroy.  is_removed: no FLUSH / drain / admin commands; in-flight requests completed manually
 *                (lost reported).  A controller in reset (prepare_for_reset): no admin commands; once the
 *                reset's disable is done (is_disconnecting clear or the admin queue DISCONNECTED: the target
 *                detached every route with records before CSTS.RDY = 0) the drain ends every command (P4.10a);
 *                before it the delete runs the disable itself (P4.10d, reset_disable(): the DISCONNECTING admin
 *                queue's poll, which runs nvme_ctrlr_disable_poll(), under the recursive controller lock the delete
 *                holds; a delete nested in that run, from an admin callback, runs it too: P4.10f), then drains; if
 *                the disable is not done within the drain bound the route ends LOST at once (route_lost(),
 *                reason=reset), the ring memory kept (the target may still write it).  Every
 *                path completes abort_q.  LOST (m4): no Delete I/O CQ, so the target's CQ outlives the SSD
 *                SQ / CQ memory and the qid the delete frees; a later qpair on that qid can fail its Create
 *                I/O CQ (fail-safe).
 *   connect      a reconnect (A6, V10: the reset path's nvme_ctrlr_reinitialize_io_qpair(); .qpair_reset
 *                never sees IO qpairs) ends the old incarnation first: FLUSH once (so a PARKED record read in
 *                the drain gets no disposition: no new descriptor goes into a dying ring), then the drain; it
 *                is refused unless the route ends GONE and drained (IAU_TILE_RESET_REFUSED, -EBUSY; GONE but not
 *                drained -> LOST, -EIO; LOST refuses at once); then the private ring is
 *                reset (zeroed memory, d_head = d_tail = 0) and the registration is re-published with the next
 *                member gen BEFORE Create I/O CQ / SQ (the target zeroes the IAU page at disable).  A private
 *                ring's rejoin is a ring reset: its old engine ring is unbound before the target completes
 *                Delete I/O SQ or reports CSTS.RDY = 0, so the GCB d_head precondition of a shared ring
 *                (P4.8) is replaced by the reset (descriptors written after the unbind are wiped, never run).
 *                Inside a controller reset (spdk_nvme_ctrlr_reconnect_poll_async()) a refused reconnect fails
 *                the whole controller, and the drain busy-waits up to 1 s under the controller lock (m4, m6).
 *   R_DETACHED   pending entries end there. With rflags CLOSED (a private ring's last record, after the
 *                engine's final fetch, P4.5 fix A) every descriptor written before the detach-time d_tail got its
 *                one record before it, so the commands still in flight then (written after that d_tail:
 *                callback resubmits, RAW retries) end there too with ABORTED_SQ_DELETION (dnr), exactly once.
 *                Without CLOSED (a non-last member's R_DETACHED on a shared ring; a target without the final
 *                fetch) they wait for their records or end LOST at the drain bound. On a shared ring (P4.8) the
 *                last member's CLOSED R_DETACHED ends the leftovers of every route on the ring, and a member's
 *                release waits for the GCB d_head to pass its last written slot.
 * Debug printing: the fast path prints each completion under the "nvme" debug flag as stock does (a DEBUG build
 * only; SPDK_DEBUGLOG_FLAG_ENABLED is false otherwise).
 *
 * P4.9 the request-free fast path (D14, SPDK_IAU_FAST=1).  qpair->iau_fast is set at every connect (latch_fast())
 * only if the route's namespace has no metadata, no PI and no extended LBA (extended LBA is refused at create), the
 * page size is 4096 and the stride is 64; the route's lba_count bound (fast_nlb = min(nlb_lim, sectors_per_max_io))
 * and stripe (sectors_per_stripe) are latched with it, as lba_shift and nsid are.  The four contiguous entries
 * spdk_nvme_ns_cmd_{read,write}{,_with_md} test the bit and return nvme_iau_fast_rw() (P4.9b) with the 12 arguments
 * of nvme_ns_cmd_rw_contig(), their stock body.  An IO qualifies only if an IO credit is free and queued_req,
 * err_cmd_head, err_req_head are empty (tested first), md is NULL, io_flags is a subset of {FUA, LR}, 1 <= lba_count
 * <= fast_nlb (<= 128 KiB, never split), it crosses no stripe, the buffer is dword aligned, ns->id is the route's, the
 * qpair is ENABLED without a transport failure, the route ACTIVE and the controller not failed; else it is counted
 * (st.declined), nothing written, and the stock body runs (a tail call).  A qualifying IO writes the IO-kind descriptor
 * the request path writes for the same command (d0, d1 = lba, d2 = buffer, d4 = nsid) with d5 / d6 = the user's
 * cb_fn / cb_arg and no nvme_request: queue_depth and num_outstanding_reqs count it (DV9: up to credits +
 * io_queue_requests IOs outstanding), d_written counts its slot.  Its record is the user's callback (a NULL cb_fn is
 * skipped, as stock); a PARKED error takes the echo slab's SQE for the error policy (D8); a removed member's records
 * go through slow_record() / cold_cb() in the qpair's own completion context.  A CLOSED R_DETACHED ends the
 * request-free IO whose descriptors sit between the engine's last fetch (GCB d_head) and d_written (end_unfetched():
 * IO kind, w0 not nvme_iau_req_done, the d0 route a member of that ring with the d0 gen; a RAW header's SQE slot is
 * skipped) with ABORTED_SQ_DELETION (dnr), exactly once; on a shared ring every route of the ring first goes GONE,
 * then that walk, then each route's other leftovers.  Where no record can come (DV19 below) the request-free IO still
 * in flight is counted as lost.
 *
 * P4.10a semantics (plan rev 2 Task P4.10, split P4.10a; the P4.9 review's I1, M1, M2, M8; R04; n4):
 *   timeouts     D9 as above (latched at connect, one notice per command lifetime, dropped unless READY).
 *   abort        D10: spdk_nvme_ctrlr_cmd_abort_ext() / _abort() reach nvme_iau_ctrlr_cmd_abort_ext() / _abort()
 *                (nvme_ctrlr_cmd.c hooks on iau_kind) on the qpair's thread. Late abort: ABORT_COOKIE(cookie) for
 *                request-free IO plus ABORT_COOKIE(req) per matching request-backed request in flight, or one
 *                ABORT_TAG(cid) (gen checked by the engine); queued requests are aborted in software as stock does
 *                and count as marked. A control takes one of 2 control credits; more wait on abort_q and go out as
 *                acks return them (never -EAGAIN). One callback per abort, after its last ack, cdw0 bit0 = 1 if
 *                nothing was marked (DV6: rc 0 even with no match). A route that takes no descriptor (not ACTIVE or
 *                FLUSHING) or a removed controller: abort_ext gives stock's result (the callback at once if queued
 *                requests matched, else -ENOENT), abort by cid -ENXIO; no NVMe Abort ever goes to the SSD (DV5).
 *   CLOSED       R04: an ABORT_* written after the target's final fetch gets ack(0) from the CLOSED walk
 *                (end_unfetched()), so every control credit is back at a CLOSED (nothing reset by force).
 *   lost IO      I1: every request-free IO gets exactly one callback (ABORTED_SQ_DELETION where the route ends:
 *                the engine's record, a FLUSH disposal or the CLOSED walk) on disconnect, delete, a reset's
 *                reconnect (A6) and a delete during a reset (after its disable; P4.10d: before it too, the delete
 *                runs the disable). DV19 (as amended 2026-10-19, P4.10d) covers the rest: a removed controller (a
 *                testbed artifact: is_removed is set only on PCIe hotplug, unreachable on vfio-user); the request-free
 *                IO the engine fetched when a drain times out (withheld by choice, DV5; P4.10d: the unfetched ones end
 *                by the cold D walk); a delete during a reset whose disable does not finish within the drain bound
 *                (P4.10f: the same DV5 policy; a delete nested in another delete's run of the disable runs it). The
 *                IAU_TILE_LOST line (once per incarnation, M1) and IAU_TILE_STATS lost= count those request-free IOs
 *                only (M2: requests and aborts always get their callback); a LOST route always prints the line.
 *   walk range   P4.10d (P4.10a review I3): end_unfetched() checks [GCB d_head, d_written) in every build; a range
 *                past d_written or of d_entries or more ends nothing (IAU_TILE_WALK_CLAMP; the routes end LOST).
 *   M8           a FLUSHING route clears iau_fast (the entries' stock body runs; the route takes no IO).
 */
#define NVME_IAU_RING_ENTRIES_DEFAULT	16384u	/* D13: group rings (P4.8) */
#define NVME_IAU_DB_BATCH_DEFAULT	64u	/* P4.8e: the group pass's chunk; 64 by the P4.8f sweep (nx_p4a4_db) */
#define NVME_IAU_DRAIN_US		1000000u	/* D11: the 1 s drain bound (IAU_TILE_LOST) */
/* the drain bound in use (NVME_IAU_DRAIN_US; a test hook: only the tile harness changes it, cold paths read it) */
extern uint32_t g_nvme_iau_drain_us;

struct nvme_iau_cfg {
	uint8_t		tile, s2, s2_wb, group, fast, rcons;	/* s2_wb: SPDK_IAU_S2_WB (review fix M3) */
	uint32_t	rings, ring_entries, stride;
	uint32_t	db_batch;	/* P4.8e: SPDK_IAU_DB_BATCH (0: off) */
};
extern struct nvme_iau_cfg g_nvme_iau_cfg;

/* SPDK_IAU_* once: 0, or -EINVAL with err naming the offending variable or combination */
int nvme_iau_cfg_load(char *err, size_t errlen);
/* the registration's selector (nvme_vfio_user.c): cfg_load (abort on IAU_TILE_CONFIG_ERROR), then the override */
void nvme_iau_select_ops(struct spdk_nvme_transport_ops *ops);
/* ops = a copy of the stock table: the tile functions replace its qpair ops; the stock ones are kept for
 * forwarding (admin queue) */
void nvme_iau_override_ops(struct spdk_nvme_transport_ops *ops);
int nvme_iau_s2_override_ops(struct spdk_nvme_transport_ops *ops);	/* P4.11 (S2, below): 0, or -EINVAL */
/* the record callback (w0) of every request-backed descriptor (d6 = the request) */
void nvme_iau_req_done(void *arg, const struct spdk_nvme_cpl *cpl);

/* ring pair k of a group: the host's view */
struct nvme_iau_ring {
	uint8_t				*d;		/* D_k: d_entries x stride */
	volatile struct iau_tile_rec	*r;		/* R_k: r_entries x 32 B */
	volatile uint32_t		*db;		/* GCB db[k].d_tail */
	const volatile uint64_t		*d_head;	/* GCB ring[k].d_head (engine-written, A15) */
	volatile uint64_t		*r_consumed;	/* GCB ring[k].r_consumed with SPDK_IAU_RCONS=1, else NULL */
	uint32_t			d_entries, r_entries, stride;
	uint32_t			d_tail, d_rung;	/* next slot; the value in db */
	uint32_t			r_head, budget;	/* next record; records per poll (D12) */
	uint8_t				r_phase;
	uint64_t			d_written;	/* D slots written, monotonic (the A15 assert) */
	uint64_t			r_seq;		/* records consumed, monotonic (r_consumed) */
	uint64_t			need_d, need_r;	/* sum of iau_tile_route_need_* over the routes */
	uint64_t			d_iova, r_iova;
};

/* route states */
enum {
	NVME_IAU_RT_IDLE = 0,
	NVME_IAU_RT_JOINED,
	NVME_IAU_RT_ACTIVE,
	NVME_IAU_RT_FLUSHING,
	NVME_IAU_RT_GONE,
	NVME_IAU_RT_LOST,
};

struct nvme_iau_qpair;
TAILQ_HEAD(nvme_iau_qlist, nvme_iau_qpair);

/* P4.8: the host's view of a shared ring k besides its positions: closed (its CLOSED R_DETACHED was consumed: no
 * engine ring is bound to it, the next member needs a reset ring), tainted (a member ended LOST while the ring was
 * bound: the target may still poll it, so the group's memory is kept; its CLOSED clears it), the member routes on
 * it */
struct nvme_iau_gring {
	uint8_t				closed, tainted;
	uint32_t			n_on;
};

struct nvme_iau_group {
	struct iau_tile_gcb		*gcb;
	uint64_t			gcb_iova;
	uint32_t			gid, k, n_routes, n_members;
	bool				is_private, in_ring_poll;
	struct nvme_iau_ring		ring[IAU_TILE_MAX_K];
	struct spdk_nvme_qpair		*route_qp[IAU_TILE_MAX_ROUTES];	/* the record loop's lookup */
	struct nvme_iau_qpair		*route[IAU_TILE_MAX_ROUTES];	/* the cold paths' */
	uint64_t			records, stray, polls, doorbells;
	/* P4.8, a shared (poll-group) group only; after the private group's fields, which keep their offsets */
	struct spdk_nvme_transport_poll_group	*tg;		/* in_completion_context during every ring pass */
	struct spdk_nvme_pcie_stat	*stat;		/* the PCIe poll group's stats (doorbells, polls) */
	struct nvme_iau_qlist		members, attn, dead;	/* every member; the attention list; deferred destroys */
	struct nvme_iau_qpair		*attn_cur;	/* the member whose generic call the attention pass is in */
	uint32_t			sweep;		/* group polls since the last sweep (D12: every 64) */
	bool				in_member_pass, keep;	/* keep: a LOST member's rings are never freed */
	uint64_t			idle_polls, sweeps, attn_calls;
	pthread_t			owner;		/* the poll group's thread (a reconnect elsewhere: -EBUSY) */
	uint32_t			db_batch;	/* P4.8e: records per ring per round of a pass (UINT32_MAX: off) */
	struct nvme_iau_gring		gr[IAU_TILE_MAX_K];
	uint8_t				route_gen[IAU_TILE_MAX_ROUTES];	/* the last member gen of each route (A15) */
	uint8_t				route_held[IAU_TILE_MAX_ROUTES];	/* its reservation outlived its member */
	uint16_t			route_resv[IAU_TILE_MAX_ROUTES];	/* credits the target reserves for the route */
	/* P4.10c (A19 as amended by A21), cold: the GCB ctx region, IAU-owned and zeroed, published in the GCB header at
	 * the group's creation (before any member's Create I/O SQ) and freed with the group; its iova and size as
	 * published (ctx_iova 4 KiB aligned, ctx_bytes the A21 size). At the end, so no field of the hot paths moves */
	void				*ctx;
	uint64_t			ctx_iova;
	uint32_t			ctx_bytes;
	/* P4.10f (A23), cold: a shared group's one credit count, fixed at its creation (its first member's); every member
	 * joins with it (more of its own: cut to it; fewer: refused); the region (ctx_bytes) is sized from it */
	uint32_t			credits;
};

/* a parked command the host re-issued (or holds after its FLUSH): cleared by that tag's next final record,
 * delivered as ABORTED_SQ_DELETION by R_DETACHED (D8) */
struct nvme_iau_pend {
	uint16_t	tag;
	uint64_t	w0, w1;
};

/* P4.10a (D10): one abort of the application's. spdk_nvme_ctrlr_cmd_abort_ext() makes n_ctrl = 1 + the matching
 * request-backed requests in flight (ABORT_COOKIE(cookie) for request-free IO, then ABORT_COOKIE(req) per request),
 * spdk_nvme_ctrlr_cmd_abort() one ABORT_TAG(cid). It stays on iq->abort_q until every control is acked: the controls
 * go out in FIFO order as control credits allow (2 per route: ctrl_inflight), each ack (nvme_iau_abort_ack(), the
 * controls' w0; d6 = this context) adds the commands it marked, and the last one calls the application's callback
 * with cdw0 bit0 = 1 if nothing was marked (marked also counts the queued requests aborted in software) and frees
 * it. A teardown, a rejoin, a removed controller or a LOST route ends every context still there the same way, with
 * what was marked so far (A4; a context whose controls were never issued: cdw0 bit0 = 1). */
struct nvme_iau_abort_ctx {
	TAILQ_ENTRY(nvme_iau_abort_ctx)	link;
	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;
	uint32_t			pending, marked;	/* controls not yet acked; commands marked */
	struct nvme_iau_qpair		*iq;
	uint32_t			n_ctrl, n_issued;	/* controls in d2[]; issued so far (in order) */
	uint8_t				kind0;		/* control 0's kind (ABORT_TAG: abort by cid); the others ABORT_COOKIE */
	uint64_t			d2[];		/* each control's d2: a cookie (a cb_arg) or a tag */
};

/* the per-qpair counters of the IAU_TILE_STATS line, in print order (P4.6 reads exactly these keys) */
#define NVME_IAU_QSTATS(X) \
	X(sub_fast) X(sub_req) X(sub_raw) X(declined) X(err) X(retries) X(reissues) X(releases) X(aborted) \
	X(timeouts) X(ctrl) X(detached) X(rejoins)
#define NVME_IAU_QSTAT_FIELD(n) uint64_t n;
struct nvme_iau_qstats {
	NVME_IAU_QSTATS(NVME_IAU_QSTAT_FIELD)
};
#undef NVME_IAU_QSTAT_FIELD

struct nvme_iau_qpair {
	/* the request path's line */
	struct nvme_iau_group		*g;
	struct nvme_iau_ring		*ring;		/* &g->ring[route % g->k] */
	struct nvme_request		**inflight;	/* credits: the request of each in-flight index */
	uint16_t			*idx_stack;	/* credits: the free indices (LIFO) */
	uint64_t			d0_tmpl;	/* iau_tile_d0(0, 0, route, 0, iau_tile_cflags(tmo, gen)) */
	uint32_t			credits, nsid;
	uint16_t			idx_top, route;
	uint8_t				state, lba_shift, gen, flushed;
	uint16_t			ctrl_inflight, n_pend;
	uint32_t			nlb_lim;	/* the fast IO test's bound: IAU_TILE_IO_MAX_BYTES >> lba_shift (P4.7e) */
	/* cold */
	struct spdk_nvme_qpair		*qpair;
	struct spdk_nvme_cmd		*echo;		/* entries x 64 B (D8; host memory the engine writes) */
	uint64_t			echo_iova;
	uint32_t			entries, tmo_us;	/* the SSD SQ entries; the latched timeout (D9) */
	struct nvme_iau_pend		*pend;		/* credits */
	TAILQ_HEAD(, nvme_iau_abort_ctx)	abort_q;
	struct nvme_iau_qstats		st;
	/* P4.8, a shared group's member only */
	TAILQ_ENTRY(nvme_iau_qpair)	link;		/* g->members */
	TAILQ_ENTRY(nvme_iau_qpair)	attn_link;	/* *attn_list while in_attn, else g->dead after a deferred delete */
	struct nvme_iau_qlist		*attn_list;	/* g->attn, or the attention pass's local list (A3) */
	uint64_t			last_w;		/* the ring's d_written when the route went GONE (its last slot) */
	uint8_t				in_attn;
	uint8_t				fail_report;	/* failed in a direct poll's attention pass: the next group
							 * pass reports it (disconnected_qpair_cb), if still connected */
	uint8_t				delete_deferred;	/* its delete op ran nested in a ring pass: the
								 * attention pass runs it (I1) */
	/* P4.9, the request-free submit (D14), latched at every connect (latch_fast()); at the end, so no field of the
	 * request or record paths moves */
	uint32_t			fast_nlb;	/* lba_count <= fast_nlb: min(nlb_lim, sectors_per_max_io) */
	uint32_t			stripe;		/* the namespace's sectors_per_stripe (0: no driver-assisted striping) */
	/* P4.10a, cold: this incarnation's IAU_TILE_LOST line was printed (once, P4.9 review M1) */
	uint8_t				lost_reported;
	/* P4.10d, cold: a reconnect of this shared member was deferred (n4) and not yet carried out (P4.10a review I1) */
	uint8_t				reset_deferred;
	/* P4.10f, cold: a rejoin ended LOST and its end (the D walk, the requests, the aborts: callbacks) is owed: it never
	 * runs inside the connect op (the P4.10c/d/e review's I1); the attention pass (a shared member) or the delete runs it */
	uint8_t				lost_owed;
};

/* A tile IO qpair: the PCIe qpair first (nvme_pcie_qpair_destroy() frees the whole allocation), then the tile
 * state; struct nvme_pcie_qpair is unchanged (no host object's layout moves). */
struct nvme_iau_pqpair {
	struct nvme_pcie_qpair		pq;
	struct nvme_iau_qpair		iq __attribute__((aligned(64)));
};

static inline struct nvme_iau_qpair *
nvme_iau_qp(struct spdk_nvme_qpair *qpair)
{
	return &SPDK_CONTAINEROF(qpair, struct nvme_iau_pqpair, pq.qpair)->iq;
}

/* IO kind (D7) for request req on route iq: *buf = the payload address (IOVA == VA on vfio-user) */
static inline bool
nvme_iau_req_is_io(const struct nvme_iau_qpair *iq, struct nvme_request *req, uint64_t *buf)
{
	*buf = req->payload_size != 0 ? (uint64_t)(uintptr_t)req->payload.contig_or_cb_arg + req->payload_offset : 0;
	return nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG && req->payload.md == NULL &&
	       iau_tile_cmd_is_io(&req->cmd, *buf, iq->lba_shift, iq->nsid);
}

/*
 * P4.7e: the common case of iau_tile_cmd_is_io() in a few loads and compares, Mode D's READ / WRITE test widened
 * to the tile contract: true only for a READ or WRITE that is not fused (opc 2 / 1 with fuse, rsvd1 and psdt 0:
 * the first 16 bits are 1 or 2), moves at most IAU_TILE_IO_MAX_BYTES (nlb < nlb_lim) with cdw12 [29:16] and cdw13
 * zero (one 64-bit compare of cdw12 | cdw13 << 32, FUA and LR masked off), has a dword-aligned buffer, the
 * route's nsid, and rsvd2, rsvd3, mptr, cdw14 and cdw15 zero. Each of these is a term of iau_tile_cmd_is_io() (for
 * READ / WRITE its len = (nlb + 1) << lba_shift > 0, so the alignment term applies), so fast => full; a route
 * with nsid 0 or lba_shift > 17 has nlb_lim 0 (never fast). The harness checks fast == (READ / WRITE && fuse 0
 * && full) over every opcode, fuse and dword-0 bit pattern and the boundary values of each term.
 */
static inline bool
nvme_iau_cmd_rw_fast(const struct nvme_iau_qpair *iq, const struct spdk_nvme_cmd *c, uint64_t buf)
{
	uint16_t dw0;
	uint64_t c12_13, r2_3, c14_15;

	memcpy(&dw0, c, sizeof(dw0));
	memcpy(&c12_13, &c->cdw12, sizeof(c12_13));
	memcpy(&r2_3, &c->rsvd2, sizeof(r2_3));
	memcpy(&c14_15, &c->cdw14, sizeof(c14_15));
	return (uint16_t)(dw0 - 1u) <= 1u && (c12_13 & 0xFFFFFFFF3FFFFFFFull) < iq->nlb_lim && (buf & 3u) == 0 &&
	       c->nsid == iq->nsid && (r2_3 | c->mptr | c14_15) == 0;
}
/* iau_tile_d0_from_cmd() in fewer instructions: the command's first 16 bits are opc | fuse << 8 | rsvd1 << 10 |
 * psdt << 14, and every IO kind has rsvd1 = psdt = 0, so this equals iau_tile_d0_from_cmd() for every command
 * iau_tile_cmd_is_io() accepts, fused or not (the harness checks it on each one) */
static inline uint64_t
nvme_iau_d0_rw_fast(const struct spdk_nvme_cmd *c, uint64_t d0_tmpl)
{
	uint16_t dw0;

	memcpy(&dw0, c, sizeof(dw0));
	return (uint64_t)dw0 | ((uint64_t)((c->cdw12 >> 28) & 0xCu) << 8) | d0_tmpl | ((uint64_t)(uint16_t)c->cdw12 << 32);
}
static inline uint32_t
nvme_iau_nlb_lim(uint32_t nsid, uint8_t lba_shift)
{
	return nsid != 0 && lba_shift <= 17 ? IAU_TILE_IO_MAX_BYTES >> lba_shift : 0;
}

/*
 * ------------------------------------------------------------------------------------------------------------
 * IAU-next P4.11: the S2 software ablation (nvme_iau_s2.c; plan D15 rev 2, Task P4.11 rev 2, the controller's fairness
 * rule of 2026-10-19). S2 = Mode 0 (the stock vfio-user target, no IAU) + SPDK_IAU_S2=1: the host does in software
 * what the IAU does in hardware for tile, with the same host-side optimizations as the tile host path, so that the IAU's
 * gain splits into a hardware share (tile - S2) and a software share (S2 - S1, stock):
 *   hook         the four contiguous ns_cmd entries call nvme_iau_fast_rw() on iau_fast (P4.9b); its first test sends an
 *                S2 qpair (iau_kind S2) to nvme_iau_s2_fast_rw() by a tail call with the same twelve arguments, whose
 *                decline is the same tail call of nvme_ns_cmd_rw_contig() (the stock body)
 *   cid split    fast cids [0, F), F = num_trackers - NVME_IAU_S2_TRACKER_CIDS (0 if num_trackers <= 16), carry a
 *                cookie: a 16 B hot entry {cb_fn, cb_arg} (the completion's only load) and a 32 B cold entry {slba, buf,
 *                submit_tick, nlb_flags (cdw12), opc, state, retries, bits} (the retry rebuild, the timeout scan, the late abort);
 *                tracker cids [F, num_trackers) are stock's trackers (free_tr holds only them): the request path
 *   submit       qualification as tile's (D14; run-time terms first: a free fast cid and the three lists empty), the
 *                SQE built in software (PRP1 / PRP2, or iau_tile_prp()'s list in the cid's tracker list page) and written
 *                with eight streaming stores, the doorbell batched at the poll with delay_cmd_submit (else rung); a
 *                latched timeout, a PRP list or the debug flag continue out of line by a tail call (P4.11b: the hot
 *                body makes no call and keeps no stack frame, as tile's)
 *   completion   its own Mode 0 CQ loop: dword 3 once (phase, status and cid), a fast cid with a success status, no
 *                mark and a hot cb_fn above NVME_IAU_S2_IDLE (one compare) returns its cid, credit and outstanding count
 *                (its hot cb_fn set to the idle sentinel), then calls back with the CQE (no request); errors, marks, the
 *                "nvme" debug flag, a NULL cb_fn and a free cid go cold (s2_cpl_cold()), where a CQE of a free cid is a
 *                stray (P4.11b: logged "does not map" as stock, counted as the IAU counts a stray record, nothing
 *                pushed or called back); tracker cids complete as stock (nvme_pcie_qpair_complete_tracker())
 *   poll group   S2's override (always on with SPDK_IAU_S2=1): the stock disconnected loop, then the connected members
 *                that are ENABLED polled directly by the S2 loop (no generic wrapper) with the tgroup's
 *                in_completion_context set, the sweep every 64 group polls and the attention pass (D12, as tile's)
 * Semantics (parity with tile where software can do it; S2 holds every cookie in host memory, so nothing is ever lost):
 *   retry        an error the policy retries (nvme_iau_cpl_error_policy()) is rebuilt on the same cid from the cold entry
 *                (the same builder: the same SQE), no callback; retries in the cold entry
 *   timeouts     latched at connect (as tile's D9 / DV7); the poll's tail scans the trackers (stock's walk) and the busy
 *                fast cids (submit_tick): one timeout_cb per command lifetime, cid = the fast cid
 *   late abort   spdk_nvme_ctrlr_cmd_abort_ext() / _abort() (the nvme_ctrlr_cmd.c hooks -> nvme_iau.c ->
 *                nvme_iau_s2_cmd_abort_ext() / _abort()) mark busy cookies (cb_arg) and in-flight tracker requests
 *                (nvme_request_abort_match(), or the cid) in software; a marked command's CQE is delivered
 *                ABORTED_BY_REQUEST (dnr), as the IAU's record (DV5: no buffer back while the SSD may DMA, no NVMe Abort);
 *                queued requests are aborted in software as stock does; one callback per abort, from the qpair's next
 *                poll, cdw0 bit0 = 1 if nothing was marked (DV6: rc 0)
 *   delete       stock's sequence with the S2 poll: Delete I/O SQ (the target drops what it had scheduled, F5), the S2
 *                poll consumes the CQEs posted before it (fast cids by cookie: no "does not map"), Delete I/O CQ, then
 *                every command still in flight ends once (stock's abort_trackers() status, ABORTED_BY_REQUEST dnr 1, for
 *                cookies and trackers alike); inside a controller reset, with commands in flight, it first waits for
 *                the reset's disable as tile does (P4.11b; at the drain bound they end anyway, as stock: DV35)
 *   reset        a reconnect with commands in flight is refused (IAU_S2_RESET_REFUSED, -EBUSY) unless it is a controller
 *                reset's (is_resetting with the disable done, or a FAILURE_RESET reinitialize): those commands were dropped
 *                by the disable, so they are marked owed and end once (ABORTED_BY_REQUEST, abort_dnr, as stock PCIe's
 *                reset abort) in the qpair's next completion context, never inside the connect op; a poll-group
 *                member's reconnect inside a reset is deferred to its own poll as tile defers a shared member's (n4:
 *                FAILURE_RESET, CONNECTED, rc 0, IAU_S2_RESET_DEFERRED)
 *   removed      qpair_abort_reqs ends every command in flight once (stock's status)
 * Evidence: IAU_S2_OPS (selection), IAU_S2_QPAIR qid= fast_cids= trackers= nsid= fast= fast_nlb= tmo_us= pg=
 * sqe=nt|wb (every connect; sqe=wb: SPDK_IAU_S2_WB=1, review fix M3, the SQE in write-back stores, as I3's D slot),
 * IAU_S2_STATS qid= fast_cids= <NVME_IAU_S2_QSTATS> ended= lost= special= (delete; P4.11b: stray= last of the
 * counters), IAU_S2_PG_END (a member's leave), IAU_S2_REFUSED / IAU_S2_JOIN_REFUSED / IAU_S2_RESET_REFUSED /
 * IAU_S2_RESET_DEFERRED, IAU_S2_RESET_DISABLE qid= done= polls= (P4.11b: a delete's wait inside a reset).
 */
#define NVME_IAU_S2_TRACKER_CIDS	16u	/* D15: the tracker cids of an S2 qpair */

/* the hot half of a fast cid's cookie (16 B); cb_fn is NVME_IAU_S2_IDLE while the cid is free (P4.11b) */
struct nvme_iau_s2_hot {
	uint64_t	cb_fn, cb_arg;
};
/* P4.11b (the P4.11 review's I1, as the controller ruled it): the hot cb_fn of a free fast cid (set at create and when
 * the cid is freed, before its callback; no function is at address 1). A CQE whose cid finds it is a stray (stock's
 * "does not map", the IAU's stray record): counted, nothing pushed or called back. The completion's one unsigned compare
 * (cb_fn <= NVME_IAU_S2_IDLE) also sends a NULL cb_fn cold, so the check costs the hot path one store */
#define NVME_IAU_S2_IDLE	1u

/* the cold half (32 B): the replay fields of the IAU's slot record (D17) and its state; written at the submit with three
 * 8 B stores (plus submit_tick when the timeout is latched) */
struct nvme_iau_s2_cold {
	uint64_t	slba, buf, submit_tick;
	uint32_t	nlb_flags;	/* lba_count - 1 | FUA / LR: the SQE's cdw12 */
	uint8_t		opc;
	uint8_t		state;		/* NVME_IAU_S2_BUSY from the submit (the free-cid stack is the authority) */
	uint8_t		retries;
	uint8_t		bits;		/* NVME_IAU_S2_B_* */
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_iau_s2_hot) == 16 && sizeof(struct nvme_iau_s2_cold) == 32 &&
		   offsetof(struct nvme_iau_s2_cold, nlb_flags) == 24 && offsetof(struct nvme_iau_s2_cold, bits) == 31,
		   "S2 cookie layout: 16 B hot, 32 B cold, its last 8 B one store");
#define NVME_IAU_S2_BUSY	1u
#define NVME_IAU_S2_B_ABORT	0x01u	/* late abort marked: its CQE is delivered ABORTED_BY_REQUEST */
#define NVME_IAU_S2_B_TMO	0x02u	/* its one timeout notice was given */
#define NVME_IAU_S2_B_OWED	0x04u	/* a reset leftover: ended by the next completion context */

/* the per-qpair counters of the IAU_S2_STATS line, in print order */
#define NVME_IAU_S2_QSTATS(X) \
	X(sub_fast) X(sub_tr) X(declined) X(cpl) X(err) X(retries) X(marked) X(aborted) X(timeouts) X(leftovers) \
	X(rejoins) X(aborts) X(stray)
#define NVME_IAU_S2_QSTAT_FIELD(n) uint64_t n;
struct nvme_iau_s2_qstats {
	NVME_IAU_S2_QSTATS(NVME_IAU_S2_QSTAT_FIELD)
};
#undef NVME_IAU_S2_QSTAT_FIELD

struct nvme_iau_s2_pg;
struct nvme_iau_s2_qpair;
TAILQ_HEAD(nvme_iau_s2_qlist, nvme_iau_s2_qpair);

/* an abort of the application's whose marks are done: its callback runs at the qpair's next poll (as tile's ack record) */
struct nvme_iau_s2_abort {
	TAILQ_ENTRY(nvme_iau_s2_abort)	link;
	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;
	uint32_t			marked;
};

struct nvme_iau_s2_qpair {
	/* the submit's and the completion's line */
	struct nvme_iau_s2_hot		*hot;		/* fast_cids x 16 B */
	struct nvme_iau_s2_cold		*cold;		/* fast_cids x 32 B */
	uint16_t			*cid_stack;	/* the free fast cids (LIFO) */
	uint16_t			cid_top, fast_cids;
	uint32_t			nsid, fast_nlb, stripe;
	uint32_t			special;	/* commands marked for late abort: the completion's one extra test */
	uint8_t				lba_shift, tmo_on, work;
	uint64_t			tmo_ticks;
	/* cold */
	struct spdk_nvme_qpair		*qpair;
	struct nvme_iau_s2_qstats	st;
	TAILQ_HEAD(, nvme_iau_s2_abort)	abort_done;
	struct nvme_tracker		*owed_last;	/* reset leftovers: the trackers up to this one */
	uint32_t			owed;		/* reset leftovers: the fast cids marked NVME_IAU_S2_B_OWED */
	uint32_t			tmo_us;
	uint8_t				trk_mark[NVME_IAU_S2_TRACKER_CIDS];	/* late abort marks of the tracker cids */
	uint8_t				connected;	/* a later connect is a reconnect */
	uint8_t				reset_deferred;	/* a poll-group member's reconnect inside a reset was deferred */
	/* a member of an S2 poll group */
	struct nvme_iau_s2_pg		*pg;
	TAILQ_ENTRY(nvme_iau_s2_qpair)	link;		/* pg->members */
	TAILQ_ENTRY(nvme_iau_s2_qpair)	attn_link;	/* *attn_list while in_attn */
	struct nvme_iau_s2_qlist	*attn_list;
	uint8_t				in_attn, fail_report;
};

/* an S2 IO qpair: the PCIe qpair first (nvme_pcie_qpair_destroy() frees the whole allocation), then the S2 state */
struct nvme_iau_s2_pqpair {
	struct nvme_pcie_qpair		pq;
	struct nvme_iau_s2_qpair	s2 __attribute__((aligned(64)));
};

static inline struct nvme_iau_s2_qpair *
nvme_iau_s2_qp(struct spdk_nvme_qpair *qpair)
{
	return &SPDK_CONTAINEROF(qpair, struct nvme_iau_s2_pqpair, pq.qpair)->s2;
}

/* the S2 request-free submit (nvme_iau_fast_rw()'s S2 branch: the same twelve arguments) and the abort hooks' S2 branch */
int __attribute__((nocf_check)) nvme_iau_s2_fast_rw(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
		void *metadata, uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag);
int nvme_iau_s2_cmd_abort_ext(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, void *cmd_cb_arg,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int nvme_iau_s2_cmd_abort(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair, uint16_t cid,
			  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

#endif /* __NVME_IAU_H__ */
