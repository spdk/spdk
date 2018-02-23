from client import print_dict, print_array, int_arg


def construct_malloc_bdev(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'num_blocks': num_blocks, 'block_size': args.block_size}
    if args.name:
        params['name'] = args.name
    print_array(args.client.call(
        'construct_malloc_bdev', params))


def construct_null_bdev(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'name': args.name, 'num_blocks': num_blocks,
              'block_size': args.block_size}
    print_array(args.client.call(
        'construct_null_bdev', params))


def construct_aio_bdev(args):
    params = {'name': args.name,
              'filename': args.filename}

    if args.block_size:
        params['block_size'] = args.block_size

    print_array(args.client.call(
        'construct_aio_bdev', params))


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

    args.client.call('construct_nvme_bdev', params)


def construct_rbd_bdev(args):
    params = {
        'pool_name': args.pool_name,
        'rbd_name': args.rbd_name,
        'block_size': args.block_size,
    }

    if args.name:
    	params['name'] = args.name

    print_array(args.client.call(
        'construct_rbd_bdev', params))


def construct_error_bdev(args):
    params = {'base_name': args.base_name}
    args.client.call('construct_error_bdev', params)


def construct_pmem_bdev(args):
    params = {
        'pmem_file': args.pmem_file,
        'name': args.name
    }
    print_array(args.client.call('construct_pmem_bdev', params))


def get_bdevs(args):
    params = {}
    if args.name:
        params['name'] = args.name
    print_dict(args.client.call('get_bdevs', params))

def get_bdevs_config(args):
    params = {}
    if args.name:
        params['name'] = args.name
    print_dict(args.client.call('get_bdevs_config', params))


def delete_bdev(args):
    params = {'name': args.bdev_name}
    args.client.call('delete_bdev', params)


def bdev_inject_error(args):
    params = {
        'name': args.name,
        'io_type': args.io_type,
        'error_type': args.error_type,
        'num': args.num,
    }

    args.client.call('bdev_inject_error', params)


def apply_firmware(args):
    params = {
        'filename': args.filename,
        'bdev_name': args.bdev_name,
    }
    print_dict(args.client.call('apply_nvme_firmware', params))
