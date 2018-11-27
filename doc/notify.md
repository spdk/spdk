# Architecture and naming {#notify}
Notify library implements queue mechanism for sending and receiving events and information about those events.

Client is a library that listens for new notifications.
Sender is a library that sends notifications to all subscribers.

# Register event types {#notify_register}

During initialization sender library should register all event types.

`void spdk_notify_register_type(const char *name, spdk_notify_get_info get_object_cb, spdk_notify_get_info get_uuid_cb)`

# Get info about events {#notify_get_info}

Client library can get information about all available events using following calls:

- Get information about first registered event type
`struct spdk_notify *spdk_notify_first(void)`

- Get information about next registered event type
`struct spdk_notify *spdk_notify_next(struct spdk_notify *prev)`

# Listen for new events {#notify_listen}

Client library can listen to new notifications using following call:
`int spdk_notify_listen(spdk_notify_handler cb, void *ctx)`

To stop listening for new events client library must call:
`int spdk_notify_unlisten(spdk_notify_handler cb, void *ctx)`

# Sending events {#notify_send}

When event occurs, first step is to allocate notify structure using spdk_nofify_alloc. Then module which sends notification have to fill all fields in notify structure and call spdk_notify_send.
If client want to use event after returning from callback it needs to call spdk_notify_get to increase refcount. After notiifcation is handled asynchronously by the client, it must call spdk_nofify_put.

`struct spdk_notify *notify spdk_nofify_alloc(void)`
`spdk_nofify_get(struct spdk_notify *notify)`
`spdk_nofify_put(struct spdk_notify *notify)`
`spdk_notify_send(struct spdk_notify *notify)`

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
To avoid violation of json JSON RPC specification for each request there is only one reply. Client should issue number of requests to make sure that there will be no race condition between receiving notification and asking for another one.
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

Two type of events:
- Events that notify about events that can be replicated via JSON RPC
  e.g. construct_malloc_bdev

- Events which are not explicitly triggered or mapped to any RPC methods
  e.g. during run time SPDK detects NVMe bdev failure (due to write IO errors etc.)

Other type of events to add in the future:
- Runtime errors
- Statistics
- Nvmf/iscsi/vhost initiators connecting and disconnecting

Other ideas:
============

- Filtering of notifications
To be described.

- Coalescing of notifications

Return an array of objects or object containing array:
  -> {"jsonrpc": "2.0", "id": 1, "result": { events=[ {event1}, {event2}, ..., {eventN} ] } }
For now we will always return one element in array but we can expand this in the feature.

- Event Prioritization
We might need to prioritize some types of events.
