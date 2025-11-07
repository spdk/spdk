#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse

from spdk.rpc.client import print_array, print_dict, print_json  # noqa
from spdk.rpc.helpers import DeprecateFalseAction, DeprecateTrueAction


def add_parser(subparsers):

    def iscsi_set_options(args):
        args.client.iscsi_set_options(
            auth_file=args.auth_file,
            node_base=args.node_base,
            nop_timeout=args.nop_timeout,
            nop_in_interval=args.nop_in_interval,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap,
            chap_group=args.chap_group,
            max_sessions=args.max_sessions,
            max_queue_depth=args.max_queue_depth,
            max_connections_per_session=args.max_connections_per_session,
            default_time2wait=args.default_time2wait,
            default_time2retain=args.default_time2retain,
            first_burst_length=args.first_burst_length,
            immediate_data=args.immediate_data,
            error_recovery_level=args.error_recovery_level,
            allow_duplicated_isid=args.allow_duplicated_isid,
            max_large_datain_per_connection=args.max_large_datain_per_connection,
            max_r2t_per_connection=args.max_r2t_per_connection,
            pdu_pool_size=args.pdu_pool_size,
            immediate_data_pool_size=args.immediate_data_pool_size,
            data_out_pool_size=args.data_out_pool_size)

    p = subparsers.add_parser('iscsi_set_options',
                              help="""Set options of iSCSI subsystem""")
    p.add_argument('-f', '--auth-file', help='Path to CHAP shared secret file')
    p.add_argument('-b', '--node-base', help='Prefix of the name of iSCSI target node')
    p.add_argument('-o', '--nop-timeout', help='Timeout in seconds to nop-in request to the initiator', type=int)
    p.add_argument('-n', '--nop-in-interval', help='Time interval in secs between nop-in requests by the target', type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP for discovery session should be disabled.
    *** Mutually exclusive with --require-chap""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP for discovery session should be required.
    *** Mutually exclusive with --disable-chap""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP for discovery session should be mutual', action='store_true')
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for discovery session.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-a', '--max-sessions', help='Maximum number of sessions in the host.', type=int)
    p.add_argument('-q', '--max-queue-depth', help='Max number of outstanding I/Os per queue.', type=int)
    p.add_argument('-c', '--max-connections-per-session', help='Negotiated parameter, MaxConnections.', type=int)
    p.add_argument('-w', '--default-time2wait', help='Negotiated parameter, DefaultTime2Wait.', type=int)
    p.add_argument('-v', '--default-time2retain', help='Negotiated parameter, DefaultTime2Retain.', type=int)
    p.add_argument('-s', '--first-burst-length', help='Negotiated parameter, FirstBurstLength.', type=int)
    p.add_argument('-i', '--immediate-data', help='Negotiated parameter, ImmediateData.', action='store_true')
    p.add_argument('-l', '--error-recovery-level', help='Negotiated parameter, ErrorRecoveryLevel', type=int)
    p.add_argument('-p', '--allow-duplicated-isid', help='Allow duplicated initiator session ID.', action='store_true')
    p.add_argument('-x', '--max-large-datain-per-connection', help='Max number of outstanding split read I/Os per connection', type=int)
    p.add_argument('-k', '--max-r2t-per-connection', help='Max number of outstanding R2Ts per connection', type=int)
    p.add_argument('-u', '--pdu-pool-size', help='Number of PDUs in the pool', type=int)
    p.add_argument('-j', '--immediate-data-pool-size', help='Number of immediate data buffers in the pool', type=int)
    p.add_argument('-z', '--data-out-pool-size', help='Number of data out buffers in the pool', type=int)
    p.set_defaults(func=iscsi_set_options)

    def iscsi_set_discovery_auth(args):
        args.client.iscsi_set_discovery_auth(
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap,
            chap_group=args.chap_group)

    p = subparsers.add_parser('iscsi_set_discovery_auth',
                              help="""Set CHAP authentication for discovery session.""")
    p.add_argument('-d', '--disable-chap', help="""CHAP for discovery session should be disabled.
    *** Mutually exclusive with --require-chap""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP for discovery session should be required.
    *** Mutually exclusive with --disable-chap""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP for discovery session should be mutual', action='store_true')
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for discovery session.
    *** Authentication group must be precreated ***""", type=int)
    p.set_defaults(func=iscsi_set_discovery_auth)

    def iscsi_create_auth_group(args):
        secrets = None
        if args.secrets:
            secrets = [dict(u.split(":") for u in a.split(" ")) for a in args.secrets.split(",")]

        args.client.iscsi_create_auth_group(tag=args.tag, secrets=secrets)

    p = subparsers.add_parser('iscsi_create_auth_group',
                              help='Create authentication group for CHAP authentication.')
    p.add_argument('tag', help='Authentication group tag (unique, integer > 0).', type=int)
    p.add_argument('-c', '--secrets', help="""Comma-separated list of CHAP secrets
<user:user_name secret:chap_secret muser:mutual_user_name msecret:mutual_chap_secret> enclosed in quotes.
Format: 'user:u1 secret:s1 muser:mu1 msecret:ms1,user:u2 secret:s2 muser:mu2 msecret:ms2'""")
    p.set_defaults(func=iscsi_create_auth_group)

    def iscsi_delete_auth_group(args):
        args.client.iscsi_delete_auth_group(tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_auth_group',
                              help='Delete an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.set_defaults(func=iscsi_delete_auth_group)

    def iscsi_auth_group_add_secret(args):
        args.client.iscsi_auth_group_add_secret(
            tag=args.tag,
            user=args.user,
            secret=args.secret,
            muser=args.muser,
            msecret=args.msecret)

    p = subparsers.add_parser('iscsi_auth_group_add_secret',
                              help='Add a secret to an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.add_argument('-u', '--user', help='User name for one-way CHAP authentication', required=True)
    p.add_argument('-s', '--secret', help='Secret for one-way CHAP authentication', required=True)
    p.add_argument('-m', '--muser', help='User name for mutual CHAP authentication')
    p.add_argument('-r', '--msecret', help='Secret for mutual CHAP authentication')
    p.set_defaults(func=iscsi_auth_group_add_secret)

    def iscsi_auth_group_remove_secret(args):
        args.client.iscsi_auth_group_remove_secret(tag=args.tag, user=args.user)

    p = subparsers.add_parser('iscsi_auth_group_remove_secret',
                              help='Remove a secret from an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.add_argument('-u', '--user', help='User name for one-way CHAP authentication', required=True)
    p.set_defaults(func=iscsi_auth_group_remove_secret)

    def iscsi_get_auth_groups(args):
        print_dict(args.client.iscsi_get_auth_groups())

    p = subparsers.add_parser('iscsi_get_auth_groups',
                              help='Display current authentication group configuration')
    p.set_defaults(func=iscsi_get_auth_groups)

    def iscsi_get_portal_groups(args):
        print_dict(args.client.iscsi_get_portal_groups())

    p = subparsers.add_parser('iscsi_get_portal_groups', help='Display current portal group configuration')
    p.set_defaults(func=iscsi_get_portal_groups)

    def iscsi_get_initiator_groups(args):
        print_dict(args.client.iscsi_get_initiator_groups())

    p = subparsers.add_parser('iscsi_get_initiator_groups',
                              help='Display current initiator group configuration')
    p.set_defaults(func=iscsi_get_initiator_groups)

    def iscsi_get_target_nodes(args):
        print_dict(args.client.iscsi_get_target_nodes())

    p = subparsers.add_parser('iscsi_get_target_nodes', help='Display target nodes')
    p.set_defaults(func=iscsi_get_target_nodes)

    def iscsi_enable_histogram(args):
        args.client.iscsi_enable_histogram(name=args.name, enable=args.enable)

    p = subparsers.add_parser('iscsi_enable_histogram',
                              help='Enable or disable histogram for specified iscsi target')
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('-e', '--enable', dest='enable', action=DeprecateTrueAction,
                       help='Enable histograms on specified iscsi target', default=True)
    group.add_argument('-d', '--disable', dest='enable', action=DeprecateFalseAction,
                       help='Disable histograms on specified iscsi target')
    group.add_argument('--histogram', dest='enable', action=argparse.BooleanOptionalAction,
                       help='Enable or disable histogram for specified iscsi target')
    p.add_argument('name', help='iscsi target name')
    p.set_defaults(func=iscsi_enable_histogram)

    def iscsi_get_histogram(args):
        print_dict(args.client.iscsi_get_histogram(name=args.name))

    p = subparsers.add_parser('iscsi_get_histogram',
                              help='Get histogram for specified iscsi target')
    p.add_argument('name', help='target name')
    p.set_defaults(func=iscsi_get_histogram)

    def iscsi_create_target_node(args):
        luns = []
        for u in args.luns.strip().split(" "):
            bdev_name, lun_id = u.split(":")
            luns.append({"bdev_name": bdev_name, "lun_id": int(lun_id)})

        pg_ig_maps = []
        for u in args.pg_ig_maps.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})

        args.client.iscsi_create_target_node(
            luns=luns,
            pg_ig_maps=pg_ig_maps,
            name=args.name,
            alias_name=args.alias_name,
            queue_depth=args.queue_depth,
            chap_group=args.chap_group,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap,
            header_digest=args.header_digest,
            data_digest=args.data_digest)

    p = subparsers.add_parser('iscsi_create_target_node', help='Add a target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('alias_name', help='Target node alias name (ASCII)')
    p.add_argument('luns', help="""Whitespace-separated list of <bdev name:LUN ID> pairs enclosed
    in quotes.  Format:  'bdev_name0:id0 bdev_name1:id1' etc
    Example: 'Malloc0:0 Malloc1:1 Malloc5:2'
    *** The bdevs must pre-exist ***
    *** LUN0 (id = 0) is required ***
    *** bdevs names cannot contain space or colon characters ***""")
    p.add_argument('pg_ig_maps', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.add_argument('queue_depth', help='Desired target queue depth', type=int)
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for this target node.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP authentication should be disabled for this target node.
    *** Mutually exclusive with --require-chap ***""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP authentication should be required for this target node.
    *** Mutually exclusive with --disable-chap ***""", action='store_true')
    p.add_argument(
        '-m', '--mutual-chap', help='CHAP authentication should be mutual/bidirectional.', action='store_true')
    p.add_argument('-H', '--header-digest',
                   help='Header Digest should be required for this target node.', action='store_true')
    p.add_argument('-D', '--data-digest',
                   help='Data Digest should be required for this target node.', action='store_true')
    p.set_defaults(func=iscsi_create_target_node)

    def iscsi_target_node_add_lun(args):
        args.client.iscsi_target_node_add_lun(
            name=args.name,
            bdev_name=args.bdev_name,
            lun_id=args.lun_id)

    p = subparsers.add_parser('iscsi_target_node_add_lun',
                              help='Add LUN to the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('bdev_name', help="""bdev name enclosed in quotes.
    *** bdev name cannot contain space or colon characters ***""")
    p.add_argument('-i', dest='lun_id', help="""LUN ID (integer >= 0)
    *** If LUN ID is omitted or -1, the lowest free one is assigned ***""", type=int)
    p.set_defaults(func=iscsi_target_node_add_lun)

    def iscsi_target_node_set_auth(args):
        args.client.iscsi_target_node_set_auth(
            name=args.name,
            chap_group=args.chap_group,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap)

    p = subparsers.add_parser('iscsi_target_node_set_auth',
                              help='Set CHAP authentication for the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for this target node.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP authentication should be disabled for this target node.
    *** Mutually exclusive with --require-chap ***""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP authentication should be required for this target node.
    *** Mutually exclusive with --disable-chap ***""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP authentication should be mutual/bidirectional.',
                   action='store_true')
    p.set_defaults(func=iscsi_target_node_set_auth)

    def iscsi_target_node_add_pg_ig_maps(args):
        pg_ig_maps = []
        for u in args.pg_ig_maps.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        args.client.iscsi_target_node_add_pg_ig_maps(
            pg_ig_maps=pg_ig_maps,
            name=args.name)

    p = subparsers.add_parser('iscsi_target_node_add_pg_ig_maps',
                              help='Add PG-IG maps to the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_ig_maps', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.set_defaults(func=iscsi_target_node_add_pg_ig_maps)

    def iscsi_target_node_remove_pg_ig_maps(args):
        pg_ig_maps = []
        for u in args.pg_ig_maps.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        args.client.iscsi_target_node_remove_pg_ig_maps(
             pg_ig_maps=pg_ig_maps, name=args.name)

    p = subparsers.add_parser('iscsi_target_node_remove_pg_ig_maps',
                              help='Delete PG-IG maps from the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_ig_maps', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.set_defaults(func=iscsi_target_node_remove_pg_ig_maps)

    def iscsi_target_node_set_redirect(args):
        args.client.iscsi_target_node_set_redirect(
            name=args.name,
            pg_tag=args.pg_tag,
            redirect_host=args.redirect_host,
            redirect_port=args.redirect_port)

    p = subparsers.add_parser('iscsi_target_node_set_redirect',
                              help="""Update redirect portal of the public portal group for the target node.
    Omit redirect host and port to clear previously set redirect settings.""")
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('-a', '--redirect-host', help='Numeric IP address for redirect portal')
    p.add_argument('-p', '--redirect-port', help='Numeric TCP port for redirect portal')
    p.set_defaults(func=iscsi_target_node_set_redirect)

    def iscsi_target_node_request_logout(args):
        args.client.iscsi_target_node_request_logout(
            name=args.name,
            pg_tag=args.pg_tag)

    p = subparsers.add_parser('iscsi_target_node_request_logout',
                              help="""For the target node, request connections whose portal group tag
    match to logout, or request all connections if portal group tag is omitted.""")
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('-t', '--pg-tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_target_node_request_logout)

    def iscsi_create_portal_group(args):
        portals = []
        for p in args.portals.strip().split(' '):
            ip, port = p.split(':')
            portals.append({'host': ip, 'port': port})
        args.client.iscsi_create_portal_group(
            portals=portals,
            tag=args.tag,
            private=args.private,
            wait=args.wait)

    p = subparsers.add_parser('iscsi_create_portal_group',
                              help='Add a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('portals', help="""List of portals in host:port format, separated by whitespace
    Example: '192.168.100.100:3260 192.168.100.100:3261 192.168.100.100:3262'""")
    p.add_argument('-p', '--private', help="""Public (false) or private (true) portal group.
    Private portal groups do not have their portals returned by a discovery session. A public
    portal group may optionally specify a redirect portal for non-discovery logins. This redirect
    portal must be from a private portal group.""", action='store_true')
    p.add_argument('-w', '--wait', help="""Do not listening on portals until it is started explicitly.
    One major iSCSI initiator may not retry login once it failed. Hence for such initiator, listening
    on portals should be allowed after all associated target nodes are created.""", action='store_true')
    p.set_defaults(func=iscsi_create_portal_group)

    def iscsi_start_portal_group(args):
        args.client.iscsi_start_portal_group(tag=args.tag)

    p = subparsers.add_parser('iscsi_start_portal_group',
                              help='Start listening on portals if it is not started yet.')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_start_portal_group)

    def iscsi_create_initiator_group(args):
        args.client.iscsi_create_initiator_group(
            tag=args.tag,
            initiators=args.initiators.strip().split(),
            netmasks=args.netmasks.strip().split())

    p = subparsers.add_parser('iscsi_create_initiator_group',
                              help='Add an initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('initiators', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  Example: 'ANY' or 'iqn.2016-06.io.spdk:host1 iqn.2016-06.io.spdk:host2'""")
    p.add_argument('netmasks', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    Example: '255.255.0.0 255.248.0.0' etc""")
    p.set_defaults(func=iscsi_create_initiator_group)

    def iscsi_initiator_group_add_initiators(args):
        initiators = args.initiators.strip().split() if args.initiators else None
        netmasks = args.netmasks.strip().split() if args.netmasks else None
        args.client.iscsi_initiator_group_add_initiators(
            tag=args.tag,
            initiators=initiators,
            netmasks=netmasks)

    p = subparsers.add_parser('iscsi_initiator_group_add_initiators',
                              help='Add initiators to an existing initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('-n', dest='initiators', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  This parameter can be omitted.  Example: 'ANY' or
    'iqn.2016-06.io.spdk:host1 iqn.2016-06.io.spdk:host2'""")
    p.add_argument('-m', dest='netmasks', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    This parameter can be omitted.  Example: '255.255.0.0 255.248.0.0' etc""")
    p.set_defaults(func=iscsi_initiator_group_add_initiators)

    def iscsi_initiator_group_remove_initiators(args):
        initiators = args.initiators.strip().split() if args.initiators else None
        netmasks = args.netmasks.strip().split() if args.netmasks else None
        args.client.iscsi_initiator_group_remove_initiators(
            tag=args.tag,
            initiators=initiators,
            netmasks=netmasks)

    p = subparsers.add_parser('iscsi_initiator_group_remove_initiators',
                              help='Delete initiators from an existing initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('-n', dest='initiators', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  This parameter can be omitted.  Example: 'ANY' or
    'iqn.2016-06.io.spdk:host1 iqn.2016-06.io.spdk:host2'""")
    p.add_argument('-m', dest='netmasks', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    This parameter can be omitted.  Example: '255.255.0.0 255.248.0.0' etc""")
    p.set_defaults(func=iscsi_initiator_group_remove_initiators)

    def iscsi_delete_target_node(args):
        args.client.iscsi_delete_target_node(name=args.name)

    p = subparsers.add_parser('iscsi_delete_target_node',
                              help='Delete a target node')
    p.add_argument('name',
                   help='Target node name to be deleted. Example: iqn.2016-06.io.spdk:disk1.')
    p.set_defaults(func=iscsi_delete_target_node)

    def iscsi_delete_portal_group(args):
        args.client.iscsi_delete_portal_group(tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_portal_group',
                              help='Delete a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_delete_portal_group)

    def iscsi_delete_initiator_group(args):
        args.client.iscsi_delete_initiator_group(tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_initiator_group',
                              help='Delete an initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_delete_initiator_group)

    def iscsi_portal_group_set_auth(args):
        args.client.iscsi_portal_group_set_auth(
            tag=args.tag,
            chap_group=args.chap_group,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap)

    p = subparsers.add_parser('iscsi_portal_group_set_auth',
                              help='Set CHAP authentication for discovery sessions specific for the portal group')
    p.add_argument('tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for this portal group.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP authentication should be disabled for this portal group.
    *** Mutually exclusive with --require-chap ***""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP authentication should be required for this portal group.
    *** Mutually exclusive with --disable-chap ***""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP authentication should be mutual/bidirectional.',
                   action='store_true')
    p.set_defaults(func=iscsi_portal_group_set_auth)

    def iscsi_get_connections(args):
        print_dict(args.client.iscsi_get_connections())

    p = subparsers.add_parser('iscsi_get_connections',
                              help='Display iSCSI connections')
    p.set_defaults(func=iscsi_get_connections)

    def iscsi_get_stats(args):
        print_dict(args.client.iscsi_get_stats())

    p = subparsers.add_parser('iscsi_get_stats',
                              help='Display stat information of iSCSI connections.')
    p.set_defaults(func=iscsi_get_stats)

    def iscsi_get_options(args):
        print_dict(args.client.iscsi_get_options())

    p = subparsers.add_parser('iscsi_get_options',
                              help='Display iSCSI global parameters')
    p.set_defaults(func=iscsi_get_options)

    def scsi_get_devices(args):
        print_dict(args.client.scsi_get_devices())

    p = subparsers.add_parser('scsi_get_devices', help='Display SCSI devices')
    p.set_defaults(func=scsi_get_devices)
