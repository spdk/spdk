# User Space Drivers {#userspace}

# Controlling Hardware From User Space {#userspace_control}

Much of the documentation for SPDK talks about _user space drivers_, so it's
important to understand what that really means at a technical level. First and
foremost, a _driver_ is software that directly controls a particular device
attached to a computer. Second, operating systems divide the system's virtual
memory into two address spaces - 
[kernel space and user space](https://en.wikipedia.org/wiki/User_space). This
separation is aided by features on the CPU itself that enforce memory separation
called [protection rings](https://en.wikipedia.org/wiki/Protection_ring).
Typically, drivers run in kernel space which is in ring 0. SPDK contains drivers
that instead are designed to run in user space, but they still interface directly
with the hardware device that they are controlling.

In order for SPDK to take control of a device, it must first instruct the
operating system to relinquish control. This is often referred to as unbinding
the kernel driver from the device and on Linux is done by
[writing to a file in sysfs](https://lwn.net/Articles/143397/).
SPDK then rebinds the driver to one of two special device drivers that come
bundled with Linux -
[uio](https://www.kernel.org/doc/html/latest/driver-api/uio-howto.html) or
[vfio](https://www.kernel.org/doc/Documentation/vfio.txt). These two drivers
are "dummy" drivers in the sense that they mostly indicate to the operating
system that the device has a driver bound to it so it won't automatically try
to re-bind the default driver. They don't actually initialize the hardware in
any way, nor do they even understand what type of device it is. The primary
difference between uio and vfio is that vfio is capable of programming the
platform's
[IOMMU](https://en.wikipedia.org/wiki/Input%E2%80%93output_memory_management_unit),
which is a critical piece of hardware for ensuring memory safety in user space
drivers. See @ref memory for full details.

Once the device is unbound from the operating system kernel, that means the
operating system can't use it anymore. For example, if you unbind an NVMe
device on Linux, the devices corresponding to it such as /dev/nvme0n1 will
disappear. It further means that filesystems mounted on the device will also be
removed and kernel filesystems can no longer interact with the device. In fact,
the entire kernel block storage stack is bypassed. Instead, SPDK provides
re-imagined implementations of most of the layers in a typical operating system
storage stack all as C libraries that can be directly embedded into your
application. This includes a @ref bdev primarily, but also block allocators and
filesystem-like components such as @ref blob and @ref blobfs.

User space drivers utilize features in uio or vfio to map the
[PCI BAR](https://en.wikipedia.org/wiki/PCI_configuration_space) for the device
into the current process, which allows the driver to perform
[MMIO](https://en.wikipedia.org/wiki/Memory-mapped_I/O) directly. The SPDK @ref
nvme, for instance, maps the BAR for the NVMe device and then follows along
with the
[NVMe Specification](http://nvmexpress.org/wp-content/uploads/NVM_Express_Revision_1.3.pdf)
to initialize the device, create queue pairs, and ultimately send I/O.

# Interrupts {#userspace_interrupts}

SPDK elects to disable interrupts on the devices it controls and instead to
poll them for completions. There are a number of reasons for doing this; 1)
practically speaking, routing an interrupt to a handler in a user space process
just isn't feasible for most hardware designs, 2) interrupts introduce software
jitter and have significant overhead due to forced context switches. Operations
in SPDK are almost universally asynchronous and allow the user to provide a
callback on completion. The callback is called in response to the user calling
a function to poll for completions. Polling, at least for NVMe, is fast because
only host memory needs to be read (no MMIO) to check a queue pair for a bit
flip, and
[DDIO](https://www.intel.com/content/www/us/en/io/data-direct-i-o-technology.html)
will ensure that the host memory being checked is present in the CPU cache
after an update by the device.

# Threading {#userspace_threading}

Both NVMe and I/OAT devices expose queues for submitting requests to the
hardware. Separate queues can be accessed without coordination, so software can
send requests to the device from multiple threads of execution in parallel
without locks. Unfortunatley, kernel drivers must be designed to handle I/O
coming from lots of different places either in the operating system or in
various processes on the system, and the thread topology of those processes
changes over time. Most kernel drivers elect to map hardware queues to cores
(as close as 1:1 as possible), and then when a request is submitted look up the
correct hardware queue for whatever core the current thread happens to be
running on. This is a large improvement from older hardware interfaces that
only had a single queue or no queue at all, but still isn't always perfectly
optimal.

A user space driver, on the other hand, is embedded in exactly one process and
has exactly one user. This user also happens to know exactly how many threads
exist (because they created them). Therefore, the SPDK drivers choose to expose
the hardware queues directly to the user application with the requirement that
a hardware queue is only ever accessed from one thread at a time. In practice,
applications assign one hardware queue to each thread. That guarantees that the
thread can submit requests without having to perform any sort of coordination
(i.e. locking) with the other threads in the system.
