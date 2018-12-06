def get_notification_types(client):
    return client.call("get_notification_types")

def get_notifications(client, batch=1, event_timeout_ms=500, wait_time_ms=1000, notification_types=None):
    """
        TODO: doc
    """
    params = {
        'event_timeout_ms': event_timeout_ms,
        'wait_time_ms': wait_time_ms
    }

    # TODO: add logging
    if notification_types:
        params['notification_types'] = notification_types

    for i in range(batch):
        client.add_request("get_notifications", params)
    
    resp = []
    for i in range(batch):
        resp += client.recv()
    
    # TODO: add logging
    
    return resp
