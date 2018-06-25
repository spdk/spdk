

def set_iscsi_options(
        client,
        auth_file=None,
        node_base=None,
        nop_timeout=None,
        nop_in_interval=None,
        no_discovery_auth=None,
        req_discovery_auth=None,
        req_discovery_auth_mutual=None,
        discovery_auth_group=None,
        max_sessions=None,
        max_connections_per_session=None,
        default_time2wait=None,
        default_time2retain=None,
        immediate_data=None,
        error_recovery_level=None,
        allow_duplicated_isid=None,
        min_connections_per_session=None):
    """Set iSCSI target options.

    Args:
        auth_file: Path to CHAP shared secret file for discovery session (optional)
        node_base: Prefix of the name of iSCSI target node (optional)
        nop_timeout: Timeout in seconds to nop-in request to the initiator (optional)
        nop_in_interval: Time interval in secs between nop-in requests by the target (optional)
        no_discovery_auth: CHAP for discovery session should be disabled (optional)
        req_discovery_auth: CHAP for discovery session should be required
        req_discovery_auth_mutual: CHAP for discovery session should be mutual
        discovery_auth_group: Authentication group ID for discovery session
        max_sessions:Maximum number of sessions in the host
        max_connections_per_session:Negotiated parameter, MaxConnections
        default_time2wait: Negotiated parameter, DefaultTime2Wait
        default_time2retain: Negotiated parameter, DefaultTime2Retain
        immediate_data: Negotiated parameter, ImmediateData
        error_recovery_level: Negotiated parameter, ErrorRecoveryLevel
        allow_duplicated_isid: Allow duplicated initiator session ID
        min_connections_per_session: Allocation unit of connections per core

    Returns:
        True or False
    """
    params = {}

    if auth_file:
        params['auth_file'] = auth_file
    if node_base:
        params['node_base'] = node_base
    if nop_timeout:
        params['nop_timeout'] = nop_timeout
    if nop_in_interval:
        params['nop_in_interval'] = nop_in_interval
    if no_discovery_auth:
        params['no_discovery_auth'] = no_discovery_auth
    if req_discovery_auth:
        params['req_discovery_auth'] = req_discovery_auth
    if req_discovery_auth_mutual:
        params['req_discovery_auth_mutual'] = req_discovery_auth_mutual
    if discovery_auth_group:
        params['discovery_auth_group'] = discovery_auth_group
    if max_sessions:
        params['max_sessions'] = max_sessions
    if max_connections_per_session:
        params['max_connections_per_session'] = max_connections_per_session
    if default_time2wait:
        params['default_time2wait'] = default_time2wait
    if default_time2retain:
        params['default_time2retain'] = default_time2retain
    if immediate_data:
        params['immediate_data'] = immediate_data
    if error_recovery_level:
        params['error_recovery_level'] = error_recovery_level
    if allow_duplicated_isid:
        params['allow_duplicated_isid'] = allow_duplicated_isid
    if min_connections_per_session:
        params['min_connections_per_session'] = min_connections_per_session

    return client.call('set_iscsi_options', params)


def get_portal_groups(client):
    """Display current portal group configuration.

    Returns:
        List of current portal group configuration.
    """
    return client.call('get_portal_groups')


def get_initiator_groups(client):
    """Display current initiator group configuration.

    Returns:
        List of current initiator group configuration.
    """
    return client.call('get_initiator_groups')


def get_target_nodes(client):
    """Display target nodes.

    Returns:
        List of ISCSI target node objects.
    """
    return client.call('get_target_nodes')


def construct_target_node(
        client,
        luns,
        pg_ig_maps,
        name,
        alias_name,
        queue_depth,
        chap_group=None,
        disable_chap=None,
        require_chap=None,
        mutual_chap=None,
        header_digest=None,
        data_digest=None):
    """Add a target node.

    Args:
        luns: List of bdev_name_id_pairs, e.g. [{"bdev_name": "Malloc1", "lun_id": 1}]
        pg_ig_maps: List of pg_ig_mappings, e.g. [{"pg_tag": pg, "ig_tag": ig}]
        name: Target node name (ASCII)
        alias_name: Target node alias name (ASCII)
        queue_depth: Desired target queue depth
        chap_group: Authentication group ID for this target node
        disable_chap: CHAP authentication should be disabled for this target node
        require_chap: CHAP authentication should be required for this target node
        mutual_chap: CHAP authentication should be mutual/bidirectional
        header_digest: Header Digest should be required for this target node
        data_digest: Data Digest should be required for this target node

    Returns:
        True or False
    """
    params = {
        'name': name,
        'alias_name': alias_name,
        'pg_ig_maps': pg_ig_maps,
        'luns': luns,
        'queue_depth': queue_depth,
    }

    if chap_group:
        params['chap_group'] = chap_group
    if disable_chap:
        params['disable_chap'] = disable_chap
    if require_chap:
        params['require_chap'] = require_chap
    if mutual_chap:
        params['mutual_chap'] = mutual_chap
    if header_digest:
        params['header_digest'] = header_digest
    if data_digest:
        params['data_digest'] = data_digest
    return client.call('construct_target_node', params)


