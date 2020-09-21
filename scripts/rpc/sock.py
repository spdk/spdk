def sock_impl_get_options(client, impl_name=None):
    """Get parameters for the socket layer implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
    """
    params = {}

    params['impl_name'] = impl_name

    return client.call('sock_impl_get_options', params)


def sock_impl_set_options(client,
                          impl_name=None,
                          recv_buf_size=None,
                          send_buf_size=None,
                          enable_recv_pipe=None,
                          enable_zerocopy_send=None,
                          enable_quickack=None,
                          enable_placement_id=None):
    """Set parameters for the socket layer implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
        recv_buf_size: size of socket receive buffer in bytes (optional)
        send_buf_size: size of socket send buffer in bytes (optional)
        enable_recv_pipe: enable or disable receive pipe (optional)
        enable_zerocopy_send: enable or disable zerocopy on send (optional)
        enable_quickack: enable or disable quickack (optional)
        enable_placement_id: enable or disable placement_id (optional)
    """
    params = {}

    params['impl_name'] = impl_name
    if recv_buf_size is not None:
        params['recv_buf_size'] = recv_buf_size
    if send_buf_size is not None:
        params['send_buf_size'] = send_buf_size
    if enable_recv_pipe is not None:
        params['enable_recv_pipe'] = enable_recv_pipe
    if enable_zerocopy_send is not None:
        params['enable_zerocopy_send'] = enable_zerocopy_send
    if enable_quickack is not None:
        params['enable_quickack'] = enable_quickack
    if enable_placement_id is not None:
        params['enable_placement_id'] = enable_placement_id

    return client.call('sock_impl_set_options', params)


def sock_set_default_impl(client, impl_name=None):
    """Set the default socket implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
    """
    params = {}

    params['impl_name'] = impl_name

    return client.call('sock_set_default_impl', params)
