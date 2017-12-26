# What is SPDK? {#about}

The Storage Performance Development Kit (SPDK) provides a set of tools and
libraries for direct access to storage devices from within an application,
bypassing the operating system kernel. These libraries are designed to provide
the highest performance and best scalability possible. In the most general
sense, this is achieved by relying on polling instead of interrupts and message
passing instead of locking.

The bedrock of SPDK is a user space, polled-mode, asynchronous, lockless
[NVMe](http://www.nvmexpress.org) driver. This provides zero-copy, highly
parallel access directly to an SSD from a user space application. The driver is
written as a C library with a single public header. See @ref nvme for more
details. Similarly, SPDK provides a user space driver for the I/OAT
[DMA](https://en.wikipedia.org/wiki/Direct_memory_access) engine present on
many Intel Xeon-based platforms with all of the same properties as the NVMe
driver. See @ref ioat for more details.

SPDK further provides a full block stack as a user space library that performs
many of the same operations as a block stack in an operating system. This
includes unifying the interface between disparate storage devices, queueing to
handle conditions such as out of memory or I/O hangs, and logical volume
management. See @ref bdev for more information.

Finally, SPDK provides
[NVMe-oF](http://www.nvmexpress.org/nvm-express-over-fabrics-specification-released),
[iSCSI](https://en.wikipedia.org/wiki/ISCSI), and
[vhost](http://blog.vmsplice.net/2011/09/qemu-internals-vhost-architecture.html)
servers built on top of these components that are capable of serving disks over
the network or to other processes. The standard Linux kernel initiators for
NVMe-oF and iSCSI interoperate with these targets, as well as QEMU with vhost.
These servers can be up to an order of magnitude more CPU efficient than other
implementations. These targets can be used as examples of how to implement a
high performance storage target, or used as-is in production deployments.
