#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.


def spdk_kill_instance(client, sig_name):
    """Send a signal to the SPDK process.

    Args:
        sig_name: signal to send ("SIGINT", "SIGTERM", "SIGQUIT", "SIGHUP", or "SIGKILL")
    """
    params = {'sig_name': sig_name}
    return client.call('spdk_kill_instance', params)


def framework_monitor_context_switch(client, enabled=None):
    """Query or set state of context switch monitoring.

    Args:
        enabled: True to enable monitoring; False to disable monitoring; None to query (optional)

    Returns:
        Current context switch monitoring state (after applying enabled flag).
    """
    params = {}
    if enabled is not None:
        params['enabled'] = enabled
    return client.call('framework_monitor_context_switch', params)


def framework_get_reactors(client):
    """Query list of all reactors.

    Returns:
        List of all reactors.
    """
    return client.call('framework_get_reactors')


def framework_set_scheduler(client, name, period=None, load_limit=None, core_limit=None,
                            core_busy=None):
    """Select threads scheduler that will be activated and its period.

    Args:
        name: Name of a scheduler
        period: Scheduler period in microseconds
    Returns:
        True or False
    """
    params = {'name': name}
    if period is not None:
        params['period'] = period
    if load_limit is not None:
        params['load_limit'] = load_limit
    if core_limit is not None:
        params['core_limit'] = core_limit
    if core_busy is not None:
        params['core_busy'] = core_busy
    return client.call('framework_set_scheduler', params)


def framework_get_scheduler(client):
    """Query currently set scheduler.

    Returns:
        Name, period (in microseconds) of currently set scheduler and name of currently set governor.
    """
    return client.call('framework_get_scheduler')


def thread_get_stats(client):
    """Query threads statistics.

    Returns:
        Current threads statistics.
    """
    return client.call('thread_get_stats')


def thread_set_cpumask(client, id, cpumask):
    """Set the cpumask of the thread whose ID matches to the specified value.

    Args:
        id: thread ID
        cpumask: cpumask for this thread

    Returns:
        True or False
    """
    params = {'id': id, 'cpumask': cpumask}
    return client.call('thread_set_cpumask', params)


def log_enable_timestamps(client, enabled):
    """Enable or disable timestamps.

    Args:
        value: on or off

    Returns:
        None
    """
    params = {'enabled': enabled}
    return client.call('log_enable_timestamps', params)


def thread_get_pollers(client):
    """Query current pollers.

    Returns:
        Current pollers.
    """
    return client.call('thread_get_pollers')


def thread_get_io_channels(client):
    """Query current IO channels.

    Returns:
        Current IO channels.
    """
    return client.call('thread_get_io_channels')
