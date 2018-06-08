def kill_instance(client, sig_name):
    """Send a signal to the SPDK process.

    Args:
        sig_name: signal to send ("SIGINT", "SIGTERM", "SIGQUIT", "SIGHUP", or "SIGKILL")
    """
    params = {'sig_name': sig_name}
    return client.call('kill_instance', params)


def context_switch_monitor(client, enabled=None):
    """Query or set state of context switch monitoring.

    Args:
        enabled: True to enable monitoring; False to disable monitoring; None to query (optional)

    Returns:
        Current context switch monitoring state (after applying enabled flag).
    """
    params = {}
    if enabled is not None:
        params['enabled'] = enabled
    return client.call('context_switch_monitor', params)


def get_reactor_tsc_stats(client, core_id=None):
    """Get reactor tsc stats

    Args:
        core: Specific core id to query (optional; if omitted, query all cores)

    Returns:
        Cumulative time stamp counters for a given core in terms of busy, idle and unknown.
    """
    params = {}
    if core_id is not None:
        params['core_id'] = core_id
    return client.call('get_reactor_tsc_stats', params)
