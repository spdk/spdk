# Virtualized I/O with Vhost-user {#vhost_processing}

# Table of Contents {#vhost_processing_toc}

- @ref vhost_processing_intro
- @ref vhost_processing_qemu
- @ref vhost_processing_init
- @ref vhost_processing_io_path

# Introduction {#vhost_processing_intro}

This document is intended to provide an overview of how Vhost works behind the
scenes. Let's start by defining what Virtio is.

Reading from the
[Virtio specification](http://docs.oasis-open.org/virtio/virtio/v1.0/virtio-v1.0.html):

```
The purpose of virtio and [virtio] specification is that virtual environments
and guests should have a straightforward, efficient, standard and extensible
mechanism for virtual devices, rather than boutique per-environment or per-OS
mechanisms.
```

Virtio devices use virtqueues to transport data efficiently. Virtqueue is a pair
submission and completion rings designed to store generic scatter-gatter I/O.
Virtio is most commonly used in QEMU VMs, where the QEMU itself exposes a virtual
PCI device and the guest OS communicates with it using a specific Virtio PCI driver.
With only Virtio involved, it's always the QEMU process that handles all I/O traffic.

Vhost is a protocol for devices accessible via inter-process communication.
It uses the same virtqueue layout as Virtio to allow Vhost devices to be mapped
directly to Virtio devices. This allows a Vhost device, exposed for example by
an SPDK application, to be accessed directly by a guest OS inside a QEMU process
with an existing Virtio (PCI) driver. Only the configuration, I/O submission
notification, and I/O completion interruption are piped through QEMU.

The initial vhost implementation is a part of the Linux kernel and uses ioctl
interface to communicate with userspace applications. What makes it possible for
SPDK to expose a vhost device is Vhost-user protocol.

The [Vhost-user specification](https://git.qemu.org/?p=qemu.git;a=blob_plain;f=docs/interop/vhost-user.txt;hb=HEAD)
describes the protocol as follows:

```
[Vhost-user protocol] is aiming to complement the ioctl interface used to
control the vhost implementation in the Linux kernel. It implements the control
plane needed to establish virtqueue sharing with a user space process on the
same host. It uses communication over a Unix domain socket to share file
descriptors in the ancillary data of the message.

The protocol defines 2 sides of the communication, master and slave. Master is
the application that shares its virtqueues, in our case QEMU. Slave is the
consumer of the virtqueues.

In the current implementation QEMU is the Master, and the Slave is intended to
be a software Ethernet switch running in user space, such as Snabbswitch.

Master and slave can be either a client (i.e. connecting) or server (listening)
in the socket communication.
```

SPDK vhost is a Vhost-user slave server. It exposes Unix domain sockets and
allows external applications to connect.

## QEMU {#vhost_processing_qemu}

One of major Vhost-user use cases is networking (DPDK) or storage (SPDK)
offload in QEMU. The following diagram presents how QEMU-based VM
communicates with SPDK Vhost-SCSI device.

![QEMU/SPDK vhost data flow](img/qemu_vhost_data_flow.svg)

## Inter-process communication

[TODO]

# Vhost overview {#vhost_processing_overview}

This section briefly describes how vhost protocol works in general, without
going into any SPDK specifics.

## Device initialization {#vhost_processing_init}

All initialization and management information is exchanged using Vhost-user
messages. The connection always starts with the feature negotiation. Both
the Master and the Slave exposes a list of their implemented features and
upon negotiation they choose a common set of those. Most of these features are
implementation-related, but also regard e.g. multiqueue support or live migration.

After the negotiation, the Vhost-user driver shares its memory so that the vhost
device (SPDK) can access it directly. The memory can be fragmented into multiple
physically-discontiguous regions and Vhost-user specification puts a limit on
their number - currently 8. The driver sends a single message for each region with
the following data:
 * file descriptor - for mmap
 * virtual address - virtual address in the other process (QEMU). This is used
   for memory translations in Vhost-user messages (e.g. translating vring addresses)
 * guest address - for QEMU this is a physical address inside the guest. It's used
   for translating data buffer addresses.
 * size

The Master will send new memory regions after each memory change - usually
hotplug/hotremove. The previous mappings will be removed.

Drivers may also request a device config, consisting of e.g. disk geometry.
Currently only Vhost-Block drivers do it. SCSI drivers use the common SCSI
I/O to inquiry the underlying disks.

Afterwards, the driver requests the number of maximum supported queues and
sends a vhost-user message for each virtqueue. It consists of:
 * unique virtqueue id
 * vring addresses
 * call descriptor (for interrupting the driver after I/O completions)
 * kick descriptor (to listen for I/O requests - unused by SPDK)

Now, each queue consists of an array and two rings:
 * desc table
 * avail ring
 * used ring

We get address for each of those, but they need to translated from the other
process' address space to local, mmapped addresses inside the previously
received memory regions. We'll describe the particular rings in detail in the
next section.

# I/O path {#vhost_processing_io_path}

The Master sends I/O by allocating proper buffers in shared memory, setting up
buffers with request data, and then putting guest addresses of all those buffers
into virtqueues.

A Virtio-Block I/O looks as follows.

```
struct virtio_blk_req {
        uint32_t type; // READ, WRITE, FLUSH (read-only)
        uint64_t offset; // offset in the disk (read-only)
        struct iovec buffers[]; // scatter-gatter list (read/write)
        uint8_t status; // I/O completion status (write-only)
};
```
And a Virtio-SCSI I/O as follows.

```
struct virtio_scsi_req_cmd {
  struct virtio_scsi_cmd_req *req; // request data (read-only)
  struct iovec read_only_buffers[]; // scatter-gatter list for WRITE I/Os
  struct virtio_scsi_cmd_resp *resp; // response data (write-only)
  struct iovec write_only_buffers[]; // scatter-gatter list for READ I/Os
}
```

In order to be put into a virtqueue, such request needs to be converted
into a set of virtio descriptors - specification-defined structures building
the **desc** table. A single descriptor can be either readable or writable, so
each I/O needs to be split into multiple descriptors chained together.

```
struct vring_desc {
        /* Address (guest-physical). */
        le64 addr;
        /* Length. */
        le32 len;

/* This marks a buffer as continuing via the next field. */
#define VIRTQ_DESC_F_NEXT   1
/* This marks a buffer as device write-only (otherwise device read-only). */
#define VIRTQ_DESC_F_WRITE     2
        /* The flags as indicated above. */
        le16 flags;
        /* Next field if flags & NEXT */
        le16 next;
};
```

The descriptors can be put anywhere in the descriptor table and don't even have
to be next to each other. Each descriptors points to the next one that can be
anywhere in the array.

After a chain of descriptors is put into the descriptor table, the Master needs
to put the head of the descriptor chain into the **avail** ring, which acts as
a submission queue.

The driver also checks if the device needs to be notified about new I/O submission
and then rings the doorbell. Virtio devices in a QEMU VM write to a PCI doorbell,
which is registered by QEMU to a ioeventfd. Practically, writing to that memory
region triggers a soft I/O exit and writes to the **kick** file descriptor that
the vhost application may poll() on. SPDK doesn't do any of that though. It sets
a flag in the Virtio queue structure that prevents the driver from writing to that
descriptor. Then, SPDK just continuously polls the avail queue head index.

After polling a new avail entry, the device starts walking through the virtio
descriptors and (re)builds the original I/O request. All addresses in the virtio
descriptors need to be translated from guest address space to proces-local mmaped
memory regions.

In the end, the device forwards such I/O to the real disk and waits for the
completion. Once it arrives, it fills the response buffer with proper data,
puts the descriptor chain's head index into the **used** ring, and notifies the
guest by doing an eventfd_write on the **call** descriptor for proper virtqueue.
There are multiple interrupt coalescing features involved, but they are not
discussed in this document.

# SPDK implementation {#vhost_implementation_pg}

This section describes how SPDK vhost works from the code perspective. It
assumes the reader is already familiar with @ref vhost. (TODO fix title; should
include "User Guide")

## Usability {#vhost_implementation_usability}

SPDK Vhost is configurable with C APIs in `include/spdk/vhost.h` as well as
corresponding RPC commands built exactly on top of those APIs.

The user can create vhost **devices** that correspond to Unix domain socket files.
When external application connects to such socket, an SPDK vhost **session** is
created. Whenever the user changes a vhost device, all sessions are updated
automatically and there's no possibility to configure specific sessions. Practically,
sessions are transparent for the end user.

## Device types {#vhost_implementation_device_types}

SPDK vhost implements two device types - Vhost-SCSI and Vhost-Block. They're also
called device **backends**. Vhost-SCSI code is all contained in `vhost_scsi.c`
file [TODO link], and Vhost-Block in `vhost_blk.c` [TODO]. Those two files
have completely different code, but they share a common base via `vhost_internal.h` [TODO]
(and its implementation in `vhost.c` [TODO])

That common interface takes care of overall device initialization, removal,
memory registration, dequeuing generic virtio descriptors from virtqueues, as
well as sending interrupts. The implementation of specific backend is mostly
concerned around handling its specific I/O structures and providing additional
configuration (like hotplugging LUNs in Vhost-SCSI).

For a vhost device to be created, a backend must call vhost_dev_register(),
which accepts e.g. the device name (to be used for the socket filename) and
struct spdk_vhost_dev_backend:

```
  /** Valid features for this kind of device. */
  uint64_t virtio_features;

  /** Features which are not implemented by this backend yet. */
  uint64_t disabled_features;

  /** Allocate a new session object for the given device */
  struct spdk_vhost_session *(*new_session)(struct spdk_vhost_dev *vdev);

  /** Free the provided session object. */
  void (*free_session)(struct spdk_vhost_session *vsession);

  /** Start polling I/O on the provided session. */
  void (*start_session)(struct spdk_vhost_session *vsession);

  /** Stop polling I/O. */
  int (*stop_session)(struct spdk_vhost_session *vsession);

  /**
   * Respond to a Virtio-PCI config request by filling the entire
   * *config* buffer (up to *len* bytes). Optional, can be NULL.
   */
  int (*get_config)(struct spdk_vhost_dev *vdev, uint8_t *config, uint32_t len);

  /**
   * Handle a Virtio-PCI config write at specified *offset* and *len*
   * in *config*. Optional, can be NULL.
   */
  int (*set_config)(struct spdk_vhost_dev *vdev, uint8_t *config,
		    uint32_t offset, uint32_t size, uint32_t flags);

  /** Dump any backend-specific information into the provided JSON context. */
  void (*dump_info_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);

  /** Write an RPC request capable of recreating this vhost device. */
  void (*write_config_json)(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w);

  /** Synchronously remove this device. */
  int (*remove_device)(struct spdk_vhost_dev *vdev);
```

```
@startuml

box "SPDK" #BlanchedAlmond
 participant "vhost.c" as vhost
 participant "vhost_blk.c" as blk
end box

hnote over blk : vhost_blk_construct
blk -> vhost  : spdk_vhost_dev_register
[<- vhost : create a unix domain\nsocket and do accept()

|||

[-> vhost : new connection\nis accepted and\npoll()-ed
vhost -> blk : backend->new_session
blk -> vhost : struct spdk_vhost_session *

|||

[-> vhost : device is\ninitialized [...]
vhost -> vhost : read backend->virtio_features

vhost -> blk : backend->get_config
blk -> vhost : struct virtio_blk_config

|||

vhost -> blk : backend->start_session
activate blk
note right : start I/O pollers
blk -> vhost : vhost_session_start_done

[-> vhost : connection is\nterminated

vhost -> blk : backend->stop_session
blk -> vhost : vhost_session_stop_done()
deactivate blk

vhost -> blk : backend->free_session

|||

hnote over vhost : spdk_vhost_dev_remove
vhost -> blk : backend->free_device
blk -> vhost : vhost_dev_unregister

@enduml
```

Starting the pollers is described in detail in the @ref vhost_threading_start_pollers
section.

## Operational Basics {#vhost_implementation_basics}

Vhost lib does not spawn any threads, pollers, and doesn't do any continuous
work unless the first vhost device is created with spdk_vhost_scsi_dev_construct(),
spdk_vhost_blk_construct(), or RPCs `construct_vhost_scsi_controller` and
`construct_vhost_blk_controller`.

When a vhost device is created, first thing SPDK does is it creates a new unix
domain socket with rte_vhost API, namely
rte_vhost_driver_register(path_to_socket, ...).

rte_vhost is a DPDK's library for polling Vhost-user Unix domain sockets and
processing the incoming requests. It implements the Vhost-user protocol (TODO link).
It was initially targetting vhost-net only, but since the vhost-user protocol
is mostly device-agnostic, SPDK makes use of it for storage as well.

Then, SPDK calls rte_vhost_driver_callback_register() which tells rte_vhost to
call SPDK-specific callbacks e.g. when vhost driver has initialized the device
by sending appropriate Vhost-user messages. Within those callbacks SPDK could
start polling the I/O queues. The actual callback list looks as follows:

```
struct vhost_device_ops {
	/** new connection was accepted on the unix domain socket */
	int (*new_connection)(int vid);
	/** socket connection was dropped */
	void (*destroy_connection)(int vid);
	/** start polling virtqueues on one connection */
	int (*new_device)(int vid);
	/** stop polling the virtqueues */
	void (*destroy_device)(int vid);

	<SNIP> - there's a few more callbacks, but SPDK doesn't use them
};
```

new_device/destroy_device are named really inaccurately and that's because they
were present before new_connection/destroy_connection. They could use a rename,
but API stability is a priority for DPDK.

rte_vhost uses `vid` to identify connections. It's a unique number that can be
passed to other rte_vhost APIs. For instance, to get the socket path on which
the connection was made, rte_vhost_get_ifname(int vid, ...) can be used.
(Again, the function name is inaccurate.)

There's one additional, major step in the rte_vhost initialization process that's
described in a separate section -> @ref vhost_rte_vhost_workarounds.

Next step for SPDK is to call rte_vhost_driver_start(path_to_socket) which
may start a background, unaffinitized pthread to poll all vhost unix domain
sockets with a blocking poll(). The rte_vhost function needs to be called for
each vhost device, but there will be only one pthread created.

## Threading {#vhost_threading}

High level overview:

```
@startuml

participant QEMU as qemu

box "SPDK" #BlanchedAlmond
 participant "rte_vhost" as rte
 participant "Primary thread" as init
 participant "Thread #2" as t2
 participant "Thread #3" as t3
end box

hnote over init : vhost lib is initialized
hnote over init : vhost device is created
init -> rte  : create new unix domain socket
note left : start pthread\ndoing poll()
activate rte

init -> rte  : register callbacks

qemu -> rte  : VM boot
rte  -> init : new session cb
activate rte
rte  -> init : start session cb
init -> t2   : start pollers
activate t2
t2   -> init : done
init -> rte  : done
rte  -> qemu : done
deactivate rte

qemu -> rte  : VM shutdown
activate rte
rte  -> init : stop session cb
init -> t2   : stop pollers
deactivate t2
t2   -> init : done
init -> rte  : done
rte  -> qemu : done
deactivate rte

hnote over init : vhost device is removed

@enduml
```

### Initialization {#vhost_threading_init}

Upon initialization, the vhost lib will gather information about all available
spdk threads and will create a poll group object for each thread using
spdk_for_each_thread(). The "poll group" objects are named like that in attempt
to mimic SPDK NVMe-oF target, but essentially they're wrappers for spdk threads.
They allow the vhost lib to store additional per-thread metadata, e.g. a refcount
of vhost devices polled on each thread that can be used for simple round-robin
scheduling.

SPDK Vhost is meant to be managed on a single thread only and this applies to all
APIs exported in `include/spdk/vhost.h`. The SPDK thread that calls spdk_vhost_init()
will be internally stored in the lib and will be the only capable of calling
all other vhost APIs. The vhost lib will also internally schedule messages to
that thread whenever it needs to change e.g. a vhost device object. The thread
reference is stored in g_vhost_init_thread.

In a typical application utilizing SPDK app framework, vhost lib will be
automatically initialized by a vhost subsystem (TODO link here) on the master
thread - the same one that handles the overall initialization, shutdown, RPC
server, etc.

### rte_vhost {#vhost_threading_rte_vhost}

The callbacks from rte_vhost are called synchronously. Within each rte_vhost
callback we send a message to g_vhost_init_thread and do a blocking wait on
a semaphore until it signals completion.

Each rte_vhost callback in SPDK calls vhost_xxx(vid, fn_t fn) [TODO]

As soon as the rte_vhost thread accepts a connection, it calls our new_connection
callback, which in turn calls spdk_vhost_dev_backend->new_session. new_session
is supposed to return a unique spdk_vhost_session object, but backends will likely
allocate a buffer slightly bigger than struct spdk_vhost_session to be able to
store additional per-session metadata:

```
struct spdk_vhost_blk_session {
	/* The session itself */
	struct spdk_vhost_session vsession;

	/* Vhost-Block-specific metadata */
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_poller *requestq_poller;
	struct spdk_io_channel *io_channel;
	struct spdk_poller *stop_poller;
};
```

At this point the session is just a dull object which can't be used for anything.
Nevertheless, the rte_vhost new_connection callback allows SPDK to:
 a) know there's actually someone using the socket
 b) register a custom vhost-user message parsing callback (see @ref vhost_rte_vhost_workarounds)

