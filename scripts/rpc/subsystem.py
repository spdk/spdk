from client import print_dict, print_array, int_arg


def get_subsystems(args):
    params = {'no_config' : False}
    if args.no_config:
        params['no_config'] = args.no_config
    if args.subsystems:
        params['subsystems'] = args.subsystems.split(',')

    print_dict(args.client.call('get_subsystems', params))
