def dsa_scan_accel_engine(client, config_kernel_mode=None):
    """Scan and enable DSA accel engine.

    Args:
        config_kernel_mode: Use kernel DSA driver. (optional)
    """
    params = {}

    if config_kernel_mode is not None:
        params['config_kernel_mode'] = config_kernel_mode
    return client.call('dsa_scan_accel_engine', params)
