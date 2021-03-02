# NVMe over Fabrics Target Programming Guide {#nvmf_tgt_pg}

## Target Audience

This programming guide is intended for developers authoring applications that
use the SPDK NVMe-oF target library (`lib/nvmf`). It is intended to provide
background context, architectural insight, and design recommendations. This
guide will not cover how to use the SPDK NVMe-oF target application. For a
guide on how to use the existing application as-is, see @ref nvmf.

## Introduction

The SPDK NVMe-oF target library is located in `lib/nvmf`. The library
implements all logic required to create an NVMe-oF target application. It is
used in the implementation of the example NVMe-oF target application in
`app/nvmf_tgt`, but is intended to be consumed independently.

This guide is written assuming that the reader is familiar with both NVMe and
NVMe over Fabrics. The best way to become familiar with those is to read their
[specifications](http://nvmexpress.org/resources/specifications/).

## Primitives

The library exposes a number of primitives - basic objects that the user
creates and interacts with. They are:

`struct spdk_nvmf_tgt`: An NVMe-oF target. This concept, surprisingly, does
not appear in the NVMe-oF specification. SPDK defines this to mean the
collection of subsystems with the associated namespaces, plus the set of
transports and their associated network connections. This will be referred to
throughout this guide as a **target**.

`struct spdk_nvmf_subsystem`: An NVMe-oF subsystem, as defined by the NVMe-oF
specification. Subsystems contain namespaces and controllers and perform
access control. This will be referred to throughout this guide as a
**subsystem**.

`struct spdk_nvmf_ns`: An NVMe-oF namespace, as defined by the NVMe-oF
specification. Namespaces are **bdevs**. See @ref bdev for an explanation of
the SPDK bdev layer. This will be referred to throughout this guide as a
**namespace**.

`struct spdk_nvmf_qpair`: An NVMe-oF queue pair, as defined by the NVMe-oF
specification. These map 1:1 to network connections. This will be referred to
throughout this guide as a **qpair**.

`struct spdk_nvmf_transport`: An abstraction for a network fabric, as defined
by the NVMe-oF specification. The specification is designed to allow for many
different network fabrics, so the code mirrors that and implements a plugin
system. Currently, only the RDMA transport is available. This will be referred
to throughout this guide as a **transport**.

`struct spdk_nvmf_poll_group`: An abstraction for a collection of network
connections that can be polled as a unit. This is an SPDK-defined concept that
does not appear in the NVMe-oF specification. Often, network transports have
facilities to check for incoming data on groups of connections more
efficiently than checking each one individually (e.g. epoll), so poll groups
provide a generic abstraction for that. This will be referred to throughout
this guide as a **poll group**.

`struct spdk_nvmf_listener`: A network address at which the target will accept
new connections.

`struct spdk_nvmf_host`: An NVMe-oF NQN representing a host (initiator)
system. This is used for access control.

## The Basics

A user of the NVMe-oF target library begins by creating a target using
spdk_nvmf_tgt_create(), setting up a set of addresses on which to accept
connections by calling spdk_nvmf_tgt_listen_ext(), then creating a subsystem
using spdk_nvmf_subsystem_create().

Subsystems begin in an inactive state and must be activated by calling
spdk_nvmf_subsystem_start(). Subsystems may be modified at run time, but only
when in the paused or inactive state. A running subsystem may be paused by
calling spdk_nvmf_subsystem_pause() and resumed by calling
spdk_nvmf_subsystem_resume().

Namespaces may be added to the subsystem by calling
spdk_nvmf_subsystem_add_ns_ext() when the subsystem is inactive or paused.
Namespaces are bdevs. See @ref bdev for more information about the SPDK bdev
layer. A bdev may be obtained by calling spdk_bdev_get_by_name().

Once a subsystem exists and the target is listening on an address, new
connections will be automatically assigned to poll groups as they are
detected.

All I/O to a subsystem is driven by a poll group, which polls for incoming
network I/O. Poll groups may be created by calling
spdk_nvmf_poll_group_create(). They automatically request to begin polling
upon creation on the thread from which they were created. Most importantly, *a
poll group may only be accessed from the thread on which it was created.*

## Access Control

Access control is performed at the subsystem level by adding allowed listen
addresses and hosts to a subsystem (see spdk_nvmf_subsystem_add_listener() and
spdk_nvmf_subsystem_add_host()). By default, a subsystem will not accept
connections from any host or over any established listen address. Listeners
and hosts may only be added to inactive or paused subsystems.

## Discovery Subsystems

A discovery subsystem, as defined by the NVMe-oF specification, is
automatically created for each NVMe-oF target constructed. Connections to the
discovery subsystem are handled in the same way as any other subsystem.

## Transports

The NVMe-oF specification defines multiple network transports (the "Fabrics"
in NVMe over Fabrics) and has an extensible system for adding new fabrics
in the future. The SPDK NVMe-oF target library implements a plugin system for
network transports to mirror the specification. The API a new transport must
implement is located in lib/nvmf/transport.h. As of this writing, only an RDMA
transport has been implemented.

The SPDK NVMe-oF target is designed to be able to process I/O from multiple
fabrics simultaneously.

## Choosing a Threading Model

The SPDK NVMe-oF target library does not strictly dictate threading model, but
poll groups do all of their polling and I/O processing on the thread they are
created on. Given that, it almost always makes sense to create one poll group
per thread used in the application.

## Scaling Across CPU Cores

Incoming I/O requests are picked up by the poll group polling their assigned
qpair. For regular NVMe commands such as READ and WRITE, the I/O request is
processed on the initial thread from start to the point where it is submitted
to the backing storage device, without interruption. Completions are
discovered by polling the backing storage device and also processed to
completion on the polling thread. **Regular NVMe commands (READ, WRITE, etc.)
do not require any cross-thread coordination, and therefore take no locks.**

NVMe ADMIN commands, which are used for managing the NVMe device itself, may
modify global state in the subsystem. For instance, an NVMe ADMIN command may
perform namespace management, such as shrinking a namespace. For these
commands, the subsystem will temporarily enter a paused state by sending a
message to each thread in the system. All new incoming I/O on any thread
targeting the subsystem will be queued during this time. Once the subsystem is
fully paused, the state change will occur, and messages will be sent to each
thread to release queued I/O and resume. Management commands are rare, so this
style of coordination is preferable to forcing all commands to take locks in
the I/O path.

## Zero Copy Support

For the RDMA transport, data is transferred from the RDMA NIC to host memory
and then host memory to the SSD (or vice versa), without any intermediate
copies. Data is never moved from one location in host memory to another. Other
transports in the future may require data copies.

## RDMA

The SPDK NVMe-oF RDMA transport is implemented on top of the libibverbs and
rdmacm libraries, which are packaged and available on most Linux
distributions. It does not use a user-space RDMA driver stack through DPDK.

In order to scale to large numbers of connections, the SPDK NVMe-oF RDMA
transport allocates a single RDMA completion queue per poll group. All new
qpairs assigned to the poll group are given their own RDMA send and receive
queues, but share this common completion queue. This allows the poll group to
poll a single queue for incoming messages instead of iterating through each
one.

Each RDMA request is handled by a state machine that walks the request through
a number of states. This keeps the code organized and makes all of the corner
cases much more obvious.

RDMA SEND, READ, and WRITE operations are ordered with respect to one another,
but RDMA RECVs are not necessarily ordered with SEND acknowledgements. For
instance, it is possible to detect an incoming RDMA RECV message containing a
new NVMe-oF capsule prior to detecting the acknowledgement of a previous SEND
containing an NVMe completion. This is problematic at full queue depth because
there may not yet be a free request structure. To handle this, the RDMA
request structure is broken into two parts - an rdma_recv and an rdma_request.
New RDMA RECVs will always grab a free rdma_recv, but may need to wait in a
queue for a SEND acknowledgement before they can acquire a full rdma_request
object.

Further, RDMA NICs expose different queue depths for READ/WRITE operations
than they do for SEND/RECV operations. The RDMA transport reports available
queue depth based on SEND/RECV operation limits and will queue in software as
necessary to accommodate (usually lower) limits on READ/WRITE operations.
