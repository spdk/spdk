from .helpers import deprecated_alias


@deprecated_alias('set_trace_flag')
def set_log_flag(client, flag):
    """Set log flag.

    Args:
        flag: log flag we want to set. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('set_log_flag', params)


@deprecated_alias('clear_trace_flag')
def clear_log_flag(client, flag):
    """Clear log flag.

    Args:
        flag: log flag we want to clear. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('clear_log_flag', params)


@deprecated_alias('get_trace_flags')
def get_log_flags(client):
    """Get log flags

    Returns:
        List of log flags
    """
    return client.call('get_log_flags')


@deprecated_alias('set_log_level')
def log_set_level(client, level):
    """Set log level.

    Args:
        level: log level we want to set. (for example "DEBUG")
    """
    params = {'level': level}
    return client.call('log_set_level', params)


@deprecated_alias('get_log_level')
def log_get_level(client):
    """Get log level

    Returns:
        Current log level
    """
    return client.call('log_get_level')


@deprecated_alias('set_log_print_level')
def log_set_print_level(client, level):
    """Set log print level.

    Args:
        level: log print level we want to set. (for example "DEBUG")
    """
    params = {'level': level}
    return client.call('log_set_print_level', params)


@deprecated_alias('get_log_print_level')
def log_get_print_level(client):
    """Get log print level

    Returns:
        Current log print level
    """
    return client.call('log_get_print_level')
