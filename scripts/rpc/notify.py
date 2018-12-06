import time

def get_notification_types(client):
    return client.call("get_notification_types")


def get_notifications(client,
                      total_timeout_ms=3000,
                      cnt=1,
                      **kwargs):
    """Listen for notifications.
    This will queue provided number of notificaton requests to the server and then listen for response.
    Total amount of time needed to complete notifications is a number of queued notifications multiplied
    by timeout for each notification.     
    
        
    Args:
        total_timeout_ms total How long to wait for all events (default: 3000)
        cnt How many requests to schedule (default: 1).

    **kwargs:
        timeout_ms Timeout per event.
        max_count Maximum notifications per request
    
    Return:
        Notifications array
    """

    params = {k: kwargs[k] for k in opt_params if k in kwargs}
    l = client.get_logger()

    l.info("Sending %d notification requests", cnt)
    for i in range(cnt):
        client.add_request("get_notifications", params)

    client.flush()

    end_time = time.clock() + total_timeout_ms * 1000.0;
    
    l.info("Waiting for response")
    resp = []
    for i in range(cnt):
        client.timeout = end_time - time.clock();
        resp.append(client.recv())
        l.info("Got %d response", i+1)

    l.info("Got all notifications")
    return resp
