# Steps Followed — SPDK Issue #3807 Investigation

## Step 1 — Cloned SPDK repo
- Shallow-cloned https://github.com/spdk/spdk into
  `/Users/nishanthmolleti/Desktop/Github-Dev/spdk` (--depth=1, no submodules)
- Full build not needed for code investigation

## Step 2 — Read issue and comments
- Issue: IBV_WC_LOC_PROT_ERR (status 4) on SEND WR during fabric connect to
  discovery subsystem, admin qpair (qid 0), after hours of sustained I/O load
- Only one substantive comment from submitter: "seen during fabric connect to
  discovery subsystem" — issue went stale (no further info provided)

## Step 3 — Located relevant source files
- `lib/nvme/nvme_rdma.c` — NVMe-oF RDMA initiator transport (3999 lines)
- `lib/rdma_utils/rdma_utils.c` — shared RDMA memory registration utilities
- `lib/nvme/nvme_fabric.c` — NVMe-oF fabric connect command implementation

## Step 4 — Traced the connect and disconnect state machines

### Connect path:
nvme_rdma_ctrlr_connect_qpair()
  → nvme_rdma_route_resolved() → nvme_rdma_qpair_init() → nvme_rdma_connect()
  → nvme_rdma_connect_established()
      • creates rqpair->mr_map (ibv_reg_mr for all DMA memory)
      • creates rqpair->rdma_reqs → send_sgl[0].lkey baked from mr_map
      • creates and posts rqpair->rsps (recv WRs)
      • state = FABRIC_CONNECT_SEND
  → nvme_fabric_qpair_connect_async() → SEND WR posted

### Stale conn disconnect path:
nvme_rdma_stale_conn_retry()
  → _nvme_rdma_ctrlr_disconnect_qpair(cb=nvme_rdma_stale_conn_disconnected)
      • QP → ERROR state (FLUSH_ERR WCs queued in shared CQ)
      • Waits for CM DISCONNECTED event
  → nvme_rdma_stale_conn_disconnected()
      • nvme_rdma_qpair_destroy() — IMMEDIATELY, NO lingering check
      • state = STALE_CONN

### Normal disconnect path (for comparison):
nvme_rdma_ctrlr_disconnect_qpair()
  → _nvme_rdma_ctrlr_disconnect_qpair(cb=nvme_rdma_qpair_disconnected)
  → nvme_rdma_qpair_disconnected()
      • checks current_num_sends / current_num_recvs
      • if > 0 → LINGERING state (waits for CQ drain before destroying)
      • if == 0 → nvme_rdma_qpair_destroy() immediately

## Step 5 — Analyzed shared mem_map lifecycle (rdma_utils.c)
- `spdk_rdma_utils_create_mem_map()`: reference-counted per PD; shared across
  qpairs using the same RDMA device
- lkey is obtained at req-creation time and baked into send_sgl[0].lkey
- If the ibv_mr is deregistered after lkey is baked in → LOC_PROT_ERR on next SEND

## Step 6 — Identified root cause
**Use-after-free → lkey corruption via shared CQ:**

1. Stale conn: QP → ERROR, FLUSH_ERR WCs placed in shared CQ
2. `nvme_rdma_stale_conn_disconnected` destroys qpair (frees rdma_reqs, rsps) WITHOUT
   waiting for CQ drain
3. Reconnect: spdk_zmalloc returns same physical memory → new rdma_reqs allocated at
   same address as freed memory
4. New lkeys baked into new rdma_reqs->send_sgl[0].lkey
5. poll_group_process_completions polls old FLUSH_ERR WCs with wr_id pointing to
   what is now the new rdma_reqs → use-after-free corrupts send_sgl[0].lkey
6. New CONNECT SEND posted with corrupted lkey → LOC_PROT_ERR

Root difference: `nvme_rdma_stale_conn_disconnected` lacks the lingering CQ drain that
`nvme_rdma_qpair_disconnected` has (lines 2423-2434).

## Step 7 — Created todo.md
- Documented root cause, fix plan, files to modify, test strategy

## Step 8 — Implemented fix in lib/nvme/nvme_rdma.c
Changes:
1. Added `NVME_RDMA_QPAIR_STATE_STALE_CONN_LINGERING` to the state enum (after STALE_CONN)
2. Extracted `nvme_rdma_stale_conn_complete_disconnect()` — performs the actual qpair
   destroy and STALE_CONN state/timeout setup (factored out of the old disconnected callback)
3. Rewrote `nvme_rdma_stale_conn_disconnected()`:
   - Now calls `nvme_rdma_qpair_flush_send_wrs()` (mirrors normal disconnect path)
   - Checks `current_num_sends` and `rsps->current_num_recvs` before destroying
   - If outstanding WCs: enters STALE_CONN_LINGERING with NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US
   - If no outstanding WCs: calls complete_disconnect directly (fast path, same as before)
4. Added `STALE_CONN_LINGERING` case in `nvme_rdma_ctrlr_connect_qpair_poll()`:
   - Polls until all WC counters hit zero OR timeout expires
   - Then calls `nvme_rdma_stale_conn_complete_disconnect()` to destroy and transition
