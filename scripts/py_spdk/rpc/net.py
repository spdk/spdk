def add_ip_address(args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    return args.client.call('add_ip_address', params)


def delete_ip_address(args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    return args.client.call('delete_ip_address', params)


def get_interfaces(args):
    return args.client.call('get_interfaces')
