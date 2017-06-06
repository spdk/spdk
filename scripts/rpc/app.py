from client import print_dict, print_array, int_arg


def kill_instance(args):
    params = {'sig_name': args.sig_name}
    args.client.call('kill_instance', params, verbose=args.verbose)


def get_rpc_methods(args):
    print_dict(args.client.call('get_rpc_methods'))
