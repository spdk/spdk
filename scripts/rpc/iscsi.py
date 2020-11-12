from .helpers import deprecated_alias


@deprecated_alias('set_iscsi_options')
def iscsi_set_options(
        client,
        auth_file=None,
        node_base=None,
        nop_timeout=None,
        nop_in_interval=None,
        disable_chap=None,
        require_chap=None,
        mutual_chap=None,
        chap_group=None,
        max_sessions=None,
        max_queue_depth=None,
        max_connections_per_session=None,
        default_time2wait=None,
        default_time2retain=None,
        first_burst_length=None,
        immediate_data=None,
        error_recovery_level=None,
        allow_duplicated_isid=None,
        max_large_datain_per_connection=None,
        max_r2t_per_connection=None):
    """Set iSCSI target options.

    Args:
        auth_file: Path to CHAP shared secret file (optional)
        node_base: Prefix of the name of iSCSI target node (optional)
        nop_timeout: Timeout in seconds to nop-in request to the initiator (optional)
        nop_in_interval: Time interval in secs between nop-in requests by the target (optional)
        disable_chap: CHAP for discovery session should be disabled (optional)
        require_chap: CHAP for discovery session should be required
        mutual_chap: CHAP for discovery session should be mutual
        chap_group: Authentication group ID for discovery session
        max_sessions: Maximum number of sessions in the host
        max_queue_depth: Maximum number of outstanding I/Os per queue
        max_connections_per_session: Negotiated parameter, MaxConnections
        default_time2wait: Negotiated parameter, DefaultTime2Wait
        default_time2retain: Negotiated parameter, DefaultTime2Retain
        first_burst_length: Negotiated parameter, FirstBurstLength
        immediate_data: Negotiated parameter, ImmediateData
        error_recovery_level: Negotiated parameter, ErrorRecoveryLevel
        allow_duplicated_isid: Allow duplicated initiator session ID
        max_large_datain_per_connection: Max number of outstanding split read I/Os per connection (optional)
        max_r2t_per_connection: Max number of outstanding R2Ts per connection (optional)

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
    if disable_chap:
        params['disable_chap'] = disable_chap
    if require_chap:
        params['require_chap'] = require_chap
    if mutual_chap:
        params['mutual_chap'] = mutual_chap
    if chap_group:
        params['chap_group'] = chap_group
    if max_sessions:
        params['max_sessions'] = max_sessions
    if max_queue_depth:
        params['max_queue_depth'] = max_queue_depth
    if max_connections_per_session:
        params['max_connections_per_session'] = max_connections_per_session
    if default_time2wait:
        params['default_time2wait'] = default_time2wait
    if default_time2retain:
        params['default_time2retain'] = default_time2retain
    if first_burst_length:
        params['first_burst_length'] = first_burst_length
    if immediate_data:
        params['immediate_data'] = immediate_data
    if error_recovery_level:
        params['error_recovery_level'] = error_recovery_level
    if allow_duplicated_isid:
        params['allow_duplicated_isid'] = allow_duplicated_isid
    if max_large_datain_per_connection:
        params['max_large_datain_per_connection'] = max_large_datain_per_connection
    if max_r2t_per_connection:
        params['max_r2t_per_connection'] = max_r2t_per_connection

    return client.call('iscsi_set_options', params)


@deprecated_alias('set_iscsi_discovery_auth')
def iscsi_set_discovery_auth(
        client,
        disable_chap=None,
        require_chap=None,
        mutual_chap=None,
        chap_group=None):
    """Set CHAP authentication for discovery service.

    Args:
        disable_chap: CHAP for discovery session should be disabled (optional)
        require_chap: CHAP for discovery session should be required (optional)
        mutual_chap: CHAP for discovery session should be mutual (optional)
        chap_group: Authentication group ID for discovery session (optional)

    Returns:
        True or False
    """
    params = {}

    if disable_chap:
        params['disable_chap'] = disable_chap
    if require_chap:
        params['require_chap'] = require_chap
    if mutual_chap:
        params['mutual_chap'] = mutual_chap
    if chap_group:
        params['chap_group'] = chap_group

    return client.call('iscsi_set_discovery_auth', params)


@deprecated_alias('get_iscsi_auth_groups')
def iscsi_get_auth_groups(client):
    """Display current authentication group configuration.

    Returns:
        List of current authentication group configuration.
    """
    return client.call('iscsi_get_auth_groups')


@deprecated_alias('get_portal_groups')
def iscsi_get_portal_groups(client):
    """Display current portal group configuration.

    Returns:
        List of current portal group configuration.
    """
    return client.call('iscsi_get_portal_groups')


@deprecated_alias('get_initiator_groups')
def iscsi_get_initiator_groups(client):
    """Display current initiator group configuration.

    Returns:
        List of current initiator group configuration.
    """
    return client.call('iscsi_get_initiator_groups')


@deprecated_alias('get_target_nodes')
def iscsi_get_target_nodes(client):
    """Display target nodes.

    Returns:
        List of ISCSI target node objects.
    """
    return client.call('iscsi_get_target_nodes')


@deprecated_alias('construct_target_node')
def iscsi_create_target_node(
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
    return client.call('iscsi_create_target_node', params)


@deprecated_alias('target_node_add_lun')
def iscsi_target_node_add_lun(client, name, bdev_name, lun_id=None):
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
    return client.call('iscsi_target_node_add_lun', params)


@deprecated_alias('set_iscsi_target_node_auth')
def iscsi_target_node_set_auth(
        client,
        name,
        chap_group=None,
        disable_chap=None,
        require_chap=None,
        mutual_chap=None):
    """Set CHAP authentication for the target node.

    Args:
        name: Target node name (ASCII)
        chap_group: Authentication group ID for this target node
        disable_chap: CHAP authentication should be disabled for this target node
        require_chap: CHAP authentication should be required for this target node
        mutual_chap: CHAP authentication should be mutual/bidirectional

    Returns:
        True or False
    """
    params = {
        'name': name,
    }

    if chap_group:
        params['chap_group'] = chap_group
    if disable_chap:
        params['disable_chap'] = disable_chap
    if require_chap:
        params['require_chap'] = require_chap
    if mutual_chap:
        params['mutual_chap'] = mutual_chap
    return client.call('iscsi_target_node_set_auth', params)


@deprecated_alias('add_iscsi_auth_group')
def iscsi_create_auth_group(client, tag, secrets=None):
    """Create authentication group for CHAP authentication.

    Args:
        tag: Authentication group tag (unique, integer > 0).
        secrets: Array of secrets objects (optional).

    Returns:
        True or False
    """
    params = {'tag': tag}

    if secrets:
        params['secrets'] = secrets
    return client.call('iscsi_create_auth_group', params)


@deprecated_alias('delete_iscsi_auth_group')
def iscsi_delete_auth_group(client, tag):
    """Delete an authentication group.

    Args:
        tag: Authentication group tag (unique, integer > 0)

    Returns:
        True or False
    """
    params = {'tag': tag}
    return client.call('iscsi_delete_auth_group', params)


@deprecated_alias('add_secret_to_iscsi_auth_group')
def iscsi_auth_group_add_secret(client, tag, user, secret, muser=None, msecret=None):
    """Add a secret to an authentication group.

    Args:
        tag: Authentication group tag (unique, integer > 0)
        user: User name for one-way CHAP authentication
        secret: Secret for one-way CHAP authentication
        muser: User name for mutual CHAP authentication (optional)
        msecret: Secret for mutual CHAP authentication (optional)

    Returns:
        True or False
    """
    params = {'tag': tag, 'user': user, 'secret': secret}

    if muser:
        params['muser'] = muser
    if msecret:
        params['msecret'] = msecret
    return client.call('iscsi_auth_group_add_secret', params)


@deprecated_alias('delete_secret_from_iscsi_auth_group')
def iscsi_auth_group_remove_secret(client, tag, user):
    """Remove a secret from an authentication group.

    Args:
        tag: Authentication group tag (unique, integer > 0)
        user: User name for one-way CHAP authentication

    Returns:
        True or False
    """
    params = {'tag': tag, 'user': user}
    return client.call('iscsi_auth_group_remove_secret', params)


@deprecated_alias('delete_pg_ig_maps')
def iscsi_target_node_remove_pg_ig_maps(client, pg_ig_maps, name):
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
    return client.call('iscsi_target_node_remove_pg_ig_maps', params)


@deprecated_alias('add_pg_ig_maps')
def iscsi_target_node_add_pg_ig_maps(client, pg_ig_maps, name):
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
    return client.call('iscsi_target_node_add_pg_ig_maps', params)


def iscsi_target_node_set_redirect(client, name, pg_tag, redirect_host, redirect_port):
    """Update redirect portal of the public portal group for the target node.

    Args:
        name: Target node name (ASCII)
        pg_tag: Portal group tag (unique, integer > 0)
        redirect_host: Numeric IP address to which the target node is redirected
        redirect_port: Numeric TCP port to which the target node is redirected

    Returns:
        True or False
    """
    params = {
        'name': name,
        'pg_tag': pg_tag
    }

    if redirect_host:
        params['redirect_host'] = redirect_host
    if redirect_port:
        params['redirect_port'] = redirect_port
    return client.call('iscsi_target_node_set_redirect', params)


def iscsi_target_node_request_logout(client, name, pg_tag):
    """Request connections to the target node to logout.

    Args:
        name: Target node name (ASCII)
        pg_tag: Portal group tag (unique, integer > 0) (optional)

    Returns:
        True or False
    """
    params = {'name': name}

    if pg_tag:
        params['pg_tag'] = pg_tag
    return client.call('iscsi_target_node_request_logout', params)


@deprecated_alias('add_portal_group')
def iscsi_create_portal_group(client, portals, tag, private, wait):
    """Add a portal group.

    Args:
        portals: List of portals, e.g. [{'host': ip, 'port': port}]
        tag: Initiator group tag (unique, integer > 0)
        private: Public (false) or private (true) portal group for login redirection.
        wait: Do not listen on portals until it is allowed explictly.

    Returns:
        True or False
    """
    params = {'tag': tag, 'portals': portals}

    if private:
        params['private'] = private
    if wait:
        params['wait'] = wait
    return client.call('iscsi_create_portal_group', params)


def iscsi_start_portal_group(client, tag):
    """Start listening on portals if it is not started yet.

    Args:
        tag: Portal group tag (unique, integer > 0)

    Returns:
        True or False
    """
    params = {'tag': tag}
    return client.call('iscsi_start_portal_group', params)


@deprecated_alias('add_initiator_group')
def iscsi_create_initiator_group(client, tag, initiators, netmasks):
    """Add an initiator group.

    Args:
        tag: Initiator group tag (unique, integer > 0)
        initiators: List of initiator hostnames or IP addresses, e.g. ["127.0.0.1","192.168.200.100"]
        netmasks: List of initiator netmasks, e.g. ["255.255.0.0","255.248.0.0"]

    Returns:
        True or False
    """
    params = {'tag': tag, 'initiators': initiators, 'netmasks': netmasks}
    return client.call('iscsi_create_initiator_group', params)


@deprecated_alias('add_initiators_to_initiator_group')
def iscsi_initiator_group_add_initiators(
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
    params = {'tag': tag}

    if initiators:
        params['initiators'] = initiators
    if netmasks:
        params['netmasks'] = netmasks
    return client.call('iscsi_initiator_group_add_initiators', params)


@deprecated_alias('delete_initiators_from_initiator_group')
def iscsi_initiator_group_remove_initiators(
        client, tag, initiators=None, netmasks=None):
    """Delete initiators from an existing initiator group.

    Args:
        tag: Initiator group tag (unique, integer > 0)
        initiators: List of initiator hostnames or IP addresses, e.g. ["127.0.0.1","192.168.200.100"]
        netmasks: List of initiator netmasks, e.g. ["255.255.0.0","255.248.0.0"]

    Returns:
        True or False
    """
    params = {'tag': tag}

    if initiators:
        params['initiators'] = initiators
    if netmasks:
        params['netmasks'] = netmasks
    return client.call('iscsi_initiator_group_remove_initiators', params)


@deprecated_alias('delete_target_node')
def iscsi_delete_target_node(client, target_node_name):
    """Delete a target node.

    Args:
        target_node_name: Target node name to be deleted. Example: iqn.2016-06.io.spdk:disk1.

    Returns:
        True or False
    """
    params = {'name': target_node_name}
    return client.call('iscsi_delete_target_node', params)


@deprecated_alias('delete_portal_group')
def iscsi_delete_portal_group(client, tag):
    """Delete a portal group.

    Args:
        tag: Portal group tag (unique, integer > 0)

    Returns:
        True or False
    """
    params = {'tag': tag}
    return client.call('iscsi_delete_portal_group', params)


@deprecated_alias('delete_initiator_group')
def iscsi_delete_initiator_group(client, tag):
    """Delete an initiator group.

    Args:
        tag: Initiator group tag (unique, integer > 0)

    Returns:
        True or False
    """
    params = {'tag': tag}
    return client.call('iscsi_delete_initiator_group', params)


def iscsi_portal_group_set_auth(
        client,
        tag,
        chap_group=None,
        disable_chap=None,
        require_chap=None,
        mutual_chap=None):
    """Set CHAP authentication for discovery sessions specific for the portal group.

    Args:
        tag: Portal group tag (unique, integer > 0)
        chap_group: Authentication group ID for this portal group
        disable_chap: CHAP authentication should be disabled for this portal group
        require_chap: CHAP authentication should be required for this portal group
        mutual_chap: CHAP authentication should be mutual/bidirectional

    Returns:
        True or False
    """
    params = {
        'tag': tag,
    }

    if chap_group:
        params['chap_group'] = chap_group
    if disable_chap:
        params['disable_chap'] = disable_chap
    if require_chap:
        params['require_chap'] = require_chap
    if mutual_chap:
        params['mutual_chap'] = mutual_chap
    return client.call('iscsi_portal_group_set_auth', params)


@deprecated_alias('get_iscsi_connections')
def iscsi_get_connections(client):
    """Display iSCSI connections.

    Returns:
        List of iSCSI connection.
    """
    return client.call('iscsi_get_connections')


@deprecated_alias('get_iscsi_global_params')
def iscsi_get_options(client):
    """Display iSCSI global parameters.

    Returns:
        List of iSCSI global parameter.
    """
    return client.call('iscsi_get_options')


@deprecated_alias('get_iscsi_devices')
def scsi_get_devices(client):
    """Display SCSI devices.

    Returns:
        List of SCSI device.
    """
    return client.call('scsi_get_devices')
