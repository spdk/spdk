# NUMA Guidelines {#numa_guidelines}

This page provides optional guidelines for experimenting with SPDK on NUMA
hardware. These steps are intended for development and performance tuning only
as specific benefits have not yet been established.

## Pre-Requisites (For Linux systems)

1. Check NUMA topology(which cores belong to each NUMA node and memory
   distribution)

    ~~~{.sh}
    numactl -H
    ~~~

2. Use the SPDK setup script to view the NUMA node for all tracked drives:

    ~~~{.sh}
    ./scripts/setup.sh status
    ~~~

    Alternatively, you can check the Linux sysfs path directly:

    ~~~{.sh}
    cat /sys/block/nvmeXnY/device/numa_node
    ~~~

3. Identify the NUMA node that each network interface is attached to:

    ~~~{.sh}
    cat /sys/class/net/NAME/device/numa_node
    ~~~

    where `NAME` is the device name as seen in the `ip a` or `ip link` command
    output.

## CPU Restrictions and NUMA Alignment

To optimize performance, SPDK applications should run on CPU cores that reside
on the same NUMA node as the attached NVMe drives. You can restrict an SPDK
application to specific CPU cores using the `-m` (hexadecimal bitmask),
`--cpumask` (hexadecimal bitmask), or `--lcores` (explicit core list)
arguments.

The `--lcores` argument is the most explicit way to ensure NUMA alignment.

### Example: Starting an application on specific NUMA cores

If your drive is on NUMA node 1, and cores 16-24 belong to NUMA node 1, start
the application using `--lcores`:

~~~{.sh}
./build/bin/spdk_tgt --lcores 16-24
~~~

## Memory Configuration and Constraints

SPDK applications offer configuration parameters to control and restrict
memory allocation for queue pairs, internal buffers, and hugepages.

### Initial Setup of SPDK

Use the `HUGENODE` environment variable to specify the number of hugepages to
allocate on each NUMA node. For clarity and consistent performance, it is
recommended to explicitly define the page count for every node in the system.

**Example:**

~~~{.sh}
HUGENODE='nodes_hp[0]=2048,nodes_hp[1]=512' ./scripts/setup.sh config
~~~

This command explicitly allocates 2048 pages for node0 and 512 pages for
node1.

### Environment Configuration

SPDK applications support the `--enforce-numa` argument to strict-bind
explicit NUMA memory allocation requests.

By default, if an internal component requests memory from a specific NUMA node
and that node has insufficient memory, the allocator will silently fall back
to allocating memory from a remote NUMA node. Enabling `--enforce-numa`
disables this fallback behavior entirely. If the requested target NUMA node
lacks sufficient memory, the allocation request will fail rather than crossing
NUMA boundaries.

#### Example: Running the target with strict NUMA enforcement

~~~{.sh}
./build/bin/spdk_tgt --enforce-numa
~~~

*Note: This parameter disables the cross-node allocation fallback for
component-level requests. It does not automatically guarantee local allocation
across all unconfigured application components.*

### DPDK EAL Parameter

As part of the DPDK Environment Abstraction Layer (EAL), the following
NUMA-related options can be configured for individual applications. These
settings are application-specific and can be passed via the `--env-context`
command-line parameter supported by the respective applications:

1. Preallocate specified amounts of memory per NUMA node through:

    ~~~{.sh}
    --numa-mem <amounts of memory per NUMA node>
    ~~~

2. Place a per-NUMA node upper limit on memory use (non-legacy memory mode
   only). A value of `0` will disable the limit for a particular NUMA node.

    ~~~{.sh}
    --numa-limit <amounts of memory per NUMA node>
    ~~~

The `--numa-limit` option is helpful in restricting the memory allocations of an
SPDK application to a specific NUMA node by setting a value of `0` for remote
nodes.

**Example:**

~~~{.sh}
--numa-mem 0,1024
~~~

This configuration reserves 1024 MB of memory on NUMA node 1 while allocating
none on NUMA node 0.

## Overall SPDK Stack Configuration Guideline

While the above sections provide an overview of various parameters exposed by
SPDK and/or DPDK to restrict operations to a particular NUMA node, below is a
generic guideline to achieve optimum throughput on NUMA hardware:

1. Ensure that the SPDK applications are launched on a set of cores that
   belong to the same NUMA node by utilizing the `--cpumask` or `--lcores`
   options.

2. Ensure that the bdevs are created with backing memory or drives that belong
   to the same NUMA node as that of the applications using them.

3. To achieve optimum throughput, configure any required network devices used
   for communication with the subsystems whose NUMA node matches with the
   core configuration of the application.

4. Ensure that the SPDK applications have both the `--enforce-numa` SPDK
   environment option and the DPDK EAL parameter `--numa-mem` configured.
