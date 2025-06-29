#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.

from .cmd_parser import *


def nvmf_set_max_subsystems(client,
                            max_subsystems=None):
    """Set NVMe-oF target options.

    Args:
        max_subsystems: Maximum number of NVMe-oF subsystems (e.g. 1024)

    Returns:
        True or False
    """
    params = {}

    params['max_subsystems'] = max_subsystems
    return client.call('nvmf_set_max_subsystems', params)


def nvmf_set_config(client,
                    passthru_identify_ctrlr=None,
                    poll_groups_mask=None,
                    discovery_filter=None, dhchap_digests=None, dhchap_dhgroups=None):
    """Set NVMe-oF target subsystem configuration.

    Args:
        discovery_filter: Set discovery filter (optional), possible values are: `match_any` (default) or
         comma separated values: `transport`, `address`, `svcid`
        dhchap_digests: List of allowed DH-HMAC-CHAP digests. (optional)
        dhchap_dhgroups: List of allowed DH-HMAC-CHAP DH groups. (optional)
    Returns:
        True or False
    """
    params = {}

    if passthru_identify_ctrlr:
        admin_cmd_passthru = {}
        admin_cmd_passthru['identify_ctrlr'] = passthru_identify_ctrlr
        params['admin_cmd_passthru'] = admin_cmd_passthru
    if poll_groups_mask:
        params['poll_groups_mask'] = poll_groups_mask
    if discovery_filter:
        params['discovery_filter'] = discovery_filter
    if dhchap_digests is not None:
        params['dhchap_digests'] = dhchap_digests
    if dhchap_dhgroups is not None:
        params['dhchap_dhgroups'] = dhchap_dhgroups

    return client.call('nvmf_set_config', params)


def nvmf_create_target(client,
                       name,
                       max_subsystems=0,
                       discovery_filter="match_any"):
    """Create a new NVMe-oF Target.

    Args:
        name: Must be unique within the application
        max_subsystems: Maximum number of NVMe-oF subsystems (e.g. 1024). default: 0 (Uses SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS).
        discovery_filter: Set discovery filter (optional), possible values are: `match_any` (default) or
         comma separated values: `transport`, `address`, `svcid`

    Returns:
        The name of the new target.
    """
    params = {}

    params['name'] = name
    params['max_subsystems'] = max_subsystems
    params['discovery_filter'] = discovery_filter
    return client.call("nvmf_create_target", params)


def nvmf_delete_target(client,
                       name):
    """Destroy an NVMe-oF Target.

    Args:
        name: The name of the target you wish to destroy

    Returns:
        True on success or False
    """
    params = {}

    params['name'] = name
    return client.call("nvmf_delete_target", params)


def nvmf_get_targets(client):
    """Get a list of all the NVMe-oF targets in this application

    Returns:
        An array of target names.
    """

    return client.call("nvmf_get_targets")


