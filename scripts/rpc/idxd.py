def idxd_scan_accel_engine(client, config_number=None, config_kernel_mode=None):
    """Scan and enable IDXD accel engine.

    Args:
        config_number: Pre-defined configuration number, see docs.
        config_kernel_mode: Use kernel IDXD driver. (optional)
    """
    params = {}

    params['config_number'] = config_number
    if config_kernel_mode is not None:
        params['config_kernel_mode'] = config_kernel_mode
    return client.call('idxd_scan_accel_engine', params)
