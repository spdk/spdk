#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.


def trace_enable_tpoint_group(client, name):
    """Enable trace on a specific tpoint group.

    Args:
        name: trace group name we want to enable in tpoint_group_mask. (for example "bdev").
    """
    params = {'name': name}
    return client.call('trace_enable_tpoint_group', params)


def trace_disable_tpoint_group(client, name):
    """Disable trace on a specific tpoint group.

    Args:
        name: trace group name we want to disable in tpoint_group_mask. (for example "bdev").
    """
    params = {'name': name}
    return client.call('trace_disable_tpoint_group', params)


def trace_set_tpoint_mask(client, name, tpoint_mask):
    """Enable tracepoint mask on a specific tpoint group.

    Args:
        name: trace group name we want to enable in tpoint_group_mask. (for example "bdev").
        tpoint_mask: tracepoints to be enabled inside decleared group
                        (for example "0x3" to enable first two tpoints).
    """
    params = {'name': name, 'tpoint_mask': tpoint_mask}
    return client.call('trace_set_tpoint_mask', params)


def trace_clear_tpoint_mask(client, name, tpoint_mask):
    """Disable tracepoint mask on a specific tpoint group.

    Args:
        name: trace group name we want to disable in tpoint_group_mask. (for example "bdev").
        tpoint_mask: tracepoints to be disabled inside decleared group
                        (for example "0x3" to disable first two tpoints).
    """
    params = {'name': name, 'tpoint_mask': tpoint_mask}
    return client.call('trace_clear_tpoint_mask', params)


def trace_get_tpoint_group_mask(client):
    """Get trace point group mask

    Returns:
        List of trace point group mask
    """
    return client.call('trace_get_tpoint_group_mask')


def trace_get_info(client):
    """Get name of shared memory file and list of the available trace point groups

    Returns:
        Name of shared memory file and list of the available trace point groups
    """
    return client.call('trace_get_info')
