# iSCSI Hotplug {#iscsi_hotplug}

At the iSCSI level, we provide the following support for Hotplug:

1. bdev/nvme:
At the bdev/nvme level, we start one hotplug monitor which will call
spdk_nvme_probe() periodically to get the hotplug events. We provide the
private attach_cb and remove_cb for spdk_nvme_probe(). For the attach_cb,
we will create the block device base on the NVMe device attached, and for the
remove_cb, we will unregister the block device, which will also notify the
upper level stack (for iSCSI target, the upper level stack is scsi/lun) to
handle the hot-remove event.

2. scsi/lun:
When the LUN receive the hot-remove notification from block device layer,
the LUN will be marked as removed, and all the IOs after this point will
return with check condition status. Then the LUN starts one poller which will
wait for all the commands which have already been submitted to block device to
return back; after all the commands return back, the LUN will be deleted.

@sa spdk_nvme_probe
