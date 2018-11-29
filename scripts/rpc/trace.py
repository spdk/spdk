def set_tpoint_group_mask(client, name):
    """Set trace point group mask.

    Args:
        name: trace group name we want to set in tpoint_group_mask. (for example "bdev").
    """
    params = {'name': name}
    return client.call('set_tpoint_group_mask', params)


def clear_tpoint_group_mask(client, name):
    """Clear trace point group mask.

    Args:
        name: trace group name we want to clear in tpoint_group_mask. (for example "bdev").
    """
    params = {'name': name}
    return client.call('clear_tpoint_group_mask', params)


def get_tpoint_group_mask(client):
    """Get trace point group mask

    Returns:
        List of trace point group mask
    """
    return client.call('get_tpoint_group_mask')
