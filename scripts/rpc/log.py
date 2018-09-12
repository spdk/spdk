def set_trace_flag(client, flag):
    """Set trace flag.

    Args:
        flag: trace mask we want to set. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('set_trace_flag', params)


def clear_trace_flag(client, flag):
    """Clear trace flag.

    Args:
        flag: trace mask we want to clear. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('clear_trace_flag', params)


def get_trace_flags(client):
    """Get trace flags

    Returns:
        List of trace flag
    """
    return client.call('get_trace_flags')


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
