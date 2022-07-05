from .helpers import deprecated_alias


@deprecated_alias('enable_vmd')
def vmd_enable(client):
    """Enable VMD enumeration."""
    return client.call('vmd_enable')


def vmd_remove_device(client, addr):
    """Remove a device behind VMD"""
    return client.call('vmd_remove_device', {'addr': addr})