Each session object stores its `vid`, which will be used to find it from all
subsequent rte_vhost callbacks with spdk_vhost_session_find_by_vid(vid).

Once rte_vhost receives information about vhost features supported by the driver,
then the shared memory, and finally the virtqueues, it tells SPDK to start polling
by calling vhost_device_ops->new_device. As always, SPDK redirect all action to
g_vhost_init_thread.

rte_vhost provides us with following APIs:

rte_vhost_get_vhost_vring(vid, ...);
rte_vhost_get_vring_base(vid, ...);
rte_vhost_get_negotiated_features(vid, ...);
rte_vhost_get_mem_table(vid, ...);

SPDK calls those just once at this point and caches their results in
struct spdk_vhost_session. They will used in the hot I/O path later on.
As a part of new_device callback, SPDK also registers the entire vhost shared
memory of this session for DMA with spdk_mem_register(). This can be a *very*
long operation, especially if there's an NVMe-oF RDMA connection in SPDK.

Once that's done, SPDK tells the session's backend that session is ready to be
polled. spdk_vhost_dev_backend->start_session gets called, which is supposed
to start I/O pollers. See @ref vhost_threading_start_pollers for details.

Once the session is "started" and any vhost-user message is received, rte_vhost
will stop the session by calling vhost_device_ops->destroy_device. Then it will
process the message (e.g. about a new memory hotplug/hotremove or new virtqueues)
and (possibly) start the session again.