def nvmf_create_transport(client, **params):
    """NVMf Transport Create options.

    Args:
        trtype: Transport type (ex. RDMA)
        max_queue_depth: Max number of outstanding I/O per queue (optional)
        max_io_qpairs_per_ctrlr: Max number of IO qpairs per controller (optional)
        in_capsule_data_size: Maximum in-capsule data size in bytes (optional)
        max_io_size: Maximum I/O data size in bytes (optional)
        io_unit_size: I/O unit size in bytes (optional)
        max_aq_depth: Max size admin queue per controller (optional)
        num_shared_buffers: The number of pooled data buffers available to the transport (optional)
        buf_cache_size: The number of shared buffers to reserve for each poll group (optional)
        zcopy: Use zero-copy operations if the underlying bdev supports them (optional)
        num_cqe: The number of CQ entries to configure CQ size. Only used when no_srq=true - RDMA specific (optional)
        max_srq_depth: Max number of outstanding I/O per shared receive queue - RDMA specific (optional)
        no_srq: Boolean flag to disable SRQ even for devices that support it - RDMA specific (optional)
        c2h_success: Boolean flag to disable the C2H success optimization - TCP specific (optional)
        dif_insert_or_strip: Boolean flag to enable DIF insert/strip for I/O - TCP specific (optional)
        acceptor_backlog: Pending connections allowed at one time - RDMA specific (optional)
        abort_timeout_sec: Abort execution timeout value, in seconds (optional)
        no_wr_batching: Boolean flag to disable work requests batching - RDMA specific (optional)
        control_msg_num: The number of control messages per poll group - TCP specific (optional)
        disable_mappable_bar0: disable client mmap() of BAR0 - VFIO-USER specific (optional)
        disable_adaptive_irq: Disable adaptive interrupt feature - VFIO-USER specific (optional)
        disable_shadow_doorbells: disable shadow doorbell support - VFIO-USER specific (optional)
        acceptor_poll_rate: Acceptor poll period in microseconds (optional)
        ack_timeout: ACK timeout in milliseconds (optional)
        data_wr_pool_size: RDMA data WR pool size. RDMA specific (optional)
        disable_command_passthru: Disallow command passthru.
        kas: The granularity of the KATO (Keep Alive Timeout) in 100 millisecond units (optional)
        min_kato: The minimum keep alive timeout value in milliseconds (optional)
    Returns:
        True or False
    """

    strip_globals(params)
    apply_defaults(params, no_srq=False, c2h_success=True)
    remove_null(params)

    return client.call('nvmf_create_transport', params)


def nvmf_get_transports(client, trtype=None, tgt_name=None):
    """Get list of NVMe-oF transports.
    Args:
        trtype: Transport type (optional; if omitted, query all transports).
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of NVMe-oF transport objects.
    """

    params = {}

    if tgt_name:
        params['tgt_name'] = tgt_name

    if trtype:
        params['trtype'] = trtype

    return client.call('nvmf_get_transports', params)


def nvmf_get_subsystems(client, nqn=None, tgt_name=None):
    """Get list of NVMe-oF subsystems.
    Args:
        nqn: Subsystem NQN (optional; if omitted, query all subsystems).
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of NVMe-oF subsystem objects.
    """

    params = {}

    if tgt_name:
        params['tgt_name'] = tgt_name

    if nqn:
        params['nqn'] = nqn

    return client.call('nvmf_get_subsystems', params)


def nvmf_create_subsystem(client,
                          nqn,
                          serial_number,
                          tgt_name=None,
                          model_number='SPDK bdev Controller',
                          allow_any_host=False,
                          max_namespaces=0,
                          ana_reporting=False,
                          min_cntlid=1,
                          max_cntlid=0xffef,
                          max_discard_size_kib=0,
                          max_write_zeroes_size_kib=0,
                          passthrough=False):
    """Construct an NVMe over Fabrics target subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: Parent NVMe-oF target name. (Optional)
        serial_number: Serial number of virtual controller. (Optional)
        model_number: Model number of virtual controller. (Optional)
        max_namespaces: Maximum number of namespaces that can be attached to the subsystem. Default: 32 (also used if user specifies 0)
        allow_any_host: Allow any host (`true`) or enforce allowed host list (`false`). Default: `false`
        ana_reporting: Enable ANA reporting feature. Default: `false`
        min_cntlid: Minimum controller ID. Default: 1
        max_cntlid: Maximum controller ID. Default: 0xffef
        max_discard_size_kib: Maximum discard size (Kib). Default: 0
        max_write_zeroes_size_kib: Maximum write_zeroes size (Kib). Default: 0
        passthrough: Use NVMe passthrough for I/O commands and namespace-directed admin commands. Default: `false`


    Returns:
        True or False
    """
    params = {
        'nqn': nqn,
    }

    if serial_number:
        params['serial_number'] = serial_number

    if model_number:
        params['model_number'] = model_number

    if allow_any_host:
        params['allow_any_host'] = True

    if max_namespaces is not None:
        params['max_namespaces'] = max_namespaces

    if tgt_name:
        params['tgt_name'] = tgt_name

    if ana_reporting:
        params['ana_reporting'] = ana_reporting

    if min_cntlid is not None:
        params['min_cntlid'] = min_cntlid

    if max_cntlid is not None:
        params['max_cntlid'] = max_cntlid

    if max_discard_size_kib is not None:
        params['max_discard_size_kib'] = max_discard_size_kib

    if max_write_zeroes_size_kib is not None:
        params['max_write_zeroes_size_kib'] = max_write_zeroes_size_kib

    if passthrough:
        params['passthrough'] = passthrough

    return client.call('nvmf_create_subsystem', params)


