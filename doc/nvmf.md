# NVMe over Fabrics Target {#nvmf}

@sa @ref nvme_fabrics_host
@sa @ref nvmf_tgt_tracepoints

## NVMe-oF Target Getting Started Guide {#nvmf_getting_started}

The SPDK NVMe over Fabrics target is a user space application that presents block devices over a fabrics
such as Ethernet, Infiniband or Fibre Channel. SPDK currently supports RDMA and TCP transports.

The NVMe over Fabrics specification defines subsystems that can be exported over different transports.
SPDK has chosen to call the software that exports these subsystems a "target", which is the term used
for iSCSI. The specification refers to the "client" that connects to the target as a "host". Many
people will also refer to the host as an "initiator", which is the equivalent thing in iSCSI
parlance. SPDK will try to stick to the terms "target" and "host" to match the specification.

The Linux kernel also implements an NVMe-oF target and host, and SPDK is tested for
interoperability with the Linux kernel implementations.

If you want to kill the application using signal, make sure use the SIGTERM, then the application
will release all the share memory resource before exit, the SIGKILL will make the share memory
resource have no chance to be released by application, you may need to release the resource manually.

## RDMA transport support {#nvmf_rdma_transport}

It requires an RDMA-capable NIC with its corresponding OFED (OpenFabrics Enterprise Distribution)
software package installed to run. Maybe OS distributions provide packages, but OFED is also
available [here](https://downloads.openfabrics.org/OFED/).

### Prerequisites {#nvmf_prereqs}

To build nvmf_tgt with the RDMA transport, there are some additional dependencies,
which can be install using pkgdep.sh script.

~~~{.sh}
sudo scripts/pkgdep.sh --rdma
~~~

Then build SPDK with RDMA enabled:

~~~{.sh}
./configure --with-rdma <other config parameters>
make
~~~

Once built, the binary will be in `build/bin`.

### Prerequisites for InfiniBand/RDMA Verbs {#nvmf_prereqs_verbs}

Before starting our NVMe-oF target with the RDMA transport we must load the InfiniBand and RDMA modules
that allow userspace processes to use InfiniBand/RDMA verbs directly.

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

### Prerequisites for RDMA NICs {#nvmf_prereqs_rdma_nics}

Before starting our NVMe-oF target we must detect RDMA NICs and assign them IP addresses.

### Finding RDMA NICs and associated network interfaces

~~~{.sh}
ls /sys/class/infiniband/*/device/net
~~~

#### Mellanox ConnectX-3 RDMA NICs

~~~{.sh}
modprobe mlx4_core
modprobe mlx4_ib
modprobe mlx4_en
~~~

#### Mellanox ConnectX-4 RDMA NICs

~~~{.sh}
modprobe mlx5_core
modprobe mlx5_ib
~~~

#### Assigning IP addresses to RDMA NICs

~~~{.sh}
ifconfig eth1 192.168.100.8 netmask 255.255.255.0 up
ifconfig eth2 192.168.100.9 netmask 255.255.255.0 up
~~~

### RDMA Limitations {#nvmf_rdma_limitations}

As RDMA NICs put a limitation on the number of memory regions registered, the SPDK NVMe-oF
target application may eventually start failing to allocate more DMA-able memory. This is
an imperfection of the DPDK dynamic memory management and is most likely to occur with too
many 2MB hugepages reserved at runtime. One type of memory bottleneck is the number of NIC memory
regions, e.g., some NICs report as many as 2048 for the maximum number of memory regions. This
gives us a 4GB memory limit with 2MB hugepages for the total memory regions. It can be overcome by
using 1GB hugepages or by pre-reserving memory at application startup with `--mem-size` or `-s`
option. All pre-reserved memory will be registered as a single region, but won't be returned to the
system until the SPDK application is terminated.

Another known issue occurs when using the E810 NICs in RoCE mode. Specifically, the NVMe-oF target
sometimes cannot destroy a qpair, because its posted work requests don't get flushed.  It can cause
the NVMe-oF target application unable to terminate cleanly.

## TCP transport support {#nvmf_tcp_transport}

The transport is built into the nvmf_tgt by default, and it does not need any special libraries.

## FC transport support {#nvmf_fc_transport}

To build nvmf_tgt with the FC transport, there is an additional FC LLD (Low Level Driver) code dependency.
Please contact your FC vendor for instructions to obtain FC driver module.

### Broadcom FC LLD code

FC LLD driver for Broadcom FC NVMe capable adapters can be obtained from,
https://github.com/ecdufcdrvr/bcmufctdrvr.

### Fetch FC LLD module and then build SPDK with FC enabled

After cloning SPDK repo and initialize submodules, FC LLD library is built which then can be linked with
the fc transport.

~~~{.sh}
git clone https://github.com/spdk/spdk spdk
git clone https://github.com/ecdufcdrvr/bcmufctdrvr fc
cd spdk
git submodule update --init
cd ../fc
make DPDK_DIR=../spdk/dpdk/build SPDK_DIR=../spdk
cd ../spdk
./configure --with-fc=../fc/build
make
~~~

## Configuring the SPDK NVMe over Fabrics Target {#nvmf_config}

An NVMe over Fabrics target can be configured using JSON RPCs.
The basic RPCs needed to configure the NVMe-oF subsystem are detailed below. More information about
working with NVMe over Fabrics specific RPCs can be found on the @ref jsonrpc_components_nvmf_tgt RPC page.

### Using RPCs {#nvmf_config_rpc}

Start the nvmf_tgt application with elevated privileges. Once the target is started,
the nvmf_create_transport rpc can be used to initialize a given transport. Below is an
example where the target is started and configured with two different transports.
The RDMA transport is configured with an I/O unit size of 8192 bytes, 4 max qpairs per controller,
and an in capsule data size of 0 bytes. The TCP transport is configured with an I/O unit size of
16384 bytes, 8 max qpairs per controller, and an in capsule data size of 8192 bytes.

~~~{.sh}
build/bin/nvmf_tgt
scripts/rpc.py nvmf_create_transport -t RDMA -u 8192 -m 4 -c 0
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
~~~

Below is an example of creating a malloc bdev and assigning it to a subsystem. Adjust the bdevs,
NQN, serial number, and IP address with RDMA transport to your own circumstances. If you replace
"rdma" with "TCP", then the subsystem will add a listener with TCP transport.

~~~{.sh}
scripts/rpc.py bdev_malloc_create -b Malloc0 512 512
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a 192.168.100.8 -s 4420
~~~

### NQN Formal Definition

NVMe qualified names or NQNs are defined in section 7.9 of the
[NVMe specification](http://nvmexpress.org/wp-content/uploads/NVM_Express_Revision_1.3.pdf). SPDK has attempted to
formalize that definition using [Extended Backus-Naur form](https://en.wikipedia.org/wiki/Extended_Backus%E2%80%93Naur_form).
SPDK modules use this formal definition (provided below) when validating NQNs.

~~~{.sh}

Basic Types
year = 4 * digit ;
month = '01' | '02' | '03' | '04' | '05' | '06' | '07' | '08' | '09' | '10' | '11' | '12' ;
digit = '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9' ;
hex digit = 'A' | 'B' | 'C' | 'D' | 'E' | 'F' | 'a' | 'b' | 'c' | 'd' | 'e' | 'f' | '0' |
'1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9' ;

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
build/bin/nvmf_tgt -m 0xF000000
~~~

## Configuring the Linux NVMe over Fabrics Host {#nvmf_host}

Both the Linux kernel and SPDK implement an NVMe over Fabrics host.
The Linux kernel NVMe-oF RDMA host support is provided by the `nvme-rdma` driver
(to support RDMA transport) and `nvme-tcp` (to support TCP transport). And the
following shows two different commands for loading the driver.

~~~{.sh}
modprobe nvme-rdma
modprobe nvme-tcp
~~~

The nvme-cli tool may be used to interface with the Linux kernel NVMe over Fabrics host.
See below for examples of the discover, connect and disconnect commands. In all three instances, the
transport can be changed to TCP by interchanging 'rdma' for 'tcp'.

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

SPDK has a tracing framework for capturing low-level event information at runtime.
@ref nvmf_tgt_tracepoints enable analysis of both performance and application crashes.
