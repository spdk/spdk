# What is SPDK {#about}

The Storage Performance Development Kit (SPDK) provides a set of tools and
libraries for writing high performance, scalable, user-mode storage
applications. It achieves high performance through the use of a number of key
techniques:

* Moving all of the necessary drivers into userspace, which avoids syscalls
  and enables zero-copy access from the application.
* Polling hardware for completions instead of relying on interrupts, which
  lowers both total latency and latency variance.
* Avoiding all locks in the I/O path, instead relying on message passing.

The bedrock of SPDK is a user space, polled-mode, asynchronous, lockless
[NVMe](http://www.nvmexpress.org) driver. This provides zero-copy, highly
parallel access directly to an SSD from a user space application. The driver is
written as a C library with a single public header. See @ref nvme for more
details.

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
high performance storage target, or used as the basis for production
deployments.
