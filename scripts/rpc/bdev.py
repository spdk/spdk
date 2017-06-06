from client import print_dict, print_array, int_arg


def construct_malloc_bdev(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'num_blocks': num_blocks, 'block_size': args.block_size}
    print_array(args.client.call(
        'construct_malloc_bdev', params, verbose=args.verbose))


def construct_null_bdev(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'name': args.name, 'num_blocks': num_blocks,
              'block_size': args.block_size}
    print_array(args.client.call(
        'construct_null_bdev', params, verbose=args.verbose))


def construct_aio_bdev(args):
    params = {'name': args.name,
              'fname': args.fname}

    print_array(args.client.call(
        'construct_aio_bdev', params, verbose=args.verbose))


def construct_nvme_bdev(args):
    params = {'name': args.name,
              'trtype': args.trtype,
              'traddr': args.traddr}

    if args.adrfam:
        params['adrfam'] = args.adrfam

    if args.trsvcid:
        params['trsvcid'] = args.trsvcid

    if args.subnqn:
        params['subnqn'] = args.subnqn

    args.client.call('construct_nvme_bdev', params, verbose=args.verbose)


def construct_rbd_bdev(args):
    params = {
        'pool_name': args.pool_name,
        'rbd_name': args.rbd_name,
        'block_size': args.block_size,
    }
    print_array(args.client.call(
        'construct_rbd_bdev', params, verbose=args.verbose))


def construct_error_bdev(args):
    params = {'base_name': args.base_name}
    args.client.call('construct_error_bdev', params, verbose=args.verbose)


def get_bdevs(args):
    print_dict(args.client.call('get_bdevs', verbose=args.verbose))


def delete_bdev(args):
    params = {'name': args.bdev_name}
    args.client.call('delete_bdev', params, verbose=args.verbose)


def bdev_inject_error(args):
    params = {
        'type': args.type,
        'num': args.num,
    }

    args.client.call('bdev_inject_error', params, verbose=args.verbose)
