def get_notification_types(client):
    return client.call("get_notification_types")


def get_notifications(client, total_timeout_ms=3000, cnt=1, **kwargs):
    """
        TODO: doc
        TODO: timeout
        total_timeout_ms total timeout for all event
    """
    opt_params = ('min_events', 'max_events', 'notification_types', 'timeout_ms')
    params = {k: kwargs[k] for k in opt_params if k in kwargs}
    l = client.get_logger()
    l.info("Sending %d notififcation requests", cnt)

    for i in range(cnt):
        client.add_request("get_notifications", params)

    client.flush()

    l.info("Waiting for response")
    resp = []
    for i in range(cnt):
        resp.append(client.recv())
        l.info("Got %d response", i+1)

    l.info("Got all notifications")
    return resp
