# Virtio SCSI driver {#virtio}

# Introduction {#virtio_intro}

Virtio SCSI driver is an initiator for SPDK @ref vhost application. The
driver allows any SPDK app to connect to other SPDK instance exposing
a vhost-scsi controller.  The driver will enumerate targets on the controller
and create *virtual* bdevs usable by any SPDK application (e.g. @ref iscsi,
@ref nvmf, or even yet another @ref vhost).  Sending an I/O request to the
Virtio SCSI bdev will put the request data into a Virtio queue that is being
processed by the host SPDK app exposing the controller. The host, after sending
I/O to the real drive, will put the response back into the Virtio queue. Then,
the response is received by the Virtio SCSI driver.

The driver, just like the SPDK @ref vhost, is using pollers instead of standard
interrupts to check for I/O response. It bypasses QEMU/guest kernel interrupt
and context switching overhead, significantly boosting the overall I/O
performance.

Virtio SCSI driver supports two different usage models:
* PCI - This is the standard mode of operation when used in a guest virtual
machine, where QEMU has presented the virtio-scsi controller as a virtual
PCI device.
* User vhost - Can be used to connect to an vhost-scsi socket directly on the
same host.

# Getting Started {#virtio_getting_started}

Use the following configuration file snippet to enumerate a virtio-scsi PCI
device and present its LUNs as bdevs.

~~~{.sh}
[Virtio]
  Dev Pci
~~~

Use the following configuration file snippet to enumerate an SPDK vhost-scsi
controller and present its LUNs as bdevs.  In this case, the SPDK vhost-scsi
host app has created an SPDK vhost-scsi controller which is accessible through
the /tmp/vhost.0 domain socket.

~~~{.sh}
[Virtio]
  Dev User /tmp/vhost.0
~~~

# Limitations {#virtio_limitations}

The Virtio SCSI driver is still experimental.  Current implementation has many
limitations:
 * only up to 8 hugepages, this makes the driver practically only work
with 1GB hugepages.
 * single LUN per target
 * only SPDK vhost-scsi controllers supported
 * no RPC
 * no multiqueue