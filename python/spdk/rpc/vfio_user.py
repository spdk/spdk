def vfu_tgt_set_base_path(client, path):
    """Set socket base path.

    Args:
        path: base path
    """
    params = {
            'path': path
    }

    return client.call('vfu_tgt_set_base_path', params)