def nvmf_subsystem_add_listener(client, **params):

    """Add a new listen address to an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID (required for RDMA or TCP).
        tgt_name: name of the parent NVMe-oF target (optional).
        adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").
        sock_impl: The socket implementation to use for the listener (optional).

    Returns:
        True or False
    """

    strip_globals(params)
    apply_defaults(params, tgt_name=None)
    group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
    remove_null(params)

    if params['nqn'] == 'discovery':
        params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'

    return client.call('nvmf_subsystem_add_listener', params)


def nvmf_subsystem_remove_listener(
        client,
        nqn,
        trtype,
        traddr,
        trsvcid,
        adrfam,
        tgt_name=None):
    """Remove existing listen address from an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        tgt_name: name of the parent NVMe-oF target (optional).
        adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").

    Returns:
            True or False
    """
    listen_address = {'trtype': trtype,
                      'traddr': traddr}

    if trsvcid:
        listen_address['trsvcid'] = trsvcid

    if adrfam:
        listen_address['adrfam'] = adrfam

    params = {'nqn': nqn,
              'listen_address': listen_address}

    if tgt_name:
        params['tgt_name'] = tgt_name

    if params['nqn'] == 'discovery':
        params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'

    return client.call('nvmf_subsystem_remove_listener', params)


def nvmf_subsystem_listener_set_ana_state(
        client,
        nqn,
        ana_state,
        trtype,
        traddr,
        trsvcid,
        adrfam,
        tgt_name=None,
        anagrpid=None):
    """Set ANA state of a listener for an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        ana_state: ANA state to set ("optimized", "non_optimized", or "inaccessible").
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        tgt_name: name of the parent NVMe-oF target (optional).
        adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").
        anagrpid: ANA group ID (optional)

    Returns:
            True or False
    """
    listen_address = {'trtype': trtype,
                      'traddr': traddr,
                      'trsvcid': trsvcid}

    if adrfam:
        listen_address['adrfam'] = adrfam

    params = {'nqn': nqn,
              'listen_address': listen_address,
              'ana_state': ana_state}

    if tgt_name:
        params['tgt_name'] = tgt_name

    if anagrpid:
        params['anagrpid'] = anagrpid

    return client.call('nvmf_subsystem_listener_set_ana_state', params)


