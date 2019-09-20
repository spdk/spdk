from .helpers import deprecated_alias


@deprecated_alias('get_notification_types')
def notify_get_types(client):
    return client.call("notify_get_types")


@deprecated_alias('get_notifications')
def notify_get_notifications(client,
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

    return client.call("notify_get_notifications", params)
