# NVMe-oF Interrupt Mode {#nvmf_interrupt_mode}

The NVMe-oF target supports interrupt mode for the vfio-user, TCP, and RDMA transports.
Interrupt mode allows reactors to sleep when idle and wake on network or device events,
reducing CPU usage when the target is not under load.

For general SPDK interrupt mode design, see @ref interrupt_mode.

---

## Using the nvmf_tgt Application

This section is for users who want to test or use interrupt mode with the `nvmf_tgt`
application.

### Quick Checklist

1. Linux only; ensure the transport provides a userspace wakeup event source; for
   RDMA, I/O completions are delivered through the completion channel fd.
2. Start `nvmf_tgt` with the `--interrupt-mode` flag.
3. Configure transports via RPC; use `-M` for vfio-user.
4. Verify reactors go idle when traffic stops (use `spdk_top` or
   `scripts/rpc.py framework_get_reactors`).

### Starting nvmf_tgt with interrupt mode

1. Start `nvmf_tgt` with interrupt mode enabled using the `--interrupt-mode` flag:

   ```sh
   build/bin/nvmf_tgt --interrupt-mode -m 0xF
   ```

   The flag enables interrupt mode during app initialization.

2. Configure NVMe-oF transports via RPC. TCP and RDMA do not require any
   interrupt-mode-specific transport options. vfio-user requires the `-M` flag:

   ```sh
   scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -c 8192
   scripts/rpc.py nvmf_create_transport -t RDMA -u 8192 -i 131072 -c 8192
   scripts/rpc.py nvmf_create_transport -t VFIOUSER -M
   ```

   For vfio-user, the `-M` (`--disable-mappable-bar0`) flag is required for
   interrupt mode.

3. Configure subsystems and listeners normally. When idle, reactors should sleep;
   under load, pollers wake on transport events.

### Operational notes

* Ensure all modules used by the application support interrupt mode; otherwise,
  threads hosting non-interrupt pollers will continue polling.
* The TCP acceptor poller ignores `acceptor_poll_rate` in interrupt mode because it
  is fully event-driven.
* Expect slightly higher wake latency than pure polling; tune NIC interrupt moderation
  to balance latency and power.

### Verification

* Use `scripts/rpc.py framework_get_reactors` or `spdk_top` to confirm reactors report
  idle when traffic stops.
* For TCP, a `strace -e epoll_wait` on the `nvmf_tgt` process should show sleeps when
  idle; for RDMA, `ibv_req_notify_cq()` counters should increase only when new
  completions arrive.
* The regression test `test/nvmf/target/interrupt.sh` demonstrates expected idle/busy
  transitions under load.

### Troubleshooting

* If threads stay busy when idle, check for non-interrupt pollers on the same thread
  or transports that do not yet support interrupt mode (see @ref interrupt_mode for
  subsystem support details).
* RDMA: ensure completion channels are created and `ibv_req_notify_cq()` succeeds;
  otherwise poll group creation will fail.
* TCP: verify socket groups are created and interrupt registration succeeds;
  otherwise transport or poll group creation will fail.
* vfio-user: ensure `disable_mappable_bar0` is set and interrupt fds are properly
  registered; check guest VM interrupt configuration.

### Limitations

* FC transport does not support interrupt mode — poll group creation fails.
* For TCP, interrupt mode requires the POSIX socket implementation; the uring socket
  implementation does not support interrupt mode.
* Linux is required; interrupt mode is not supported on other platforms.
* For best power savings, pin NVMe-oF poll groups to threads that host only
  interrupt-capable pollers and ensure NIC/CQ interrupt moderation is tuned for your
  latency/power goals.

---

## Developer Guide

This section is for developers embedding the `lib/nvmf` library into their own
applications.

### Transport-Specific Integration

Each NVMe-oF transport integrates with SPDK's event-driven framework differently:

* **vfio-user transport**:
  * Requires `disable_mappable_bar0` option for interrupt mode compatibility.
  * Registers interrupts on the libvfio-user socket fd and per-poll-group eventfds.
  * Re-arms SQ event indexes in poll group handlers to guarantee wakeups on
    subsequent doorbell writes.

* **TCP transport**:
  * Acceptor poller switches to 0µs period and registers an interrupt on the listen
    socket group to wake on new connections (see `nvmf_tcp_create()` in `lib/nvmf/tcp.c`).
  * Poll groups register interrupts on their socket groups so RX/TX events wake the
    group instead of busy polling (see `nvmf_tcp_poll_group_create()` in `lib/nvmf/tcp.c`).

