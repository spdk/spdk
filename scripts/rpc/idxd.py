def idxd_scan_accel_engine(client, config_number):
    """Scan and enable IDXD accel engine.

    Args:
        config_number: Pre-defined configuration number, see docs.
    """
    params = {'config_number': config_number}
    return client.call('idxd_scan_accel_engine', params)
