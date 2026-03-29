# SPDK Issue #3807 — nvme_rdma_log_wc_status local protection error (status 4) during I/O

> Issue: https://github.com/spdk/spdk/issues/3807
> Component: lib/nvme/nvme_rdma.c, lib/rdma_utils/rdma_utils.c
> Symptom: IBV_WC_LOC_PROT_ERR (status 4) on SEND WR (type 1) for fabric CONNECT
>          command on admin qpair (qid 0) to discovery subsystem. Appears after
>          hours of sustained I/O load — intermittent, reconnect-triggered.

---

## Investigation Findings

### Error Context
```
nvme_rdma_log_wc_status: WC error, qid 0, qp state 2 (CONNECTING),
    request 0xXXXX type 1 (SEND), status: (4): local protection error
```

- `qp state 2` = `NVME_QPAIR_CONNECTING` — error fires during fabric CONNECT command send
- `type 1` = `RDMA_WR_TYPE_SEND` — the CONNECT command SEND WR fails
- `IBV_WC_LOC_PROT_ERR` = local HCA tried to access a buffer whose memory region (MR)
  is invalid (deregistered or wrong PD)

### Key Code Paths

**lkey baked at creation time:**
`nvme_rdma_create_reqs()` (line 1063-1067) — `send_sgl[0].lkey` is obtained from
`rqpair->mr_map` once at req creation and stored permanently in the RDMA SGL struct.
If the underlying ibv_mr is later deregistered, any SEND using that lkey → LOC_PROT_ERR.

**Stale connection disconnect path:**
`nvme_rdma_stale_conn_disconnected()` (line 2577) calls `nvme_rdma_qpair_destroy()`
IMMEDIATELY after the QP disconnect CM event, WITHOUT waiting for pending FLUSH_ERR
WCs to drain from the shared CQ.

Compare to `nvme_rdma_qpair_disconnected()` (line 2395) which has a lingering
mechanism: checks `current_num_sends` and `current_num_recvs` before destroying,
enters `NVME_RDMA_QPAIR_STATE_LINGERING` if there are outstanding WCs.

**`nvme_rdma_qpair_destroy()` frees resources (lines 2293-2349):**
1. `spdk_rdma_utils_free_mem_map(&rqpair->mr_map)` — deregisters MRs (if last ref)
2. `spdk_rdma_utils_put_pd(...)` — releases PD ref
3. `spdk_rdma_provider_qp_destroy(...)` — destroys QP
4. `nvme_rdma_free_reqs(rqpair)` — frees `rqpair->rdma_reqs` and `rqpair->cmds`
5. `nvme_rdma_free_rsps(rqpair->rsps)` — frees response buffers

**Shared CQ (poll group):**
When using a poll group, the CQ is shared among qpairs (`poller->cq`). After a QP
goes to ERROR state, FLUSH_ERR WCs accumulate in the shared CQ. These WCs contain
`wr_id` pointing to `rdma_wr` in `rqpair->rsps->rsps[i]` or `rqpair->rdma_reqs[i]`.

### Root Cause

**Use-after-free leading to lkey corruption:**

1. Admin qpair connects to discovery subsystem; gets STALE_CONN rejection.
2. `nvme_rdma_stale_conn_retry()` → `_nvme_rdma_ctrlr_disconnect_qpair()` → QP enters
   ERROR state; pending SEND/RECV WRs are flushed → FLUSH_ERR WCs placed in shared CQ.
3. CM DISCONNECTED event fires → `nvme_rdma_stale_conn_disconnected()`:
   - `nvme_rdma_qpair_destroy()` is called IMMEDIATELY (no lingering check)
   - `rqpair->rdma_reqs` and `rqpair->rsps->rsps` are freed (`spdk_free`)
   - The FLUSH_ERR WCs in the shared CQ still have `wr_id` pointing to the freed structs
4. State set to `NVME_RDMA_QPAIR_STATE_STALE_CONN`; reconnect starts.
5. Reconnect: `nvme_rdma_connect_established()` allocates NEW `rdma_reqs` via
   `spdk_zmalloc` — which can return the SAME physical memory just freed in step 3.
6. `nvme_rdma_create_reqs()` bakes new lkeys into `send_sgl[0].lkey` of the new reqs.
7. In the next `nvme_rdma_poll_group_process_completions()`:
   - Old FLUSH_ERR WCs are polled from the shared CQ
   - `wc->wr_id` = pointer to freed (now reused) memory in the NEW `rdma_reqs`
   - `nvme_rdma_process_recv_completion()` / `nvme_rdma_process_send_completion()` runs
     on what is now the NEW qpair's data → **use-after-free / memory corruption**
   - The `send_sgl[0].lkey` field of the new qpair's reqs gets overwritten with garbage
8. New CONNECT SEND WR is posted with the garbage lkey → `IBV_WC_LOC_PROT_ERR`

The "hours of runtime" requirement is explained by memory reuse patterns: `spdk_zmalloc`
must return the exact same physical address after the free for the corruption to hit
the new qpair's SGE data — this only happens under sufficient allocation pressure.

---

## Fix Plan

- [ ] **Investigate**: Confirm root cause by adding instrumentation to verify
  `current_num_sends > 0` in `nvme_rdma_stale_conn_disconnected` when it fires
  under load.

- [x] **Fix: Add lingering drain to stale conn disconnect path**
  Added `NVME_RDMA_QPAIR_STATE_STALE_CONN_LINGERING` state to the enum.
  Split `nvme_rdma_stale_conn_disconnected()` into:
  - `nvme_rdma_stale_conn_complete_disconnect()` — does the actual destroy + STALE_CONN transition
  - `nvme_rdma_stale_conn_disconnected()` — now flushes pending sends, checks outstanding
    WC counters; if any are non-zero enters STALE_CONN_LINGERING state (defers destroy)
  Added `STALE_CONN_LINGERING` case to `nvme_rdma_ctrlr_connect_qpair_poll()` which
  polls until `current_num_sends == 0 && current_num_recvs == 0` (or timeout), then
  calls `nvme_rdma_stale_conn_complete_disconnect()`.

- [x] **Fix: Optionally tighten MR/QP destroy order**
  In `nvme_rdma_qpair_destroy()`, moved `spdk_rdma_utils_free_mem_map()` to run
  AFTER `spdk_rdma_provider_qp_destroy()` so the HCA fully releases the QP before
  the underlying ibv_mr registrations are potentially removed.

- [x] **Test**: Added unit tests in `test/unit/lib/nvme/nvme_rdma.c/nvme_rdma_ut.c`:
  - `test_nvme_rdma_stale_conn_disconnected`: covers fast path (no outstanding WCs →
    STALE_CONN), sends outstanding → STALE_CONN_LINGERING, recvs outstanding →
    STALE_CONN_LINGERING, SRQ case (recv check skipped) → STALE_CONN.
  - `test_nvme_rdma_stale_conn_lingering_drain`: covers poll loop staying in
    STALE_CONN_LINGERING while WCs are outstanding, transitioning to STALE_CONN
    once drained, and timeout-forced transition.

- [ ] **Update issue**: Comment on #3807 with root cause analysis and fix PR link

---

## Files to Modify

| File | Change |
|------|--------|
| `lib/nvme/nvme_rdma.c` | Add lingering drain to `nvme_rdma_stale_conn_disconnected()` |
| `lib/nvme/nvme_rdma.c` | Optionally reorder `free_mem_map` / `qp_destroy` in `nvme_rdma_qpair_destroy()` |
| `test/unit/lib/nvme/nvme_rdma.c/nvme_rdma_ut.c` | Add stale conn reconnect test |
