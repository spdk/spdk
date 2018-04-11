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


def start(client, arg):
    return client.call('app_start', {})


def set_opts(client, args):
    params = {}
    if args.rpc_addr:
        params['rpc_addr'] = args.rpc_addr
    if args.reactor_mask:
        params['reactor_mask'] = args.reactor_mask
    if args.tpoint_group_mask:
        params['tpoint_group_mask'] = args.tpoint_group_mask
    if args.shm_id:
        params['shm_id'] = args.shm_id
    if args.enable_coredump:
        params['enable_coredump'] = args.enable_coredump
    if args.master_core:
        params['master_core'] = args.master_core
    if args.mem_channel:
        params['mem_channel'] = args.mem_channel
    if args.mem_size:
        params['mem_size'] = args.mem_size
    if args.no_pci:
        params['no_pci'] = args.no_pci
    if args.hugepage_single_segments:
        params['hugepage_single_segments'] = args.hugepage_single_segments
    return client.call('set_app_opts', params)
