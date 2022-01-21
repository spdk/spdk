def idxd_scan_accel_engine(client, config_kernel_mode=None):
    """Scan and enable IDXD accel engine.

    Args:
        config_kernel_mode: Use kernel IDXD driver. (optional)
    """
    params = {}

    if config_kernel_mode is not None:
        params['config_kernel_mode'] = config_kernel_mode
    return client.call('idxd_scan_accel_engine', params)
