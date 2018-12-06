def get_notification_types(client):
    return client.call("get_notification_types")


def get_notifications(client,
                      id=None):
    """

    Args:
        id First ID to start fetching from

    Return:
        Notifications array
    """

    params = {}
    if id:
        params['id'] = id

    return client.call("get_notifications", params)
