def add_ip_address(client, args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    return client.call('add_ip_address', params)


def delete_ip_address(client, args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    return client.call('delete_ip_address', params)


def get_interfaces(client, args):
    return client.call('get_interfaces')
