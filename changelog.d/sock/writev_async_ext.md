Added `spdk_sock_writev_async_ext()`, which accepts `struct spdk_sock_request_opts` to describe
the memory domain of the request payload, and `spdk_sock_get_memory_domain()`, which returns the
socket's memory domain, or NULL if the sock module doesn't support memory domains.
