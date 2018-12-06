def get_notification_types(client):
    return client.call("get_notification_types")


def get_notifications(client,
                      id=None,
                      max=None):
    """

    Args:
        id First ID to start fetching from
        max Maximum number of notifications to return in response

    Return:
        Notifications array
    """

    params = {}
    if id:
        params['id'] = id

    if max:
        params['max'] = max

    return client.call("get_notifications", params)
