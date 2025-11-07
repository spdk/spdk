#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse
import sys

from spdk.rpc.client import print_array, print_dict, print_json  # noqa  # noqa
from spdk.rpc.cmd_parser import apply_defaults, group_as, strip_globals
from spdk.rpc.helpers import DeprecateFalseAction, DeprecateTrueAction


def add_parser(subparsers):

    def nvmf_set_max_subsystems(args):
        args.client.nvmf_set_max_subsystems(max_subsystems=args.max_subsystems)

    p = subparsers.add_parser('nvmf_set_max_subsystems',
                              help='Set the maximum number of NVMf target subsystems')
    p.add_argument('-x', '--max-subsystems', help='Max number of NVMf subsystems', type=int, required=True)
    p.set_defaults(func=nvmf_set_max_subsystems)

    def nvmf_set_config(args):
        all_admin_cmd_passthru = ('identify_ctrlr', 'identify_uuid_list', 'get_log_page', 'get_set_features', 'sanitize',
                                  'security_send_recv', 'fw_update', 'nvme_mi', 'vendor_specific')
        invalid_admin_cmd_passthru = set(args.admin_cmd_passthru) - set(all_admin_cmd_passthru) - {'all'}
        if invalid_admin_cmd_passthru:
            print(f"Invalid passthru-admin-cmds: '{', '.join(invalid_admin_cmd_passthru)}'. See help for valid options.", file=sys.stderr)
            exit(1)
        if not args.admin_cmd_passthru:
            admin_cmd_passthru = None
        elif 'all' in args.admin_cmd_passthru:
            admin_cmd_passthru = {cmd: True for cmd in all_admin_cmd_passthru}
        else:
            admin_cmd_passthru = {cmd: True for cmd in args.admin_cmd_passthru}
        args.client.nvmf_set_config(admin_cmd_passthru=admin_cmd_passthru,
                                    poll_groups_mask=args.poll_groups_mask,
                                    discovery_filter=args.discovery_filter,
                                    dhchap_digests=args.dhchap_digests,
                                    dhchap_dhgroups=args.dhchap_dhgroups)

    p = subparsers.add_parser('nvmf_set_config', help='Set NVMf target config')
    p.add_argument('-p', '--passthru-admin-cmds', dest='admin_cmd_passthru', help="""Comma-separated list of admin commands to be passthru
                   when the controller has a single namespace that is an NVMe bdev.
                   Available options are: all, identify_ctrlr, identify_uuid_list, get_log_page, get_set_features, sanitize,
                   security_send_recv, fw_update, nvme_mi, vendor_specific""",
                   type=lambda d: d.split(','), default=[])
    p.add_argument('-m', '--poll-groups-mask', help='Set cpumask for NVMf poll groups (optional)', type=str)
    p.add_argument('-d', '--discovery-filter', help="""Set discovery filter (optional), possible values are: `match_any` (default) or
         comma separated values: `transport`, `address`, `svcid`""", type=str)
    p.add_argument('--dhchap-digests', help='Comma-separated list of allowed DH-HMAC-CHAP digests',
                   type=lambda d: d.split(','))
    p.add_argument('--dhchap-dhgroups', help='Comma-separated list of allowed DH-HMAC-CHAP DH groups',
                   type=lambda d: d.split(','))
    p.set_defaults(func=nvmf_set_config)

    def nvmf_create_transport(args):
        params = strip_globals(vars(args))
        params = apply_defaults(params, no_srq=False, c2h_success=True)
        args.client.nvmf_create_transport(**params)

    p = subparsers.add_parser('nvmf_create_transport', help='Create NVMf transport')
    p.add_argument('-t', '--trtype', help='Transport type (ex. RDMA)', type=str, required=True)
    p.add_argument('-g', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-q', '--max-queue-depth', help='Max number of outstanding I/O per queue', type=int)
    p.add_argument('-m', '--max-io-qpairs-per-ctrlr', help='Max number of IO qpairs per controller', type=int)
    p.add_argument('-c', '--in-capsule-data-size', help='Max number of in-capsule data size', type=int)
    p.add_argument('-i', '--max-io-size', help='Max I/O size (bytes)', type=int)
    p.add_argument('-u', '--io-unit-size', help='I/O unit size (bytes)', type=int)
    p.add_argument('-a', '--max-aq-depth', help='Max number of admin cmds per AQ', type=int)
    p.add_argument('-n', '--num-shared-buffers', help='The number of pooled data buffers available to the transport', type=int)
    p.add_argument('-b', '--buf-cache-size', help='The number of shared buffers to reserve for each poll group', type=int)
    p.add_argument('-z', '--zcopy', action='store_true', help='''Use zero-copy operations if the
    underlying bdev supports them''')
    p.add_argument('-d', '--num-cqe', help="""The number of CQ entries. Only used when no_srq=true.
    Relevant only for RDMA transport""", type=int)
    p.add_argument('-s', '--max-srq-depth', help='Max number of outstanding I/O per SRQ. Relevant only for RDMA transport', type=int)
    p.add_argument('-r', '--no-srq', action='store_true', help='Disable per-thread shared receive queue. Relevant only for RDMA transport')
    p.add_argument('-o', '--c2h-success', action='store_false', help='Disable C2H success optimization. Relevant only for TCP transport')
    p.add_argument('-f', '--dif-insert-or-strip', action='store_true', help='Enable DIF insert/strip. Relevant only for TCP transport')
    p.add_argument('-y', '--sock-priority', help='The sock priority of the tcp connection. Relevant only for TCP transport', type=int)
    p.add_argument('-l', '--acceptor-backlog', help='Pending connections allowed at one time. Relevant only for RDMA transport', type=int)
    p.add_argument('-x', '--abort-timeout-sec', help='Abort execution timeout value, in seconds', type=int)
    p.add_argument('-w', '--no-wr-batching', action='store_true', help='Disable work requests batching. Relevant only for RDMA transport')
    p.add_argument('-e', '--control-msg-num', help="""The number of control messages per poll group.
    Relevant only for TCP transport""", type=int)
    p.add_argument('-M', '--disable-mappable-bar0', action='store_true', help="""Disable mmap() of BAR0.
    Relevant only for VFIO-USER transport""")
    p.add_argument('-I', '--disable-adaptive-irq', action='store_true', help="""Disable adaptive interrupt feature.
    Relevant only for VFIO-USER transport""")
    p.add_argument('-S', '--disable-shadow-doorbells', action='store_true', help="""Disable shadow doorbell support.
    Relevant only for VFIO-USER transport""")
    p.add_argument('--acceptor-poll-rate', help='Polling interval of the acceptor for incoming connections (usec)', type=int)
    p.add_argument('--ack-timeout', help='ACK timeout in milliseconds', type=int)
    p.add_argument('--data-wr-pool-size', help='RDMA data WR pool size. Relevant only for RDMA transport', type=int)
    p.add_argument('--disable-command-passthru', help='Disallow command passthru', action='store_true')
    p.add_argument('--kas', help="Keep alive support", type=int)
    p.add_argument('--min-kato', help="The minimum keep alive timeout in milliseconds", type=int)
    p.set_defaults(func=nvmf_create_transport)

    def nvmf_get_transports(args):
        print_dict(args.client.nvmf_get_transports(trtype=args.trtype, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_transports', help='Display nvmf transports or required transport')
    p.add_argument('--trtype', help='Transport type (optional)')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_transports)

    def nvmf_get_subsystems(args):
        print_dict(args.client.nvmf_get_subsystems(nqn=args.nqn, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_subsystems', help='Display nvmf subsystems or required subsystem')
    p.add_argument('nqn', help='Subsystem NQN (optional)', nargs="?")
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_subsystems)

    def nvmf_create_subsystem(args):
        args.client.nvmf_create_subsystem(
                                       nqn=args.nqn,
                                       tgt_name=args.tgt_name,
                                       serial_number=args.serial_number,
                                       model_number=args.model_number,
                                       allow_any_host=args.allow_any_host,
                                       max_namespaces=args.max_namespaces,
                                       ana_reporting=args.ana_reporting,
                                       min_cntlid=args.min_cntlid,
                                       max_cntlid=args.max_cntlid,
                                       max_discard_size_kib=args.max_discard_size_kib,
                                       max_write_zeroes_size_kib=args.max_write_zeroes_size_kib,
                                       passthrough=args.passthrough,
                                       enable_nssr=args.enable_nssr)

    p = subparsers.add_parser('nvmf_create_subsystem', help='Create an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN (ASCII)')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument("-s", "--serial-number", help="""
    Format:  'sn' etc
    Example: 'SPDK00000000000001'""")
    p.add_argument("-d", "--model-number", help="""
    Format:  'mn' etc
    Example: 'SPDK Controller'""")
    p.add_argument("-a", "--allow-any-host", action='store_true', help="Allow any host to connect (don't enforce allowed host NQN list)")
    p.add_argument("-m", "--max-namespaces", help="Maximum number of namespaces allowed",
                   type=int)
    p.add_argument("-r", "--ana-reporting", action='store_true', help="Enable ANA reporting feature")
    p.add_argument("-i", "--min-cntlid", help="Minimum controller ID", type=int)
    p.add_argument("-I", "--max-cntlid", help="Maximum controller ID", type=int)
    p.add_argument("--max-discard-size", dest='max_discard_size_kib', help="Maximum discard size (Kib)", type=int)
    p.add_argument("--max-write-zeroes-size", dest='max_write_zeroes_size_kib', help="Maximum write_zeroes size (Kib)", type=int)
    p.add_argument("-p", "--passthrough", action='store_true', help="""Use NVMe passthrough for all I/O commands and namespace-directed
                   admin commands""")
    p.add_argument("-n", "--enable-nssr", action='store_true', help="""Enable NSSR (NVMe subsystem reset) support""")
    p.set_defaults(func=nvmf_create_subsystem)

    def nvmf_delete_subsystem(args):
        args.client.nvmf_delete_subsystem(
                                       nqn=args.nqn,
                                       tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_delete_subsystem', help='Delete a nvmf subsystem')
    p.add_argument('nqn',
                   help='subsystem nqn to be deleted. Example: nqn.2016-06.io.spdk:cnode1.')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_delete_subsystem)

    def nvmf_subsystem_add_listener(args):
        params = strip_globals(vars(args))
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('nqn') == 'discovery':
            params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        args.client.nvmf_subsystem_add_listener(**params)

    p = subparsers.add_parser('nvmf_subsystem_add_listener', help='Add a listener to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN (\'discovery\' can be used as shortcut for discovery NQN)')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.add_argument('-k', '--secure-channel', help='Immediately establish a secure channel', action="store_true")
    p.add_argument('-n', '--ana-state', help='ANA state to set: optimized, non_optimized, or inaccessible', type=str)
    p.add_argument('-S', '--sock-impl', help='The socket implementation to use for the listener (ex. posix)', type=str)
    p.set_defaults(func=nvmf_subsystem_add_listener)

    def nvmf_subsystem_remove_listener(args):
        params = strip_globals(vars(args))
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('nqn') == 'discovery':
            params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        args.client.nvmf_subsystem_remove_listener(**params)

    p = subparsers.add_parser('nvmf_subsystem_remove_listener', help='Remove a listener from an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN (\'discovery\' can be used as shortcut for discovery NQN)')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.set_defaults(func=nvmf_subsystem_remove_listener)

    def nvmf_subsystem_listener_set_ana_state(args):
        params = strip_globals(vars(args))
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        args.client.nvmf_subsystem_listener_set_ana_state(**params)

    p = subparsers.add_parser('nvmf_subsystem_listener_set_ana_state', help='Set ANA state of a listener for an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-n', '--ana-state', help='ANA state to set: optimized, non_optimized, or inaccessible', required=True)
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.add_argument('-g', '--anagrpid', help='ANA group ID (optional)', type=int)
    p.set_defaults(func=nvmf_subsystem_listener_set_ana_state)

    def nvmf_discovery_add_referral(args):
        params = strip_globals(vars(args))
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('subnqn') == 'discovery':
            params['subnqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        args.client.nvmf_discovery_add_referral(**params)

    p = subparsers.add_parser('nvmf_discovery_add_referral', help='Add a discovery service referral to an NVMe-oF target')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.add_argument('-k', '--secure-channel', help='The connection to that discovery subsystem requires a secure channel',
                   action="store_true")
    p.add_argument('-n', '--subnqn', help='Subsystem NQN')
    p.set_defaults(func=nvmf_discovery_add_referral)

    def nvmf_discovery_remove_referral(args):
        params = strip_globals(vars(args))
        params = group_as(params, 'address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('subnqn') == 'discovery':
            params['subnqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        args.client.nvmf_discovery_remove_referral(**params)

    p = subparsers.add_parser('nvmf_discovery_remove_referral', help='Remove a discovery service referral from an NVMe-oF target')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.add_argument('-n', '--subnqn', help='Subsystem NQN')
    p.set_defaults(func=nvmf_discovery_remove_referral)

    def nvmf_discovery_get_referrals(args):
        print_dict(args.client.nvmf_discovery_get_referrals(tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_discovery_get_referrals',
                              help='Display discovery subsystem referrals of an NVMe-oF target')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_discovery_get_referrals)

    def nvmf_subsystem_add_ns(args):
        params = strip_globals(vars(args))
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'namespace', ['bdev_name', 'ptpl_file', 'nsid',
                          'nguid', 'eui64', 'uuid', 'anagrpid', 'no_auto_visible', 'hide_metadata'])
        args.client.nvmf_subsystem_add_ns(**params)

    p = subparsers.add_parser('nvmf_subsystem_add_ns', help='Add a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('bdev_name', help='The name of the bdev that will back this namespace')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-p', '--ptpl-file', help='The persistent reservation storage location (optional)', type=str)
    p.add_argument('-n', '--nsid', help='The requested NSID (optional)', type=int)
    p.add_argument('-g', '--nguid', help='Namespace globally unique identifier (optional)')
    p.add_argument('-e', '--eui64', help='Namespace EUI-64 identifier (optional)')
    p.add_argument('-u', '--uuid', help='Namespace UUID (optional)')
    p.add_argument('-a', '--anagrpid', help='ANA group ID (optional)', type=int)
    p.add_argument('-i', '--no-auto-visible', action='store_true',
                   help='Do not auto make namespace visible to controllers (optional)')
    p.add_argument('-N', '--hide-metadata', action='store_true',
                   help='Enable hide_metadata option to the bdev (optional)')
    p.set_defaults(func=nvmf_subsystem_add_ns)

    def nvmf_subsystem_set_ns_ana_group(args):
        args.client.nvmf_subsystem_set_ns_ana_group(
                                                 nqn=args.nqn,
                                                 nsid=args.nsid,
                                                 anagrpid=args.anagrpid,
                                                 tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_set_ns_ana_group', help='Change ANA group ID of a namespace')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('nsid', help='The requested NSID', type=int)
    p.add_argument('anagrpid', help='ANA group ID', type=int)
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_set_ns_ana_group)

    def nvmf_subsystem_remove_ns(args):
        args.client.nvmf_subsystem_remove_ns(
                                          nqn=args.nqn,
                                          nsid=args.nsid,
                                          tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_remove_ns', help='Remove a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('nsid', help='The requested NSID', type=int)
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_remove_ns)

    def nvmf_ns_add_host(args):
        args.client.nvmf_ns_add_host(
                                    nqn=args.nqn,
                                    nsid=args.nsid,
                                    host=args.host,
                                    tgt_name=args.tgt_name)

    def nvmf_ns_visible_add_args(p):
        p.add_argument('nqn', help='NVMe-oF subsystem NQN')
        p.add_argument('nsid', help='The requested NSID', type=int)
        p.add_argument('host', help='Host NQN to make namespace visible to')
        p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)

    p = subparsers.add_parser('nvmf_ns_add_host', help='Make namespace visible to controllers of host')
    nvmf_ns_visible_add_args(p)
    p.set_defaults(func=nvmf_ns_add_host)

    def nvmf_ns_remove_host(args):
        args.client.nvmf_ns_remove_host(
                                    nqn=args.nqn,
                                    nsid=args.nsid,
                                    host=args.host,
                                    tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_ns_remove_host', help='Make namespace not visible to controllers of host')
    nvmf_ns_visible_add_args(p)
    p.set_defaults(func=nvmf_ns_remove_host)

    def nvmf_subsystem_add_host(args):
        args.client.nvmf_subsystem_add_host(
                                         nqn=args.nqn,
                                         host=args.host,
                                         tgt_name=args.tgt_name,
                                         psk=args.psk,
                                         dhchap_key=args.dhchap_key,
                                         dhchap_ctrlr_key=args.dhchap_ctrlr_key)

    p = subparsers.add_parser('nvmf_subsystem_add_host', help='Add a host to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('host', help='Host NQN to allow')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('--psk', help='Path to PSK file for TLS authentication (optional). Only applicable for TCP transport.', type=str)
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name (optional)')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name (optional)')
    p.set_defaults(func=nvmf_subsystem_add_host)

    def nvmf_subsystem_remove_host(args):
        args.client.nvmf_subsystem_remove_host(
                                            nqn=args.nqn,
                                            host=args.host,
                                            tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_remove_host', help='Remove a host from an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('host', help='Host NQN to remove')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_remove_host)

    def nvmf_subsystem_set_keys(args):
        args.client.nvmf_subsystem_set_keys(
                                         nqn=args.nqn,
                                         host=args.host,
                                         tgt_name=args.tgt_name,
                                         dhchap_key=args.dhchap_key,
                                         dhchap_ctrlr_key=args.dhchap_ctrlr_key)

    p = subparsers.add_parser('nvmf_subsystem_set_keys', help='Set keys required for a host to connect to a given subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('host', help='Host NQN')
    p.add_argument('-t', '--tgt-name', help='Name of the NVMe-oF target')
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.set_defaults(func=nvmf_subsystem_set_keys)

    def nvmf_subsystem_allow_any_host(args):
        args.client.nvmf_subsystem_allow_any_host(
                                               nqn=args.nqn,
                                               allow_any_host=args.allow_any_host,
                                               tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_allow_any_host', help='Allow any host to connect to the subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('-e', '--enable', dest='allow_any_host', action=DeprecateTrueAction, help='Enable allowing any host')
    group.add_argument('-d', '--disable', dest='allow_any_host', action=DeprecateFalseAction, help='Disable allowing any host')
    group.add_argument('--allow-any-host', action=argparse.BooleanOptionalAction, help='Enable or disable allowing any host')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_allow_any_host)

    def nvmf_subsystem_get_controllers(args):
        print_dict(args.client.nvmf_subsystem_get_controllers(
                                                           nqn=args.nqn,
                                                           tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_controllers',
                              help='Display controllers of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_get_controllers)

    def nvmf_subsystem_get_qpairs(args):
        print_dict(args.client.nvmf_subsystem_get_qpairs(
                                                      nqn=args.nqn,
                                                      tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_qpairs',
                              help='Display queue pairs of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_get_qpairs)

    def nvmf_subsystem_get_listeners(args):
        print_dict(args.client.nvmf_subsystem_get_listeners(
                                                         nqn=args.nqn,
                                                         tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_listeners',
                              help='Display listeners of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_get_listeners)

    def nvmf_get_stats(args):
        print_dict(args.client.nvmf_get_stats(tgt_name=args.tgt_name))

    p = subparsers.add_parser(
        'nvmf_get_stats', help='Display current statistics for NVMf subsystem')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_stats)

    def nvmf_set_crdt(args):
        print_dict(args.client.nvmf_set_crdt(crdt1=args.crdt1, crdt2=args.crdt2, crdt3=args.crdt3))

    p = subparsers.add_parser(
        'nvmf_set_crdt',
        help="""Set the 3 crdt (Command Retry Delay Time) values for NVMf subsystem. All
        values are in units of 100 milliseconds (same as the NVM Express specification).""")
    p.add_argument('-t1', '--crdt1', help='Command Retry Delay Time 1, in units of 100 milliseconds', type=int)
    p.add_argument('-t2', '--crdt2', help='Command Retry Delay Time 2, in units of 100 milliseconds', type=int)
    p.add_argument('-t3', '--crdt3', help='Command Retry Delay Time 3, in units of 100 milliseconds', type=int)
    p.set_defaults(func=nvmf_set_crdt)

    def nvmf_publish_mdns_prr(args):
        args.client.nvmf_publish_mdns_prr(tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_publish_mdns_prr',
                              help='Publish pull registration request through mdns')
    p.add_argument('-t', '--tgt-name', help='The name of the NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_publish_mdns_prr)

    def nvmf_stop_mdns_prr(args):
        args.client.nvmf_stop_mdns_prr(tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_stop_mdns_prr',
                              help='Stop publishing pull registration request through mdns')
    p.add_argument('-t', '--tgt-name', help='The name of the NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_stop_mdns_prr)
