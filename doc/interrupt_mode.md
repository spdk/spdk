# Interrupt Mode {#interrupt_mode}

## Overview

SPDK's default operating mode is **poll mode** — every core runs a tight loop that
continuously checks for work without ever blocking. This gives the lowest latency
but burns 100% CPU. **Interrupt mode** lets a core sleep when there's no work,
waking it only when an event arrives via a file descriptor. The key abstractions
involved are **reactors**, **threads**, **pollers**, **fd_groups**, and **interrupts**.

---

## fd_groups — the Foundation

An `spdk_fd_group` is a thin wrapper around a Linux **epoll** instance. It holds:

- An **epoll fd** (`epfd`) created via `epoll_create1()`
- A list of **event handlers**, each binding an fd to a callback
- A list of **child fd_groups** (nesting)
- An optional **wrapper function** that wraps all callbacks (used for thread-context
  switching)

```c
struct spdk_fd_group {
    int epfd;
    uint32_t num_fds;
    struct spdk_fd_group *parent;
    spdk_fd_group_wrapper_fn wrapper_fn;
    void *wrapper_arg;
    TAILQ_HEAD(, event_handler) event_handlers;
    TAILQ_HEAD(, spdk_fd_group) children;
    TAILQ_ENTRY(spdk_fd_group) link;   /* linkage in parent's children list */
};
```

The key APIs are:

| API | What it does |
|-----|-------------|
| `spdk_fd_group_create()` | Creates a new epoll instance |
| `spdk_fd_group_add()` | Adds an fd + callback via `epoll_ctl(EPOLL_CTL_ADD)` |
| `spdk_fd_group_wait(fgrp, timeout)` | Calls `epoll_wait()`, dispatches callbacks for ready fds |
| `spdk_fd_group_nest(parent, child)` | Moves all child fds into parent's epoll |
| `spdk_fd_group_unnest(parent, child)` | Moves child fds back to their own epoll |

### Nesting

Nesting is the critical mechanism that allows a single `epoll_wait()` at the reactor
level to monitor *all* fds from all threads and all pollers. When a child fd_group is
nested into a parent, all of the child's fds are moved from the child's epoll to the
**root** epoll (the top-level parent). This means one `epoll_wait()` call sees
everything.

---

## Reactors

A reactor is a per-core event loop. Each reactor has:

- Its own `spdk_fd_group *fgrp` (the root epoll)
- An `events_fd` (eventfd) — wakes the reactor when events are enqueued from other
  cores
- A `resched_fd` (eventfd) — wakes the reactor when thread scheduling changes
- A `notify_cpuset` — bitmask of which remote reactors are in interrupt mode and need
  notification

### Poll Mode Loop

```c
_reactor_run(reactor) {
    event_queue_run_batch();  /* process cross-reactor events */
    for each thread on this reactor:
        spdk_thread_poll(thread);  /* run all pollers */
}
```

This never blocks — it just spins.

### Interrupt Mode Loop

```c
reactor_interrupt_run(reactor) {
    spdk_fd_group_wait(reactor->fgrp, -1);  /* blocks in epoll_wait */
}
```

The reactor blocks indefinitely (timeout = -1) until *any* fd in its fd_group
hierarchy fires. When an fd fires, the registered callback runs, which ultimately
executes the poller or processes a message.

### Cross-Reactor Notification

When a reactor is in interrupt mode, other cores need a way to wake it. This is
handled by `events_fd`:

```c
/* In spdk_event_call(), when sending to an interrupt-mode reactor: */
if (local_reactor == NULL ||
    spdk_cpuset_get_cpu(&local_reactor->notify_cpuset, event->lcore)) {
    write(reactor->events_fd, &notify, sizeof(notify));
}
```

If the caller is not on a reactor (e.g. an external thread), it always sends the
notification. Otherwise, the `notify_cpuset` bit for the destination reactor tells
the sender whether to write. These bits are set on every reactor when a reactor
enters interrupt mode and cleared when it returns to poll mode.

