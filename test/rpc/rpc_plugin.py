from rpc.client import print_json


def malloc_create(args):
    params = {'num_blocks': 256, 'block_size': 4096}
    return args.client.call('bdev_malloc_create', params)


def malloc_delete(args):
    params = {'name': args.name}
    return args.client.call('bdev_malloc_delete', params)


def create_malloc(args):
    print_json(malloc_create(args))


def spdk_rpc_plugin_initialize(subparsers):
    p = subparsers.add_parser('create_malloc', help='Create malloc backend')
    p.set_defaults(func=create_malloc)

    p = subparsers.add_parser('delete_malloc', help='Delete malloc backend')
    p.add_argument('name', help='malloc bdev name')
    p.set_defaults(func=malloc_delete)
