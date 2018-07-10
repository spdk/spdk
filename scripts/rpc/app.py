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


def get_poller_information(client, lcore_id):
    """Retrieve a list of poller function pointers for the specified lcore.abs

    Args:
        lcore_id: If set, provide only the pollers on the specified lcore. If unset provide information on all cores

    Returns:
        A list of pollers for one or all cores, separated between timed and active pollers.
    """
    params = {}
    if lcore_id is not None:
        params['lcore_id'] = lcore_id
    return client.call('get_poller_information', params)
