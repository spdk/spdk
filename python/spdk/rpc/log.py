#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.


def log_set_flag(client, flag):
    """Set log flag.

    Args:
        flag: log flag we want to set. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('log_set_flag', params)


def log_clear_flag(client, flag):
    """Clear log flag.

    Args:
        flag: log flag we want to clear. (for example "nvme")
    """
    params = {'flag': flag}
    return client.call('log_clear_flag', params)


def log_get_flags(client):
    """Get log flags

    Returns:
        List of log flags
    """
    return client.call('log_get_flags')


def log_set_level(client, level):
    """Set log level.

    Args:
        level: log level we want to set. (for example "DEBUG")
    """
    params = {'level': level}
    return client.call('log_set_level', params)


def log_get_level(client):
    """Get log level

    Returns:
        Current log level
    """
    return client.call('log_get_level')


def log_set_print_level(client, level):
    """Set log print level.

    Args:
        level: log print level we want to set. (for example "DEBUG")
    """
    params = {'level': level}
    return client.call('log_set_print_level', params)


def log_get_print_level(client):
    """Get log print level

    Returns:
        Current log print level
    """
    return client.call('log_get_print_level')


def log_set_rate_limit_interval(client, interval):
    """Set log rate limit interval.

    Args:
        interval: log rate limit interval.
    """
    params = {'interval': interval}
    return client.call('log_set_rate_limit_interval', params)


def log_get_rate_limit_interval(client):
    """Get log rate limit interval

    Returns:
        Current log rate limit interval
    """
    return client.call('log_get_rate_limit_interval')


def log_set_rate_limit_burst(client, burst):
    """Set log rate limit burst.

    Args:
        burst: log rate limit burst.
    """
    params = {'burst': burst}
    return client.call('log_set_rate_limit_burst', params)


def log_get_rate_limit_burst(client):
    """Get log rate limit burst

    Returns:
        Current log rate limit burst
    """
    return client.call('log_get_rate_limit_burst')
