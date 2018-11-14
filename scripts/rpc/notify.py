def get_notification_types(client):
    return client.call("get_notification_types")

def get_notifications(client, batch=1, event_timeout_ms=500, wait_time_ms=1000, notification_types=None):
    params={
        'event_timeout_ms': event_timeout_ms,
        'wait_time_ms': wait_time_ms
    }
    
    
    
    if notification_types:
        params['notification_types'] = notification_types.split(',')
    
    for i in range(batch):
        client.call("get_notification_types")
    
    pass
