# Notify library {#notify}
Notify library implements queue mechanism for sending and receiving events and
information about those events. It is possible to use notify library in two ways.
First, is a library that wants to generate events, i.e. bdev that wants to notify
about specific actions like creation or deletion of a bdev. Library that generates
events is called *Sender*.  Events are placed in circular ring and then can be
fetched by  a client, that wants to get and consume the events like logging or RPC
library. Note, that events in the circular ring can be overwritten and lost if not
polled quick enough.

# Register event types {#notify_register}

During initialization sender library should register its own event types using
`spdk_notify_type_register(const char *type)`. Parameter 'type' is the name of
notification type.

# Get info about events {#notify_get_info}

Consumer can get information about available events during runtime using
`spdk_notify_get_types`, which iterates over registered notification types and
calls callback on each of them, so that user can produce detailed information
about notification.

# Get new events {#notify_listen}

Consumer library can get events by calling function `spdk_notify_get_events`.
Caller should specify last received event, maximum number of invocations. There might
 be multile consumers of each event. The stream is built on the circular buffer,
 so that older events might be overriden by new ones.

# Send events {#notify_send}

When event occurs, library can invoke `spdk_notify_send` with two strings.
One containing type of the event, like "spdk_bdev_register", second with context,
for example "Nvme0n1"

# RPC Calls {#rpc_calls}

See [JSON-RPC documentation](jsonrpc.md/#rpc_get_notification_types)
