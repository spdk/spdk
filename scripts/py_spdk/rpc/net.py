from client import print_dict, print_array, int_arg


def add_ip_address(args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    args.client.call('add_ip_address', params)


def delete_ip_address(args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    args.client.call('delete_ip_address', params)


def get_interfaces(args):
    print_dict(args.client.call('get_interfaces'))
