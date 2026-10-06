`spdk_nvme_ctrlr_get_next_active_ns()` now returns the next active NSID even if `prev_nsid` is no
longer tracked by the controller.
