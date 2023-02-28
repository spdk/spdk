# NVMe Multipath {#nvme_multipath}

## Introduction

The NVMe bdev module supports two modes: failover and multipath. In failover mode, only one
active connection is maintained and alternate paths are connected only during the switch-over.
This can lead to delays and failed I/O reported to upper layers, but it does reduce the number
of active connections at any given time. In multipath, active connections are maintained for
every path and used based on a policy of either active-passive or active-active. The multipath
mode also supports Asymmetric Namespace Access (ANA) and uses that to make policy decisions.

## Design

### Multipath Mode

A user may establish connections on multiple independent paths to the same NVMe-oF subsystem
for NVMe bdevs by calling the `bdev_nvme_attach_controller` RPC multiple times with the same NVMe
bdev controller name. Additionally, the `multipath` parameter for this RPC must be set to
"multipath" when connecting the second or later paths.

For each path created by the `bdev_nvme_attach_controller` RPC, an NVMe-oF controller is created.
Then the set of namespaces presented by that controller are discovered. For each namespace found,
the NVMe bdev module attempts to match it with an existing NVMe bdev. If it finds a match, it adds
the given namespace as an alternate path. If it does not find a match, it creates a new NVMe bdev.

I/O and admin qpairs are necessary to access an NVMe-oF controller. A single admin qpair is created
and is shared by all SPDK threads. To submit I/O without taking locks, for each SPDK thread, an I/O
qpair is created as a dynamic context of an I/O channel for an NVMe-oF controller.

For each SPDK thread, the NVMe bdev module creates an I/O channel for an NVMe bdev and provides it to
the upper layer. The I/O channel for the NVMe bdev has an I/O path for each namespace. I/O path is
an additional abstraction to submit I/O to a namespace, and consists of an I/O qpair context and a
namespace. If an NVMe bdev has multiple namespaces, an I/O channel for the NVMe bdev has a list of
multiple I/O paths. The I/O channel for the NVMe bdev has a retry I/O list and has a path selection
policy.

### Path Error Recovery

If the NVMe driver detects an error on a qpair, it disconnects the qpair and notifies the error to
the NVMe bdev module. Then the NVMe bdev module starts resetting the corresponding NVMe-oF controller.
The NVMe-oF controller reset consists of the following steps: 1) disconnect and delete all I/O qpairs,
2) disconnect admin qpair, 3) connect admin qpair, 4) configure the NVMe-oF controller, and
5) create and connect all I/O qpairs.

If the step 3, 4, or 5 fails, the reset reverts to the step 3 and then it is retried after
`reconnect_delay_sec` seconds. Then the NVMe-oF controller is deleted automatically if it is not
recovered within `ctrlr_loss_timeout_sec` seconds. If `ctrlr_loss_timeout_sec` is -1, it retries
indefinitely.

By default, error detection on a qpair is very slow for TCP and RDMA transports. For fast error
detection, a global option, `transport_ack_timeout`, is useful.

### Path Selection

Multipath mode supports two path selection policies, active-passive or active-active.

For both path selection policies, only ANA optimal I/O paths are used unless there are no ANA
optimal I/O paths available.

For active-passive policy, each I/O channel for an NVMe bdev has a cache to store the first found
I/O path which is connected and optimal from ANA and use it for I/O submission. Some users may want
to specify the preferred I/O path manually. They can dynamically set the preferred I/O path using
the `bdev_nvme_set_preferred_path` RPC. Such assignment is realized naturally by moving the
I/O path to the head of the I/O path list. By default, if the preferred I/O path is restored,
failback to it is done automatically. The automatic failback can be disabled by a global option
`disable_auto_failback`. In this case, the `bdev_nvme_set_preferred_path` RPC can be used
to do manual failback.

The active-active policy uses the round-robin algorithm and submits an I/O to each I/O path in
circular order.

### I/O Retry

The NVMe bdev module has a global option, `bdev_retry_count`, to control the number of retries when
an I/O is returned with error. Each I/O has a retry count. If the retry count of an I/O is less than
the `bdev_retry_count`, the I/O is allowed to retry and the retry count is incremented.

NOTE: The `bdev_retry_count` is not directly used but is required to be non-zero for the process
of multipath mode failing over to a different path because the retry count is checked first always
when an I/O is returned with error.

