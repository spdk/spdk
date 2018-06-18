def create_pmem_pool(client, pmem_file, num_blocks, block_size):
    """Create pmem pool at specified path.
    Args:
        pmem_file: path at which to create pmem pool
        num_blocks: number of blocks for created pmem pool file
        block_size: block size for pmem pool file
    """
    params = {'pmem_file': pmem_file,
              'num_blocks': num_blocks,
              'block_size': block_size}
    return client.call('create_pmem_pool', params)


def pmem_pool_info(client, pmem_file):
    """Get details about pmem pool.
    Args:
        pmem_file: path to pmem pool
    """
    params = {'pmem_file': pmem_file}
    return client.call('pmem_pool_info', params)


def delete_pmem_pool(client, pmem_file):
    """Delete pmem pool.
    Args:
        pmem_file: path to pmem pool
    """
    params = {'pmem_file': pmem_file}
    return client.call('delete_pmem_pool', params)
