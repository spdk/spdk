from .helpers import deprecated_alias


@deprecated_alias('kill_instance')
def spdk_kill_instance(client, sig_name):
    """Send a signal to the SPDK process.

    Args:
        sig_name: signal to send ("SIGINT", "SIGTERM", "SIGQUIT", "SIGHUP", or "SIGKILL")
    """
    params = {'sig_name': sig_name}
    return client.call('spdk_kill_instance', params)


@deprecated_alias('context_switch_monitor')
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


def framework_set_thread_affinity(client, name, cpumask):
    """Set the cpumask of the thread whose name matches to the specified value.

    Args:
        name: thread name
        cpumask: cpumask for this thread

    Returns:
        True or False
    """
    params = {'name': name, 'cpumask': cpumask}
    return client.call('framework_set_thread_affinity', params)


def thread_get_stats(client):
    """Query threads statistics.

    Returns:
        Current threads statistics.
    """
    return client.call('thread_get_stats')
