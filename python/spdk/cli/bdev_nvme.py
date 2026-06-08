#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022, 2025 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse
from functools import partial

from spdk.rpc.cmd_parser import group_as, print_array, print_dict, print_json, strip_globals


def add_parser(subparsers):

    def bdev_nvme_start_mdns_discovery(args):
        args.client.bdev_nvme_start_mdns_discovery(
                                                name=args.name,
                                                svcname=args.svcname,
                                                hostnqn=args.hostnqn)

    p = subparsers.add_parser('bdev_nvme_start_mdns_discovery', help='Start mdns based automatic discovery')
    p.add_argument('-b', '--name', help="Name of the NVMe controller prefix for each bdev name", required=True)
    p.add_argument('-s', '--svcname', help='Service type to discover: e.g., _nvme-disc._tcp', required=True)
    p.add_argument('-q', '--hostnqn', help='NVMe-oF host subnqn')
    p.set_defaults(func=bdev_nvme_start_mdns_discovery)

    def bdev_nvme_stop_mdns_discovery(args):
        args.client.bdev_nvme_stop_mdns_discovery(name=args.name)

    p = subparsers.add_parser('bdev_nvme_stop_mdns_discovery', help='Stop automatic mdns discovery')
    p.add_argument('-b', '--name', help="Name of the service to stop", required=True)
    p.set_defaults(func=bdev_nvme_stop_mdns_discovery)

    def bdev_nvme_get_mdns_discovery_info(args):
        print_dict(args.client.bdev_nvme_get_mdns_discovery_info())

    p = subparsers.add_parser('bdev_nvme_get_mdns_discovery_info', help='Get information about the automatic mdns discovery')
    p.set_defaults(func=bdev_nvme_get_mdns_discovery_info)

    def bdev_nvme_set_options(args):
        params = strip_globals(vars(args))
        params = group_as(params, 'multipath_opts', ['policy', 'selector', 'min_io'])
        args.client.bdev_nvme_set_options(**params)

    p = subparsers.add_parser('bdev_nvme_set_options',
                              help='Set options for the bdev nvme type. This is startup command.')
    p.add_argument('-a', '--action-on-timeout',
                   choices=['none', 'reset', 'abort'],
                   help="Action to take on command time out")
    p.add_argument('-t', '--timeout-us',
                   help="Timeout for each command, in microseconds. If 0, don't track timeouts.", type=int)
    p.add_argument('--timeout-admin-us',
                   help="Timeout for each admin command, in microseconds. If 0, treat same as io timeouts.", type=int)
    p.add_argument('-k', '--keep-alive-timeout-ms',
                   help="Keep alive timeout period in millisecond. If 0, disable keep-alive.", type=int)
    p.add_argument('--arbitration-burst',
                   help='the value is expressed as a power of two', type=int)
    p.add_argument('--low-priority-weight',
                   help='the maximum number of commands that the controller may launch at one time from a low priority queue', type=int)
    p.add_argument('--medium-priority-weight',
                   help='the maximum number of commands that the controller may launch at one time from a medium priority queue', type=int)
    p.add_argument('--high-priority-weight',
                   help='the maximum number of commands that the controller may launch at one time from a high priority queue', type=int)
    p.add_argument('-p', '--nvme-adminq-poll-period-us',
                   help='How often the admin queue is polled for asynchronous events', type=int)
    p.add_argument('-i', '--nvme-ioq-poll-period-us',
                   help='How often to poll I/O queues for completions', type=int)
    p.add_argument('-s', '--io-queue-requests',
                   help='The number of requests allocated for each NVMe I/O queue. Default: 512', type=int)
    p.add_argument('-d', '--disable-delay-cmd-submit',
                   help='Disable delaying NVMe command submission, i.e. no batching of multiple commands',
                   action='store_false', dest='delay_cmd_submit')
    p.add_argument('-c', '--transport-retry-count',
                   help='the number of attempts per I/O in the transport layer when an I/O fails.', type=int)
    p.add_argument('-r', '--bdev-retry-count',
                   help='the number of attempts per I/O in the bdev layer when an I/O fails. -1 means infinite retries.', type=int)
    p.add_argument('-e', '--transport-ack-timeout',
                   help="""Time to wait ack until packet retransmission for RDMA or until closes connection for TCP.
                   Range 0-31 where 0 is driver-specific default value.""", type=int)
    p.add_argument('-l', '--ctrlr-loss-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before deleting ctrlr.
                   -1 means infinite reconnect retries. 0 means no reconnect retry.
                   If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
                   If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than
                   reconnect_delay_sec.
                   This can be overridden by bdev_nvme_attach_controller.""",
                   type=int)
    p.add_argument('-o', '--reconnect-delay-sec',
                   help="""Time to delay a reconnect retry.
                   If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
                   If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
                   If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_delay_sec has to be non-zero and
                   less than ctrlr_loss_timeout_sec.
                   This can be overridden by bdev_nvme_attach_controller.""",
                   type=int)
    p.add_argument('-u', '--fast-io-fail-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
                   0 means no such timeout.
                   If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and
                   less than ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1.
                   This can be overridden by bdev_nvme_attach_controller.""",
                   type=int)
    p.add_argument('-f', '--disable-auto-failback',
                   help="""Disable automatic failback. bdev_nvme_set_preferred_path can be used to do manual failback.
                   By default, immediately failback to the preferred I/O path if it restored.""",
                   action='store_true')
    p.add_argument('--generate-uuids',
                   help="""Enable generation of unique identifiers for NVMe bdevs only if they do
                   not provide UUID themselves. These strings are based on device serial number and
                   namespace ID and will always be the same for that device.""", action='store_true')
    p.add_argument('--transport-tos',
                   help="""IPv4 Type of Service value. Only applicable for RDMA transports.
                   The default is 0 which means no TOS is applied.""", type=int)
    p.add_argument('-m', '--nvme-error-stat', help="Enable collecting NVMe error counts.", action='store_true')
    p.add_argument('-q', '--rdma-srq-size',
                   help='Set the size of a shared rdma receive queue. Default: 0 (disabled)', type=int)
    p.add_argument('--io-path-stat',
                   help="""Enable collecting I/O path stat of each io path.""",
                   action='store_true')
    p.add_argument('--allow-accel-sequence',
                   help='''Allow NVMe bdevs to advertise support for accel sequences if the
                   controller also supports them.''', action='store_true')
    p.add_argument('--rdma-max-cq-size',
                   help='The maximum size of a rdma completion queue. Default: 0 (unlimited)', type=int)
    p.add_argument('--rdma-cm-event-timeout-ms',
                   help='Time to wait for RDMA CM event. Only applicable for RDMA transports.', type=int)
    p.add_argument('--dhchap-digests', help='Comma-separated list of allowed DH-HMAC-CHAP digests',
                   type=partial(str.split, sep=','))
    p.add_argument('--dhchap-dhgroups', help='Comma-separated list of allowed DH-HMAC-CHAP DH groups',
                   type=partial(str.split, sep=','))
    p.add_argument('--rdma-umr-per-io', action=argparse.BooleanOptionalAction,
                   help='Enable or disable scatter-gather RDMA Memory Region per IO if supported by the system.')
    p.add_argument('--tcp-connect-timeout-ms',
                   help='Time to wait until TCP connection is done. Default: 0 (no timeout).', type=int)
    p.add_argument('--enable-flush', help='Pass flush to NVMe when volatile write cache is present',
                   action='store_true')
    p.add_argument('--policy', choices=['active_passive', 'active_active'], help='Multipath policy')
    p.add_argument('--selector', choices=['round_robin', 'queue_depth'], help='Multipath selector')
    p.add_argument('--min-io', type=int,
                   help='Number of IO to route to a path before switching (round_robin selector only)')

    p.set_defaults(func=bdev_nvme_set_options)

    def bdev_nvme_set_hotplug(args):
        args.client.bdev_nvme_set_hotplug(enable=args.enable, period_us=args.period_us)

    p = subparsers.add_parser('bdev_nvme_set_hotplug', help='Set hotplug options for bdev nvme type.')
    p.add_argument('--hotplug', dest='enable', action=argparse.BooleanOptionalAction,
                   required=True, help='Enable or disable hotplug')
    p.add_argument('-r', '--period-us',
                   help='How often the hotplug is processed for insert and remove events', type=int)
    p.set_defaults(func=bdev_nvme_set_hotplug)

    def bdev_nvme_attach_controller(args):
        params = strip_globals(vars(args))
        params = group_as(params, 'multipath_opts', ['policy', 'selector', 'min_io'])
        print_array(args.client.bdev_nvme_attach_controller(**params))

    p = subparsers.add_parser('bdev_nvme_attach_controller', help='Add bdevs with nvme backend')
    p.add_argument('-b', '--name', help="Name of the NVMe controller, prefix for each bdev name", required=True)
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie', required=True)
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF', required=True)
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
    p.add_argument('-p', '--priority',
                   help='NVMe-oF connection priority: e.g., a priority number')
    p.add_argument('-n', '--subnqn', help='NVMe-oF target subnqn')
    p.add_argument('-q', '--hostnqn', help='NVMe-oF host subnqn')
    p.add_argument('-i', '--hostaddr',
                   help='NVMe-oF host address: e.g., an ip address')
    p.add_argument('-c', '--hostsvcid',
                   help='NVMe-oF host svcid: e.g., a port number')
    p.add_argument('-r', '--prchk-reftag',
                   help='Enable checking of PI reference tag for I/O processing.', action='store_true')
    p.add_argument('-g', '--prchk-guard',
                   help='Enable checking of PI guard for I/O processing.', action='store_true')
    p.add_argument('-e', '--hdgst',
                   help='Enable TCP header digest.', action='store_true')
    p.add_argument('-d', '--ddgst',
                   help='Enable TCP data digest.', action='store_true')
    p.add_argument('--fabrics-timeout', type=int, help='Fabrics connect timeout in microseconds',
                   dest="fabrics_connect_timeout_us")
    p.add_argument('-x', '--multipath', choices=['disable', 'failover', 'multipath'],
                   help='Set multipath behavior')
    p.add_argument('--num-io-queues', type=int, help='Set the number of IO queues to request during initialization.')
    p.add_argument('-l', '--ctrlr-loss-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before deleting ctrlr.
                   -1 means infinite reconnect retries. 0 means no reconnect retry.
                   If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
                   If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than
                   reconnect_delay_sec.""",
                   type=int)
    p.add_argument('-o', '--reconnect-delay-sec',
                   help="""Time to delay a reconnect retry.
                   If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
                   If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
                   If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_delay_sec has to be non-zero and
                   less than ctrlr_loss_timeout_sec.""",
                   type=int)
    p.add_argument('-u', '--fast-io-fail-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
                   0 means no such timeout.
                   If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and
                   less than ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1.""",
                   type=int)
    p.add_argument('-k', '--psk',
                   help="""Set PSK and enable TCP SSL socket implementation.  The PSK can either be a
                   name of a key attached to the keyring or a path to a file containing the key.  The
                   latter method is deprecated.""")
    p.add_argument('-m', '--max-bdevs', type=int,
                   help='The size of the name array for newly created bdevs. Default is 128')
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.add_argument('-U', '--allow-unrecognized-csi', help="""Allow attaching namespaces with unrecognized command set identifiers.
                   These will only support NVMe passthrough.""", action='store_true')
    p.add_argument('--policy', choices=['active_passive', 'active_active'], help='Multipath policy')
    p.add_argument('--selector', choices=['round_robin', 'queue_depth'], help='Multipath selector')
    p.add_argument('--min-io', type=int,
                   help='Number of IO to route to a path before switching (round_robin selector only)')

    p.set_defaults(func=bdev_nvme_attach_controller)

    def bdev_nvme_get_controllers(args):
        print_dict(args.client.bdev_nvme_get_controllers(name=args.name))

    p = subparsers.add_parser(
        'bdev_nvme_get_controllers', help='Display current NVMe controllers list or required NVMe controller')
    p.add_argument('-n', '--name', help="Name of the NVMe controller. Example: Nvme0")
    p.set_defaults(func=bdev_nvme_get_controllers)

    def bdev_nvme_detach_controller(args):
        args.client.bdev_nvme_detach_controller(
                                             name=args.name,
                                             trtype=args.trtype,
                                             traddr=args.traddr,
                                             adrfam=args.adrfam,
                                             trsvcid=args.trsvcid,
                                             subnqn=args.subnqn,
                                             hostaddr=args.hostaddr,
                                             hostsvcid=args.hostsvcid)

    p = subparsers.add_parser('bdev_nvme_detach_controller',
                              help='Detach an NVMe controller and delete any associated bdevs')
    p.add_argument('name', help="Name of the controller")
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie')
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF')
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
    p.add_argument('-n', '--subnqn', help='NVMe-oF target subnqn')
    p.add_argument('-i', '--hostaddr',
                   help='NVMe-oF host address: e.g., an ip address')
    p.add_argument('-c', '--hostsvcid',
                   help='NVMe-oF host svcid: e.g., a port number')
    p.set_defaults(func=bdev_nvme_detach_controller)

    def bdev_nvme_reset_controller(args):
        args.client.bdev_nvme_reset_controller(
                                            name=args.name,
                                            cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_reset_controller',
                              help='Reset an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_reset_controller)

    def bdev_nvme_enable_controller(args):
        args.client.bdev_nvme_enable_controller(
                                             name=args.name,
                                             cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_enable_controller',
                              help='Enable an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_enable_controller)

    def bdev_nvme_disable_controller(args):
        args.client.bdev_nvme_disable_controller(
                                              name=args.name,
                                              cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_disable_controller',
                              help='Disable an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_disable_controller)

    def bdev_nvme_start_discovery(args):
        args.client.bdev_nvme_start_discovery(
                                           name=args.name,
                                           trtype=args.trtype,
                                           traddr=args.traddr,
                                           adrfam=args.adrfam,
                                           trsvcid=args.trsvcid,
                                           hostnqn=args.hostnqn,
                                           wait_for_attach=args.wait_for_attach,
                                           attach_timeout_ms=args.attach_timeout_ms,
                                           ctrlr_loss_timeout_sec=args.ctrlr_loss_timeout_sec,
                                           reconnect_delay_sec=args.reconnect_delay_sec,
                                           fast_io_fail_timeout_sec=args.fast_io_fail_timeout_sec)

    p = subparsers.add_parser('bdev_nvme_start_discovery', help='Start automatic discovery')
    p.add_argument('-b', '--name', help="Name of the NVMe controller prefix for each bdev name", required=True)
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie', required=True)
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF', required=True)
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
    p.add_argument('-q', '--hostnqn', help='NVMe-oF host subnqn')
    p.add_argument('-w', '--wait-for-attach', action='store_true',
                   help='Do not complete RPC until all discovered NVM subsystems are attached')
    p.add_argument('-T', '--attach-timeout-ms', type=int,
                   help="""Time to wait until the discovery and all discovered NVM subsystems
                        are attached (default: 0, meaning wait indefinitely).  Automatically
                        selects the --wait-for-attach option.""")
    p.add_argument('-l', '--ctrlr-loss-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before deleting ctrlr.
                   -1 means infinite reconnect retries. 0 means no reconnect retry.
                   If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
                   If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than
                   reconnect_delay_sec.""",
                   type=int)
    p.add_argument('-o', '--reconnect-delay-sec',
                   help="""Time to delay a reconnect retry.
                   If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
                   If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
                   If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_delay_sec has to be non-zero and
                   less than ctrlr_loss_timeout_sec.""",
                   type=int)
    p.add_argument('-u', '--fast-io-fail-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
                   0 means no such timeout.
                   If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and
                   less than ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1.""",
                   type=int)
    p.set_defaults(func=bdev_nvme_start_discovery)

    def bdev_nvme_stop_discovery(args):
        args.client.bdev_nvme_stop_discovery(name=args.name)

    p = subparsers.add_parser('bdev_nvme_stop_discovery', help='Stop automatic discovery')
    p.add_argument('-b', '--name', help="Name of the service to stop", required=True)
    p.set_defaults(func=bdev_nvme_stop_discovery)

    def bdev_nvme_get_discovery_info(args):
        print_dict(args.client.bdev_nvme_get_discovery_info())

    p = subparsers.add_parser('bdev_nvme_get_discovery_info', help='Get information about the automatic discovery')
    p.set_defaults(func=bdev_nvme_get_discovery_info)

    def bdev_nvme_get_io_paths(args):
        print_dict(args.client.bdev_nvme_get_io_paths(name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_io_paths', help='Display active I/O paths')
    p.add_argument('-n', '--name', help="Name of the NVMe bdev")
    p.set_defaults(func=bdev_nvme_get_io_paths)

    def bdev_nvme_set_preferred_path(args):
        args.client.bdev_nvme_set_preferred_path(
                                              name=args.name,
                                              cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_set_preferred_path',
                              help="""Set the preferred I/O path for an NVMe bdev when in multipath mode""")
    p.add_argument('-b', '--name', help='Name of the NVMe bdev', required=True)
    p.add_argument('-c', '--cntlid', help='NVMe-oF controller ID', type=int, required=True)
    p.set_defaults(func=bdev_nvme_set_preferred_path)

    def bdev_nvme_set_multipath_policy(args):
        args.client.bdev_nvme_set_multipath_policy(
                                                name=args.name,
                                                policy=args.policy,
                                                selector=args.selector,
                                                rr_min_io=args.rr_min_io)

    p = subparsers.add_parser('bdev_nvme_set_multipath_policy',
                              help="""Set multipath policy of the NVMe bdev""")
    p.add_argument('-b', '--name', help='Name of the NVMe bdev', required=True)
    p.add_argument('-p', '--policy', choices=['active_passive', 'active_active'], help='Multipath policy', required=True)
    p.add_argument('-s', '--selector', choices=['round_robin', 'queue_depth'], help='Multipath selector')
    p.add_argument('-r', '--rr-min-io',
                   help='Number of IO to route to a path before switching to another for round-robin',
                   type=int)
    p.set_defaults(func=bdev_nvme_set_multipath_policy)

    def bdev_nvme_get_path_iostat(args):
        print_dict(args.client.bdev_nvme_get_path_iostat(name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_path_iostat',
                              help="""Display current I/O statistics of all the IO paths of the blockdev. It can be
                              called when io_path_stat is true.""")
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: NVMe0n1", required=True)
    p.set_defaults(func=bdev_nvme_get_path_iostat)

    def bdev_nvme_cuse_register(args):
        args.client.bdev_nvme_cuse_register(name=args.name)

    p = subparsers.add_parser('bdev_nvme_cuse_register',
                              help='Register CUSE devices on NVMe controller')
    p.add_argument('-n', '--name',
                   help='Name of the NVMe controller. Example: Nvme0', required=True)
    p.set_defaults(func=bdev_nvme_cuse_register)

    def bdev_nvme_cuse_unregister(args):
        args.client.bdev_nvme_cuse_unregister(name=args.name)

    p = subparsers.add_parser('bdev_nvme_cuse_unregister',
                              help='Unregister CUSE devices on NVMe controller')
    p.add_argument('-n', '--name',
                   help='Name of the NVMe controller. Example: Nvme0', required=True)
    p.set_defaults(func=bdev_nvme_cuse_unregister)

    def bdev_nvme_set_keys(args):
        args.client.bdev_nvme_set_keys(name=args.name,
                                       dhchap_key=args.dhchap_key,
                                       dhchap_ctrlr_key=args.dhchap_ctrlr_key)

    p = subparsers.add_parser('bdev_nvme_set_keys',
                              help='Set DH-HMAC-CHAP keys and force (re)authentication on all '
                              'connected qpairs')
    p.add_argument('name', help='Name of the NVMe controller')
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.set_defaults(func=bdev_nvme_set_keys)

    def bdev_nvme_apply_firmware(args):
        print_dict(args.client.bdev_nvme_apply_firmware(
                                                     bdev_name=args.bdev_name,
                                                     filename=args.filename))

    p = subparsers.add_parser('bdev_nvme_apply_firmware', help='Download and commit firmware to NVMe device')
    p.add_argument('filename', help='filename of the firmware to download')
    p.add_argument('bdev_name', help='name of the NVMe device')
    p.set_defaults(func=bdev_nvme_apply_firmware)

    def bdev_nvme_get_transport_statistics(args):
        print_dict(args.client.bdev_nvme_get_transport_statistics())

    p = subparsers.add_parser('bdev_nvme_get_transport_statistics',
                              help='Get bdev_nvme poll group transport statistics')
    p.set_defaults(func=bdev_nvme_get_transport_statistics)

    def bdev_nvme_get_controller_health_info(args):
        print_dict(args.client.bdev_nvme_get_controller_health_info(name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_controller_health_info',
                              help='Display health log of the required NVMe bdev controller.')
    p.add_argument('-c', '--name', help="Name of the NVMe bdev controller. Example: Nvme0", required=True)
    p.set_defaults(func=bdev_nvme_get_controller_health_info)

    # opal
    def bdev_nvme_opal_init(args):
        args.client.bdev_nvme_opal_init(
                                     nvme_ctrlr_name=args.nvme_ctrlr_name,
                                     password=args.password)

    p = subparsers.add_parser('bdev_nvme_opal_init', help='take ownership and activate')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name', required=True)
    p.add_argument('-p', '--password', help='password for admin', required=True)
    p.set_defaults(func=bdev_nvme_opal_init)

    def bdev_nvme_opal_revert(args):
        args.client.bdev_nvme_opal_revert(
                                       nvme_ctrlr_name=args.nvme_ctrlr_name,
                                       password=args.password)
    p = subparsers.add_parser('bdev_nvme_opal_revert', help='Revert to default factory settings')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name', required=True)
    p.add_argument('-p', '--password', help='password', required=True)
    p.set_defaults(func=bdev_nvme_opal_revert)

    # bdev_nvme_send_cmd
    def bdev_nvme_send_cmd(args):
        print_dict(args.client.bdev_nvme_send_cmd(
                                               name=args.name,
                                               cmd_type=args.cmd_type,
                                               data_direction=args.data_direction,
                                               cmdbuf=args.cmdbuf,
                                               data=args.data,
                                               metadata=args.metadata,
                                               data_len=args.data_len,
                                               metadata_len=args.metadata_len,
                                               timeout_ms=args.timeout_ms))

    p = subparsers.add_parser('bdev_nvme_send_cmd', help='NVMe passthrough cmd.')
    p.add_argument('-n', '--nvme-name', dest='name', help="""Name of the operating NVMe controller""", required=True)
    p.add_argument('-t', '--cmd-type', choices=['admin', 'io'], help='Type of NVMe cmd', required=True)
    p.add_argument('-r', '--data-direction', choices=['c2h', 'h2c'], help='Direction of data transfer', required=True)
    p.add_argument('-c', '--cmdbuf', help="""NVMe command encoded by base64 urlsafe""", required=True)
    p.add_argument('-d', '--data', help="""Data transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-m', '--metadata', help="""Metadata transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-D', '--data-length', dest='data_len', help="""Data length required to transfer from controller to host""", type=int)
    p.add_argument('-M', '--metadata-length', dest='metadata_len',
                   help="""Metadata length required to transfer from controller to host""", type=int)
    p.add_argument('-T', '--timeout-ms',
                   help="""Command execution timeout value, in milliseconds,  if 0, don't track timeout""", type=int)
    p.set_defaults(func=bdev_nvme_send_cmd)

    # bdev_nvme_add_error_injection
    def bdev_nvme_add_error_injection(args):
        print_dict(args.client.bdev_nvme_add_error_injection(
                                                          name=args.name,
                                                          cmd_type=args.cmd_type,
                                                          opc=args.opc,
                                                          do_not_submit=args.do_not_submit,
                                                          timeout_in_us=args.timeout_in_us,
                                                          err_count=args.err_count,
                                                          sct=args.sct,
                                                          sc=args.sc))
    p = subparsers.add_parser('bdev_nvme_add_error_injection',
                              help='Add a NVMe command error injection.')
    p.add_argument('-n', '--nvme-name', dest='name', help="""Name of the operating NVMe controller""", required=True)
    p.add_argument('-t', '--cmd-type', choices=['admin', 'io'], help='Type of NVMe command', required=True)
    p.add_argument('-o', '--opc', help="""Opcode of the NVMe command.""", required=True, type=int)
    p.add_argument('-s', '--do-not-submit',
                   help="""Set to true if request should not be submitted to the controller""",
                   dest="do_not_submit", action='store_true')
    p.add_argument('-w', '--timeout-in-us', help="""Wait specified microseconds when do_not_submit is true""", type=int)
    p.add_argument('-e', '--err-count', help="""Number of matching NVMe commands to inject errors""", type=int)
    p.add_argument('-u', '--sct', help="""Status code type""", type=int)
    p.add_argument('-c', '--sc', help="""Status code""", type=int)
    p.set_defaults(func=bdev_nvme_add_error_injection)

    # bdev_nvme_remove_error_injection
    def bdev_nvme_remove_error_injection(args):
        print_dict(args.client.bdev_nvme_remove_error_injection(
                                                             name=args.name,
                                                             cmd_type=args.cmd_type,
                                                             opc=args.opc))
    p = subparsers.add_parser('bdev_nvme_remove_error_injection',
                              help='Removes a NVMe command error injection.')
    p.add_argument('-n', '--nvme-name', dest='name', help="""Name of the operating NVMe controller""", required=True)
    p.add_argument('-t', '--cmd-type', choices=['admin', 'io'], help='Type of NVMe cmd', required=True)
    p.add_argument('-o', '--opc', help="""Opcode of the nvme cmd.""", required=True, type=int)
    p.set_defaults(func=bdev_nvme_remove_error_injection)

    # opal
    def bdev_opal_create(args):
        print_json(args.client.bdev_opal_create(
                                             nvme_ctrlr_name=args.nvme_ctrlr_name,
                                             nsid=args.nsid,
                                             locking_range_id=args.locking_range_id,
                                             range_start=args.range_start,
                                             range_length=args.range_length,
                                             password=args.password))

    p = subparsers.add_parser('bdev_opal_create', help="""Create opal bdev on specified NVMe controller""")
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name', required=True)
    p.add_argument('-n', '--nsid', help='namespace ID (only support nsid=1 for now)', type=int, required=True)
    p.add_argument('-i', '--locking-range-id', help='locking range id', type=int, required=True)
    p.add_argument('-s', '--range-start', help='locking range start LBA', type=int, required=True)
    p.add_argument('-l', '--range-length', help='locking range length (in blocks)', type=int, required=True)
    p.add_argument('-p', '--password', help='admin password', required=True)
    p.set_defaults(func=bdev_opal_create)

    def bdev_opal_get_info(args):
        print_dict(args.client.bdev_opal_get_info(
                                               bdev_name=args.bdev_name,
                                               password=args.password))

    p = subparsers.add_parser('bdev_opal_get_info', help='get opal locking range info for this bdev')
    p.add_argument('-b', '--bdev-name', help='opal bdev', required=True)
    p.add_argument('-p', '--password', help='password', required=True)
    p.set_defaults(func=bdev_opal_get_info)

    def bdev_opal_delete(args):
        args.client.bdev_opal_delete(
                                  bdev_name=args.bdev_name,
                                  password=args.password)

    p = subparsers.add_parser('bdev_opal_delete', help="""delete a virtual opal bdev""")
    p.add_argument('-b', '--bdev-name', help='opal virtual bdev', required=True)
    p.add_argument('-p', '--password', help='admin password', required=True)
    p.set_defaults(func=bdev_opal_delete)

    def bdev_opal_new_user(args):
        args.client.bdev_opal_new_user(
                                    bdev_name=args.bdev_name,
                                    admin_password=args.admin_password,
                                    user_id=args.user_id,
                                    user_password=args.user_password)

    p = subparsers.add_parser('bdev_opal_new_user', help="""Add a user to opal bdev who can set lock state for this bdev""")
    p.add_argument('-b', '--bdev-name', help='opal bdev', required=True)
    p.add_argument('-p', '--admin-password', help='admin password', required=True)
    p.add_argument('-i', '--user-id', help='ID for new user', type=int, required=True)
    p.add_argument('-u', '--user-password', help='password set for this user', required=True)
    p.set_defaults(func=bdev_opal_new_user)

    def bdev_opal_set_lock_state(args):
        args.client.bdev_opal_set_lock_state(
                                          bdev_name=args.bdev_name,
                                          user_id=args.user_id,
                                          password=args.password,
                                          lock_state=args.lock_state)

    p = subparsers.add_parser('bdev_opal_set_lock_state', help="""set lock state for an opal bdev""")
    p.add_argument('-b', '--bdev-name', help='opal bdev', required=True)
    p.add_argument('-i', '--user-id', help='ID of the user who want to set lock state, either admin or a user assigned to this bdev',
                   type=int, required=True)
    p.add_argument('-p', '--password', help='password of this user', required=True)
    p.add_argument('-l', '--lock-state', help='lock state to set, choose from {readwrite, readonly, rwlock}', required=True)
    p.set_defaults(func=bdev_opal_set_lock_state)
