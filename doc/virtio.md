# Virtio SCSI driver {#virtio}

# Introduction {#virtio_intro}

Virtio SCSI driver is an initiator for SPDK @ref vhost application. The
driver allows any SPDK app to connect to another SPDK instance exposing
a vhost-scsi device. The driver will enumerate targets on the device (which acts
as a SCSI controller) and create *virtual* bdevs usable by any SPDK application.
Sending an I/O request to the Virtio SCSI bdev will put the request data into
a Virtio queue that is processed by the host SPDK app exposing the
controller. The host, after sending I/O to the real drive, will put the response
back into the Virtio queue. Then, the response is received by the Virtio SCSI
driver.

Virtio SCSI driver supports two different usage models:
* PCI - This is the standard mode of operation when used in a guest virtual
machine, where QEMU has presented the virtio-scsi controller as a virtual
PCI device.
* User vhost - Can be used to connect to a vhost-scsi socket directly on the
same host.

The driver, just like the SPDK @ref vhost, is using pollers instead of standard
interrupts to check for an I/O response. If used inside a VM, it bypasses interrupt
and context switching overhead of QEMU and guest kernel, significantly boosting
the overall I/O performance.

# Limitations {#virtio_limitations}

Current Virtio-SCSI implementation has a couple of limitations:
 * supports only up to 8 hugepages (implies only 1GB sized pages are practical)
 * single LUN per target
 * only SPDK vhost-scsi controllers supported