def nvmf_discovery_add_referral(client, **params):

    """Add a discovery service referral

    Args:
        tgt_name: name of the parent NVMe-oF target (optional).
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID (required for RDMA or TCP).
        adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").
        secure_channel: The connection to that discovery
            subsystem requires a secure channel (optional).
        subnqn: Subsystem NQN.

    Returns:
        True or False
    """

    strip_globals(params)
    apply_defaults(params, tgt_name=None)
    group_as(params, 'address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
    remove_null(params)

    if params.get('subnqn') == 'discovery':
        params['subnqn'] = 'nqn.2014-08.org.nvmexpress.discovery'

    return client.call('nvmf_discovery_add_referral', params)


def nvmf_discovery_remove_referral(
        client,
        trtype,
        traddr,
        trsvcid,
        adrfam,
        tgt_name=None,
        subnqn=None):
    """Remove a discovery service referral

    Args:
        trtype: Transport type ("RDMA").
        traddr: Transport address.
        trsvcid: Transport service ID.
        adrfam: Address family ("IPv4", "IPv6", "IB", or "FC").
        tgt_name: name of the parent NVMe-oF target (optional).
        subnqn: Subsystem NQN.

    Returns:
            True or False
    """
    address = {'trtype': trtype,
               'traddr': traddr}

    if trsvcid:
        address['trsvcid'] = trsvcid

    if adrfam:
        address['adrfam'] = adrfam

    params = {'address': address}

    if tgt_name:
        params['tgt_name'] = tgt_name
    if subnqn is not None:
        if subnqn == 'discovery':
            subnqn = 'nqn.2014-08.org.nvmexpress.discovery'
        params['subnqn'] = subnqn

    return client.call('nvmf_discovery_remove_referral', params)


def nvmf_discovery_get_referrals(client, tgt_name=None):
    """Get list of referrals of an NVMe-oF target.

    Args:
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of referral objects of an NVMe-oF target.
    """
    params = {}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_discovery_get_referrals', params)


def nvmf_subsystem_add_ns(client, **params):
    """Add a namespace to a subsystem.

    Args:
        nqn: Subsystem NQN.
        bdev_name: Name of bdev to expose as a namespace.
        tgt_name: name of the parent NVMe-oF target (optional).
        nsid: Namespace ID (optional).
        nguid: 16-byte namespace globally unique identifier in hexadecimal (optional).
        eui64: 8-byte namespace EUI-64 in hexadecimal (e.g. "ABCDEF0123456789") (optional).
        uuid: Namespace UUID (optional).
        anagrpid: ANA group ID (optional).
        no_auto_visible: Do not automatically make namespace visible to controllers (optional)
        hide_metadata: Enable hide_metadata option to the bdev (optional)

    Returns:
        The namespace ID
    """

    strip_globals(params)
    apply_defaults(params, tgt_name=None)
    group_as(params, 'namespace', ['bdev_name', 'ptpl_file', 'nsid',
                                   'nguid', 'eui64', 'uuid', 'anagrpid', 'no_auto_visible',
                                   'hide_metadata'])
    remove_null(params)

    return client.call('nvmf_subsystem_add_ns', params)


def nvmf_subsystem_set_ns_ana_group(client, nqn, nsid, anagrpid, tgt_name=None):
    """Change ANA group ID of a namespace.

    Args:
        nqn: Subsystem NQN.
        nsid: Namespace ID.
        anagrpid: ANA group ID.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'nsid': nsid,
              'anagrpid': anagrpid}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_set_ns_ana_group', params)


def nvmf_subsystem_set_ns_visibility(client, nqn, nsid, auto_visible, tgt_name=None):
    """Change visibility of a namespace.

    Args:
        nqn: Subsystem NQN.
        nsid: Namespace ID.
        auto_visible: visibility
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'nsid': nsid,
              'auto_visible': auto_visible}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_set_ns_visibility', params)


def nvmf_subsystem_remove_ns(client, nqn, nsid, tgt_name=None):
    """Remove a existing namespace from a subsystem.

    Args:
        nqn: Subsystem NQN.
        nsid: Namespace ID.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'nsid': nsid}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_remove_ns', params)


def nvmf_ns_visible(visible, client, nqn, nsid, host, tgt_name=None):
    """Set visibility of namespace for a host's controllers

    Args:
        nqn: Subsystem NQN.
        nsid: Namespace ID.
        host: Host NQN to set visibility
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'nsid': nsid,
              'host': host}

    if tgt_name:
        params['tgt_name'] = tgt_name

    if visible:
        return client.call('nvmf_ns_add_host', params)
    else:
        return client.call('nvmf_ns_remove_host', params)


def nvmf_subsystem_add_host(client, nqn, host, tgt_name=None, psk=None, dhchap_key=None,
                            dhchap_ctrlr_key=None):
    """Add a host NQN to the list of allowed hosts.

    Args:
        nqn: Subsystem NQN.
        host: Host NQN to add to the list of allowed host NQNs
        tgt_name: name of the parent NVMe-oF target (optional)
        psk: PSK file path for TLS (optional)
        dhchap_key: DH-HMAC-CHAP key name (optional)
        dhchap_ctrlr_key: DH-HMAC-CHAP controller key name (optional)

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'host': host}

    if tgt_name:
        params['tgt_name'] = tgt_name
    if psk:
        params['psk'] = psk
    if dhchap_key is not None:
        params['dhchap_key'] = dhchap_key
    if dhchap_ctrlr_key is not None:
        params['dhchap_ctrlr_key'] = dhchap_ctrlr_key

    return client.call('nvmf_subsystem_add_host', params)


def nvmf_subsystem_remove_host(client, nqn, host, tgt_name=None):
    """Remove a host NQN from the list of allowed hosts.

    Args:
        nqn: Subsystem NQN.
        host: Host NQN to remove to the list of allowed host NQNs
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'host': host}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_remove_host', params)


