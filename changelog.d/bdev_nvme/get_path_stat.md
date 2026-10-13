Added `spdk_bdev_nvme_get_path_stat()` as a public C-callable equivalent of the
`bdev_nvme_get_path_iostat` JSON-RPC. The caller provides an already-open
`spdk_bdev_desc` and receives an array of public `spdk_bdev_nvme_path_stat`
objects (`trid` and `stat`) in `spdk_bdev_nvme_get_path_stat_cb`. The array is
owned by SPDK and is valid only during the callback. The API requires
`spdk_bdev_nvme_opts.io_path_stat` to be enabled.
