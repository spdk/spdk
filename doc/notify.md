# Architecture and naming {#notify}
Notify library implements queue mechanism for sending and receiving events and
information about those events. It is possible to use notify library in two ways.
First, is a library that wants to generate events, i.e. bdev that wants to notify
about specific actions like creation or deletion of a bdev. Library that generates
events is called *Sender*. Second type, is a library that wants to consume such
events, i.e. logging library or RPC library. Such library is called Client.

# Register event types {#notify_register}

During initialization sender library should register its own event types using
spdk_notify_type_register(const char *type). Parameter 'type' is the name of notification type.

# Get info about events {#notify_get_info}

Consumer can get information about available events during runtime using `spdk_notify_get_types`, which iterates over registered notification types and calls callback on each of them, so that user can produce detailed information about notification.

# Get new events {#notify_listen}

Consumer library can subscribe to event stream. There might be multile listeners.
In function `spdk_notify_get_events` consumer should specify last received event,
maximum number of invocations. The stream is built on the circular buffer, so that
older events might be overriden by new ones.

# Send events {#notify_send}

When event occurs, library can invoke `spdk_notify_send` with two string buffers.
One containing type of the event, like "spdk_bdev_register", second with context,
for example "Nvme0n1"

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
