from rpc.client import print_json


def thread_create(args):
    params = {'active': args.active}
    if args.name:
        params['name'] = args.name
    if args.cpu_mask:
        params['cpu_mask'] = args.cpu_mask
    return args.client.call('scheduler_thread_create', params)


def create_thread(args):
    print_json(thread_create(args))


def thread_set_active(args):
    params = {'thread_id': args.thread_id, 'active': args.active}
    return args.client.call('scheduler_thread_set_active', params)


def thread_delete(args):
    params = {'thread_id': args.thread_id}
    return args.client.call('scheduler_thread_delete', params)


def spdk_rpc_plugin_initialize(subparsers):
    p = subparsers.add_parser('scheduler_thread_create', help='Create spdk thread')
    p.add_argument('-n', '--name', help='Name of spdk thread and poller')
    p.add_argument('-m', '--cpu_mask', help='CPU mask for spdk thread')
    p.add_argument('-a', '--active', help='Percent of time thread is active', type=int)
    p.set_defaults(func=create_thread)

    p = subparsers.add_parser('scheduler_thread_set_active', help='Change percent of time the spdk thread is active')
    p.add_argument('thread_id', help='spdk_thread id', type=int)
    p.add_argument('active', help='Percent of time thread is active', type=int)
    p.set_defaults(func=thread_set_active)

    p = subparsers.add_parser('scheduler_thread_delete', help='Delete spdk thread')
    p.add_argument('thread_id', help='spdk_thread id', type=int)
    p.set_defaults(func=thread_delete)
