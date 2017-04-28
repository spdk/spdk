# NVMe Driver {#nvme}

# Public Interface {#nvme_interface}

- spdk/nvme.h

# Key Functions {#nvme_key_functions}

Function                                    | Description
------------------------------------------- | -----------
spdk_nvme_probe()                           | @copybrief spdk_nvme_probe()
spdk_nvme_ns_cmd_read()                     | @copybrief spdk_nvme_ns_cmd_read()
spdk_nvme_ns_cmd_write()                    | @copybrief spdk_nvme_ns_cmd_write()
spdk_nvme_ns_cmd_dataset_management()       | @copybrief spdk_nvme_ns_cmd_dataset_management()
spdk_nvme_ns_cmd_flush()                    | @copybrief spdk_nvme_ns_cmd_flush()
spdk_nvme_qpair_process_completions()       | @copybrief spdk_nvme_qpair_process_completions()
spdk_nvme_ctrlr_cmd_admin_raw()             | @copybrief spdk_nvme_ctrlr_cmd_admin_raw()
spdk_nvme_ctrlr_process_admin_completions() | @copybrief spdk_nvme_ctrlr_process_admin_completions()


# NVMe Initialization {#nvme_initialization}

\msc

	app [label="Application"], nvme [label="NVMe Driver"];
	app=>nvme [label="nvme_probe()"];
	app<<nvme [label="probe_cb(pci_dev)"];
	nvme=>nvme [label="nvme_attach(devhandle)"];
	nvme=>nvme [label="nvme_ctrlr_start(nvme_controller ptr)"];
	nvme=>nvme [label="identify controller"];
	nvme=>nvme [label="create queue pairs"];
	nvme=>nvme [label="identify namespace(s)"];
	app<<nvme [label="attach_cb(pci_dev, nvme_controller)"];
	app=>app [label="create block devices based on controller's namespaces"];

\endmsc


# NVMe I/O Submission {#nvme_io_submission}

I/O is submitted to an NVMe namespace using nvme_ns_cmd_xxx functions
defined in nvme_ns_cmd.c.  The NVMe driver submits the I/O request
as an NVMe submission queue entry on the queue pair specified in the command.
The application must poll for I/O completion on each queue pair with outstanding I/O
to receive completion callbacks.

@sa spdk_nvme_ns_cmd_read, spdk_nvme_ns_cmd_write, spdk_nvme_ns_cmd_dataset_management,
spdk_nvme_ns_cmd_flush, spdk_nvme_qpair_process_completions


# NVMe Asynchronous Completion {#nvme_async_completion}

The userspace NVMe driver follows an asynchronous polled model for
I/O completion.

## I/O commands {#nvme_async_io}

The application may submit I/O from one or more threads on one or more queue pairs
and must call spdk_nvme_qpair_process_completions()
for each queue pair that submitted I/O.

When the application calls spdk_nvme_qpair_process_completions(),
if the NVMe driver detects completed I/Os that were submitted on that queue,
it will invoke the registered callback function
for each I/O within the context of spdk_nvme_qpair_process_completions().

## Admin commands {#nvme_async_admin}

The application may submit admin commands from one or more threads
and must call spdk_nvme_ctrlr_process_admin_completions()
from at least one thread to receive admin command completions.
The thread that processes admin completions need not be the same thread that submitted the
admin commands.

When the application calls spdk_nvme_ctrlr_process_admin_completions(),
if the NVMe driver detects completed admin commands submitted from any thread,
it will invote the registered callback function
for each command within the context of spdk_nvme_ctrlr_process_admin_completions().

It is the application's responsibility to manage the order of submitted admin commands.
If certain admin commands must be submitted while no other commands are outstanding,
it is the application's responsibility to enforce this rule
using its own synchronization method.


# NVMe over Fabrics Host Support {#nvme_fabrics_host}

The NVMe driver supports connecting to remote NVMe-oF targets and
interacting with them in the same manner as local NVMe controllers.

## Specifying Remote NVMe over Fabrics Targets {#nvme_fabrics_trid}

