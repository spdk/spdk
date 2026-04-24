#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#  Copyright (c) 2026, Oracle and/or its affiliates.
#

import argparse
import sys
from functools import partial

from spdk.rpc.cmd_parser import apply_defaults, group_as, print_dict, strip_globals


def add_parser(subparsers):

    def nvmf_set_max_subsystems(args):
        args.client.nvmf_set_max_subsystems(max_subsystems=args.max_subsystems)

    p = subparsers.add_parser('nvmf_set_max_subsystems',
                              help='Set the maximum number of NVMf target subsystems')
    p.add_argument('-x', '--max-subsystems', help='Maximum number of NVMe-oF subsystems', type=int, required=True)
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
                                    discovery_filters=args.discovery_filters,
                                    dhchap_digests=args.dhchap_digests,
                                    dhchap_dhgroups=args.dhchap_dhgroups,
                                    dup_host_policy=args.dup_host_policy)

    p = subparsers.add_parser('nvmf_set_config', help='Set NVMf target config')
    p.add_argument('-p', '--passthru-admin-cmds', dest='admin_cmd_passthru',
                   help='Comma-separated list of admin commands to pass through when the subsystem has a single NVMe-backed namespace.'
                        ' Options: all, identify_ctrlr, identify_uuid_list, get_log_page, get_set_features,'
                        ' sanitize, security_send_recv, fw_update, nvme_mi, vendor_specific',
                   type=partial(str.split, sep=','), default=[])
    p.add_argument('-m', '--poll-groups-mask', help='CPU mask for NVMe-oF poll groups', type=str)
    group = p.add_mutually_exclusive_group()
    group.add_argument('-d', '--discovery-filter', help='Deprecated, use --discovery-filters instead', type=str)
    group.add_argument('--discovery-filters', help='Comma-separated list of discovery filters', type=partial(str.split, sep=','))
    p.add_argument('--dhchap-digests', help='Comma-separated list of allowed DH-HMAC-CHAP digests',
                   type=partial(str.split, sep=','))
    p.add_argument('--dhchap-dhgroups', help='Comma-separated list of allowed DH-HMAC-CHAP DH groups',
                   type=partial(str.split, sep=','))
    p.add_argument('--dup-host-policy', choices=['allow', 'restrict_per_listener'],
                   help='Duplicate host policy')
    p.set_defaults(func=nvmf_set_config)

    oncs = ('nvmcmps', 'nvmdsmsv', 'nvmwzsv', 'reservs', 'nvmcpys')
    help_oncs = ", ".join(("all", *oncs))

    fuses = ('fcws',)
    help_fuses = ", ".join(("all", *fuses))

    def nvmf_create_transport(args):
        params = strip_globals(vars(args))
        params = apply_defaults(params, no_srq=False, c2h_success=True)
        if args.masked_oncs:
            invalid_oncs = set(args.masked_oncs) - set(oncs) - {'all'}
            if invalid_oncs:
                print(f"Invalid oncs: '{', '.join(invalid_oncs)}'. Available options: {help_oncs}.", file=sys.stderr)
                exit(1)
            params['masked_oncs'] = oncs if 'all' in args.masked_oncs else args.masked_oncs
        if args.masked_fuses:
            invalid_fuses = set(args.masked_fuses) - set(fuses) - {'all'}
            if invalid_fuses:
                print(f"Invalid fuses: '{', '.join(invalid_fuses)}'. Available options: {help_fuses}.", file=sys.stderr)
                exit(1)
            params['masked_fuses'] = fuses if 'all' in args.masked_fuses else args.masked_fuses
        args.client.nvmf_create_transport(**params)

    p = subparsers.add_parser('nvmf_create_transport', help='Create NVMf transport')
    p.add_argument('-t', '--trtype', help='Transport type (ex. RDMA)', type=str, required=True)
    p.add_argument('-g', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('-q', '--max-queue-depth', help='Max number of outstanding I/O per queue', type=int)
    p.add_argument('-m', '--max-io-qpairs-per-ctrlr', help='Max number of IO qpairs per controller', type=int)
    p.add_argument('-c', '--in-capsule-data-size', help='Max number of in-capsule data size', type=int)
    p.add_argument('-i', '--max-io-size', help='Max I/O size (bytes)', type=int)
    p.add_argument('-u', '--io-unit-size', help='I/O unit size (bytes). Deprecated, use iobuf_set_options instead.', type=int)
    p.add_argument('-a', '--max-aq-depth', help='Max number of admin cmds per AQ', type=int)
    p.add_argument('-n', '--num-shared-buffers',
                   help='Number of pooled data buffers available to the transport. Deprecated, use iobuf_set_options instead',
                   type=int)
    group = p.add_mutually_exclusive_group()
    group.add_argument('-b', '--buf-cache-size', help="""The number of shared buffers to reserve for each poll group.
    Deprecated, use iobuf-small-cache-size instead""", type=int)
    group.add_argument('--iobuf-small-cache-size', help="""The number of shared buffers from a small iobuf pool to reserve
    for each poll group (optional)""", type=int)
    p.add_argument('--iobuf-large-cache-size',
                   help='Number of shared buffers from the large iobuf pool to reserve for each poll group',
                   type=int)
    p.add_argument('-z', '--zcopy', action='store_true', help='''Use zero-copy operations if the
    underlying bdev supports them''')
    p.add_argument('-d', '--num-cqe',
                   help='Number of CQ entries, only used when no_srq=true (RDMA only)',
                   type=int)
    p.add_argument('-s', '--max-srq-depth',
                   help='Number of elements in the per-thread shared receive queue (RDMA only)',
                   type=int)
    p.add_argument('-r', '--no-srq', action='store_true',
                   help='Disable shared receive queue even for devices that support it (RDMA only)')
    p.add_argument('-o', '--c2h-success', action='store_false',
                   help='Disable C2H success optimization (TCP only)')
    p.add_argument('-f', '--dif-insert-or-strip', action='store_true',
                   help='Enable DIF insert for write I/O and DIF strip for read I/O (TCP only)')
    p.add_argument('-y', '--sock-priority',
                   help='Socket priority of connections owned by this transport (TCP only)',
                   type=int)
    p.add_argument('-l', '--acceptor-backlog',
                   help='Number of pending connections allowed in the backlog before failing new connection attempts (RDMA only)',
                   type=int)
    p.add_argument('-x', '--abort-timeout-sec', help='Abort execution timeout value, in seconds', type=int)
    p.add_argument('-w', '--no-wr-batching', action='store_true',
                   help='Disable work requests batching (RDMA only)')
    p.add_argument('-e', '--control-msg-num',
                   help='Number of control messages per poll group (TCP only)',
                   type=int)
    p.add_argument('-M', '--disable-mappable-bar0', action='store_true',
                   help='Disable client mmap() of BAR0 (VFIO-USER only)')
    p.add_argument('-I', '--disable-adaptive-irq', action='store_true',
                   help='Disable adaptive interrupt feature (VFIO-USER only)')
    p.add_argument('-S', '--disable-shadow-doorbells', action='store_true',
                   help='Disable shadow doorbell support (VFIO-USER only)')
    p.add_argument('--acceptor-poll-rate',
                   help='Polling interval of the acceptor for incoming connections (microseconds)',
                   type=int)
    p.add_argument('--ack-timeout', help='ACK timeout in milliseconds', type=int)
    p.add_argument('--data-wr-pool-size', help='RDMA data WR pool size (RDMA only)', type=int)
    p.add_argument('--disable-command-passthru', action='store_true',
                   help='Disallow forwarding unrecognized I/O opcodes and the Identify Namespace admin command'
                        ' to the underlying bdev. Passthrough subsystems and admin_cmd_passthru are unaffected')
    p.add_argument('--kas', help='KATO (Keep Alive Timeout) granularity in units of 100 milliseconds', type=int)
    p.add_argument('--min-kato', help='Minimum Keep Alive Timeout value in milliseconds', type=int)
    p.add_argument('--masked-oncs', help=f"Comma-separated list of ONCS features to mask (disable). Available options: {help_oncs}",
                   type=partial(str.split, sep=','))
    p.add_argument('--masked-fuses', help=f"Comma-separated list of FUSES features to mask (disable). Available options: {help_fuses}",
                   type=partial(str.split, sep=','))
    p.set_defaults(func=nvmf_create_transport)

    def nvmf_get_transports(args):
        print_dict(args.client.nvmf_get_transports(trtype=args.trtype, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_transports', help='Display nvmf transports or required transport')
    p.add_argument('--trtype', help='Transport type')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_get_transports)

    def nvmf_get_subsystems(args):
        print_dict(args.client.nvmf_get_subsystems(nqn=args.nqn, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_subsystems', help='Display nvmf subsystems or required subsystem')
    p.add_argument('nqn', help='Subsystem NQN', nargs="?")
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_get_subsystems)

    def nvmf_create_subsystem(args):
        params = strip_globals(vars(args))
        args.client.nvmf_create_subsystem(**params)

    p = subparsers.add_parser('nvmf_create_subsystem', help='Create an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument("-s", "--serial-number", help='Serial number of the virtual NVMe controller')
    p.add_argument("-d", "--model-number", help='Model number of the virtual NVMe controller')
    p.add_argument("-a", "--allow-any-host", action='store_true',
                   help="Allow any host to connect (default: enforce allowed host NQN list)")
    p.add_argument("-m", "--max-namespaces",
                   help="Maximum number of namespaces that can be attached to the subsystem",
                   type=int)
    p.add_argument("-r", "--ana-reporting", action='store_true', help="Enable ANA reporting feature")
    p.add_argument("-i", "--min-cntlid", help="Minimum controller ID", type=int)
    p.add_argument("-I", "--max-cntlid", help="Maximum controller ID", type=int)
    p.add_argument("--dmrsl", help="Dataset Management Range Size Limit in logical block units", type=int)
    p.add_argument("--wzsl",
                   help="Write Zeroes Size Limit as a power of two (2^wzsl) in units of minimum memory page size", type=int)
    p.add_argument("-p", "--passthrough", action='store_true', help="""Use NVMe passthrough for all I/O commands and namespace-directed
                   admin commands""")
    p.add_argument("-n", "--enable-nssr", action='store_true', help="Enable NSSR (NVMe subsystem reset)")
    p.set_defaults(func=nvmf_create_subsystem)

    def nvmf_delete_subsystem(args):
        args.client.nvmf_delete_subsystem(
                                       nqn=args.nqn,
                                       tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_delete_subsystem', help='Delete a nvmf subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_delete_subsystem)

    def nvmf_subsystem_add_listener(args):
        params = strip_globals(vars(args))
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('nqn') == 'discovery':
            params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        args.client.nvmf_subsystem_add_listener(**params)

    p = subparsers.add_parser('nvmf_subsystem_add_listener', help='Add a listener to an NVMe-oF subsystem')
    p.add_argument('nqn', help="Subsystem NQN ('discovery' can be used as shortcut for discovery NQN)")
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.add_argument('-k', '--secure-channel',
                   help='Require all connections to immediately establish a secure channel',
                   action="store_true")
    p.add_argument('-n', '--ana-state',
                   help='ANA state to set',
                   type=str, choices=['optimized', 'non_optimized', 'inaccessible'])
    p.add_argument('-S', '--sock-impl', help='Socket implementation to use for the listener', type=str)
    p.add_argument('--numa-id', type=int,
                   help='Required NUMA node ID for all namespaces, if -1 then any namespace can be used.'
                   ' Default is -1. Relevant only for VFIOUSER transport.')
    p.set_defaults(func=nvmf_subsystem_add_listener)

    def nvmf_subsystem_remove_listener(args):
        params = strip_globals(vars(args))
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('nqn') == 'discovery':
            params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        args.client.nvmf_subsystem_remove_listener(**params)

    p = subparsers.add_parser('nvmf_subsystem_remove_listener', help='Remove a listener from an NVMe-oF subsystem')
    p.add_argument('nqn', help="Subsystem NQN ('discovery' can be used as shortcut for discovery NQN)")
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.set_defaults(func=nvmf_subsystem_remove_listener)

    def nvmf_subsystem_listener_set_ana_state(args):
        params = strip_globals(vars(args))
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        args.client.nvmf_subsystem_listener_set_ana_state(**params)

    p = subparsers.add_parser('nvmf_subsystem_listener_set_ana_state', help='Set ANA state of a listener for an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('-n', '--ana-state',
                   help='ANA state to set',
                   required=True, choices=['optimized', 'non_optimized', 'inaccessible'])
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.add_argument('-g', '--anagrpid', help='ANA group ID', type=int)
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
    p.add_argument('-p', '--tgt-name', help='Parent NVMe-oF target name', type=str)
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
    p.add_argument('-p', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.add_argument('-n', '--subnqn', help='Subsystem NQN')
    p.set_defaults(func=nvmf_discovery_remove_referral)

    def nvmf_discovery_get_referrals(args):
        print_dict(args.client.nvmf_discovery_get_referrals(tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_discovery_get_referrals',
                              help='Display discovery subsystem referrals of an NVMe-oF target')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_discovery_get_referrals)

    def nvmf_subsystem_add_ns(args):
        params = strip_globals(vars(args))
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'namespace', ['bdev_name', 'ptpl_file', 'nsid',
                          'nguid', 'eui64', 'uuid', 'anagrpid', 'no_auto_visible', 'hide_metadata'])
        args.client.nvmf_subsystem_add_ns(**params)

    p = subparsers.add_parser('nvmf_subsystem_add_ns', help='Add a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('bdev_name', help='Name of the bdev to expose as a namespace')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('-p', '--ptpl-file', help='File path to save/restore persistent reservation information', type=str)
    p.add_argument('-n', '--nsid', help='Namespace ID between 1 and 4294967294', type=int)
    p.add_argument('-g', '--nguid', help='16-byte namespace globally unique identifier in hexadecimal')
    p.add_argument('-e', '--eui64', help='8-byte namespace EUI-64 in hexadecimal')
    p.add_argument('-u', '--uuid', help='RFC 4122 UUID (e.g. "ceccf520-691e-4b46-9546-34af789907c5")')
    p.add_argument('-a', '--anagrpid', help='ANA group ID', type=int)
    p.add_argument('-i', '--no-auto-visible', action='store_true',
                   help='Do not auto make namespace visible to controllers')
    p.add_argument('-N', '--hide-metadata', action='store_true',
                   help='[Deprecated] Enable hide_metadata option to the bdev')
    p.set_defaults(func=nvmf_subsystem_add_ns)

    def nvmf_subsystem_set_ns_ana_group(args):
        args.client.nvmf_subsystem_set_ns_ana_group(
                                                 nqn=args.nqn,
                                                 nsid=args.nsid,
                                                 anagrpid=args.anagrpid,
                                                 tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_set_ns_ana_group', help='Change ANA group ID of a namespace')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('nsid', help='Namespace ID', type=int)
    p.add_argument('anagrpid', help='ANA group ID', type=int)
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_subsystem_set_ns_ana_group)

    def nvmf_subsystem_remove_ns(args):
        args.client.nvmf_subsystem_remove_ns(
                                          nqn=args.nqn,
                                          nsid=args.nsid,
                                          tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_remove_ns', help='Remove a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('nsid', help='Namespace ID', type=int)
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_subsystem_remove_ns)

    def nvmf_ns_add_host(args):
        args.client.nvmf_ns_add_host(
                                    nqn=args.nqn,
                                    nsid=args.nsid,
                                    host=args.host,
                                    tgt_name=args.tgt_name)

    def nvmf_ns_visible_add_args(p):
        p.add_argument('nqn', help='Subsystem NQN')
        p.add_argument('nsid', help='Namespace ID', type=int)
        p.add_argument('host', help='Host NQN')
        p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)

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
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('host', help='Host NQN to add to the allowed host list')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('--psk', help='Pre-shared key name for TLS authentication (TCP only)', type=str)
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name, required if a controller key is specified')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.set_defaults(func=nvmf_subsystem_add_host)

    def nvmf_subsystem_remove_host(args):
        args.client.nvmf_subsystem_remove_host(
                                            nqn=args.nqn,
                                            host=args.host,
                                            tgt_name=args.tgt_name,
                                            timeout_ms=args.timeout_ms)

    p = subparsers.add_parser('nvmf_subsystem_remove_host', help='Remove a host from an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('host', help='Host NQN to remove from the allowed host list')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.add_argument('-T', '--timeout-ms',
                   help='Timeout in ms to wait for I/Os to complete (optional). Default value is derived from the controller CAP.TO.',
                   type=int)
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
    p.add_argument('-t', '--tgt-name', help='NVMe-oF target name')
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name, required if a controller key is specified')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.set_defaults(func=nvmf_subsystem_set_keys)

    def nvmf_subsystem_allow_any_host(args):
        args.client.nvmf_subsystem_allow_any_host(
                                               nqn=args.nqn,
                                               allow_any_host=args.allow_any_host,
                                               tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_allow_any_host', help='Allow any host to connect to the subsystem')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('--allow-any-host', action=argparse.BooleanOptionalAction,
                   required=True, help='Allow any host to connect (`true`) or enforce allowed host NQN list (`false`)')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_subsystem_allow_any_host)

    def nvmf_subsystem_get_controllers(args):
        print_dict(args.client.nvmf_subsystem_get_controllers(
                                                           nqn=args.nqn,
                                                           tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_controllers',
                              help='Display controllers of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_subsystem_get_controllers)

    def nvmf_subsystem_get_qpairs(args):
        print_dict(args.client.nvmf_subsystem_get_qpairs(
                                                      nqn=args.nqn,
                                                      tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_qpairs',
                              help='Display queue pairs of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_subsystem_get_qpairs)

    def nvmf_subsystem_get_listeners(args):
        print_dict(args.client.nvmf_subsystem_get_listeners(
                                                         nqn=args.nqn,
                                                         tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_listeners',
                              help='Display listeners of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='Subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_subsystem_get_listeners)

    def nvmf_get_stats(args):
        print_dict(args.client.nvmf_get_stats(tgt_name=args.tgt_name))

    p = subparsers.add_parser(
        'nvmf_get_stats', help='Display current statistics for NVMf subsystem')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_get_stats)

    def nvmf_set_crdt(args):
        print_dict(args.client.nvmf_set_crdt(crdt1=args.crdt1, crdt2=args.crdt2, crdt3=args.crdt3))

    p = subparsers.add_parser(
        'nvmf_set_crdt',
        help="""Set the 3 crdt (Command Retry Delay Time) values for NVMf subsystem. All
        values are in units of 100 milliseconds (same as the NVM Express specification).""")
    p.add_argument('-t1', '--crdt1', help='Command Retry Delay Time 1 in units of 100 milliseconds', type=int)
    p.add_argument('-t2', '--crdt2', help='Command Retry Delay Time 2 in units of 100 milliseconds', type=int)
    p.add_argument('-t3', '--crdt3', help='Command Retry Delay Time 3 in units of 100 milliseconds', type=int)
    p.set_defaults(func=nvmf_set_crdt)

    def nvmf_publish_mdns_prr(args):
        args.client.nvmf_publish_mdns_prr(tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_publish_mdns_prr',
                              help='Publish pull registration request through mdns')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_publish_mdns_prr)

    def nvmf_stop_mdns_prr(args):
        args.client.nvmf_stop_mdns_prr(tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_stop_mdns_prr',
                              help='Stop publishing pull registration request through mdns')
    p.add_argument('-t', '--tgt-name', help='Parent NVMe-oF target name', type=str)
    p.set_defaults(func=nvmf_stop_mdns_prr)
