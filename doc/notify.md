# Notify library {#notify}

The notify library implements an event bus, allowing users to register, generate,
and listen for events. For example, the bdev library may register a new event type
for bdev creation. Any time a bdev is created, it "sends" the event. Consumers of
that event may periodically poll for new events to retrieve them.
The event bus is implemented as a circular ring of fixed size. If event consumers
do not poll frequently enough, events may be lost. All events are identified by a
monotonically increasing integer, so missing events may be detected, although
not recovered.

# Register event types {#notify_register}

During initialization the sender library should register its own event types using
`spdk_notify_type_register(const char *type)`. Parameter 'type' is the name of
notification type.

# Get info about events {#notify_get_info}

A consumer can get information about the available event types during runtime using
`spdk_notify_foreach_type`, which iterates over registered notification types and
calls a callback on each of them, so that user can produce detailed information
about notification.

# Get new events {#notify_listen}

A consumer can get events by calling function `spdk_notify_foreach_event`.
The caller should specify last received event and the maximum number of invocations.
There might be multiple consumers of each event. The event bus is implemented as a
circular buffer, so older events may be overwritten by newer ones.

# Send events {#notify_send}

When an event occurs, a library can invoke `spdk_notify_send` with two strings.
One containing the type of the event, like "spdk_bdev_register", second with context,
for example "Nvme0n1"

# RPC Calls {#rpc_calls}

See [JSON-RPC documentation](jsonrpc.md/#rpc_notify_get_types)
