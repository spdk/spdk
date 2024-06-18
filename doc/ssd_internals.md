# NAND Flash SSD Internals {#ssd_internals}

Solid State Devices (SSD) are complex devices and their performance depends on
how they're used. The following description is intended to help software
developers understand what is occurring inside the SSD, so that they can come
up with better software designs. It should not be thought of as a strictly
accurate guide to how SSD hardware really works.

 As of this writing, SSDs are generally implemented on top of
 [NAND Flash](https://en.wikipedia.org/wiki/Flash_memory) memory. At a
 very high level, this media has a few important properties:

* The media is grouped onto chips called NAND dies and each die can
  operate in parallel.
* Flipping a bit is a highly asymmetric process. Flipping it one way is
  easy, but flipping it back is quite hard.

NAND Flash media is grouped into large units often referred to as **erase
blocks**. The size of an erase block is highly implementation specific, but
can be thought of as somewhere between 1MiB and 8MiB. For each erase block,
each bit may be written to (i.e. have its bit flipped from 0 to 1) with
bit-granularity once. In order to write to the erase block a second time, the
entire block must be erased (i.e. all bits in the block are flipped back to
0). This is the asymmetry part from above. Erasing a block causes a measurable
amount of wear and each block may only be erased a limited number of times.

SSDs expose an interface to the host system that makes it appear as if the
drive is composed of a set of fixed size **logical blocks** which are usually
512B or 4KiB in size. These blocks are entirely logical constructs of the
device firmware and they do not statically map to a location on the backing
media. Instead, upon each write to a logical block, a new location on the NAND
Flash is selected and written and the mapping of the logical block to its
physical location is updated. The algorithm for choosing this location is a
key part of overall SSD performance and is often called the **flash
translation layer** or FTL. This algorithm must correctly distribute the
blocks to account for wear (called **wear-leveling**) and spread them across
NAND dies to improve total available performance. The simplest model is to
group all of the physical media on each die together using an algorithm
similar to RAID and then write to that set sequentially. Real SSDs are far
more complicated, but this is an excellent simple model for software
developers - imagine they are simply logging to a RAID volume and updating an
in-memory hash-table.

One consequence of the flash translation layer is that logical blocks do not
necessarily correspond to physical locations on the NAND at all times. In
fact, there is a command that clears the translation for a block. In NVMe,
this command is called deallocate, in SCSI it is called unmap, and in SATA it
is called trim. When a user attempts to read a block that doesn't have a
mapping to a physical location, drives will do one of two things:

1. Immediately complete the read request successfully, without performing any
   data transfer. This is acceptable because the data the drive would return
   is no more valid than the data already in the user's data buffer.
2. Return all 0's as the data.

Choice #1 is much more common and performing reads to a fully deallocated
device will often show performance far beyond what the drive claims to be
capable of precisely because it is not actually transferring any data. Write
to all blocks prior to reading them when benchmarking!

As SSDs are written to, the internal log will eventually consume all of the
available erase blocks. In order to continue writing, the SSD must free some
of them. This process is often called **garbage collection**. All SSDs reserve
some number of erase blocks so that they can guarantee there are free erase
blocks available for garbage collection. Garbage collection generally proceeds
by:

1. Selecting a target erase block (a good mental model is that it picks the least recently used erase block)
2. Walking through each entry in the erase block and determining if it is still a valid logical block.
3. Moving valid logical blocks by reading them and writing them to a different erase block (i.e. the current head of the log)
4. Erasing the entire erase block and marking it available for use.

Garbage collection is clearly far more efficient when step #3 can be skipped
because the erase block is already empty. There are two ways to make it much
more likely that step #3 can be skipped. The first is that SSDs reserve
additional erase blocks beyond their reported capacity (called
**over-provisioning**), so that statistically its much more likely that an
erase block will not contain valid data. The second is software can write to
the blocks on the device in sequential order in a circular pattern, throwing
away old data when it is no longer needed. In this case, the software
guarantees that the least recently used erase blocks will not contain any
valid data that must be moved.

The amount of over-provisioning a device has can dramatically impact the
performance on random read and write workloads, if the workload is filling up
the entire device. However, the same effect can typically be obtained by
simply reserving a given amount of space on the device in software. This
understanding is critical to producing consistent benchmarks. In particular,
if background garbage collection cannot keep up and the drive must switch to
on-demand garbage collection, the latency of writes will increase
dramatically. Therefore the internal state of the device must be forced into
some known state prior to running benchmarks for consistency. This is usually
accomplished by writing to the device sequentially two times, from start to
finish. For a highly detailed description of exactly how to force an SSD into
a known state for benchmarking see this
[SNIA Article](http://www.snia.org/sites/default/files/SSS_PTS_Enterprise_v1.1.pdf).
