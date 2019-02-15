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


def get_reactors_stat(client, reset=None):
    """Query and optionally reset reactors statistics.

    Args:
        reset: True to reset statistics (optional)

    Returns:
        Current reactors statistics.
    """
    params = {}
    if reset is not None:
        params['reset'] = reset
    return client.call('get_reactors_stat', params)
