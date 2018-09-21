# NVMe over Fabrics Target {#nvmf}

@sa @ref nvme_fabrics_host


# NVMe-oF Target Getting Started Guide {#nvmf_getting_started}

The NVMe over Fabrics target is a user space application that presents block devices over the
network using RDMA. It requires an RDMA-capable NIC with its corresponding OFED software package
installed to run. The target should work on all flavors of RDMA, but it is currently tested against
Mellanox NICs (RoCEv2) and Chelsio NICs (iWARP).

The NVMe over Fabrics specification defines subsystems that can be exported over the network. SPDK
has chosen to call the software that exports these subsystems a "target", which is the term used
for iSCSI. The specification refers to the "client" that connects to the target as a "host". Many
people will also refer to the host as an "initiator", which is the equivalent thing in iSCSI
parlance. SPDK will try to stick to the terms "target" and "host" to match the specification.

The Linux kernel also implements an NVMe-oF target and host, and SPDK is tested for
interoperability with the Linux kernel implementations.

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

Then build SPDK with RDMA enabled:

~~~{.sh}
./configure --with-rdma <other config parameters>
make
~~~

Once built, the binary will be in `app/nvmf_tgt`.

## Prerequisites for InfiniBand/RDMA Verbs {#nvmf_prereqs_verbs}

Before starting our NVMe-oF target we must load the InfiniBand and RDMA modules that allow
userspace processes to use InfiniBand/RDMA verbs directly.

~~~{.sh}
modprobe ib_cm
modprobe ib_core
# Please note that ib_ucm does not exist in newer versions of the kernel and is not required.
modprobe ib_ucm || true
modprobe ib_umad
modprobe ib_uverbs
modprobe iw_cm
modprobe rdma_cm
modprobe rdma_ucm
~~~

## Prerequisites for RDMA NICs {#nvmf_prereqs_rdma_nics}

Before starting our NVMe-oF target we must detect RDMA NICs and assign them IP addresses.

### Finding RDMA NICs and associated network interfaces