The method for connecting to a remote NVMe-oF target is very similar
to the normal enumeration process for local PCIe-attached NVMe devices.
To connect to a remote NVMe over Fabrics subsystem, the user may call
spdk_nvme_probe() with the `trid` parameter specifying the address of
the NVMe-oF target.
The caller may fill out the spdk_nvme_transport_id structure manually
or use the spdk_nvme_transport_id_parse() function to convert a
human-readable string representation into the required structure.

The spdk_nvme_transport_id may contain the address of a discovery service
or a single NVM subsystem.  If a discovery service address is specified,
the NVMe library will call the spdk_nvme_probe() `probe_cb` for each
discovered NVM subsystem, which allows the user to select the desired
subsystems to be attached.  Alternatively, if the address specifies a
single NVM subsystem directly, the NVMe library will call `probe_cb`
for just that subsystem; this allows the user to skip the discovery step
and connect directly to a subsystem with a known address.


# NVMe Multi Process {#nvme_multi_process}

This capability enables the SPDK NVMe driver to support multiple processes accessing the
same NVMe device. The NVMe driver allocates critical structures from shared memory, so
that each process can map that memory and create its own queue pairs or share the admin
queue. There is a limited number of I/O queue pairs per NVMe controller.

The primary motivation for this feature is to support management tools that can attach
to long running applications, perform some maintenance work or gather information, and
then detach.

## Configuration {#nvme_multi_process_configuration}

DPDK EAL allows different types of processes to be spawned, each with different permissions
on the hugepage memory used by the applications.

There are two types of processes:
1. a primary process which initializes the shared memory and has full privileges and
2. a secondary process which can attach to the primary process by mapping its shared memory
regions and perform NVMe operations including creating queue pairs.

This feature is enabled by default and is controlled by selecting a value for the shared
memory group ID. This ID is a positive integer and two applications with the same shared
memory group ID will share memory. The first application with a given shared memory group
ID will be considered the primary and all others secondary.

Example: identical shm_id and non-overlapping core masks
~~~{.sh}
./perf options [AIO device(s)]...
	[-c core mask for I/O submission/completion]
	[-i shared memory group ID]

./perf -q 1 -s 4096 -w randread -c 0x1 -t 60 -i 1
./perf -q 8 -s 131072 -w write -c 0x10 -t 60 -i 1
~~~

## Scalability and Performance {#nvme_multi_process_scalability_performance}

To maximize the I/O bandwidth of an NVMe device, ensure that each application has its own
queue pairs.

The optimal threading model for SPDK is one thread per core, regardless of which processes
that thread belongs to in the case of multi-process environment. To achieve maximum
performance, each thread should also have its own I/O queue pair. Applications that share
memory should be given core masks that do not overlap.

However, admin commands may have some performance impact as there is only one admin queue
pair per NVMe SSD. The NVMe driver will automatically take a cross-process capable lock
to enable the sharing of admin queue pair. Further, when each process polls the admin
queue for completions, it will only see completions for commands that it originated.

## Limitations {#nvme_multi_process_limitations}

1. Two processes sharing memory may not share any cores in their core mask.
2. If a primary process exits while secondary processes are still running, those processes
will continue to run. However, a new primary process cannot be created.
3. Applications are responsible for coordinating access to logical blocks.

@sa spdk_nvme_probe, spdk_nvme_ctrlr_process_admin_completions


# NVMe Hotplug {#nvme_hotplug}

At the NVMe driver level, we provide the following support for Hotplug:

1. Hotplug events detection:
The user of the NVMe library can call spdk_nvme_probe() periodically to detect
hotplug events. The probe_cb, followed by the attach_cb, will be called for each
new device detected. The user may optionally also provide a remove_cb that will be
called if a previously attached NVMe device is no longer present on the system.
All subsequent I/O to the removed device will return an error.

2. Hot remove NVMe with IO loads:
When a device is hot removed while I/O is occurring, all access to the PCI BAR will
result in a SIGBUS error. The NVMe driver automatically handles this case by installing
a SIGBUS handler and remapping the PCI BAR to a new, placeholder memory location.
This means I/O in flight during a hot remove will complete with an appropriate error
code and will not crash the application.

@sa spdk_nvme_probe
