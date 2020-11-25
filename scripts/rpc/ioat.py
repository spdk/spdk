from .helpers import deprecated_alias


@deprecated_alias('ioat_scan_copy_engine')
@deprecated_alias('scan_ioat_copy_engine')
def ioat_scan_accel_engine(client):
    """Enable IOAT accel engine.
    """
    return client.call('ioat_scan_accel_engine')
