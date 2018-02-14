from client import print_dict, print_array, int_arg


def kill_instance(args):
    params = {'sig_name': args.sig_name}
    args.client.call('kill_instance', params)


def context_switch_monitor(args):
    params = {}
    if args.enable:
        params['enabled'] = True
    if args.disable:
        params['enabled'] = False
    print_dict(args.client.call('context_switch_monitor', params))
