def get_subsystems(client):
    return client.call('get_subsystems')


def get_subsystem_config(client, name):
    params = {'name': name}
    return client.call('get_subsystem_config', params)


def initialize_iscsi_subsystem(client, args):
    params = {}

    if args.auth_file:
        params['auth_file'] = args.auth_file
    if args.node_base:
        params['node_base'] = args.node_base
    if args.nop_timeout:
        params['nop_timeout'] = args.nop_timeout
    if args.nop_in_interval:
        params['nop_in_interval'] = args.nop_in_interval
    if args.no_discovery_auth:
        params['no_discovery_auth'] = args.no_discovery_auth
    if args.req_discovery_auth:
        params['req_discovery_auth'] = args.req_discovery_auth
    if args.req_discovery_auth_mutual:
        params['req_discovery_auth_mutual'] = args.req_discovery_auth_mutual
    if args.discovery_auth_group:
        params['discovery_auth_group'] = args.discovery_auth_group
    if args.max_sessions:
        params['max_sessions'] = args.max_sessions
    if args.max_connections_per_session:
        params['max_connections_per_session'] = args.max_connections_per_session
    if args.default_time2wait:
        params['default_time2wait'] = args.default_time2wait
    if args.default_time2retain:
        params['default_time2retain'] = args.default_time2retain
    if args.immediate_data:
        params['immediate_data'] = args.immediate_data
    if args.error_recovery_level:
        params['error_recovery_level'] = args.error_recovery_level
    if args.allow_duplicated_isid:
        params['allow_duplicated_isid'] = args.allow_duplicated_isid
    if args.min_connections_per_session:
        params['min_connections_per_session'] = args.min_connections_per_session
    return client.call('initialize_iscsi_subsystem', params)


def initialize_nvmf_subsystem(client, args):
    params = {}

    if args.max_queue_depth:
        params['max_queue_depth'] = args.max_queue_depth
    if args.max_qpairs_per_session:
        params['max_qpairs_per_session'] = args.max_qpairs_per_session
    if args.in_capsule_data_size:
        params['in_capsule_data_size'] = args.in_capsule_data_size
    if args.max_io_size:
        params['max_io_size'] = args.max_io_size
    if args.acceptor_poll_rate:
        params['acceptor_poll_rate'] = args.acceptor_poll_rate
    return client.call('initialize_nvmf_subsystem', params)
