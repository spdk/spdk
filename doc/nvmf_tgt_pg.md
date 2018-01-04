# SPDK NVMe-oF Target Programming Guide {#nvmf_tgt_pg}

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

`master_thread`: global structure to handle the fabric and admin commands from other running threads
when more than one cores have been configured for the NVMe-oF target application. These commands are
processed in the asynchronous mode through the inter-thread messages. Need to pay attention on this
model with other on going operations like IOs. Especially this master thread also handles the name
space hot remove event.

`discovery_subsystem`: global structure for the discovery subsystem `nqn.2014-08.org.nvmexpress.discovery`.
This subsystem is created at the starting of the NVMe-oF target application and is to service the
below functions:
1. log discovery operation which is used to list all available subsystem information including subsystem
nqn, transport address, port ID and so on.
2. identify controller which is used to get detailed information of the remote controller.

### Key Work Flows

State machine `enum nvmf_tgt_state`: the overall SPDK NVMe-oF application is managed through this
state machine from the configuration parsing, poll_groups creating, subsystems_starting, acceptor
starting, target running, acceptor_stopping, poll_groups destroying, subsystems_stopping, resources
freeing and etc. For users want to build the NVMe-oF applications, may refer to or update this work
flow.

~~~{.sh}
enum nvmf_tgt_state {
	NVMF_TGT_INIT_NONE = 0,
	NVMF_TGT_INIT_PARSE_CONFIG,
	NVMF_TGT_INIT_CREATE_POLL_GROUPS,
	NVMF_TGT_INIT_START_SUBSYSTEMS,
	NVMF_TGT_INIT_START_ACCEPTOR,
	NVMF_TGT_RUNNING,
	NVMF_TGT_FINI_STOP_ACCEPTOR,
	NVMF_TGT_FINI_DESTROY_POLL_GROUPS,
	NVMF_TGT_FINI_STOP_SUBSYSTEMS,
	NVMF_TGT_FINI_FREE_RESOURCES,
	NVMF_TGT_STOPPED,
	NVMF_TGT_ERROR,
};
~~~

State machine `enum spdk_nvmf_subsystem_state`: the NVMe-oF target subsystem is managed through this
state machine from the creating, starting, adding name space, changing on the poll_group, stopping
and destroying. For users want to build the NVMe-oF applications, may refer to or update this work
flow.

~~~{.sh}
enum spdk_nvmf_subsystem_state {
	SPDK_NVMF_SUBSYSTEM_INACTIVE = 0,
	SPDK_NVMF_SUBSYSTEM_ACTIVATING,
	SPDK_NVMF_SUBSYSTEM_ACTIVE,
	SPDK_NVMF_SUBSYSTEM_PAUSING,
	SPDK_NVMF_SUBSYSTEM_PAUSED,
	SPDK_NVMF_SUBSYSTEM_RESUMING,
	SPDK_NVMF_SUBSYSTEM_DEACTIVATING,
};
~~~

State machine `enum spdk_nvmf_rdma_request_state`: the RDMA request is managed through this state
machine from the receiving of the request, queuing to need buffer without data buffer, waiting on
the RDMA queue depth to transfer data to the controller, transfering the data from host to the
controller, ready to execute at the block device, executing at the block device, readying to send
the completion, completing the request and so on. For users want to build the RDMA request handling,
may efer to or update this state machine.

~~~{.sh}
enum spdk_nvmf_rdma_request_state {
	RDMA_REQUEST_STATE_FREE = 0,
	RDMA_REQUEST_STATE_NEW,
	RDMA_REQUEST_STATE_NEED_BUFFER,
	RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER,
	RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
	RDMA_REQUEST_STATE_READY_TO_EXECUTE,
	RDMA_REQUEST_STATE_EXECUTING,
	RDMA_REQUEST_STATE_EXECUTED,
	RDMA_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST,
	RDMA_REQUEST_STATE_READY_TO_COMPLETE,
	RDMA_REQUEST_STATE_COMPLETING,
	RDMA_REQUEST_STATE_COMPLETED,
};
~~~

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

### Key Data Structures

`g_tgt`: the global and single instance for the nvmf_tgt. It manages the NVMe-oF target sate and core
usage for the qpair assignment.

