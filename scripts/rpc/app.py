def kill_instance(args):
    params = {'sig_name': args.sig_name}
    return args.client.call('kill_instance', params)


def context_switch_monitor(args):
    params = {}
    if args.enable:
        params['enabled'] = True
    if args.disable:
        params['enabled'] = False
    return args.client.call('context_switch_monitor', params)
