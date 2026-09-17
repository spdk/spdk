Removed the deprecated `cb_arg` parameter from `spdk_sock_group_add_sock()`. Use
`spdk_sock_set_user_ctx()` to set a socket's callback context.
