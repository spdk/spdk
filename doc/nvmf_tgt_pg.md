# SPDK NVMe-oF Target Programming Guide {#nvmf_tgt_pg}

* @ref nvmf_tgt_pg_audience
* @ref nvmf_tgt_pg_intro
* @ref nvmf_tgt_pg_theory
* @ref nvmf_tgt_pg_design
* @ref nvmf_tgt_pg_examples
* @ref nvmf_tgt_pg_config
* @ref nvmf_tgt_pg_component

## Target Audience {#nvmf_tgt_pg_audience}

The programming guide is intended for developers authoring applications that utilize and integrate
the SPDK NVMe-oF target.
It is intended to supplement the source code in providing an overall understanding of how to
integrate SPDK NVMe-oF target into an application as well as providing some high level insights
into how SPDK NVMe-oF target works behind the scenes. It is not intended to serve as a design
document or an API reference and in some cases source code snippets and high level sequences
will be discussed.

For the latest source code reference, refer to the [repo](https://github.com/spdk).

## Introduction {#nvmf_tgt_pg_intro}

The SPDK NVMe-oF target includes two parts: 1) the NVMe-oF target application; 2) the NVMe-oF
target libraries. The target application also invokes to interact with the libraries. This served
two purposes: 1) developers can directly start this standalone application with or without
updating the libraries; 2) developers to invoke the libaries in their own NVMe-oF applications
with or without integrating the SPDK NVMe-oF target application.

More SPDK NVMe-oF target introduction can refer to @ref nvmf_getting_started for details.

## Theory of Operation {#nvmf_tgt_pg_theory}

The purpose of NVMe-oF target is to provide users the related APIs and implementions for
constructing a high performance NVMe-oF target reference solution based on other components
in spdk, e.g., application framework, storage service layer (i.e., bdev), low level device
drivers (e.g., user space NVMe driver). With this NVMe-oF target programing guide, users can
understand the whole NVMe-oF target handling logic, and can add their own functions or refactor
the lib according to their purpose.

## Design Considerations {#nvmf_tgt_pg_design}

Following aspects are considered to design and implement a high performance and efficient NVMe-oF
target with basic functionalities.

### Per CPU core Performanance

The purpose is to improve the per CPU core performance in the following aspects (e.g., IOPS, latency)
comparing with kernel NVMe-oF target. To achieve that, following methods are used:

1. NUMA friendly, place the NICs and NVMe SSDs on the same node and configure the CPU cores on
that node.

2. Pin NVMe-oF connections to dedicated CPU core. Each connection should be aways handled by one
CPU core, but not be handled by different cores. The only case is connection migration for load
balance purpose.

3. Same polling mechnism for the completion queue as the SPDK user mode NVMe polling mode driver.
Perfect fit with the run-to-completion IO handling on one core.

### Lockless I/O Handling Optimization

The purpose is to reduce the usage of global or shared locks especially on the I/O path. To achieve
this, following method is used:

1. Asynchronous I/O programing: Leverage application framework (i.e., reactor, poller, event, I/O
channel) to implement asynchronous I/O programing methods.

### Zero Copy Support

1. We leverage the zero copy methods which means that pinned buffers for RDMA are allocated in SPDK
bdev layer, and this buffer will be moved in and out of the NVMe SSD via DMA.

### Key Data Structures

`struct nvmf_tgt`: global structure to manage the NVMe-oF target different running phases including
creating poll_groups, starting subsystems, starting acceptor and so on. And the global spdk_nvmf_tgt
to manage the master_thread, array of subsystems, discovery log pages and so on.

`struct spdk_nvmf_tgt`: global structure to manage the master thread for the in-order operation
of fabric and admin commands. And the array of subsystems including the discovery subsystem are
managed through the subsystem create, poll_group associate, destroy and so on. The other is the
list of transport for the different operations including accepting and listening new connections,
creating, removing, polling and destorying the poll_group and so on.

### Key Work Flows

State machine `enum nvmf_tgt_state`: the overall SPDK NVMe-oF application is managed through this
state machine from the configuration parsing, poll_groups creating, subsystems_starting, acceptor
starting, target running, acceptor_stopping, poll_groups destroying, subsystems_stopping, resources
freeing and etc. For users want to build the NVMe-oF applications, may refer to or update this work
flow.

Function groups `struct spdk_nvmf_transport_ops`: the common NVMe-oF related operations are defined
through the functionalities including listening new connections at the assigned address, accepting
new connections, filling out the discovery log for the specific listen address, creating the poll_group,
adding qpair to the poll_group, polling the poll_group to process I/O and etc. For users want to build
the NVMe-oF application with this library, may define the same function pointers.

## Examples {#nvmf_tgt_pg_examples}

**nvmf_tgt:** Actually named nvmf_tgt.c, which is a NVMe-oF target application based on SPDK
librararies including NVMe-oF, bdev, NVMe driver lib, app framework and etc. It is an example
application demonstrates how to glue SPDK NVMe-oF target lib and others to construct a high
performance NVMe-oF target implemention.

For more details on how to configure and evaluate the NVMe-oF target provided by SPDK, you can refer
the NVMe-oF target user guide @ref nvmf_getting_started.

## Configuration {#nvmf_tgt_pg_config}

For configuration details on SPDK NVMe-oF target, you can refer the information in NVMe-oF target user
guide @ref nvmf_getting_started.

## Component Detail {#nvmf_tgt_pg_component}

Following shows the detailed API of files in lib/nvmf folder:
