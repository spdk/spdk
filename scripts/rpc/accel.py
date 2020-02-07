def accel_set_module(client, module):
    """Set the module for the acceleration framework.

    Args:
        pmd: 0 = auto-select, 1 = Software, 2 = CBDMA
    """
    params = {'module': module}

    return client.call('accel_set_module', params)
