from client import print_dict, print_array, int_arg


def get_subsystems(args):
    print_dict(args.client.call('get_subsystems'))
