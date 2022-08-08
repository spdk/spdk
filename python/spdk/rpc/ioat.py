from spdk.rpc.helpers import deprecated_alias


@deprecated_alias('ioat_scan_accel_engine')
def ioat_scan_accel_module(client):
    """Enable IOAT accel module.
    """
    return client.call('ioat_scan_accel_module')
