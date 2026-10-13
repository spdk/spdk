In interrupt mode on fabrics transports, `spdk_nvme_ctrlr_get_admin_qp_fd()` now returns an fd
that stays valid across controller resets and reconnects. Previously it returned the transport fd
of the admin queue, which was closed and recreated on every reconnect.