def target_node_add_lun(client, name, bdev_name, lun_id=None):
    """Add LUN to the target node.

    Args:
        name: Target node name (ASCII)
        bdev_name: bdev name
        lun_id: LUN ID (integer >= 0)

    Returns:
        True or False
    """
    params = {
        'name': name,
        'bdev_name': bdev_name,
    }
    if lun_id:
        params['lun_id'] = lun_id
    return client.call('target_node_add_lun', params)


def delete_pg_ig_maps(client, pg_ig_maps, name):
    """Delete PG-IG maps from the target node.

    Args:
        pg_ig_maps: List of pg_ig_mappings, e.g. [{"pg_tag": pg, "ig_tag": ig}]
        name: Target node alias name (ASCII)

    Returns:
        True or False
    """
    params = {
        'name': name,
        'pg_ig_maps': pg_ig_maps,
    }
    return client.call('delete_pg_ig_maps', params)


def add_pg_ig_maps(client, pg_ig_maps, name):
    """Add PG-IG maps to the target node.

    Args:
        pg_ig_maps: List of pg_ig_mappings, e.g. [{"pg_tag": pg, "ig_tag": ig}]
        name: Target node alias name (ASCII)

    Returns:
        True or False
    """
    params = {
        'name': name,
        'pg_ig_maps': pg_ig_maps,
    }
    return client.call('add_pg_ig_maps', params)


def add_portal_group(client, portals, tag):
    """Add a portal group.

    Args:
        portals: List of portals, e.g. [{'host': ip, 'port': port}] or [{'host': ip, 'port': port, 'cpumask': cpumask}]
        tag: Initiator group tag (unique, integer > 0)

    Returns:
        True or False
    """
    params = {'tag': tag, 'portals': portals}
    return client.call('add_portal_group', params)


def add_initiator_group(client, tag, initiators, netmasks):
    """Add an initiator group.

    Args:
        tag: Initiator group tag (unique, integer > 0)
        initiators: List of initiator hostnames or IP addresses, e.g. ["127.0.0.1","192.168.200.100"]
        netmasks: List of initiator netmasks, e.g. ["255.255.0.0","255.248.0.0"]

    Returns:
        True or False
    """
    params = {'tag': tag, 'initiators': initiators, 'netmasks': netmasks}
    return client.call('add_initiator_group', params)


def add_initiators_to_initiator_group(
        client,
        tag,
        initiators=None,
        netmasks=None):
    """Add initiators to an existing initiator group.

    Args:
        tag: Initiator group tag (unique, integer > 0)
        initiators: List of initiator hostnames or IP addresses, e.g. ["127.0.0.1","192.168.200.100"]
        netmasks: List of initiator netmasks, e.g. ["255.255.0.0","255.248.0.0"]

    Returns:
        True or False
    """
    params = {'tag': tag, 'initiators': initiators, 'netmasks': netmasks}
    return client.call('add_initiators_to_initiator_group', params)


def delete_initiators_from_initiator_group(
        client, tag, initiators=None, netmasks=None):
    """Delete initiators from an existing initiator group.

    Args:
        tag: Initiator group tag (unique, integer > 0)
        initiators: List of initiator hostnames or IP addresses, e.g. ["127.0.0.1","192.168.200.100"]
        netmasks: List of initiator netmasks, e.g. ["255.255.0.0","255.248.0.0"]

    Returns:
        True or False
    """
    params = {'tag': tag, 'initiators': initiators, 'netmasks': netmasks}
    return client.call('delete_initiators_from_initiator_group', params)


def delete_target_node(client, target_node_name):
    """Delete a target node.

    Args:
        target_node_name: Target node name to be deleted. Example: iqn.2016-06.io.spdk:disk1.

    Returns:
        True or False
    """
    params = {'name': target_node_name}
    return client.call('delete_target_node', params)


def delete_portal_group(client, tag):
    """Delete a portal group.

    Args:
        tag: Portal group tag (unique, integer > 0)

    Returns:
        True or False
    """
    params = {'tag': tag}
    return client.call('delete_portal_group', params)


def delete_initiator_group(client, tag):
    """Delete an initiator group.

    Args:
        tag: Initiator group tag (unique, integer > 0)

    Returns:
        True or False
    """
    params = {'tag': tag}
    return client.call('delete_initiator_group', params)


def get_iscsi_connections(client):
    """Display iSCSI connections.

    Returns:
        List of iSCSI connection.
    """
    return client.call('get_iscsi_connections')


def get_iscsi_global_params(client):
    """Display iSCSI global parameters.

    Returns:
        List of iSCSI global parameter.
    """
    return client.call('get_iscsi_global_params')


def get_scsi_devices(client):
    """Display SCSI devices.

    Returns:
        List of SCSI device.
    """
    return client.call('get_scsi_devices')