`g_poll_groups`: array of poll_groups whose count is equal to the assigned number of cores. One each
configured core, a single poll_group will be created and running. It also manages the poller, an
array of subsystem_poll_groups and a list of transport_poll_groups.

`g_acceptor_poller`: the global and single acceptor poller running on the first assigned core. It
starts to accept the incoming requests for each tranport.

For more details on how to configure and evaluate the NVMe-oF target provided by SPDK, you can refer
the NVMe-oF target user guide @ref nvmf_getting_started.

### Managability via RPC

There are several RPC methods defined to manage this `nvmf_tgt` application.

`get_nvmf_subsystems`: list all available nvmf subsystems.
`construct_nvmf_subsystem`: construct a new nvmf subsystem at the runtime.
`delete_nvmf_subsystem`: destory an existing subsystem ar the runtime.

Users can refer to these existing RPC methods to update or extend the manageable capability for
the NVMe-oF applications.

### Shutdown Handling

In our `nvmf_tgt` example, a shutdown callback is added to respond to user's action on ther termination
of the NVMe-oF application. When user sends a terminated signal like SIGTERM, this callback will
be executed as some point from the SPDK application framework to stop the acceptor, stop the subsystems,
destory the poll_groups and free the resoruces. For users want to build the own NVMe-oF application,
may refer to this shutdown handling.

## Configuration {#nvmf_tgt_pg_config}

For configuration details on SPDK NVMe-oF target, you can refer the information in NVMe-oF target user
guide @ref nvmf_getting_started.

## Component Detail {#nvmf_tgt_pg_component}

Following shows some detailed functionalities regarding subsystems and transports.

### Subsystem Operations

The subsystem in NVMe-oF library is an important logical concept to group the resource together,
including a unique ID and a state machine of subsystem state for the proper stage of the subsystem,
the unique subsystem nqn, a list of nvmf controllers from the backend block devices, a list of
allowed hosts, a list of configured listener addresses.

Following shows the most important operation to construct a subsystem.

#### Constructing the Subsystem

The subsystem constructing can be completed through two channels:
1. With the pre-defined configuration file and started the NVMe-oF application
2. Through the RPC method to create a new subsystem with required parameters at the run time.

The major things to do:
1. Check the parameters for the validity including the non-existing subnqn.
2. Allocate the memory to hold the `struct spdk_nvmf_subsystem` data structure and properly initialize.
3. Create the a new transport for the NVMe-oF operations and add this transport to existing global
poll_groups.
4. Listen through the transport on the address.
5. Add the listener to the just created subsystem.
6. Add the allowed hosts to the just created subsystem.
7. Add the configured backend block devices (name spaces) to the just created subsystem.
8. If any happens, destroy the just created subsystem.

### Transport Operations

A global data structure `const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma` has been
defined in the NVMe-oF library (`lib/nvmf/rdma.c`) for the related transport operations as below:

~~~{.sh}
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.create = spdk_nvmf_rdma_create,
	.destroy = spdk_nvmf_rdma_destroy,

	.listen = spdk_nvmf_rdma_listen,
	.stop_listen = spdk_nvmf_rdma_stop_listen,
	.accept = spdk_nvmf_rdma_accept,

	.listener_discover = spdk_nvmf_rdma_discover,

	.poll_group_create = spdk_nvmf_rdma_poll_group_create,
	.poll_group_destroy = spdk_nvmf_rdma_poll_group_destroy,
	.poll_group_add = spdk_nvmf_rdma_poll_group_add,
	.poll_group_remove = spdk_nvmf_rdma_poll_group_remove,
	.poll_group_poll = spdk_nvmf_rdma_poll_group_poll,

	.req_complete = spdk_nvmf_rdma_request_complete,

	.qpair_fini = spdk_nvmf_rdma_close_qpair,
	.qpair_is_idle = spdk_nvmf_rdma_qpair_is_idle,
};
~~~

Detailed explanation as following:

#### RDMA Create (spdk_nvmf_rdma_create())

To create the RDMA transport, majorly including:
1. Allocate the memory for the RDMA transport data structure.
2. Initilize the RDMA parameters like `max_queue_depth`, `max_io_size`, `in_capsulte_data_size`
and so on.
3. Create the event channel through `rdma_create_event_channel()`.
4. Allocate the data buffer pool for the rdma operations.
5. Get and query the RDMA devices through `rdma_get_devices()` and `ibv_query_device()`.

