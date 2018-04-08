def construct_malloc_bdev(client, args):
    num_blocks = (args.total_size * 1024 * 1024) // args.block_size
    params = {'num_blocks': num_blocks, 'block_size': args.block_size}
    if args.name:
        params['name'] = args.name
    if args.uuid:
        params['uuid'] = args.uuid
    return client.call('construct_malloc_bdev', params)


def construct_null_bdev(client, args):
    num_blocks = (args.total_size * 1024 * 1024) // args.block_size
    params = {'name': args.name, 'num_blocks': num_blocks,
              'block_size': args.block_size}
    if args.uuid:
        params['uuid'] = args.uuid
    return client.call('construct_null_bdev', params)


def construct_aio_bdev(client, args):
    params = {'name': args.name,
              'filename': args.filename}

    if args.block_size:
        params['block_size'] = args.block_size

    return client.call('construct_aio_bdev', params)


def construct_nvme_bdev(client, args):
    params = {'name': args.name,
              'trtype': args.trtype,
              'traddr': args.traddr}

    if args.adrfam:
        params['adrfam'] = args.adrfam

    if args.trsvcid:
        params['trsvcid'] = args.trsvcid

    if args.subnqn:
        params['subnqn'] = args.subnqn

    return client.call('construct_nvme_bdev', params)


def construct_rbd_bdev(client, args):
    params = {
        'pool_name': args.pool_name,
        'rbd_name': args.rbd_name,
        'block_size': args.block_size,
    }

    if args.name:
        params['name'] = args.name

    return client.call('construct_rbd_bdev', params)


def construct_error_bdev(client, args):
    params = {'base_name': args.base_name}
    return client.call('construct_error_bdev', params)


def construct_pmem_bdev(client, args):
    params = {
        'pmem_file': args.pmem_file,
        'name': args.name
    }
    return client.call('construct_pmem_bdev', params)


def construct_passthru_bdev(client, args):
    params = {
        'base_bdev_name': args.base_bdev_name,
        'passthru_bdev_name': args.passthru_bdev_name,
    }
    return client.call('construct_passthru_bdev', params)


def construct_split_vbdev(client, args):
    params = {
        'base_bdev': args.base_bdev,
        'split_count': args.split_count,
    }
    if args.split_size_mb:
        params['split_size_mb'] = args.split_size_mb

    return client.call('construct_split_vbdev', params)


def destruct_split_vbdev(client, args):
    params = {
        'base_bdev': args.base_bdev,
    }

    return client.call('destruct_split_vbdev', params)


def get_bdevs(client, args):
    params = {}
    if args.name:
        params['name'] = args.name
    return client.call('get_bdevs', params)


def get_bdevs_config(client, args):
    params = {}
    if args.name:
        params['name'] = args.name
    return client.call('get_bdevs_config', params)


def delete_bdev(client, args):
    params = {'name': args.bdev_name}
    return client.call('delete_bdev', params)


def bdev_inject_error(client, args):
    params = {
        'name': args.name,
        'io_type': args.io_type,
        'error_type': args.error_type,
        'num': args.num,
    }

    return client.call('bdev_inject_error', params)


def set_bdev_qos_limit_iops(client, args):
    params = {}
    params['name'] = args.name
    params['ios_per_sec'] = args.ios_per_sec
    return client.call('set_bdev_qos_limit_iops', params)


def apply_firmware(client, args):
    params = {
        'filename': args.filename,
        'bdev_name': args.bdev_name,
    }
    return client.call('apply_nvme_firmware', params)
