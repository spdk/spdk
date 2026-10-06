Added `spdk_nvme_ctrlr_release_inactive_ns()`. It frees the driver's memory for an inactive
namespace during the call. An active namespace, an untracked NSID, a namespace whose Identify is
still in progress, and a controller with another process attached are not freed.
