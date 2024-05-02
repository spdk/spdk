#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.

import json


def fsdev_get_opts(client):
    """Get fsdev subsystem opts.

    Args:
        NONE
    """
    return client.call('fsdev_get_opts')


def fsdev_set_opts(client, fsdev_io_pool_size: int = None, fsdev_io_cache_size: int = None):
    """Set fsdev subsystem opts.

    Args:
        fsdev_io_pool_size: size of fsdev IO objects pool
        fsdev_io_cache_size: size of fsdev IO objects cache per thread
    """
    params = {
    }

    if fsdev_io_pool_size is not None:
        params['fsdev_io_pool_size'] = fsdev_io_pool_size
    if fsdev_io_cache_size is not None:
        params['fsdev_io_cache_size'] = fsdev_io_cache_size

    return client.call('fsdev_set_opts', params)