* **RDMA transport**:
  * Each RDMA device registers an interrupt on the verbs async FD for device-level
    events (see `create_ib_device()` in `lib/nvmf/rdma.c`).
  * The transport also registers an interrupt on the RDMA CM event channel so
    connection-management events wake the acceptor path (see `nvmf_rdma_create()` in
    `lib/nvmf/rdma.c`).
  * RDMA pollers create completion channels, register interrupts on the completion
    channel fd, and arm `ibv_req_notify_cq()` for completions (see
    `nvmf_rdma_poller_create()` in `lib/nvmf/rdma.c`).

### Minimal setup sequence

Developers embedding the NVMe-oF target should enable interrupt mode before
initializing SPDK threading and then create transports normally.

```c
struct transport_ctx {
    struct spdk_nvmf_tgt *tgt;
};

static void
transport_added(void *cb_arg, int status)
{
    if (status != 0) {
        /* Handle spdk_nvmf_tgt_add_transport() failure. */
    }
}

static void
transport_created(void *cb_arg, struct spdk_nvmf_transport *transport)
{
    struct transport_ctx *ctx = cb_arg;

    if (transport == NULL) {
        /* Handle spdk_nvmf_transport_create_async() failure. */
        return;
    }

    spdk_nvmf_tgt_add_transport(ctx->tgt, transport, transport_added, ctx);
}

int
main(void)
{
    struct spdk_nvmf_tgt *tgt;
    struct transport_ctx *ctx;
    struct spdk_nvmf_transport_opts tcp_opts;
    struct spdk_nvmf_transport_opts rdma_opts;
    int rc;

    /* Enable interrupt mode before thread library init */
    rc = spdk_interrupt_mode_enable();
    if (rc != 0) {
        return rc;
    }

    /* Initialize env/threading via spdk_thread_lib_init_ext().
     * If using spdk_app_start() instead, set opts->interrupt_mode = true
     * and skip the manual spdk_interrupt_mode_enable() call above. */
    /* ... */

    /* Create target before adding transports. */
    tgt = /* ... */;
    ctx = /* app-owned or heap-allocated state */;
    ctx->tgt = tgt;

    /* Initialize transport options per transport (defaults differ) */
    if (!spdk_nvmf_transport_opts_init("TCP", &tcp_opts, sizeof(tcp_opts))) {
        return -1;
    }

    if (!spdk_nvmf_transport_opts_init("RDMA", &rdma_opts, sizeof(rdma_opts))) {
        return -1;
    }

    /* Create transports asynchronously */
    rc = spdk_nvmf_transport_create_async("TCP", &tcp_opts, transport_created, ctx);
    if (rc != 0) {
        return rc;
    }

    rc = spdk_nvmf_transport_create_async("RDMA", &rdma_opts, transport_created, ctx);
    if (rc != 0) {
        return rc;
    }

    /* Continue initialization; transports will be added in the callback. */
    /* ... */
}
```

### Key points for developers

* For custom initialization, call `spdk_interrupt_mode_enable()` once before
  `spdk_thread_lib_init_ext()`. If using the event framework (`spdk_app_start()`),
  set `opts->interrupt_mode = true` instead — the framework calls
  `spdk_interrupt_mode_enable()` automatically.
* Transports arm acceptors and poll groups automatically when global interrupt mode is
  enabled (see `lib/nvmf/tcp.c`, `lib/nvmf/rdma.c`, and `lib/nvmf/vfio_user.c`).
  vfio-user additionally requires the `disable_mappable_bar0` transport option.
* Use `spdk_nvmf_transport_opts_init()` with the transport name to get correct
  per-transport defaults, and check its return value before using the options structure.
* `spdk_nvmf_transport_create_async()` has two failure paths: it can fail immediately
  via its return code, or later via the callback with `transport == NULL`.
* Check the completion status from `spdk_nvmf_tgt_add_transport()` before assuming the
  transport is usable.
* Any callback context passed to `spdk_nvmf_transport_create_async()` must remain
  valid until all outstanding callbacks complete; keep it in app-owned or
  heap-allocated state rather than transient stack state.
* If you register additional pollers on the same threads, mark them with
  `spdk_poller_register_interrupt()` so they do not force the thread to stay in poll
  mode.
