# Architecture and naming {#notify}
Notify library implements queue mechanism for sending and receiving events and information about those events.
It is possible to use notify library in two ways. First, is a library that wants to generate events, i.e.
bdev that wants to notify about specific actions like creation or deletion of a bdev.
Library that generates events is called *Sender*. Second type, is a library that wants to consume such events, i.e.
logging library or RPC library. Such library is called Client.

# Register event types {#notify_register}

During initialization sender library should register its own event types using macro SPDK_NOTIFY_TYPE_REGISTER.
To define sdpk_notify_type library should fill following fields:
name - Name of notification type
destroy_cb -  Destroy callback to release any resources from notification context (notify->ctx).
write_info_cb - Callback for writing notification information into JSON write context.

Example:

```
static struct spdk_notify_type construct_malloc_bdev_notify = {
	.name = "construct_malloc_bdev",
	.write_info_cb = construct_malloc_bdev_notify_write_info,
};

SPDK_NOTIFY_TYPE_REGISTER(construct_malloc_bdev_notify);
```

# Get info about events {#notify_get_info}

Client library can get information about available events during runtime using following calls:

- Get information about first registered event type
`struct spdk_notify *spdk_notify_type_first(void)`

- Get information about next registered event type
`struct spdk_notify *spdk_notify_type_next(struct spdk_notify_type *prev)`

This allows listing all available notifaction types depending on which modules were compiled.

# Listen for new events {#notify_listen}

Client library can listen to new notifications using following call:
`int spdk_notify_listen(spdk_notify_handler cb, void *ctx)`

To stop listening for new events client library must call:
`int spdk_notify_unlisten(spdk_notify_handler cb, void *ctx)`

# Sending events {#notify_send}

When event occurs, first step is to allocate notify structure using spdk_nofify_alloc.
Then, the module which sends notification have to fill all fields in notify structure and call spdk_notify_send on that notification.
During this call all listening clients are called with theirs callbacks. Client can handle the notification in two ways.
Synchronously, meaning that notification is handled inside callback and notifaction resources can be released after the client return control from callback.
Asynchronously, If client want to use event resources after returning from callback.
In that case client needs to call spdk_notify_get to increase notification refcount.
After notiifcation is handled asynchronously by the client, it must call spdk_nofify_put.

# Recevinig event {#notify_receive}

When event occurs, spdk_notify_handler callback will be called:
`typedef void (*spdk_notify_handler)(struct spdk_notify *notify, void *ctx)`

Receiving library can call following calls on notify structure:

- spdk_notify_write_json - Callback from module that sent notification is called to write information about notification.

Example:
During nvme removal notification with name 'delete_bdev_nvme' will be submitted
`const char *spdk_notify_get_type(struct spdk_notify *notify)`

- write_json

- spdk_notify_get_object returns positional argument to JSON RPC request, which is same as JSON RPC request that would cause such event
`const char *spdk_notify_get_object(struct spdk_notify *notify)`

- spdk_notify_get_uuid returns uuid of object or null if not applicable
`const char *spdk_notify_get_uuid(struct spdk_notify *notify)`

Examples:

type: delete_lvol_store, object: lvol_store_name
type: delete_lvol, object: lvol_name

RPC Calls
================

`get_notification_types` - show available events.

`get_notifications` - request single notification

Client discovers events by calling `get_notification_types`. To receive single event client have to call ,`get_notifications`.
To avoid violation of json JSON RPC specification for each request there is only one reply.
Client should issue number of requests to make sure that there will be no race condition between receiving notification and asking for another one.
Client should keep the connection for each request until appropiate reply is received.

Examples:
-> Requests outgoing from client to rpc server
<- Response outgoing from rpc server to client
-> {"jsonrpc": "2.0", "id": 1, "result": { 'method': "get_notifications", "params"  : {...} } }
-> {"jsonrpc": "2.0", "id": 2, "result": { 'method': "get_notifications", "params"  : {...} } }
-> {"jsonrpc": "2.0", "id": 3, "result": { 'method': "get_notifications", "params"  : {...} } }

-> {"jsonrpc": "2.0", "id": 1, "result": { "method": "construct_nvme_bdev", "params": { "name": "Nvme1", "traddr": "0000:01:00.0", "trtype": "pcie" } } }
-> {"jsonrpc": "2.0", "id": 2, "result": {  "method": "construct_malloc_bdev", "params": { "name": "Malloc1", "total_size": "128", "block_size": "512" } }
-> {"jsonrpc": "2.0", "id": 3, "result": {  "method": "delete_malloc_bdev", "params": { "name": "Malloc1" } }

Event Types
========

Two type of notifications:
- Notifications about events that can be replicated via JSON RPC
  e.g. construct_malloc_bdev

- Notifications which are not explicitly triggered or mapped to any RPC methods
  e.g. during run time SPDK detects NVMe bdev failure (due to write IO errors etc.)

Other type of events to add in the future:
- Runtime errors
- Statistics
- Nvmf/iscsi/vhost initiators connecting and disconnecting
