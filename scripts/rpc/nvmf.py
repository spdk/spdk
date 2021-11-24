from .helpers import deprecated_alias
from .cmd_parser import *


@deprecated_alias('set_nvmf_target_max_subsystems')
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


@deprecated_alias('set_nvmf_target_config')
def nvmf_set_config(client,
                    conn_sched=None,
                    passthru_identify_ctrlr=None,
                    poll_groups_mask=None,
                    discovery_filter=None):
    """Set NVMe-oF target subsystem configuration.

    Args:
        conn_sched: (Deprecated) Ignored
        discovery_filter: Set discovery filter (optional), possible values are: `match_any` (default) or
         comma separated values: `transport`, `address`, `svcid`

    Returns:
        True or False
    """
    params = {}

    if conn_sched:
        print("WARNING: conn_sched is deprecated and ignored.")
    if passthru_identify_ctrlr:
        admin_cmd_passthru = {}
        admin_cmd_passthru['identify_ctrlr'] = passthru_identify_ctrlr
        params['admin_cmd_passthru'] = admin_cmd_passthru
    if poll_groups_mask:
        params['poll_groups_mask'] = poll_groups_mask
    if discovery_filter:
        params['discovery_filter'] = discovery_filter

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
        max_qpairs_per_ctrlr: Max number of SQ and CQ per controller (optional, deprecated, use max_io_qpairs_per_ctrlr)
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
        acceptor_poll_rate: Acceptor poll period in microseconds (optional)
    Returns:
        True or False
    """

    strip_globals(params)
    apply_defaults(params, no_srq=False, c2h_success=True)
    remove_null(params)

    if 'max_qpairs_per_ctrlr' in params:
        print("WARNING: max_qpairs_per_ctrlr is deprecated, please use max_io_qpairs_per_ctrlr.")

    return client.call('nvmf_create_transport', params)


@deprecated_alias('get_nvmf_transports')
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


@deprecated_alias('get_nvmf_subsystems')
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


@deprecated_alias('nvmf_subsystem_create')
def nvmf_create_subsystem(client,
                          nqn,
                          serial_number,
                          tgt_name=None,
                          model_number='SPDK bdev Controller',
                          allow_any_host=False,
                          max_namespaces=0,
                          ana_reporting=False,
                          min_cntlid=1,
                          max_cntlid=0xffef):
    """Construct an NVMe over Fabrics target subsystem.

    Args:
        nqn: Subsystem NQN.
        tgt_name: name of the parent NVMe-oF target (optional).
        serial_number: Serial number of virtual controller.
        model_number: Model number of virtual controller.
        allow_any_host: Allow any host (True) or enforce allowed host list (False). Default: False.
        max_namespaces: Maximum number of namespaces that can be attached to the subsystem (optional). Default: 0 (Unlimited).
        ana_reporting: Enable ANA reporting feature. Default: False.
        min_cntlid: Minimum controller ID. Default: 1
        max_cntlid: Maximum controller ID. Default: 0xffef


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

    Returns:
        True or False
    """

    strip_globals(params)
    apply_defaults(params, tgt_name=None)
    group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
    remove_null(params)

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


def nvmf_subsystem_add_ns(client,
                          nqn,
                          bdev_name,
                          tgt_name=None,
                          ptpl_file=None,
                          nsid=None,
                          nguid=None,
                          eui64=None,
                          uuid=None,
                          anagrpid=None):
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

    Returns:
        The namespace ID
    """
    ns = {'bdev_name': bdev_name}

    if ptpl_file:
        ns['ptpl_file'] = ptpl_file

    if nsid:
        ns['nsid'] = nsid

    if nguid:
        ns['nguid'] = nguid

    if eui64:
        ns['eui64'] = eui64

    if uuid:
        ns['uuid'] = uuid

    if anagrpid:
        ns['anagrpid'] = anagrpid

    params = {'nqn': nqn,
              'namespace': ns}

    if tgt_name:
        params['tgt_name'] = tgt_name

    return client.call('nvmf_subsystem_add_ns', params)


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


def nvmf_subsystem_add_host(client, nqn, host, tgt_name=None):
    """Add a host NQN to the list of allowed hosts.

    Args:
        nqn: Subsystem NQN.
        host: Host NQN to add to the list of allowed host NQNs
        tgt_name: name of the parent NVMe-oF target (optional).

    Returns:
        True or False
    """
    params = {'nqn': nqn,
              'host': host}

    if tgt_name:
        params['tgt_name'] = tgt_name

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


@deprecated_alias('delete_nvmf_subsystem')
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
