def enable_tpoint_group(client, name):
    """Enable trace on a specific tpoint group.

    Args:
        name: trace group name we want to enable in tpoint_group_mask. (for example "bdev").
    """
    params = {'name': name}
    return client.call('enable_tpoint_group', params)


def disable_tpoint_group(client, name):
    """Disable trace on a specific tpoint group.

    Args:
        name: trace group name we want to disable in tpoint_group_mask. (for example "bdev").
    """
    params = {'name': name}
    return client.call('disable_tpoint_group', params)


def get_tpoint_group_mask(client):
    """Get trace point group mask

    Returns:
        List of trace point group mask
    """
    return client.call('get_tpoint_group_mask')