`destroy_device` is the most problematic rte_vhost callback. As soon as it
returns, rte_vhost expects us not to look at any I/O queues anymore. To be
worse, rte_vhost could also unmap the entire shared memory right after this
callback returns, so there must be no pending DMA I/Os in SPDK at that point as
well. This implies that SPDK needs to block and wait inside rte_vhost callback
until all such I/Os are completed.

Similarily to session start, session stop will be mostly handed over to the
session's backend -> spdk_vhost_dev_backend->stop_session.

Currently Vhost-Block and Vhost-SCSI backends stop the I/O pollers in that
callback and register another poller waiting for the pending I/O to complete.

After that, spdk_vhost_session_stop_done() is called (on any thread),
which results in unregistering the shared memory (spdk_mem_unregister()) and
then returning from the rte_vhost callback.

### Starting I/O pollers {#vhost_threading_start_pollers}

*This part is a subject to change*

Once spdk_vhost_dev_backend->start_session is called, the backend is responsible
for picking a poll group on which the pollers should be started.

Vhost-Block simply calls vhost_get_poll_group() to pick a poll group in around-robin
fashion for each session, then sends a message to that poll group's thread with
vhost_session_send_event(), then starts a poller there and immediately calls
vhost_session_start_done(), which practically completes DPDK's new_device callback.

