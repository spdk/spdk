#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.


def iobuf_set_options(client, small_pool_count, large_pool_count, small_bufsize, large_bufsize):
    """Set iobuf pool options.

    Args:
        small_pool_count: number of small buffers in the global pool
        large_pool_count: number of large buffers in the global pool
        small_bufsize: size of a small buffer
        large_bufsize: size of a large buffer
    """
    params = {}

    if small_pool_count is not None:
        params['small_pool_count'] = small_pool_count
    if large_pool_count is not None:
        params['large_pool_count'] = large_pool_count
    if small_bufsize is not None:
        params['small_bufsize'] = small_bufsize
    if large_bufsize is not None:
        params['large_bufsize'] = large_bufsize

    return client.call('iobuf_set_options', params)
