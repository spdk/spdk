Added the `xlio` sock implementation, enabled with `--with-xlio`. It uses the XLIO ULTRA API of
[libxlio](https://github.com/mellanox/libxlio), a userspace TCP stack, and supports zero-copy
receive through `spdk_sock_recv_next()` and memory domains through `spdk_sock_writev_async_ext()`.