Each I/O has a timer to schedule an I/O retry at a particular time in the future. Each I/O channel
for an NVMe bdev has a sorted I/O retry list. Retried I/Os are inserted into the I/O retry list.

If an I/O is returned with error, the I/O completion handler in the NVMe bdev module executes the
following steps:

1. If the DNR (Do Not Retry) bit is set or the retry count exceeds the limit, then complete the
   I/O with the returned error.
2. If the error is a path error, insert the I/O to the I/O retry list with no delay.
3. Otherwise, insert the I/O to the I/O retry list with the delay reported by the CRD (Command
   Retry Delay).

Then the I/O retry poller is scheduled to the closest expiration. If there is no retried I/O,
the I/O retry poller is stopped.

When submitting an I/O, there may be no available I/O path. If there is any I/O path which is
recovering, the I/O is inserted to the I/O retry list with one second delay. This may result in
queueing many I/Os indefinitely. To avoid such indefinite queueing, per NVMe-oF controller option,
`fast_io_fail_timeout_sec`, is added. If the corresponding NVMe-oF controller is not recovered
within `fast_io_fail_timeout_sec` seconds, the I/O is not queued to wait the recovery but returned
with an I/O error to the upper layer.

### Asymmetric Namespace Accesses (ANA) Handling

If an I/O is returned with an ANA error or an ANA change notice event is received, the ANA log page
may be changed. In this case, the NVMe bdev module reads the ANA log page to check the ANA state
changes.

As described before, only ANA optimal I/O paths will be used unless there are no ANA optimal paths
available.

If an I/O path is in ANA transition, i.e., its namespace reports the ANA inaccessible state or the ANA
change state, the NVMe bdev module queues I/Os to wait until the namespace becomes accessible again.
The ANA transition should end within the ANATT (ANA Transition Time) seconds. If the namespace does
not report the ANA optimized state or the ANA accessible state within the ANATT seconds, I/Os are
returned with an I/O error to the upper layer.

### I/O Timeout

The NVMe driver supports I/O timeout for submitted I/Os. The NVMe bdev module provides three
actions when an I/O timeout is notified from the NVMe driver, ABORT, RESET, or NONE. Users can
choose one of the actions as a global option, `action_on_timeout`. Users can set different timeout
values for I/O commands and admin commands by global options, `timeout_us` and `timeout_admin_us`.

For ABORT, the NVMe bdev module tries aborting the timed out I/O, and if failed, it starts the
NVMe-oF controller reset. For RESET, the NVMe bdev module starts the NVMe-oF controller reset.

## Usage

The following is an example to attach two NVMe-oF controllers and aggregate these into a single
NVMe bdev controller `Nvme0`.

```bash
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme0 -t rdma -a 192.168.100.8 -s 4420 -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -l -1 -o 20
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme0 -t rdma -a 192.168.100.9 -s 4420 -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -l -1 -o 20 -x multipath
```

In this example, if these two NVMe-oF controllers have a shared namespace whose namespace ID is 1,
a single NVMe bdev `Nvme0n1` is created. For the NVMe bdev module, the default value of
`bdev_retry_count` is 3 and I/O retry is enabled by default. `ctrlr_loss_timeout_sec` is set to -1
and `reconnect_delay_sec` is set to 20. Hence, NVMe-oF controller reconnect will be retried once
per 20 seconds until it succeeds.

To confirm if multipath is configured correctly, two RPCs, `bdev_get_bdevs` and
`bdev_nvme_get_controllers` are available.

```bash
./scripts/rpc.py bdev_get_bdevs -b Nvme0n1
./scripts/rpc.py bdev_nvme_get_controllers -n Nvme0
```

To monitor the current multipath state, a RPC `bdev_nvme_get_io_paths` are available.

```bash
./scripts/rpc.py bdev_nvme_get_io_paths -n Nvme0n1
```

## Limitations

SPDK NVMe multipath is transport protocol independent. Heterogeneous multipath configuration (e.g.,
TCP and RDMA) is supported. However, in this type of configuration, memory domain is not available
yet because memory domain is supported only by the RDMA transport now.

The RPCs, `bdev_get_iostat` and  `bdev_nvme_get_transport_statistics` display I/O statistics but
both are not aware of multipath.