Vhost-SCSI is slightly more complicated because it's only capable of polling
all sessions for a single device on a single thread. It gets a poll group in
a round-robin fashion only when the first session for a given device is created,
then reuses the same poll group for subsequent sessions on that device.

vhost_session_start_done() needs to be called on the thread designated for this
session's pollers. Even though it's the backend that manages the pollers, the
generic vhost layer still needs to know on which thread those pollers run. It's
required for the generic vhost layer to be able to properly schedule any
session-changing spdk_thread_msg(s), namely vhost_dev_foreach_session().

Each session can be accessed on only one thread at a time. Even if a session
has multiple I/O queues, all of them need to polled on a single thread.

### Device management at runtime

To keep the threading as simple as possible, there can be only one management
operation done at a time. This means that any management operation will immediately
fail if there's another asynchronous operation pending. This behavior is mostly
transparent to e.g. rpc.py users, because any request effectively blocks until
it's completed.

All device-changing actions look as follows:

```
@startuml

box "SPDK" #BlanchedAlmond
 participant "Primary thread" as init
 participant "Thread #2\nSession A" as t2
 participant "Thread #3\nSession B" as t3
end box

hnote over init : update vdev object
init -> t2 : vhost_dev_foreach_session()
hnote over t2  : pull vdev changes\ninto vsession
t2 -> t3 : continue iterating
hnote over t3  : pull vdev changes\ninto vsession
t3 -> init : finish iterating
hnote over init : complete the action

@enduml
```

