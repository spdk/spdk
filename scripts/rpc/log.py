from .helpers import deprecated_alias


@deprecated_alias('set_log_flag')
def log_set_flag(client, flag):
    """Set log flag.

    Args:
        flag: log flag we want to set. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('log_set_flag', params)


@deprecated_alias('clear_log_flag')
def log_clear_flag(client, flag):
    """Clear log flag.

    Args:
        flag: log flag we want to clear. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('log_clear_flag', params)


@deprecated_alias('get_log_flags')
def log_get_flags(client):
    """Get log flags

    Returns:
        List of log flags
    """
    return client.call('log_get_flags')


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
