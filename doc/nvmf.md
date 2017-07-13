# NVMe over Fabrics Target {#nvmf}

@sa @ref nvme_fabrics_host


# Getting Started Guide {#nvmf_getting_started}

The NVMe over Fabrics target is a user space application that presents block devices over the
network using RDMA. It requires an RDMA-capable NIC with its corresponding OFED software package
installed to run. The target should work on all flavors of RDMA, but it is currently tested against
Mellanox NICs (RoCEv2) and Chelsio NICs (iWARP).

The NVMe over Fabrics specification defines subsystems that can be exported over the network. SPDK
has chosen to call the software that exports these subsystems a "target", which is the term used
for iSCSI. The specification refers to the "client" that connects to the target as a "host". Many
people will also refer to the host as an "initiator", which is the equivalent thing in iSCSI
parlance. SPDK will try to stick to the terms "target" and "host" to match the specification.

There will be both a target and a host implemented in the Linux kernel, and these are available
today as a set of patches against the kernel 4.8 release candidate. All of the testing against th
SPDK target has been against the proposed Linux kernel host. This means that for at least the host
machine, the kernel will need to be a release candidate until the code is actually merged. For the
system running the SPDK target, however, you can run any modern flavor of Linux as required by your
NIC vendor's OFED distribution.

If you want to kill the application using signal, make sure use the SIGTERM, then the application
will release all the share memory resource before exit, the SIGKILL will make the share memory
resource have no chance to be released by application, you may need to release the resource manually.

## Prerequisites {#nvmf_prereqs}

This guide starts by assuming that you can already build the standard SPDK distribution on your
platform. By default, the NVMe over Fabrics target is not built. To build nvmf_tgt there are some
additional dependencies.

Fedora:
~~~{.sh}
dnf install libibverbs-devel librdmacm-devel
~~~

Ubuntu:
~~~{.sh}
apt-get install libibverbs-dev librdmacm-dev
~~~

Then build SPDK with RDMA enabled, either by editing CONFIG to enable CONFIG_RDMA or
enabling it on the `make` command line:

~~~{.sh}
make CONFIG_RDMA=y <other config parameters>
~~~

Once built, the binary will be in `app/nvmf_tgt`.

## Prerequisites for InfiniBand/RDMA Verbs {#nvmf_prereqs_verbs}

Before starting our NVMe-oF target we must load the InfiniBand and RDMA modules that allow
userspace processes to use InfiniBand/RDMA verbs directly.

~~~{.sh}
modprobe ib_cm
modprobe ib_core
modprobe ib_ucm
modprobe ib_umad
modprobe ib_uverbs
modprobe iw_cm
modprobe rdma_cm
modprobe rdma_ucm
~~~

## Prerequisites for RDMA NICs {#nvmf_prereqs_rdma_nics}

Before starting our NVMe-oF target we must detect RDMA NICs and assign them IP addresses.

### Mellanox ConnectX-3 RDMA NICs

~~~{.sh}
modprobe mlx4_core
modprobe mlx4_ib
modprobe mlx4_en
~~~

### Mellanox ConnectX-4 RDMA NICs

~~~{.sh}
modprobe mlx5_core
modprobe mlx5_ib
~~~

### Assigning IP addresses to RDMA NICs

~~~{.sh}
ifconfig eth1 192.168.100.8 netmask 255.255.255.0 up
ifconfig eth2 192.168.100.9 netmask 255.255.255.0 up
~~~


## Configuring NVMe over Fabrics Target {#nvmf_config}

A `nvmf_tgt`-specific configuration file is used to configure the NVMe over Fabrics target. This
file's primary purpose is to define subsystems. A fully documented example configuration file is
located at `etc/spdk/nvmf.conf.in`.

You should make a copy of the example configuration file, modify it to suit your environment, and
then run the nvmf_tgt application and pass it the configuration file using the -c option. Right now,
the target requires elevated privileges (root) to run.

