def kill_instance(client, args):
    params = {'sig_name': args.sig_name}
    return client.call('kill_instance', params)


def context_switch_monitor(client, args):
    params = {}
    if args.enable:
        params['enabled'] = True
    if args.disable:
        params['enabled'] = False
    return client.call('context_switch_monitor', params)
