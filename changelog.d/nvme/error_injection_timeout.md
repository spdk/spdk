Requests held by `spdk_nvme_qpair_add_cmd_error_injection()` with `do_not_submit` set are now
checked against the timeouts registered with `spdk_nvme_ctrlr_register_timeout_callback()`.
The timeout callback receives a CID of 0xFFFF for every held request. This allows
`bdev_nvme_add_error_injection` to trigger the `bdev_nvme_set_options` `action_on_timeout=reset`
handling on any transport, including PCIe. With `action_on_timeout=abort`, the abort of a held
request always fails and falls back to a controller reset.
