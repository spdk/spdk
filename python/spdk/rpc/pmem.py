#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.


def bdev_pmem_create_pool(client, pmem_file, num_blocks, block_size):
    """Create pmem pool at specified path.
    Args:
        pmem_file: path at which to create pmem pool
        num_blocks: number of blocks for created pmem pool file
        block_size: block size for pmem pool file
    """
    params = {'pmem_file': pmem_file,
              'num_blocks': num_blocks,
              'block_size': block_size}
    return client.call('bdev_pmem_create_pool', params)


def bdev_pmem_get_pool_info(client, pmem_file):
    """Get details about pmem pool.
    Args:
        pmem_file: path to pmem pool
    """
    params = {'pmem_file': pmem_file}
    return client.call('bdev_pmem_get_pool_info', params)


def bdev_pmem_delete_pool(client, pmem_file):
    """Delete pmem pool.
    Args:
        pmem_file: path to pmem pool
    """
    params = {'pmem_file': pmem_file}
    return client.call('bdev_pmem_delete_pool', params)
