def sock_impl_get_options(client, impl_name=None):
    """Get parameters for the socket layer implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
    """
    params = {}

    params['impl_name'] = impl_name

    return client.call('sock_impl_get_options', params)


def sock_impl_set_options(client, impl_name=None, recv_buf_size=None, send_buf_size=None):
    """Set parameters for the socket layer implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
        recv_buf_size: size of socket receive buffer in bytes (optional)
        send_buf_size: size of socket send buffer in bytes (optional)
    """
    params = {}

    params['impl_name'] = impl_name
    if recv_buf_size is not None:
        params['recv_buf_size'] = recv_buf_size
    if send_buf_size is not None:
        params['send_buf_size'] = send_buf_size

    return client.call('sock_impl_set_options', params)
