# SPDK iSCSI Programmer Guide {#iscsi_pg}

## Target Audience

This programmer guide is intended for developers to utilize the SPDK iSCSI library and application.
It is intended to supplement the source code in providing an overall understanding of how to
integrate iSCSI into an application as well as provide some high level insight into how iSCSI works
behind the scenes. It is not intended to serve as a design document or an API reference and in some
cases source code snippets and high level sequences will be discussed; for the latest source code
reference refer to the [repo](https://github.com/spdk).

## Introduction

The iSCSI subsystem in SPDK library implements the iSCSI server logic according to iSCSI protocol
(i.e., RFC 3720) based on the application framework inside SPDK. Generally the related code can be
divided into several parts:

1. iSCSI target side initialization, which includes target side parameter configuration, memory pool
initialization, target node and group configuration, connection configuration and etc.

2. iSCSI target side iSCSI command handling;

3. Asynchronous I/O handling mechanism among network, iSCSI main components and bdev layers.

### Layer of software component

For SPDK iSCSI target implmentation, the code is divided into several layers - application framework,
network, iSCSI, SCSI, bdev, and low level device drivers. And this guide only covers iSCSI library
introduction. For the iSCSI target, it leverages application framwork to start, and parses the related
configuration file to intialize, then construct different subsystems (e.g., iSCSI, SCSI, bdev).
For the I/O handling, it received the iSCSI PDUs from the initiator, and the requests are handled in
network,  iSCSI, SCSI, bdev layer and finally handled by device drivers. When the I/O come back,
it will be handled in the reverse order, i.e., bdev, SCSI, ISCSI, network layer. And we leverage run
to completion model to inform a lockess iSCSI handling for acceleration purpose.

## Theory of Operation

The purpose of iSCSI library is to provide users the related API and implementions for constructing
a high performance iSCSI target solution based on other components in spdk, e.g., application framework,
storage service layer (i.e., bdev), low level device drivers (e.g., user space NVMe driver). After
iSCSI programing users understand the whole iSCSI handling logic in this lib, they can add their own
functions or refactor the lib according to their purpose.

### Basic Primitives

1. *iSCSI session*: The group of iSCSI connections that link an initiator with a target form a session.
iSCSI connections can be added and removed from a session.  Across all connections within a session,
an initiator sees one and the same target. For details, refer to `struct spdk_iscsi_sess`.

2. *iSCSI connection*: A TCP connection between initaitor and target which carries control messages,
SCSI commands, parameter, and data within iSCSI protocol Data Units (iSCSI PDUs). For details,
refer to `struct spdk_iscsi_conn`.

3. *iSCSI target node*: the logical entity which is accessible via one or more network portals. An
iSCSI target node is identified by its iSCSI name. For details, refer to `struct spdk_iscsi_tgt_node`.

4. *Portal group*: A Portal group contains a set of Network Portals within an iSCSI Network Entity
that collectively supports the capability of coordinating a session with connections spanning these
portals. One or more Portal Groups may provide access to an iSCSI Node. Each Network Portal, as
utilized by a given iSCSI Node, belongs to exactly one portal group within that node. For details,
refer to `struct spdk_iscsi_portal_grp`.

5. *iSCSI task*:  An iSCSI task is an iSCSI request for which a response is expected. For details,
refer to `struct spdk_iscsi_task`.

6. *iSCSI device*: A SCSI Device using an iSCSI service delivery subsystem, which provides the
transport mechanism for SCSI commands and responses. For details, refer to `struct spdk_scsi_dev`.

Among those primitives, iSCSI target nodes contain iSCSI device related info, and also the initiator
and portal group mapping info. And iSCSI session contains information about connection, and a
connection contains information about several iSCSI tasks, the session belonged to, also the
backend iSCSI device.

## Design Considerations

The following are considerations when designing and implementing a high performance and efficient iSCSI
targets with basic functionalities.

### Per CPU core Performance

The SPDK iSCSI target uses several methods to improve per core CPU performance as compared to other
common iSCSI taget implementations (Linux IO target, iSCSI tgt):

1. Pin iSCSI connections to dedicated CPU core: Every single iSCSI connection is handled by one CPU
core. Which CPU core is used for the target is determined automatically based on the simple load
balancing algorithm. Also we should consider how to manage the idle connection. Every time,
the connection is handled by the poller with function `spdk_iscsi_conn_full_feature_do_work`,
it checks whether this connection is active or not within this function, e.g., during a time interval,
there is no active tasks. If connection is not active, we will stop the poller to handle this connection,
and leverage epoll related mechasim by registering the socket file descriptor(fd) of this connection.
Also we have a poller registered with function `spdk_iscsi_conn_idle_do_work`, it leverages the epoll
related mechasim to check whether the socket fd is active or not periodically. If the connection is active,
it will be deleted from the `idle list` maintained by epoll mechanism, and generate a connection
migration event in order to choose a CPU core according to the load balance algorithm, and then
generate anther event call to regsiter the function `spdk_iscsi_conn_full_feature_do_work` to
handle the connection again.


2. Threading model: `Run to completion model` is chosen, which leverages application framework
(i.e., reactor, poller, event, I/O channel) to implement run to completion model based on
asynchronous I/O programing methods. Also for network communication, non blocking TCP/IP is selected.
The reason that we chose this model is to avoid locking among different threads, and efficiently use
each CPU core.

3. Zero copy support: For iSCSI read commands, we leverage the zero copy methods which means that
buffers are allocated in SPDK bdev layer, and this buffer will be freed after the iSCSI datain
response pdus are sent to iSCSI initiator. During all the iSCSI read handling process, there is no
data copy from storage module to network module.

4. Leverage SPDK user space NVMe drivers if the backend storage device are PCIe NVMe SSDs or NVMe SSDs
exported by remote NVMe-oF target.

### Functionality Consideration

1 The library implements the basic iSCSI supports described in RFC 3720. Currently SPDK iSCSI target
implements ERL (Error recovery level) 0 and some support in ERL 1.

2 Pass the basic iSCSI/SCSI compliance test. We use UNH, calsoft, lib-iSCSI test suites to verify
the iSCSI/SCSI compliance tests.

3 Support both legacy Linux and windows iSCSI initaitor for basic tests. For example, SPDK iSCSI
target works with iscsiadm in Linux.

### Scaling Consideration

According to our design, each connection is handled by the single CPU core within one thread. It can
scale with CPU cores. Since the connections, sessions, target nodes, portal map related info occupies
memory space, it will be dependent on the memory size of the system. The network work connection
determines the latency and IOPS. The design can be scaled with CPU, but still depends the memory,
NIC restrictions. Mainly, memory and NIC will be the bottleneck.

## Examples

**iscsi_tgt:** iscsi_tgt.c is a iSCSI tgt application based on SPDK librararies including iSCSI, SCSI,
bdev, NVMe driver lib, app framework and etc. It is an example that demonstrates how to glue SPDK iSCSI
lib and others to construct a high performance iSCSI target implemention. For more details on how to
configure and evaluate the iSCSI target provided by SPDK, refer the iSCSI target user guide.

## Configuration

For configuration details on SPDK iSCSI target, you can refer the information in iSCSI target user
guide.

## Component Detail

Following shows the files in lib/iSCSI folder:

###File: acceptor.h/c

The two files are used to handle the iSCSI connection requests from network.

### File: conn.h/c

The two files are used to handle the iSCSI connection.

### File: init_grp.h/c

The two files are used to manage the iSCSI initator groups, more detailed info are listed:

### File: iscsi.h/c

The two files are used to handle the iSCSI protocols, serveral functions and structures are defined
to handle the pdu, tasks.

## File: iscsi_rpc.c

This file exports several rpc functions to manage the iSCSI targets, all the supported functions are
registered int the following mananers:

SPDK_RPC_REGISTER(method_name, func), e.g., SPDK_RPC_REGISTER("get_initiator_group",
spdk_rpc_get_initiator_group).

### File: iscsi_subsystem.c

This file is used to initialize iSCSI subsystem.

### File: md5.h/c

The two files are used to privide and wrap the md5 related functions which is
used for iSCSI.

### File: param.h/c

The two files define some structs and functions which are used for the parameter negoitation between

### File: portal_grp.h/c

The two files are used to manange the portal group of iSCSI target.

### File: task.h/c

The two files define the iSCSI task for iSCSI connections, which defines the struct of iSCSI tasks,
and related operations on the iSCSI task.

### File: tgt_node.h/c

The two files manage the iSCSI target node, which involes several operations on the target node with
iSCSI portal groups and initiator groups.
