# NVMe Driver {#nvme}

# In this document {#nvme_toc}

* @ref nvme_intro
* @ref nvme_examples
* @ref nvme_interface
* @ref nvme_design
* @ref nvme_fabrics_host
* @ref nvme_multi_process
* @ref nvme_hotplug
* @ref nvme_cuse

# Introduction {#nvme_intro}

The NVMe driver is a C library that may be linked directly into an application
that provides direct, zero-copy data transfer to and from
[NVMe SSDs](http://nvmexpress.org/). It is entirely passive, meaning that it spawns
no threads and only performs actions in response to function calls from the
application itself. The library controls NVMe devices by directly mapping the
[PCI BAR](https://en.wikipedia.org/wiki/PCI_configuration_space) into the local
process and performing [MMIO](https://en.wikipedia.org/wiki/Memory-mapped_I/O).
I/O is submitted asynchronously via queue pairs and the general flow isn't
entirely dissimilar from Linux's
[libaio](http://man7.org/linux/man-pages/man2/io_submit.2.html).

More recently, the library has been improved to also connect to remote NVMe
devices via NVMe over Fabrics. Users may now call spdk_nvme_probe() on both
local PCI busses and on remote NVMe over Fabrics discovery services. The API is
otherwise unchanged.

# Examples {#nvme_examples}

## Getting Start with Hello World {#nvme_helloworld}

There are a number of examples provided that demonstrate how to use the NVMe
library. They are all in the [examples/nvme](https://github.com/spdk/spdk/tree/master/examples/nvme)
directory in the repository. The best place to start is
[hello_world](https://github.com/spdk/spdk/blob/master/examples/nvme/hello_world/hello_world.c).

## Running Benchmarks with Fio Plugin {#nvme_fioplugin}

SPDK provides a plugin to the very popular [fio](https://github.com/axboe/fio)
tool for running some basic benchmarks. See the fio start up
[guide](https://github.com/spdk/spdk/blob/master/examples/nvme/fio_plugin/)
for more details.

## Running Benchmarks with Perf Tool {#nvme_perf}

NVMe perf utility in the [examples/nvme/perf](https://github.com/spdk/spdk/tree/master/examples/nvme/perf)
is one of the examples which also can be used for performance tests. The fio
tool is widely used because it is very flexible. However, that flexibility adds
overhead and reduces the efficiency of SPDK. Therefore, SPDK provides a perf
benchmarking tool which has minimal overhead during benchmarking. We have
measured up to 2.6 times more IOPS/core when using perf vs. fio with the
4K 100% Random Read workload. The perf benchmarking tool provides several
run time options to support the most common workload. The following examples
demonstrate how to use perf.

Example: Using perf for 4K 100% Random Read workload to a local NVMe SSD for 300 seconds
~~~{.sh}
perf -q 128 -o 4096 -w randread -r 'trtype:PCIe traddr:0000:04:00.0' -t 300
~~~

Example: Using perf for 4K 100% Random Read workload to a remote NVMe SSD exported over the network via NVMe-oF
~~~{.sh}
perf -q 128 -o 4096 -w randread -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420' -t 300
~~~

Example: Using perf for 4K 70/30 Random Read/Write mix workload to all local NVMe SSDs for 300 seconds
~~~{.sh}
perf -q 128 -o 4096 -w randrw -M 70 -t 300
~~~

Example: Using perf for extended LBA format CRC guard test to a local NVMe SSD,
users must write to the SSD before reading the LBA from SSD
~~~{.sh}
perf -q 1 -o 4096 -w write -r 'trtype:PCIe traddr:0000:04:00.0' -t 300 -e 'PRACT=0,PRCKH=GUARD'
perf -q 1 -o 4096 -w read -r 'trtype:PCIe traddr:0000:04:00.0' -t 200 -e 'PRACT=0,PRCKH=GUARD'
~~~

# Public Interface {#nvme_interface}

- spdk/nvme.h

Key Functions                               | Description
------------------------------------------- | -----------
spdk_nvme_probe()                           | @copybrief spdk_nvme_probe()
spdk_nvme_ctrlr_alloc_io_qpair()            | @copybrief spdk_nvme_ctrlr_alloc_io_qpair()
spdk_nvme_ctrlr_get_ns()                    | @copybrief spdk_nvme_ctrlr_get_ns()
spdk_nvme_ns_cmd_read()                     | @copybrief spdk_nvme_ns_cmd_read()
spdk_nvme_ns_cmd_readv()                    | @copybrief spdk_nvme_ns_cmd_readv()
spdk_nvme_ns_cmd_read_with_md()             | @copybrief spdk_nvme_ns_cmd_read_with_md()
spdk_nvme_ns_cmd_write()                    | @copybrief spdk_nvme_ns_cmd_write()
spdk_nvme_ns_cmd_writev()                   | @copybrief spdk_nvme_ns_cmd_writev()
spdk_nvme_ns_cmd_write_with_md()            | @copybrief spdk_nvme_ns_cmd_write_with_md()
spdk_nvme_ns_cmd_write_zeroes()             | @copybrief spdk_nvme_ns_cmd_write_zeroes()
spdk_nvme_ns_cmd_dataset_management()       | @copybrief spdk_nvme_ns_cmd_dataset_management()
spdk_nvme_ns_cmd_flush()                    | @copybrief spdk_nvme_ns_cmd_flush()
spdk_nvme_qpair_process_completions()       | @copybrief spdk_nvme_qpair_process_completions()
spdk_nvme_ctrlr_cmd_admin_raw()             | @copybrief spdk_nvme_ctrlr_cmd_admin_raw()
spdk_nvme_ctrlr_process_admin_completions() | @copybrief spdk_nvme_ctrlr_process_admin_completions()
spdk_nvme_ctrlr_cmd_io_raw()                | @copybrief spdk_nvme_ctrlr_cmd_io_raw()
spdk_nvme_ctrlr_cmd_io_raw_with_md()        | @copybrief spdk_nvme_ctrlr_cmd_io_raw_with_md()

# NVMe Driver Design {#nvme_design}

## NVMe I/O Submission {#nvme_io_submission}

I/O is submitted to an NVMe namespace using nvme_ns_cmd_xxx functions. The NVMe
driver submits the I/O request as an NVMe submission queue entry on the queue
pair specified in the command. The function returns immediately, prior to the
completion of the command. The application must poll for I/O completion on each
queue pair with outstanding I/O to receive completion callbacks by calling
spdk_nvme_qpair_process_completions().

@sa spdk_nvme_ns_cmd_read, spdk_nvme_ns_cmd_write, spdk_nvme_ns_cmd_dataset_management,
spdk_nvme_ns_cmd_flush, spdk_nvme_qpair_process_completions

### Scaling Performance {#nvme_scaling}

NVMe queue pairs (struct spdk_nvme_qpair) provide parallel submission paths for
I/O. I/O may be submitted on multiple queue pairs simultaneously from different
threads. Queue pairs contain no locks or atomics, however, so a given queue
pair may only be used by a single thread at a time. This requirement is not
enforced by the NVMe driver (doing so would require a lock), and violating this
requirement results in undefined behavior.

The number of queue pairs allowed is dictated by the NVMe SSD itself. The
specification allows for thousands, but most devices support between 32
and 128. The specification makes no guarantees about the performance available from
each queue pair, but in practice the full performance of a device is almost
always achievable using just one queue pair. For example, if a device claims to
be capable of 450,000 I/O per second at queue depth 128, in practice it does
not matter if the driver is using 4 queue pairs each with queue depth 32, or a
single queue pair with queue depth 128.

Given the above, the easiest threading model for an application using SPDK is
to spawn a fixed number of threads in a pool and dedicate a single NVMe queue
pair to each thread. A further improvement would be to pin each thread to a
separate CPU core, and often the SPDK documentation will use "CPU core" and
"thread" interchangeably because we have this threading model in mind.

The NVMe driver takes no locks in the I/O path, so it scales linearly in terms
of performance per thread as long as a queue pair and a CPU core are dedicated
to each new thread. In order to take full advantage of this scaling,
applications should consider organizing their internal data structures such
that data is assigned exclusively to a single thread. All operations that
require that data should be done by sending a request to the owning thread.
This results in a message passing architecture, as opposed to a locking
architecture, and will result in superior scaling across CPU cores.

## NVMe Driver Internal Memory Usage {#nvme_memory_usage}

The SPDK NVMe driver provides a zero-copy data transfer path, which means that
there are no data buffers for I/O commands. However, some Admin commands have
data copies depending on the API used by the user.

Each queue pair has a number of trackers used to track commands submitted by the
caller. The number trackers for I/O queues depend on the users' input for queue
size and the value read from controller capabilities register field Maximum Queue
Entries Supported(MQES, 0 based value). Each tracker has a fixed size 4096 Bytes,
so the maximum memory used for each I/O queue is: (MQES + 1) * 4 KiB.

I/O queue pairs can be allocated in host memory, this is used for most NVMe controllers,
some NVMe controllers which can support Controller Memory Buffer may put I/O queue
pairs at controllers' PCI BAR space, SPDK NVMe driver can put I/O submission queue
into controller memory buffer, it depends on users' input and controller capabilities.
Each submission queue entry (SQE) and completion queue entry (CQE) consumes 64 bytes
and 16 bytes respectively. Therefore, the maximum memory used for each I/O queue
pair is (MQES + 1) * (64 + 16) Bytes.

# NVMe over Fabrics Host Support {#nvme_fabrics_host}

The NVMe driver supports connecting to remote NVMe-oF targets and
interacting with them in the same manner as local NVMe SSDs.

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

## RDMA Limitations

Please refer to NVMe-oF target's @ref nvmf_rdma_limitations

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

./perf -q 1 -o 4096 -w randread -c 0x1 -t 60 -i 1
./perf -q 8 -o 131072 -w write -c 0x10 -t 60 -i 1
~~~

## Limitations {#nvme_multi_process_limitations}

1. Two processes sharing memory may not share any cores in their core mask.
2. If a primary process exits while secondary processes are still running, those processes
will continue to run. However, a new primary process cannot be created.
3. Applications are responsible for coordinating access to logical blocks.
4. If a process exits unexpectedly, the allocated memory will be released when the last
process exits.

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

# NVMe Character Devices {#nvme_cuse}

This feature is considered as experimental.

![NVMe character devices processing diagram](nvme_cuse.svg)

For each controller as well as namespace, character devices are created in the
locations:
~~~{.sh}
    /dev/'dev_path'
    /dev/'dev_path'nY
    ...
~~~

Requests from CUSE are handled by pthreads when controller and namespaces are created.
Those pass the I/O or admin commands via a ring to a thread that processes them using
spdk_nvme_io_msg_process().

Ioctls that request information attained when attaching NVMe controller receive an
immediate response, without passing them through the ring.

This interface reserves one qpair for sending down the I/O for each controller.

## Enabling cuse support for NVMe

Cuse support is disabled by default. To enable support for NVMe devices SPDK
must be compiled with "./configure --with-nvme-cuse".

## Limitations

NVMe CUSE presents character device for controller and namespaces only at the time
the controller is being attached. Dynamic creation/deletion of namespaces is not
supported yet.
