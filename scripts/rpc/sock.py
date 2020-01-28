def sock_get_options(client, impl_name=None):
    """Get parameters for the socket layer implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
    """
    params = {}

    params['impl_name'] = impl_name

    return client.call('sock_get_options', params)


def sock_set_options(client, impl_name=None, recv_pipe_size=None):
    """Set parameters for the socket layer implementation.

    Args:
        impl_name: name of socket implementation, e.g. posix
        recv_pipe_size: size of socket receive pipe in bytes (optional)
    """
    params = {}

    params['impl_name'] = impl_name
    if recv_pipe_size is not None:
        params['recv_pipe_size'] = recv_pipe_size

    return client.call('sock_set_options', params)
