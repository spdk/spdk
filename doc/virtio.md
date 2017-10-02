# Virtio SCSI driver {#virtio}

# Introduction {#virtio_intro}

Virtio SCSI driver is an initiator for SPDK @ref vhost application. The
driver allows any SPDK app to connect to other SPDK instance exposing
a vhost-scsi device. The driver will enumerate targets on the device (which acts
as a SCSI controller) and create *virtual* bdevs usable by any SPDK application.
Sending an I/O request to the Virtio SCSI bdev will put the request data into
a Virtio queue that is being processed by the host SPDK app exposing the
controller. The host after sending I/O to the real drive, will put the response
back into the Virtio queue. Then, the response is received by the Virtio SCSI
driver.

The driver, just like the SPDK @ref vhost, is using pollers instead of standard
interrupts to check for I/O response. It bypasses kernel interrupt and context
switching overhead of QEMU and guest kernel, significantly boosting the overall
I/O performance.

Virtio SCSI driver supports two different usage models:
* PCI - This is the standard mode of operation when used in a guest virtual
machine, where QEMU has presented the virtio-scsi controller as a virtual
PCI device.
* User vhost - Can be used to connect to an vhost-scsi socket directly on the
same host.

# Multiqueue {#virtio_multiqueue}

The Virtio SCSI controller will automatically manage virtio queues distribution.
Currently each thread doing an I/O on a single bdev will get an exclusive queue.
Multi-threaded I/O on bdevs from a single Virtio-SCSI controller is not
possible yet.

# Limitations {#virtio_limitations}

The Virtio SCSI driver is still experimental.  Current implementation has many
limitations:
 * only up to 8 hugepages, this makes the driver practically only work
with 1GB hugepages.
 * single LUN per target
 * only SPDK vhost-scsi controllers supported
 * no RPC
 * no multi-threaded I/O for single-queue virtio devices
