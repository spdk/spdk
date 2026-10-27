Added `disable_tls_session_tickets` to `struct spdk_sock_impl_opts` and the `sock_impl_set_options`
RPC (`--disable-tls-session-tickets`). When set, no TLS 1.3 `NewSessionTicket` message is sent
after the handshake. It defaults to true for the ssl socket module.