As for the implementation itself:

```
 * struct spdk_vhost_dev *vdev = spdk_vhost_dev_find("socket_basename");
 * verify there are no other asynchronous operations happening on that vdev
 * spdk_vhost_scsi_dev_add_tgt(vdev, ...);
   * verify vdev->backend == &g_vhost_scsi_device_backend
   * downcast vdev to scsi-specific struct (svdev)
   * check if the requested action is valid - for adding a new SCSI target
     check e.g. if the requested SCSI slot is not occupied yet. This usually
     just acceses a field in svdev
   * if the slot is empty, put the new target there - set a field in svdev
   * call vhost_dev_foreach_session() and ask each session to update itself
     * call provided callback on each session's thread
       * copy the corresponding SCSI target information from svdev to the
         Vhost-SCSI session object (without disrupting any ongoing I/O)
     * call the completion callback back on the g_vhost_init_thread
       * complete the RPC request, print a message, etc
```

We manage to be thread-safe without using any locks, because
 1) all the device management happens on a single thread
 2) the device can't be changed while we iterate through sessions

#2 is true because there can't be any other management operations happening
until this one completes. All actions triggered directly by the user will
immediately fail, and handling rte_vhost callbacks will be simply delayed
until the management operation is done - the rte_vhost background thread will
kindly wait on a semaphore as long as we need.

