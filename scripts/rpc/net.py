def add_ip_address(client, ifc_index, ip_addr):
    """Add IP address.

    Args:
        ifc_index: ifc index of the nic device (int)
        ip_addr: ip address will be added
    """
    params = {'ifc_index': ifc_index, 'ip_address': ip_addr}
    return client.call('add_ip_address', params)


def delete_ip_address(client, ifc_index, ip_addr):
    """Delete IP address.

    Args:
        ifc_index: ifc index of the nic device (int)
        ip_addr: ip address will be deleted
    """
    params = {'ifc_index': ifc_index, 'ip_address': ip_addr}
    return client.call('delete_ip_address', params)


def get_interfaces(client):
    """Display current interface list

    Returns:
        List of current interface
    """
    return client.call('get_interfaces')
