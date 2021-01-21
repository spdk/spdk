# Submitting I/O to an NVMe Device {#nvme_spec}

## The NVMe Specification

The NVMe specification describes a hardware interface for interacting with
storage devices. The specification includes network transport definitions for
remote storage as well as a hardware register layout for local PCIe devices.
What follows here is an overview of how an I/O is submitted to a local PCIe
device through SPDK.

NVMe devices allow host software (in our case, the SPDK NVMe driver) to allocate
queue pairs in host memory. The term "host" is used a lot, so to clarify that's
the system that the NVMe SSD is plugged into. A queue pair consists of two
queues - a submission queue and a completion queue. These queues are more
accurately described as circular rings of fixed size entries. The submission
queue is an array of 64 byte command structures, plus 2 integers (head and tail
indices). The completion queue is similarly an array of 16 byte completion
structures, plus 2 integers (head and tail indices). There are also two 32-bit
registers involved that are called doorbells.

An I/O is submitted to an NVMe device by constructing a 64 byte command, placing
it into the submission queue at the current location of the submission queue
tail index, and then writing the new index of the submission queue tail to the
submission queue tail doorbell register. It's actually valid to copy a whole set
of commands into open slots in the ring and then write the doorbell just one
time to submit the whole batch.

There is a very detailed description of the command submission and completion
process in the NVMe specification, which is conveniently available from the main
page over at [NVM Express](https://nvmexpress.org).

Most importantly, the command itself describes the operation and also, if
necessary, a location in host memory containing a descriptor for host memory
associated with the command. This host memory is the data to be written on a
write command, or the location to place the data on a read command. Data is
transferred to or from this location using a DMA engine on the NVMe device.

The completion queue works similarly, but the device is instead the one writing
entries into the ring. Each entry contains a "phase" bit that toggles between 0
and 1 on each loop through the entire ring. When a queue pair is set up to
generate interrupts, the interrupt contains the index of the completion queue
head. However, SPDK doesn't enable interrupts and instead polls on the phase
bit to detect completions. Interrupts are very heavy operations, so polling this
phase bit is often far more efficient.

## The SPDK NVMe Driver I/O Path

Now that we know how the ring structures work, let's cover how the SPDK NVMe
driver uses them. The user is going to construct a queue pair at some early time
in the life cycle of the program, so that's not part of the "hot" path. Then,
they'll call functions like spdk_nvme_ns_cmd_read() to perform an I/O operation.
The user supplies a data buffer, the target LBA, and the length, as well as
other information like which NVMe namespace the command is targeted at and which
NVMe queue pair to use. Finally, the user provides a callback function and
context pointer that will be called when a completion for the resulting command
is discovered during a later call to spdk_nvme_qpair_process_completions().

The first stage in the driver is allocating a request object to track the operation. The
operations are asynchronous, so it can't simply track the state of the request
on the call stack. Allocating a new request object on the heap would be far too
slow, so SPDK keeps a pre-allocated set of request objects inside of the NVMe
queue pair object - `struct spdk_nvme_qpair`. The number of requests allocated to
the queue pair is larger than the actual queue depth of the NVMe submission
queue because SPDK supports a couple of key convenience features. The first is
software queueing - SPDK will allow the user to submit more requests than the
hardware queue can actually hold and SPDK will automatically queue in software.
The second is splitting. SPDK will split a request for many reasons, some of
which are outlined next. The number of request objects is configurable at queue
pair creation time and if not specified, SPDK will pick a sensible number based
on the hardware queue depth.

The second stage is building the 64 byte NVMe command itself. The command is
built into memory embedded into the request object - not directly into an NVMe
submission queue slot. Once the command has been constructed, SPDK attempts to
obtain an open slot in the NVMe submission queue. For each element in the
submission queue an object called a tracker is allocated. The trackers are
allocated in an array, so they can be quickly looked up by an index. The tracker
itself contains a pointer to the request currently occupying that slot. When a
particular tracker is obtained, the command's CID value is updated with the
index of the tracker. The NVMe specification provides that CID value in the
completion, so the request can be recovered by looking up the tracker via the
CID value and then following the pointer.

Once a tracker (slot) is obtained, the data buffer associated with it is
processed to build a PRP list. That's essentially an NVMe scatter gather list,
although it is a bit more restricted. The user provides SPDK with the virtual
address of the buffer, so SPDK has to go do a page table look up to find the
physical address (pa) or I/O virtual addresses (iova) backing that virtual
memory. A virtually contiguous memory region may not be physically contiguous,
so this may result in a PRP list with multiple elements. Sometimes this may
result in a set of physical addresses that can't actually be expressed as a
single PRP list, so SPDK will automatically split the user operation into two
separate requests transparently. For more information on how memory is managed,
see @ref memory.

The reason the PRP list is not built until a tracker is obtained is because the
PRP list description must be allocated in DMA-able memory and can be quite
large. Since SPDK typically allocates a large number of requests, we didn't want
to allocate enough space to pre-build the worst case scenario PRP list,
especially given that the common case does not require a separate PRP list at
all.

Each NVMe command has two PRP list elements embedded into it, so a separate PRP
list isn't required if the request is 4KiB (or if it is 8KiB and aligned
perfectly). Profiling shows that this section of the code is not a major
contributor to the overall CPU use.

With a tracker filled out, SPDK copies the 64 byte command into the actual NVMe
submission queue slot and then rings the submission queue tail doorbell to tell
the device to go process it. SPDK then returns back to the user, without waiting
for a completion.

The user can periodically call `spdk_nvme_qpair_process_completions()` to tell
SPDK to examine the completion queue. Specifically, it reads the phase bit of
the next expected completion slot and when it flips, looks at the CID value to
find the tracker, which points at the request object. The request object
contains a function pointer that the user provided initially, which is then
called to complete the command.

The `spdk_nvme_qpair_process_completions()` function will keep advancing to the
next completion slot until it runs out of completions, at which point it will
write the completion queue head doorbell to let the device know that it can use
the completion queue slots for new completions and return.
