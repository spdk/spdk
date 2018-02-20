from client import print_dict, print_array, int_arg


def get_subsystem_dependency(args):
    params = {}
    if args.subsystem:
        params['subsystem'] = args.subsystem
    print_dict(args.client.call('get_subsystem_dependency', params))