~~~{.sh}
ls /sys/class/infiniband/*/device/net
~~~

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

## Configuring the SPDK NVMe over Fabrics Target {#nvmf_config}

An NVMe over Fabrics target can be configured using JSON RPC calls or a .ini style configuration file.
While both methods will be detailed below, the preferred method is over RPC.
The basic RPC calls needed to configure the NVMe-oF subsystem are detailed below. More information about
working with NVMe over Fabrics specific RPCs can be found at (@ref jsonrpc_components_nvmf_tgt).

### Using RPCs {#nvmf_config_rpc}

Start the nvmf_tgt application with elevated privileges and instruct it to wait for rpcs.
The set_nvmf_target_options RPC can then be used to configure basic target parameters.
Below is an example where the target is configured with an I/O unit size of 8192,
4 max qpairs per controller, and an in capsule data size of 0. The parameters controlled
by set_nvmf_target_options may only be modified before the SPDK NVMe-oF subsystem is initialized.
Once the target options are configured. You need to start the NVMe-oF subsystem with start_subsystem_init.

~~~{.sh}
app/nvmf_tgt/nvmf_tgt --wait-for-rpc
scripts/rpc.py set_nvmf_target_options -u 8192 -p 4 -c 0
scripts/rpc.py start_subsystem_init
~~~

Note: The start_subsystem_init rpc is referring to SPDK application subsystems and not the NVMe over Fabrics concept.

#### Subsystem Configuration with RPCs

Below is an example of creating a malloc bdev and assigning it to a subsystem. Please adjust the bdevs,
NQN, serial number, and IP address to your own circumstances.

~~~{.sh}
scripts/rpc.py construct_malloc_bdev -b Malloc0 512 512
scripts/rpc.py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a 192.168.100.8 -s 4420
~~~

### Using a configuration file (Legacy) {#nvmf_config_file}

A `nvmf_tgt`-specific configuration file can be used to configure the NVMe over Fabrics target. This
file's primary purpose is to define subsystems. A fully documented example configuration file is
located at `etc/spdk/nvmf.conf.in`.

You should make a copy of the example configuration file, modify it to suit your environment, and
then run the nvmf_tgt application and pass it the configuration file using the -c option. Right now,
the target requires elevated privileges (root) to run.

~~~{.sh}
app/nvmf_tgt/nvmf_tgt -c /path/to/nvmf.conf
~~~

#### Subsystem Configuration with a configuration file

The `[Subsystem]` section in the configuration file is used to configure
subsystems for the NVMe-oF target.

This example shows two local PCIe NVMe devices exposed as separate NVMe-oF target subsystems:

~~~{.sh}
[Nvme]
TransportID "trtype:PCIe traddr:0000:02:00.0" Nvme0
TransportID "trtype:PCIe traddr:0000:82:00.0" Nvme1

[Subsystem1]
NQN nqn.2016-06.io.spdk:cnode1
Listen RDMA 192.168.100.8:4420
AllowAnyHost No
Host nqn.2016-06.io.spdk:init
SN SPDK00000000000001
Namespace Nvme0n1 1

[Subsystem2]
NQN nqn.2016-06.io.spdk:cnode2
Listen RDMA 192.168.100.9:4420
AllowAnyHost Yes
SN SPDK00000000000002
Namespace Nvme1n1 1
~~~

Any bdev may be presented as a namespace.
See @ref bdev for details on setting up bdevs.
For example, to create a virtual controller with two namespaces backed by the malloc bdevs
named Malloc0 and Malloc1 and made available as NSID 1 and 2:
~~~{.sh}
[Subsystem3]
  NQN nqn.2016-06.io.spdk:cnode3
  Listen RDMA 192.168.2.21:4420
  AllowAnyHost Yes
  SN SPDK00000000000003
  Namespace Malloc0 1
  Namespace Malloc1 2
~~~

#### NQN Formal Definition

NVMe qualified names or NQNs are defined in section 7.9 of the
[NVMe specification](http://nvmexpress.org/wp-content/uploads/NVM_Express_Revision_1.3.pdf). SPDK has attempted to
formalize that definition using [Extended Backus-Naur form](https://en.wikipedia.org/wiki/Extended_Backus%E2%80%93Naur_form).
SPDK modules use this formal definition (provided below) when validating NQNs.

~~~{.sh}

Basic Types
year = 4 * digit ;
month = '01' | '02' | '03' | '04' | '05' | '06' | '07' | '08' | '09' | '10' | '11' | '12' ;
digit = '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9' ;
hex digit = 'A' | 'B' | 'C' | 'D' | 'E' | 'F' | 'a' | 'b' | 'c' | 'd' | 'e' | 'f' | '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9' ;

NQN Definition
NVMe Qualified Name = ( NVMe-oF Discovery NQN | NVMe UUID NQN | NVMe Domain NQN ), '\0' ;
NVMe-oF Discovery NQN = "nqn.2014-08.org.nvmexpress.discovery" ;
NVMe UUID NQN = "nqn.2014-08.org.nvmexpress:uuid:", string UUID ;
string UUID = 8 * hex digit, '-', 3 * (4 * hex digit, '-'), 12 * hex digit ;
NVMe Domain NQN = "nqn.", year, '-', month, '.', reverse domain, ':', utf-8 string ;

~~~

Please note that the following types from the definition above are defined elsewhere:
1. utf-8 string: Defined in [rfc 3629](https://tools.ietf.org/html/rfc3629).
2. reverse domain: Equivalent to domain name as defined in [rfc 1034](https://tools.ietf.org/html/rfc1034).

While not stated in the formal definition, SPDK enforces the requirement from the spec that the
"maximum name is 223 bytes in length". SPDK does not include the null terminating character when
defining the length of an nqn, and will accept an nqn containing up to 223 valid bytes with an
additional null terminator. To be precise, SPDK follows the same conventions as the c standard
library function [strlen()](http://man7.org/linux/man-pages/man3/strlen.3.html).

#### NQN Comparisons

SPDK compares NQNs byte for byte without case matching or unicode normalization. This has specific implications for
uuid based NQNs. The following pair of NQNs, for example, would not match when compared in the SPDK NVMe-oF Target:

nqn.2014-08.org.nvmexpress:uuid:11111111-aaaa-bbdd-ffee-123456789abc
nqn.2014-08.org.nvmexpress:uuid:11111111-AAAA-BBDD-FFEE-123456789ABC

In order to ensure the consistency of uuid based NQNs while using SPDK, users should use lowercase when representing
alphabetic hex digits in their NQNs.

### Assigning CPU Cores to the NVMe over Fabrics Target {#nvmf_config_lcore}

SPDK uses the [DPDK Environment Abstraction Layer](http://dpdk.org/doc/guides/prog_guide/env_abstraction_layer.html)
to gain access to hardware resources such as huge memory pages and CPU core(s). DPDK EAL provides
functions to assign threads to specific cores.
To ensure the SPDK NVMe-oF target has the best performance, configure the NICs and NVMe devices to
be located on the same NUMA node.

The `-m` core mask option specifies a bit mask of the CPU cores that
SPDK is allowed to execute work items on.
For example, to allow SPDK to use cores 24, 25, 26 and 27:
~~~{.sh}
app/nvmf_tgt/nvmf_tgt -m 0xF000000
~~~

## Configuring the Linux NVMe over Fabrics Host {#nvmf_host}

Both the Linux kernel and SPDK implement an NVMe over Fabrics host.
The Linux kernel NVMe-oF RDMA host support is provided by the `nvme-rdma` driver.

~~~{.sh}
modprobe nvme-rdma
~~~

The nvme-cli tool may be used to interface with the Linux kernel NVMe over Fabrics host.

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
nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
~~~

## Enabling NVMe-oF target tracepoints for offline analysis and debug {#nvmf_trace}

SPDK has a tracing framework for capturing low-level event information at runtime.  The NVMe-oF
target is instrumented with tracepoints to enable analysis of both performance and application
crashes.  (Note: the SPDK tracing framework should still be considered experimental.  Work to
formalize and document the framework is in progress.)

To enable the instrumentation, start the target with the -e parameter:

~~~{.sh}
app/nvmf_tgt/nvmf_tgt -e 0xFFFF
~~~

Information about the shared memory file will appear in the log:

~~~{.sh}
app.c: 527:spdk_app_setup_trace: *NOTICE*: Tracepoint Group Mask 0xFFFF specified.
app.c: 531:spdk_app_setup_trace: *NOTICE*: Use 'spdk_trace -s nvmf -p 24147' to capture a snapshot of events at runtime.
app.c: 533:spdk_app_setup_trace: *NOTICE*: Or copy /dev/shm/nvmf_trace.pid24147 for offline analysis/debug.
~~~

Note that when tracepoints are enabled, the shared memory files are not deleted when the application
exits.  This ensures the file can be used for analysis after the applicatione exits.  On Linux, the
shared memory files are in /dev/shm, and can be deleted manually to free shm space if needed.  A system
reboot will also free all of the /dev/shm files.

The spdk_trace program can be found in the app/trace directory.  To analyze the tracepoints on the same
system running the NVMe-oF target, simply execute the command line shown in the log:

~~~{.sh}
app/trace/spdk_trace -s nvmf -p 24147
~~~

To analyze the tracepoints on a different system, first prepare the tracepoint file for transfer.  The
tracepoint file can be large, but usually compresses very well.  This step can also be used to prepare
a tracepoint file to attach to a GitHub issue for debugging NVMe-oF application crashes.

~~~{.sh}
bzip2 -c /dev/shm/nvmf_trace.pid24147 > /tmp/trace.bz2
~~~

After transferring the /tmp/trace.bz2 tracepoint file to a different system:

~~~{.sh}
bunzip2 /tmp/trace.bz2
app/trace/spdk_trace -f /tmp/trace
~~~
