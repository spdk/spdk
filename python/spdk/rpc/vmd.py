from .helpers import deprecated_alias


@deprecated_alias('enable_vmd')
def vmd_enable(client):
    """Enable VMD enumeration."""
    return client.call('vmd_enable')
