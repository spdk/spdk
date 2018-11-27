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

1. Module allocates notify structure using spdk_nofify_alloc
2. Fill all fields in notify
3. Module sends notification using spdk_notify_send
4. In spdk_notify_handler callback each client calls spdk_notify_get(struct spdk_notify* notify) to increase refcount
5. After notiifcation is handled by the client, it must call spdk_nofify_put


`struct spdk_notify *notify spdk_nofify_alloc(void)`
`spdk_nofify_get(struct spdk_notify *notify)`
`spdk_nofify_put(struct spdk_notify *notify)`
`spdk_notify_send(struct spdk_notify *notify)`

# Recevinig event {#notify_receive}

When event occurs, spdk_notify_handler callback will be called:
`typedef void (*spdk_notify_handler)(struct spdk_notify *notify, void *ctx)`

Receiving library can call following calls on notify structure:

- spdk_notify_get_type return notification name, which is same as rpc request that would cause such event
Example:
During nvme removal notification with name 'delete_bdev_nvme' will be submitted
`const char *spdk_notify_get_type(struct spdk_notify *notify)`

- spdk_notify_get_object returns positional argument to rpc request, which is same as rpc request that would cause such event
`const char *spdk_notify_get_object(struct spdk_notify *notify)`

- spdk_notify_get_uuid returns uuid of object or null if not applicable
`const char *spdk_notify_get_uuid(struct spdk_notify *notify)`

Examples:

type: delete_lvol_store, object: lvol_store_name
type: delete_lvol, object: lvol_name