~~~{.sh}
app/nvmf_tgt/nvmf_tgt -c /path/to/nvmf.conf
~~~

## Configuring NVMe over Fabrics Host {#nvmf_host}

Both the Linux kernel and SPDK implemented NVMe over Fabrics host. Users who want to test
`nvmf_tgt` with kernel based host should upgrade to Linux kernel 4.8 or later, or can use
Linux distributions Fedora or Ubuntu with Linux 4.8 kernel or later. A client tool nvme-cli
is recommended to connect/disconect with NVMe over Fabrics target subsystems. Before
connecting to remote subsystems, users should verify nvme-rdma driver is loaded.

Discovery:
~~~{.sh}
nvme discover -t rdma -a 192.168.100.8 -s 4420
~~~

Connect:
~~~{.sh}
nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a 192.168.100.8 -s 4420
~~~

Disconnect:
~~~{.sh}
nvme disconnect -n "nqn.2016-06.io.spdk.cnode1"
~~~

## Assigning CPU Cores to the NVMe over Fabrics Target {#nvmf_config_lcore}

SPDK uses the [DPDK Environment Abstraction Layer](http://dpdk.org/doc/guides/prog_guide/env_abstraction_layer.html)
to gain access to hardware resources such as huge memory pages and CPU core(s). DPDK EAL provides
functions to assign threads to specific cores.
To ensure the SPDK NVMe-oF target has the best performance, configure the RNICs, NVMe
devices, and the core assigned to the NVMe-oF subsystem to all be located on the same NUMA node.
The following parameters are used to control which CPU cores SPDK executes on:

**ReactorMask:** A hexadecimal bit mask of the CPU cores that SPDK is allowed to execute work
items on. The ReactorMask is located in the [Global] section of the configuration file. For example,
to assign lcores 24,25,26 and 27 to NVMe-oF work items, set the ReactorMask to:
~~~{.sh}
ReactorMask 0xF000000
~~~

**Assign each Subsystem to a CPU core:** This is accomplished by specifying the "Core" value in
the [Subsystem] section of the configuration file. For example,
to assign the Subsystems to lcores 25 and 26:
~~~{.sh}
[Nvme]
TransportID "trtype:PCIe traddr:0000:02:00.0" Nvme0
TransportID "trtype:PCIe traddr:0000:82:00.0" Nvme1

[Subsystem1]
NQN nqn.2016-06.io.spdk:cnode1
Core 25
Listen RDMA 192.168.100.8:4420
Host nqn.2016-06.io.spdk:init
SN SPDK00000000000001
Namespace Nvme0n1

[Subsystem2]
NQN nqn.2016-06.io.spdk:cnode2
Core 26
Listen RDMA 192.168.100.9:4420
SN SPDK00000000000002
Namespace Nvme1n1
~~~
SPDK executes all code for an NVMe-oF subsystem on a single thread. Different subsystems may execute
on different threads. SPDK gives the user maximum control to determine how many CPU cores are used
to execute subsystems. Configuring different subsystems to execute on different CPU cores prevents
the subsystem data from being evicted from limited CPU cache space.

## Emulating an NVMe controller {#nvmf_config_virtual_controller}

The SPDK NVMe-oF target provides the capability to emulate an NVMe controller using a virtual
controller. Using virtual controllers allows storage software developers to run the NVMe-oF target
on a system that does not have NVMe devices. You can configure a virtual controller in the configuration
file as follows:

**Create malloc LUNs:** See @ref bdev_getting_started for details on creating Malloc block devices.

**Create a virtual controller:** Any bdev may be presented as a namespace. For example, to create a
virtual controller with two namespaces backed by the malloc LUNs named Malloc0 and Malloc1:
~~~{.sh}
# Virtual controller
[Subsystem2]
  NQN nqn.2016-06.io.spdk:cnode2
  Core 0
  Listen RDMA 192.168.2.21:4420
  Host nqn.2016-06.io.spdk:init
  SN SPDK00000000000001
  Namespace Malloc0
  Namespace Malloc1
~~~
