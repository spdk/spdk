# Scheduler {#scheduler}

SPDK's event/application framework (`lib/event`) now supports scheduling of
lightweight threads. Schedulers are provided as plugins, called
implementations. A default implementation is provided, but users may wish to
write their own scheduler to integrate into broader code frameworks or meet
their performance needs.

This feature should be considered experimental and is disabled by default. When
enabled, the scheduler framework gathers data for each spdk thread and reactor
and passes it to a scheduler implementation to perform one of the following
actions.

## Actions

### Move a thread

`spdk_thread`s can be moved to another reactor. Schedulers can examine the
suggested cpu_mask value for each lightweight thread to see if the user has
requested specific reactors, or choose a reactor using whatever algorithm they
deem fit.

### Switch reactor mode

Reactors by default run in a mode that constantly polls for new actions for the
most efficient processing. Schedulers can switch a reactor into a mode that
instead waits for an event on a file descriptor. On Linux, this is implemented
using epoll. This results in reduced CPU usage but may be less responsive when
events occur. A reactor cannot enter this mode if any `spdk_threads` are
currently scheduled to it. This limitation is expected to be lifted in the
future, allowing `spdk_threads` to enter interrupt mode.

### Set frequency of CPU core

The frequency of CPU cores can be modified by the scheduler in response to
load. Only CPU cores that match the application cpu_mask may be modified. The
mechanism for controlling CPU frequency is pluggable and the default provided
implementation is called `dpdk_governor`, based on the `rte_power` library from
DPDK.

#### Known limitation

When SMT (Hyperthreading) is enabled the two logical CPU cores sharing a single
physical CPU core must run at the same frequency. If one of two of such logical
CPU cores is outside the application cpu_mask, the policy and frequency on that
core has to be managed by the administrator.

## Scheduler implementations

The scheduler in use may be controlled by JSON-RPC. Please use the
[framework_set_scheduler](jsonrpc.html#rpc_framework_set_scheduler) RPC to
switch between schedulers or change their options. Currently only dynamic
scheduler supports changing its parameters.

[spdk_top](spdk_top.html#spdk_top) is a useful tool to observe the behavior of
schedulers in different scenarios and workloads.

### static [default]

The `static` scheduler is the default scheduler and does no dynamic scheduling.
Lightweight threads are distributed round-robin among reactors, respecting
their requested cpu_mask, only at application startup, and then they are never
moved. This is equivalent to the previous behavior of the SPDK event/application
framework.

The `static` scheduler cannot be re-enabled after a different scheduler has been
selected, because currently there is no way to save original SPDK thread distribution
configuration.

### dynamic

The `dynamic` scheduler is designed for power saving and reduction of CPU
utilization, especially in cases where workloads show large variations over
time. In SPDK thread and core workloads are measured in CPU ticks. Those
values are then compared with all the ticks since the last check, which allows
to calculate `busy time`.

`busy time = busy ticks / (busy tick + idle tick) * 100 %`

The thread is considered to be active, if its busy time is over the `load limit`
parameter.

Active threads are distributed equally among reactors, taking cpu_mask into
account. All idle threads are moved to the main core. Once an idle thread becomes
active, it is redistributed again. Dynamic scheduler monitors core workloads and
redistributes SPDK threads on cores in a way that none of them is over `core limit`.
In case a core utilization surpasses this threshold, scheduler should move threads
out of it until this condition no longer applies. Cores might also be in overloaded
state, which indicates that moving threads out of this core will not decrease its
utilization under the `core limit` and the threads are unable to process all the I/O
they are capable of, because they share CPU ticks with other threads. The threshold
to decide if a core is overloaded is called `core busy`. Note that threads residing
on an overloaded core will not perform as good as other threads, because the CPU ticks
intended for them are limited by other threads on the same core.

When a reactor has no scheduled `spdk_thread`s it is switched into interrupt
mode and stops actively polling. After enough threads become active, the
reactor is switched back into poll mode and threads are assigned to it again.

The main core can contain active threads only when their execution time does
not exceed the sum of all idle threads. When no active threads are present on
the main core, the frequency of that CPU core will decrease as the load
decreases. All CPU cores corresponding to the other reactors remain at maximum
frequency.

The dynamic scheduler is currently the only one that allows manual setting of
its parameters.

Current values of scheduler parameters can be displayed by using
[framework_get_scheduler](jsonrpc.html#rpc_framework_get_scheduler) RPC.
