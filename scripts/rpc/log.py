def set_log_flag(client, flag):
    """Set log flag.

    Args:
        flag: log flag we want to set. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('set_log_flag', params)


def set_trace_flag(client, flag):
    return set_log_flag(client, flag)


def clear_log_flag(client, flag):
    """Clear log flag.

    Args:
        flag: log flag we want to clear. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('clear_log_flag', params)


def clear_trace_flag(client, flag):
    return clear_log_flag(client, flag)


def get_log_flags(client):
    """Get log flags

    Returns:
        List of log flags
    """
    return client.call('get_log_flags')


def get_trace_flags(client):
    return get_log_flags(client)


def set_log_level(client, level):
    """Set log level.

    Args:
        level: log level we want to set. (for example "DEBUG")
    """
    params = {'level': level}
    return client.call('set_log_level', params)


def get_log_level(client):
    """Get log level

    Returns:
        Current log level
    """
    return client.call('get_log_level')


def set_log_print_level(client, level):
    """Set log print level.

    Args:
        level: log print level we want to set. (for example "DEBUG")
    """
    params = {'level': level}
    return client.call('set_log_print_level', params)


def get_log_print_level(client):
    """Get log print level

    Returns:
        Current log print level
    """
    return client.call('get_log_print_level')