---

## Threads (spdk_thread)

Each `spdk_thread` also has:

- Its own `spdk_fd_group *fgrp`
- A `msg_fd` (eventfd) — wakes the thread when messages arrive from other threads

### Poll Mode — Thread Execution

In poll mode, the reactor calls `spdk_thread_poll()` for each thread on every
iteration. This calls `thread_poll()`, which iterates over active pollers and
checks timed pollers:

```c
/* Active pollers — run round-robin */
TAILQ_FOREACH(poller, &thread->active_pollers, ...) {
    thread_execute_poller(thread, poller);
}
/* Timed pollers — run when next_run_tick has passed */
while (timed poller && now >= poller->next_run_tick) {
    thread_execute_timed_poller(thread, poller, now);
}
```

### Interrupt Mode — Callback-Driven Execution

In interrupt mode, the reactor does **not** call `spdk_thread_poll()`. Instead,
all work is driven by fd callbacks dispatched from the reactor's
`spdk_fd_group_wait()`. Each thread's fd_group is nested into the reactor's
fd_group, so when any fd fires, the reactor's `epoll_wait` sees it and invokes
the registered callback directly. These callbacks include:

- `thread_interrupt_msg_process` — drains the thread's message ring (registered
  on `msg_fd` with `SPDK_FD_TYPE_EVENTFD`)
