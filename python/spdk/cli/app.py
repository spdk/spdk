#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
import spdk.rpc as rpc  # noqa
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def spdk_kill_instance(args):
        rpc.app.spdk_kill_instance(args.client,
                                   sig_name=args.sig_name)

    p = subparsers.add_parser('spdk_kill_instance', help='Send signal to instance')
    p.add_argument('sig_name', help='signal will be sent to server.')
    p.set_defaults(func=spdk_kill_instance)

    def framework_monitor_context_switch(args):
        enabled = None
        if args.enable:
            enabled = True
        if args.disable:
            enabled = False
        print_dict(rpc.app.framework_monitor_context_switch(args.client,
                                                            enabled=enabled))

    p = subparsers.add_parser('framework_monitor_context_switch',
                              help='Control whether the context switch monitor is enabled')
    p.add_argument('-e', '--enable', action='store_true', help='Enable context switch monitoring')
    p.add_argument('-d', '--disable', action='store_true', help='Disable context switch monitoring')
    p.set_defaults(func=framework_monitor_context_switch)

    def framework_get_reactors(args):
        print_dict(rpc.app.framework_get_reactors(args.client))

    p = subparsers.add_parser(
        'framework_get_reactors', help='Display list of all reactors')
    p.set_defaults(func=framework_get_reactors)

    def framework_set_scheduler(args):
        rpc.app.framework_set_scheduler(args.client,
                                        name=args.name,
                                        period=args.period,
                                        load_limit=args.load_limit,
                                        core_limit=args.core_limit,
                                        core_busy=args.core_busy,
                                        mappings=args.mappings)

    p = subparsers.add_parser(
        'framework_set_scheduler', help='Select thread scheduler that will be activated and its period (experimental)')
    p.add_argument('name', help="Name of a scheduler")
    p.add_argument('-p', '--period', help="Scheduler period in microseconds", type=int)
    p.add_argument('--load-limit', help="Scheduler load limit. Reserved for dynamic scheduler", type=int)
    p.add_argument('--core-limit', help="Scheduler core limit. Reserved for dynamic scheduler", type=int)
    p.add_argument('--core-busy', help="Scheduler core busy limit. Reserved for dynamic scheduler", type=int)
    p.add_argument('--mappings', help="Comma-separated list of thread:core mappings. Reserved for static scheduler")
    p.set_defaults(func=framework_set_scheduler)

    def framework_get_scheduler(args):
        print_dict(rpc.app.framework_get_scheduler(args.client))

    p = subparsers.add_parser(
        'framework_get_scheduler', help='Display currently set scheduler and its properties.')
    p.set_defaults(func=framework_get_scheduler)

    def framework_get_governor(args):
        print_dict(rpc.app.framework_get_governor(args.client))

    p = subparsers.add_parser(
        'framework_get_governor', help='Display currently set governor and the available, set CPU frequencies.')
    p.set_defaults(func=framework_get_governor)

    def scheduler_set_options(args):
        rpc.app.scheduler_set_options(args.client,
                                      isolated_core_mask=args.isolated_core_mask,
                                      scheduling_core=args.scheduling_core)
    p = subparsers.add_parser('scheduler_set_options', help='Set scheduler options')
    p.add_argument('-i', '--isolated-core-mask', help="Mask of CPU cores to isolate from scheduling change", type=str)
    p.add_argument('-s', '--scheduling-core', help="Scheduler scheduling core. Idle threads will move to scheduling core."
                   "Reserved for dynamic scheduler.", type=int)
    p.set_defaults(func=scheduler_set_options)

    def framework_disable_cpumask_locks(args):
        rpc.framework_disable_cpumask_locks(args.client)

    p = subparsers.add_parser('framework_disable_cpumask_locks',
                              help='Disable CPU core lock files.')
    p.set_defaults(func=framework_disable_cpumask_locks)

    def framework_enable_cpumask_locks(args):
        rpc.framework_enable_cpumask_locks(args.client)

    p = subparsers.add_parser('framework_enable_cpumask_locks',
                              help='Enable CPU core lock files.')
    p.set_defaults(func=framework_enable_cpumask_locks)
