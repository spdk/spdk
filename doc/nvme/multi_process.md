# NVMe Multi Process {#nvme_multi_process}

This capability enables the SPDK NVMe driver to support multiple processes accessing the
same NVMe device. The NVMe driver allocates critical structures from shared memory, so
that each process can map that memory and create its own queue pairs or share the admin
queue. There is a limited number of I/O queue pairs per NVMe controller.

The primary motivation for this feature is to support management tools that can attach
to long running applications, perform some maintenance work or gather information, and
then detach.

# Configuration {#nvme_multi_process_configuration}

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

# Scalability and Performance {#nvme_multi_process_scalability_performance}

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

# Limitations {#nvme_multi_process_limitations}

1. Two processes sharing memory may not share any cores in their core mask.
2. If a primary process exits while secondary processes are still running, those processes
will continue to run. However, a new primary process cannot be created.
3. Applications are responsible for coordinating access to logical blocks.

@sa spdk_nvme_probe, spdk_nvme_ctrlr_process_admin_completions