#### RDMA Destroy (spdk_nvmf_rdma_destroy())

To release the RDMA transport, majory including:
1. Destroy the ports via `rdma_destroy_id()`.
2. Destroy the event channel via `rdma_destroy_event_channel()`.
3. Free the associated RDMA devices.
4. Free the data buffer pool.
5. Free the memory for the RDMA transport data structure.

#### RDMA Listen (spdk_nvmf_rdma_listen())

To listen on the specific address, majorly including:
1. Allocate the memory for the RDMA port.
2. Check whether the address already listened.
3. Create an identifier used to track communication information via `rdma_create_id()`.
4. Associate the address with an rdma_cm_id via `rdma_bind_addr()`.
5. Initiate a listen for incoming connection requests via `rdma_listen()`.
6. Use the reference count to track the usage of the same listen and initialize to one.

#### RDMA Stop Listen (spdk_nvmf_rdma_stop_listen())

To stop the RDMA listen, majorly including:
1. Decrease the referecne count on this RDMA port.
2. If reference cout is zero, destroy the specified rdma_cm_id via `rdma_destroy_id()`.

#### RDMA Accept (spdk_nvmf_rdma_accept())

To accept a communication event, majorly including:
1. Retrieve a communication event via `rdma_get_cm_event()` on the event channel.
2. Dispatch to the operation handlers based on the event like connect request.
3. Free the communication event via `rdma_ack_cm_event()`.

#### Discover Listener (spdk_nvmf_rdma_discover())

Fill out a discovery log entry for a specific listen address.

#### Create Poll Group (spdk_nvmf_rdma_poll_group_create())

To create the RDMA poll group, majorly including:
1. Allocate the memory for the RDMA poll group and initilize the associated pollers.
2. Allocate the memory for the poller and assign the RDMA device and poll group.
3. Initialize the qpairs for the poller.
4. Create the single completion queue for the poller via `ibv_create_cq()`.

#### Destroy Poll Group (spdk_nvmf_rdma_poll_group_destroy())

To destroy the RDMA poll group, majorly including:
1. Remove the poller from the RDMA poll group.
2. Destroy the single completion queue for the poller via `ibv_destroy_cq()`.
3. Free the memory for the poller and RDMA poll group.

#### Add QPair to Poll Group (spdk_nvmf_rdma_poll_group_add())

To add a qpair to the poll group, majorly including:
1. Check the validity of protention domains.
2. Check whether the poller already created for the RDMA device.
3. Initialize the RDMA qpair via `rdma_create_qp()` and:
a). Related memory resources for the requests, receivers, commands, completions, and buffers.
b). Register the memory regions for the protetion domains via `ibv_reg_mr()`.
c). Setup the memory to receive the commands and post capsule for the RDMA receive via `ibv_post_recv`.
d). Setup the memory to send the responses and data buffers.
4. Start the RDMA event accept via `rdma_accept()`.

#### Remove QPair from Poll Group (spdk_nvmf_rdma_poll_group_remove())

To remove a qpair from the poll group, majorly including:
1. Check whether the poller already associted for the RDMA device.
2. Remove the qpair from the poller.

#### Poll the Poll Group (spdk_nvmf_rdma_poll_group_poll())

To poll the poll group for the completing operations, majorly including:
1. Call the `ibv_poll_cq()` to get the completed operations.
2. Dispatch to the different handlers for the work completions like `IBV_WC_SEND`,
`IBV_WC_RDMA_WRITE`, `IBV_WC_RDMA_READ` and `IBV_WC_RECV`.

#### Signal Request Completion (spdk_nvmf_rdma_request_complete())

The request has been processed and send the response to the originator.

#### Destry RDMA QPair (spdk_nvmf_rdma_close_qpair())

To destroy the RDMA qpair, majorly including:
1. Remove the qpair from the associated poller.
2. Deregister the memory resources via `ibv_dereg_mr()`.
3. Destroy the protection domain via `rdma_destroy_qp()` and the communication identifier via
`rdma_destroy_id()`.
4. Free all the allocated memory resources.

#### Check QPair Idle (spdk_nvmf_rdma_qpair_is_idle())

Check whether there is pending IO on the qpair.
