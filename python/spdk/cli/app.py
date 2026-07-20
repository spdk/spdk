#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse

from spdk.rpc.cmd_parser import print_dict


def add_parser(subparsers):

    def rpc_get_methods(args):
        print_dict(args.client.rpc_get_methods(
                                       current=args.current,
                                       include_aliases=args.include_aliases))

    p = subparsers.add_parser('rpc_get_methods', help="Get an array of supported RPC methods.")
    p.add_argument('-c', '--current', help='Return only RPC methods callable in the current state. Default: false', action='store_true')
    p.add_argument('-i', '--include-aliases', help='Include deprecated method aliases in the result. Default: false',
                   action='store_true')
    p.set_defaults(func=rpc_get_methods)

    def spdk_kill_instance(args):
        args.client.spdk_kill_instance(sig_name=args.sig_name)

    p = subparsers.add_parser('spdk_kill_instance', help="Send a signal to the application.")
    p.add_argument('sig_name', help='Signal to send',
                   choices=['SIGINT', 'SIGTERM', 'SIGQUIT', 'SIGHUP', 'SIGKILL', 'SIGUSR1'])
    p.set_defaults(func=spdk_kill_instance)

    def framework_start_init(args):
        args.client.framework_start_init()

    p = subparsers.add_parser('framework_start_init',
                              help="Start initialization of SPDK subsystems when it is deferred by starting"
                                   " SPDK application with option -w. During its deferral some RPCs can be"
                                   " used to set global parameters for SPDK subsystems. This RPC can be"
                                   " called only once.")
    p.set_defaults(func=framework_start_init)

    def framework_wait_init(args):
        args.client.framework_wait_init()

    p = subparsers.add_parser('framework_wait_init',
                              help="Do not return until all subsystems have been initialized and the RPC"
                                   " system state is running. If the application is already running, this"
                                   " call will return immediately. This RPC can be called at any time.")
    p.set_defaults(func=framework_wait_init)

    def framework_monitor_context_switch(args):
        print_dict(args.client.framework_monitor_context_switch(enabled=args.enabled))

    p = subparsers.add_parser('framework_monitor_context_switch',
                              help="Query, enable, or disable the context switch monitoring functionality.")
    p.add_argument('--monitor', dest='enabled', action=argparse.BooleanOptionalAction,
                   help='Enable or disable context switch monitoring; omit to query the current state')
    p.set_defaults(func=framework_monitor_context_switch)

    def framework_get_reactors(args):
        print_dict(args.client.framework_get_reactors())

    p = subparsers.add_parser(
        'framework_get_reactors', help="Retrieve an array of all reactors.")
    p.set_defaults(func=framework_get_reactors)

    def framework_set_scheduler(args):
        args.client.framework_set_scheduler(
                                        name=args.name,
                                        period=args.period,
                                        load_limit=args.load_limit,
                                        core_limit=args.core_limit,
                                        core_busy=args.core_busy,
                                        mappings=args.mappings)

    p = subparsers.add_parser(
        'framework_set_scheduler', help="Select thread scheduler that will be activated."
                                        " This feature is considered as experimental.")
    p.add_argument('name', help='Scheduler name: `static` (fixed core pinning) or `dynamic` (load-based thread migration across cores)')
    p.add_argument('-p', '--period', help='Scheduler period in microseconds. Default: 1000000 (1 second) on first set', type=int)
    p.add_argument('--load-limit',
                   help='Thread load percentage above which threads may move (dynamic only). Default: 20', type=int)
    p.add_argument('--core-limit',
                   help='Core busy percentage at which the core is considered full (dynamic only). Default: 80', type=int)
    p.add_argument('--core-busy',
                   help='Core busy percentage at which the scheduler starts moving threads to other cores (dynamic only). Default: 95',
                   type=int)
    p.add_argument('--mappings', help='Comma-separated list of thread:core mappings (static only)')
    p.set_defaults(func=framework_set_scheduler)

    def framework_get_scheduler(args):
        print_dict(args.client.framework_get_scheduler())

    p = subparsers.add_parser(
        'framework_get_scheduler', help="Retrieve currently set scheduler name and period, along with current governor name.")
    p.set_defaults(func=framework_get_scheduler)

    def framework_get_governor(args):
        print_dict(args.client.framework_get_governor())

    p = subparsers.add_parser(
        'framework_get_governor', help="Retrieve current governor name, power env, frequencies available and frequency set to the cpu"
                                        " cores.")
    p.set_defaults(func=framework_get_governor)

    def scheduler_set_options(args):
        args.client.scheduler_set_options(
                                      isolated_core_mask=args.isolated_core_mask,
                                      scheduling_core=args.scheduling_core)
    p = subparsers.add_parser('scheduler_set_options',
                              help="Set options for scheduler. This RPC may only be called before SPDK"
                                   " subsystems have been initialized. This RPC can be called only once.")
    p.add_argument('-i', '--isolated-core-mask',
                   help='CPU mask of cores excluded from scheduling decisions; must not include scheduling_core', type=str)
    p.add_argument('-s', '--scheduling-core',
                   help='Core that the scheduler runs on; idle threads are moved here. Default: current scheduling core',
                   type=int)
    p.set_defaults(func=scheduler_set_options)

    def framework_disable_cpumask_locks(args):
        args.client.framework_disable_cpumask_locks()

    p = subparsers.add_parser('framework_disable_cpumask_locks',
                              help="Disable CPU core lock files. The locks can also be disabled during"
                                   " startup, when user specifies `--disable-cpumask-locks` command line"
                                   " option during SPDK launch. This RPC may be called after locks have"
                                   " already been disabled, with no effect and no error response.")
    p.set_defaults(func=framework_disable_cpumask_locks)

    def framework_enable_cpumask_locks(args):
        args.client.framework_enable_cpumask_locks()

    p = subparsers.add_parser('framework_enable_cpumask_locks',
                              help="Enable CPU core lock files to block multiple SPDK applications from"
                                   " running on the same cpumask. The CPU core locks are enabled by"
                                   " default, unless user specified `--disable-cpumask-locks` command"
                                   " line option when launching SPDK. This RPC may be called after locks"
                                   " have already been enabled, with no effect and no error response.")
    p.set_defaults(func=framework_enable_cpumask_locks)

    def thread_get_stats(args):
        print_dict(args.client.thread_get_stats())

    p = subparsers.add_parser(
        'thread_get_stats', help="Retrieve current statistics of all the threads.")
    p.set_defaults(func=thread_get_stats)

    def thread_set_cpumask(args):
        args.client.thread_set_cpumask(
                                         id=args.id,
                                         cpumask=args.cpumask)
    p = subparsers.add_parser('thread_set_cpumask',
                              help="Set the cpumask of the thread to the specified value. The thread may"
                                   " be migrated to one of the specified CPUs.")
    p.add_argument('-i', '--id', type=int, help='ID of the SPDK thread to move', required=True)
    p.add_argument('-m', '--cpumask', help='CPU mask of cores the thread is allowed to run on', required=True)
    p.set_defaults(func=thread_set_cpumask)

    def thread_get_pollers(args):
        print_dict(args.client.thread_get_pollers())

    p = subparsers.add_parser(
        'thread_get_pollers', help="Retrieve current pollers of all the threads.")
    p.set_defaults(func=thread_get_pollers)

    def thread_get_io_channels(args):
        print_dict(args.client.thread_get_io_channels())

    p = subparsers.add_parser(
        'thread_get_io_channels', help="Retrieve current IO channels of all the threads.")
    p.set_defaults(func=thread_get_io_channels)