[TODO describe the hotremove case; slightly more complex]

Just for reference, vhost_dev_foreach_session() has the following signature:

```
  /**
   * Call provided function for each session of the provide vhost device.
   *
   * \param vdev vhost device.
   * \param fn function to call for each session. If the session is being polled,
   * the function will be called on the very same thread it's polled on. Otherwise
   * the function will be called on g_vhost_init_thread. This function is called
   * one-by-one. There won't be two executions at the same time.
   * \param cpl_fn function to be called after *fn* has been called for all
   * sessions.
   * \param arg additional argument to *fn* and *cpl_fn*
   */
  void vhost_dev_foreach_session(struct spdk_vhost_dev *vdev,
                                 spdk_vhost_session_fn fn,
                                 spdk_vhost_dev_fn cpl_fn,
                                 void *arg);
```

## rte_vhost workarounds {#vhost_rte_vhost_workarounds}

rte_vhost is not fully compliant with the Vhost-user specification and doesn't
handle all Vhost-user messages as it should. While rte_vhost works for DPDK-internal
vhost-net implementation, there are certain problems when using it for Vhost-SCSI
and Vhost-Block.

Historically, SPDK vhost was shipped with an internal fork of rte_vhost lib
which had a few storage-specific changes applied. Those changes were rejected
in the upstream repository, but eventually we've managed to upstream rte_vhost
APIs to hook directly into the Vhost-user message handling code. Namely, we've
added rte_vhost_extern_callback_register(vid, ...). This allowed us to apply
the same changes (or more precisely - workarounds) to rte_vhost from SPDK,
without altering the rte_vhost code itself. Those APIs were introduced in DPDK
19.05, and SPDK 19.04+ could already utilize them. The internal rte_vhost fork
in SPDK is still kept around to support older DPDK versions, but it will be
eventually removed.

