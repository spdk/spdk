from client import print_dict, print_array, int_arg


def get_luns(args):
    print_dict(args.client.call('get_luns', verbose=args.verbose))


def get_portal_groups(args):
    print_dict(args.client.call('get_portal_groups', verbose=args.verbose))


def get_initiator_groups(args):
    print_dict(args.client.call('get_initiator_groups', verbose=args.verbose))


def get_target_nodes(args):
    print_dict(args.client.call('get_target_nodes', verbose=args.verbose))


def construct_target_node(args):
    lun_name_id_dict = dict(u.split(":")
                            for u in args.lun_name_id_pairs.strip().split(" "))
    lun_names = lun_name_id_dict.keys()
    lun_ids = list(map(int, lun_name_id_dict.values()))

    pg_tags = []
    ig_tags = []
    for u in args.pg_ig_mappings.strip().split(" "):
        pg, ig = u.split(":")
        pg_tags.append(int(pg))
        ig_tags.append(int(ig))

    params = {
        'name': args.name,
        'alias_name': args.alias_name,
        'pg_tags': pg_tags,
        'ig_tags': ig_tags,
        'lun_names': lun_names,
        'lun_ids': lun_ids,
        'queue_depth': args.queue_depth,
        'chap_disabled': args.chap_disabled,
        'chap_required': args.chap_required,
        'chap_mutual': args.chap_mutual,
        'chap_auth_group': args.chap_auth_group,
    }
    args.client.call('construct_target_node', params, verbose=args.verbose)


def add_portal_group(args):
    # parse out portal list host1:port1 host2:port2
    portals = []
    for p in args.portal_list:
        host_port = p.split(':')
        portals.append({'host': host_port[0], 'port': host_port[1]})

    params = {'tag': args.tag, 'portals': portals}
    args.client.call('add_portal_group', params, verbose=args.verbose)


def add_initiator_group(args):
    initiators = []
    netmasks = []
    for i in args.initiator_list.strip().split(' '):
        initiators.append(i)
    for n in args.netmask_list.strip().split(' '):
        netmasks.append(n)

    params = {'tag': args.tag, 'initiators': initiators, 'netmasks': netmasks}
    args.client.call('add_initiator_group', params, verbose=args.verbose)


def delete_target_node(args):
    params = {'name': args.target_node_name}
    args.client.call('delete_target_node', params, verbose=args.verbose)


def delete_portal_group(args):
    params = {'tag': args.tag}
    args.client.call('delete_portal_group', params, verbose=args.verbose)


def delete_initiator_group(args):
    params = {'tag': args.tag}
    args.client.call('delete_initiator_group', params, verbose=args.verbose)


def get_iscsi_connections(args):
    print_dict(args.client.call('get_iscsi_connections', verbose=args.verbose))


def get_scsi_devices(args):
    print_dict(args.client.call('get_scsi_devices', verbose=args.verbose))
