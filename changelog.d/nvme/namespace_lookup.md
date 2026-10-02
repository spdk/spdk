`spdk_nvme_ctrlr_get_ns()` now only looks up a namespace the controller is already tracking. It no
longer allocates a namespace object for an in-range NSID that Identify has not reported. Namespace
objects are created internally when a new NSID is identified.
