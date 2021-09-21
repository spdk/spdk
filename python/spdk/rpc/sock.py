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
                          enable_quickack=None,
                          enable_placement_id=None,
                          enable_zerocopy_send_server=None,
                          enable_zerocopy_send_client=None):
    """Set parameters for the socket layer implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
        recv_buf_size: size of socket receive buffer in bytes (optional)
        send_buf_size: size of socket send buffer in bytes (optional)
        enable_recv_pipe: enable or disable receive pipe (optional)
        enable_quickack: enable or disable quickack (optional)
        enable_placement_id: option for placement_id. 0:disable,1:incoming_napi,2:incoming_cpu (optional)
        enable_zerocopy_send_server: enable or disable zerocopy on send for server sockets(optional)
        enable_zerocopy_send_client: enable or disable zerocopy on send for client sockets(optional)
    """
    params = {}

    params['impl_name'] = impl_name
    if recv_buf_size is not None:
        params['recv_buf_size'] = recv_buf_size
    if send_buf_size is not None:
        params['send_buf_size'] = send_buf_size
    if enable_recv_pipe is not None:
        params['enable_recv_pipe'] = enable_recv_pipe
    if enable_quickack is not None:
        params['enable_quickack'] = enable_quickack
    if enable_placement_id is not None:
        params['enable_placement_id'] = enable_placement_id
    if enable_zerocopy_send_server is not None:
        params['enable_zerocopy_send_server'] = enable_zerocopy_send_server
    if enable_zerocopy_send_client is not None:
        params['enable_zerocopy_send_client'] = enable_zerocopy_send_client

    return client.call('sock_impl_set_options', params)


def sock_set_default_impl(client, impl_name=None):
    """Set the default socket implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
    """
    params = {}

    params['impl_name'] = impl_name

    return client.call('sock_set_default_impl', params)
