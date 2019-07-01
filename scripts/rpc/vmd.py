def enable_vmd(client, enable):
    """Enable VMD enumeration.

    Args:
        enable: True or False
    """
    params = {"enable": enable}
    return client.call('enable_vmd', params)
