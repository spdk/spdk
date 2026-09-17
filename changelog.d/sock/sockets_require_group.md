Every socket must now belong to a socket group. `spdk_sock_connect()` and `spdk_sock_listen()`
require `spdk_sock_opts.group`, and accepted sockets inherit the listener's group. Closing a
socket removes it from its group. `spdk_sock_opts.user_ctx` sets the initial `rx_cb` context.
