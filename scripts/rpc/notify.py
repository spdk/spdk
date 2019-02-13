import time


def get_notification_types(client):
    return client.call("get_notification_types")


def get_notifications(client,
                      timeout_ms=3000,
                      cnt=1,
                      **kwargs):
    """Listen for notifications.
    This will queue provided number of notificaton requests to the server and then listen for response.
    Total amount of time needed to complete notifications is a number of queued notifications multiplied
    by timeout for each notification.


    Args:
        cnt How many requests to schedule (default: 1).
        timeout_ms Timeout per event. If <= events won't timeout out (default: 3000)

    **kwargs:
        max_count Maximum notifications per request

    Return:
        Notifications array
    """
    params = {k: kwargs[k] for k in kwargs if k in kwargs}
    l = client.get_logger()

    l.info("Sending %d notification requests", cnt)
    for i in range(cnt):
        params['timeout_ms'] = timeout_ms * (i + 1)
        client.add_request("get_notifications", params)

    client.flush()

    if timeout_ms > 0:
        end_time = time.clock() + timeout_ms * 1000.0
    else:
        end_time = time.clock() + 3600

    l.info("Waiting for response")
    resp = []
    for i in range(cnt):
        client.timeout = end_time - time.clock()
        resp.append(client.recv())
        l.info("Got %d response", i+1)

    l.info("Got all notifications")
    return resp
