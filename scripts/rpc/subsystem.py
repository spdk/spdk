def get_subsystems(client):
    return client.call('get_subsystems')


def get_subsystem_config(client, name):
    params = {'name': name}
    return client.call('get_subsystem_config', params)


def initialize_iscsi_subsystem(client, args):
    params = {
        'auth_file': args.auth_file,
        'node_base': args.node_base,
        'nop_timeout': args.nop_timeout,
        'nop_in_interval': args.nop_in_interval,
        'max_sessions': args.max_sessions,
        'max_connections_per_session': args.max_connections_per_session,
        'default_time2wait': args.default_time2wait,
        'default_time2retain': args.default_time2retain,
        'error_recovery_level': args.error_recovery_level,
        'min_connections_per_session': args.min_connections_per_session,
    }

    if args.no_discovery_auth:
        params['no_discovery_auth'] = args.no_discovery_auth
    if args.req_discovery_auth:
        params['req_discovery_auth'] = args.req_discovery_auth
    if args.req_discovery_auth_mutual:
        params['req_discovery_auth_mutual'] = args.req_discovery_auth_mutual
    if args.discovery_auth_group:
        params['discovery_auth_group'] = args.discovery_auth_group
    if args.immediate_data:
        params['immediate_data'] = args.immediate_data
    if args.allow_duplicated_isid:
        params['allow_duplicated_isid'] = args.allow_duplicated_isid
    return client.call('initialize_iscsi_subsystem', params)


def initialize_bdev_subsystem(client):
    return client.call('initialize_bdev_subsystem')


def initialize_scsi_subsystem(client):
    return client.call('initialize_scsi_subsystem')


def initialize_nbd_subsystem(client):
    return client.call('initialize_nbd_subsystem')


def initialize_interface_subsystem(client):
    return client.call('initialize_interface_subsystem')


def initialize_net_subsystem(client):
    return client.call('initialize_net_subsystem')