SPDK 19.07+ will use the upstream rte_vhost by default, but it can be forced to
use the internal fork if configured with `./configure --with-internal-vhost-lib`.

Fixing rte_vhost to be fully spec compliant would be a huge undertaking, so
we've decided to stick with just a few workarounds that allow SPDK to be used
with standard QEMU.

All SPDK workarounds for the upstream rte_vhost are located in a separate file
`rte_vhost_compat.c` (TODO link). The file contains fair description for each
workaround. To quote some of them:

```
  case VHOST_USER_SET_MEM_TABLE:
	  /* rte_vhost will unmap previous memory that SPDK may still
	   * have pending DMA operations on. We can't let that happen,
	   * so stop the device before letting rte_vhost unmap anything.
	   * This will block until all pending I/Os are finished.
	   * We will start the device again from the post-processing
	   * message handler.
	   */
```

```
  case VHOST_USER_SET_VRING_CALL:
	  /* rte_vhost will close the previous callfd and won't notify
	   * us about any change. This will effectively make SPDK fail
	   * to deliver any subsequent interrupts until a session is
	   * restarted. We stop the session here before closing the previous
	   * fd (so that all interrupts must have been delivered by the
	   * time the descriptor is closed) and start right after (which
	   * will make SPDK retrieve the latest, up-to-date callfd from
	   * rte_vhost.
	   */
```

```
  case VHOST_USER_SET_FEATURES:
	  /* rte_vhost requires all queues to be fully initialized in order
	   * to start I/O processing. This behavior is not compliant with the
	   * vhost-user specification and doesn't work with QEMU 2.12+, which
	   * will only initialize 1 I/O queue for the SeaBIOS boot.
	   * Theoretically, we should start polling each virtqueue individually
	   * after receiving its SET_VRING_KICK message, but rte_vhost is not
	   * designed to poll individual queues. So here we use a workaround
	   * to detect when the vhost session could be potentially at that SeaBIOS
	   * stage and we mark it to start polling as soon as its first virtqueue
	   * gets initialized. This doesn't hurt any non-QEMU vhost slaves
	   * and allows QEMU 2.12+ to boot correctly. SET_FEATURES could be sent
	   * at any time, but QEMU will send it at least once on SeaBIOS
	   * initialization - whenever powered-up or rebooted.
```

This file also implements GET_CONFIG/SET_CONFIG messages used for Virtio-PCI
config access. Those are an extension to the vhost-user protocol and are completely
device-agnostic. We've tried to put those inside rte_vhost, but the patches were
rejected as they were not needed for vhost-net.