def nvmf_subsystem_set_keys(client, nqn, host, tgt_name=None,
                            dhchap_key=None, dhchap_ctrlr_key=None):
    """Set keys required for a host to connect to a given subsystem.

    Args:
        nqn: Subsystem NQN.
        host: Host NQN.
        tgt_name: Name of the NVMe-oF target (optional).
        dhchap_key: DH-HMAC-CHAP key name (optional)
        dhchap_ctrlr_key: DH-HMAC-CHAP controller key name (optional)
    """

    params = {'nqn': nqn,
              'host': host}

    if tgt_name is not None:
        params['tgt_name'] = tgt_name
    if dhchap_key is not None:
        params['dhchap_key'] = dhchap_key
    if dhchap_ctrlr_key is not None:
        params['dhchap_ctrlr_key'] = dhchap_ctrlr_key

    return client.call('nvmf_subsystem_set_keys', params)


def nvmf_subsystem_allow_any_host(client, nqn, disable, tgt_name=None):
    """Configure a subsystem to allow any host to connect or to enforce the host NQN list.

    Args:
        nqn: Subsystem NQN.
        disable: Allow any host (true) or enforce allowed host list (false).
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn, 'allow_any_host': False if disable else True}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_allow_any_host', params)


def nvmf_delete_subsystem(client, nqn, tgt_name=None):
    """Delete an existing NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_delete_subsystem', params)


def nvmf_subsystem_get_controllers(client, nqn, tgt_name=None):
    """Get list of controllers of an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of controller objects of an NVMe-oF subsystem.
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_get_controllers', params)


def nvmf_subsystem_get_qpairs(client, nqn, tgt_name=None):
    """Get list of queue pairs of an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of queue pair objects of an NVMe-oF subsystem.
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_get_qpairs', params)


def nvmf_subsystem_get_listeners(client, nqn, tgt_name=None):
    """Get list of listeners of an NVMe-oF subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        List of listener objects of an NVMe-oF subsystem.
    """
    params = {'nqn': nqn}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_get_listeners', params)


def nvmf_get_stats(client, tgt_name=None):
    """Query NVMf statistics.

    Args:
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        Current NVMf statistics.
    """

    params = {}

    if tgt_name:
        params = {
            'tgt_name': tgt_name,
        }

    return client.call('nvmf_get_stats', params)


def nvmf_set_crdt(client, crdt1=None, crdt2=None, crdt3=None):
    """Set the 3 crdt (Command Retry Delay Time) values

    Args:
        crdt1: Command Retry Delay Time 1
        crdt2: Command Retry Delay Time 2
        crdt3: Command Retry Delay Time 3

    Returns:
        True or False
    """
    params = {}
    if crdt1 is not None:
        params['crdt1'] = crdt1
    if crdt2 is not None:
        params['crdt2'] = crdt2
    if crdt3 is not None:
        params['crdt3'] = crdt3

    return client.call('nvmf_set_crdt', params)


def nvmf_publish_mdns_prr(client, tgt_name=None):
    """Publish mdns pull registration request

    Args:
        tgt_name: name of the NVMe-oF target (optional).

    Returns:
        Success or Fail
    """
    params = {}
    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_publish_mdns_prr', params)


def nvmf_stop_mdns_prr(client, tgt_name=None):
    """Stop publishing mdns pull registration request

    Args:
        tgt_name: name of the NVMe-oF target (optional).

    Returns:
        Success or Fail
    """
    params = {}
    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_stop_mdns_prr', params)