- `_interrupt_wrapper` — runs a poller's function with the correct thread context
  (registered on each poller's interrupt fd)
- `event_queue_run_batch` — processes cross-reactor events (registered on
  `events_fd`)

The thread's `msg_fd` is registered with `SPDK_FD_TYPE_EVENTFD`, which means the
fd_group reads and resets the eventfd counter before calling the handler. Poller
interrupt fds use `SPDK_FD_TYPE_DEFAULT`, so the framework does not auto-read them.

Note: `spdk_thread_poll()` does have an interrupt-mode path that calls
`spdk_fd_group_wait(thread->fgrp, 0)`, but since the thread's fd_group is nested,
this is effectively a no-op — it returns immediately without finding any events.
The real work happens through the reactor's fd_group callbacks described above.

### Cross-Thread Messaging

When a message is sent to a thread in interrupt mode:

```c
if (target_thread->in_interrupt) {
    write(target_thread->msg_fd, &notify, sizeof(notify));
}
```

This wakes the reactor (since the thread's fd_group is nested into the reactor's),
and the `thread_interrupt_msg_process` callback runs to drain the message queue.

---

## Pollers and the Interrupt Bridge

Pollers are the workhorses of SPDK — small functions that check for and process work.
A poller is registered with a `period_microseconds` value that determines its
scheduling behavior:

- **Active pollers** (`period_microseconds == 0`) — stored on the thread's
  `active_pollers` TAILQ, run round-robin on every poll iteration.
- **Timed pollers** (`period_microseconds > 0`) — stored in the thread's
  `timed_pollers` RB tree, run when `next_run_tick` has passed.
- **Paused pollers** — stored on the thread's `paused_pollers` TAILQ, waiting to
  be resumed or unregistered.

### Active Pollers in Interrupt Mode

Active pollers have no natural wake-up event, so the framework creates an
**eventfd** for each one:

```c
busy_poller_interrupt_init(poller) {
    busy_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    poller->intr = spdk_interrupt_register(busy_efd, poller->fn, poller->arg, ...);
}
```

When switching to interrupt mode, `write(busy_efd, 1)` makes the eventfd permanently
readable, so the poller fires on every `epoll_wait`. When switching back to poll mode,
`read(busy_efd)` clears it. This is essentially a "keep polling" signal for pollers
that don't have a natural fd to wait on.

### Timed Pollers in Interrupt Mode

Timed pollers use a **timerfd**:

```c
period_poller_interrupt_init(poller) {
    timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    poller->intr = spdk_interrupt_register(timerfd, interrupt_timerfd_process, poller, ...);
}
```

When switching to interrupt mode, `timerfd_settime()` arms the timer to fire at
`poller->next_run_tick`. When switching back to poll mode, the timer is disarmed and
the poller goes back into the timed poller tree for direct polling.

### Every Poller Must Have an fd

In interrupt mode, every poller must be backed by a file descriptor — there is no
mechanism to run a poller that is not attached to one. The framework ensures this
automatically: active pollers get an eventfd, timed pollers get a timerfd. Subsystems
that have their own natural notification fds (e.g., NVMe completion queues, vhost
kickfds) register them explicitly via `spdk_interrupt_register()` or
`spdk_interrupt_register_fd_group()`. A poller with no fd simply cannot fire in
interrupt mode.

### Custom Interrupt Registration

Some pollers have their own natural fds and need custom behavior when the thread
transitions between poll and interrupt mode. These use
`spdk_poller_register_interrupt()` to provide a set-interrupt-mode callback:

```c
spdk_poller_register_interrupt(poller, cb_fn, cb_arg);
```

The `cb_fn` is called with `interrupt_mode=true` when the thread enters interrupt
mode and `interrupt_mode=false` when it leaves. This gives the subsystem a chance to
arm or disarm its own notification mechanism.

#### Example: vhost-blk

The vhost-blk subsystem registers a callback for its request queue poller:

```c
spdk_poller_register_interrupt(bvsession->requestq_poller,
                               vhost_blk_poller_set_interrupt_mode,
                               bvsession);
```

The callback writes to each virtqueue's `kickfd` when entering interrupt mode:

```c
static void
vhost_user_session_set_interrupt_mode(struct spdk_vhost_session *vsession,
                                      bool interrupt_mode)
{
    uint16_t i;

    for (i = 0; i < vsession->max_queues; i++) {
        struct spdk_vhost_virtqueue *q = &vsession->virtqueue[i];
        uint64_t num_events = 1;

        if (interrupt_mode) {
            /* In case of race condition, always kick vring when switch to intr */
            write(q->vring.kickfd, &num_events, sizeof(num_events));
        }
    }
}
```

The `kickfd` is an eventfd provided by the guest VM during vhost-user negotiation
(VHOST_USER_SET_VRING_KICK). It is separately registered with
`spdk_interrupt_register()` so that guest I/O submissions wake the reactor through
the fd_group hierarchy. The callback's job is to kick the fd on the transition to
interrupt mode so that any work submitted during the race window between poll and
interrupt mode is not missed.

---

## spdk_interrupt — Wrapping FDs for Thread Safety

The `spdk_interrupt` structure wraps an fd with thread-context metadata:

```c
struct spdk_interrupt {
    int efd;                    /* the fd (eventfd, timerfd, etc.) */
    struct spdk_fd_group *fgrp; /* for fd_group-based interrupts */
    struct spdk_thread *thread; /* owning thread */
    spdk_interrupt_fn fn;       /* callback */
    void *arg;
    char name[SPDK_MAX_POLLER_NAME_LEN + 1];
};
```

When `spdk_interrupt_register()` is called, the fd is added to the thread's fd_group
with `_interrupt_wrapper` as the epoll callback. This wrapper switches the
thread-local storage to the owning thread before calling the actual callback, and
restores it after — critical because the reactor's `epoll_wait` runs all callbacks
from the reactor context, but each callback needs to execute as if it's on its own
thread.

### fd_group-based Interrupts

For subsystems that manage a group of fds (e.g., NVMe poll groups with multiple
queue-pair fds), there's `spdk_interrupt_register_fd_group()`:

```c
spdk_interrupt_register_fd_group(fgrp, "bdev_nvme_interrupt") {
    spdk_fd_group_set_wrapper(fgrp, interrupt_fd_group_wrapper, intr);
    spdk_fd_group_nest(thread->fgrp, fgrp);
}
```

This nests the subsystem's entire fd_group into the thread's fd_group, and sets a
wrapper so all callbacks in that group run with the correct thread context.

---

## Exhausting Work and Re-Asserting Readiness

When a poller's fd fires in interrupt mode, the fd_group dispatches the callback.
For fds registered with `SPDK_FD_TYPE_EVENTFD`, the framework reads the eventfd
*before* calling the handler, which resets the counter to zero. This means the fd
is no longer readable by the time the handler runs. If the handler does not drain
all pending work, the fd will not fire again and the remaining work will be lost.

Pollers in interrupt mode must therefore follow one of two rules:

1. **Exhaust all pending work** in a single callback invocation, or
2. **Re-assert fd readiness** by writing back to the eventfd so that `epoll_wait`
   fires the handler again.

The SPDK thread message queue demonstrates the re-assertion pattern. After
processing a batch of messages, it checks whether the ring still has entries:

```c
if (thread->in_interrupt && spdk_ring_count(thread->messages) != 0) {
    write(thread->msg_fd, &notify, sizeof(notify));
}
```

Other subsystems follow the same pattern. For example, bdev_aio writes the
remaining completion count back to its eventfd when more completions are pending
than can be processed in one batch. The vfio-user transport explicitly re-arms
its interrupt fd and adjusts eventidx before returning from its poll group handler
so that any subsequent guest doorbell write is guaranteed to wake the reactor.

Active pollers are exempt from this concern because their eventfd is never read by
the framework (they use `SPDK_FD_TYPE_DEFAULT`), so it stays permanently readable
as long as interrupt mode is active.

---

## The Full Nesting Hierarchy

When everything is in interrupt mode, the fd hierarchy looks like this:

```text
Reactor fd_group (root epoll — this is where epoll_wait blocks)
├── events_fd (eventfd)         — cross-reactor event notification
├── resched_fd (eventfd)        — thread scheduling changes
│
├── [nested] Thread A fd_group
│   ├── msg_fd (eventfd)        — cross-thread messages
│   ├── active_poller_1 (eventfd)— always-ready active poller
│   ├── timed_poller_1 (timerfd)— periodic timer
│   └── [nested] NVMe poll group fd_group
│       ├── qpair_1_fd          — completion queue fd
│       └── qpair_2_fd          — completion queue fd
│
├── [nested] Thread B fd_group
│   ├── msg_fd (eventfd)
│   ├── timed_poller_2 (timerfd)
│   └── ...
```

A single `epoll_wait()` at the reactor level sees every fd from every thread and
every subsystem. When any fd fires, the callback chain ensures the right thread
context is set before the handler runs.

---

## Mode Transition

Switching modes is coordinated through `spdk_reactor_set_interrupt_mode()`:

### Poll to Interrupt

1. `spdk_for_each_reactor()` sets the notification bit in every reactor's
   `notify_cpuset` so they'll write to `events_fd` when sending events to this
   reactor.
2. On the target reactor, `_reactor_set_interrupt_mode()` runs:
   a. `reactor->in_interrupt = true` — the reactor loop now calls
      `reactor_interrupt_run()` instead of `_reactor_run()`.
   b. For each thread, `spdk_fd_group_nest(reactor->fgrp, thread->fgrp)` nests
      thread fds into the reactor.
   c. Each thread receives a message to call
      `spdk_thread_set_interrupt_mode(true)`, which iterates all pollers
      (active, timed, and paused) and calls their `set_intr_cb_fn(..., true)` to
      arm timerfds/eventfds.

### Interrupt to Poll

1. On the target reactor, `_reactor_set_interrupt_mode()` runs:
   a. `reactor->in_interrupt = false`.
   b. For each thread: `spdk_fd_group_unnest(reactor->fgrp, thread->fgrp)`.
   c. Each thread receives a message to call
      `spdk_thread_set_interrupt_mode(false)`: timerfds are disarmed, eventfds
      cleared, pollers reinserted into the active/timed lists.
2. `spdk_for_each_reactor()` clears the `notify_cpuset` bits so other reactors
   stop writing to `events_fd`.

---

## Enabling Interrupt Mode

Interrupt mode is enabled globally before the application starts, either via the
`--interrupt-mode` command-line flag (available to any app using `spdk_app_parse_args`)
or by calling `spdk_interrupt_mode_enable()` before `spdk_app_start()`. This must
happen before the threading library is initialized because it controls whether
threads create fd_groups and whether pollers allocate their interrupt fds. Interrupt
mode is only supported on Linux.

When enabled, all reactors start in interrupt mode and remain there unless a
scheduler or RPC switches them back to poll mode.

## Dynamic Mode Switching

Reactors can switch between poll and interrupt mode at runtime, but this requires
interrupt mode to have been enabled at startup — without it, the fd_group
infrastructure is not initialized.

### Scheduler-Driven Switching

The **dynamic scheduler** (`scheduler_dynamic`) is currently the only scheduler that
switches modes. During its `balance()` callback it examines each core:

- Cores with **no threads** (both the scheduler's tracked count and the reactor's
  actual thread list are empty) are switched to interrupt mode and their CPU
  frequency is lowered via the governor.
- Cores with **threads** remain in poll mode. Non-main cores with threads have their
  CPU frequency raised; the main core's frequency is adjusted separately.

The **static scheduler** always sets all cores to poll mode and never switches.

### Manual Switching via RPC

The `interrupt_tgt` example application exposes a `reactor_set_interrupt_mode` RPC
that can switch individual cores between modes:

```bash
rpc.py --plugin interrupt_plugin reactor_set_interrupt_mode <lcore>
rpc.py --plugin interrupt_plugin reactor_set_interrupt_mode <lcore> -d  # disable
```

## Subsystem Support and Limitations

Not all SPDK subsystems support interrupt mode. When a subsystem does not support it,
poll group creation or initialization typically fails with `-ENOTSUP`.

| Subsystem | Interrupt Mode Support | Notes |
|-----------|----------------------|-------|
| NVMe bdev (PCIe) | Yes | PCI layer (VFIO/UIO) provides per-queue interrupt eventfds |
| NVMe bdev (fabrics) | No | TCP, RDMA, etc. return `-ENOTSUP` |
| bdev_aio | Yes | Creates its own eventfd for completions |
| vhost-blk | Yes | Registers guest-provided kickfds with `spdk_interrupt_register()` |
| vhost-scsi | Partial | No custom interrupt registration; relies on generic active-poller eventfd |
| NVMe-oF target (TCP) | Yes | |
| NVMe-oF target (RDMA) | Yes | |
| NVMe-oF target (FC) | No | Fails poll group creation |
| NVMe-oF target (vfio-user) | Yes | Requires `disable_mappable_bar0` transport option |
| sock (posix) | Yes | |
| sock (uring) | No | Returns `-ENOTSUP` at init |
| NBD | Yes | Uses socketpair fd |

---

## Key Design Points

- **Zero-copy of fds between modes**: the same timerfd/eventfd stays allocated
  regardless of mode — only its armed/disarmed state changes.
- **Thread safety through wrappers**: the `_interrupt_wrapper` and
  `interrupt_fd_group_wrapper` functions ensure callbacks always execute in the
  correct `spdk_thread` context, even though `epoll_wait` runs at the reactor level.
- **Active pollers are "degraded" in interrupt mode**: an active poller with an
  eventfd that's always readable effectively still polls — it fires on every
  `epoll_wait` return. Truly efficient interrupt mode requires subsystems to provide
  real fds (like NVMe completion eventfds) via `spdk_poller_register_interrupt`.
- **Interrupt mode must be enabled at startup**: dynamic switching at runtime depends
  on the fd_group infrastructure being initialized. Calling
  `spdk_reactor_set_interrupt_mode()` without prior `spdk_interrupt_mode_enable()` has
  no effect — fd_groups are not nested and pollers do not wake reactors correctly.
