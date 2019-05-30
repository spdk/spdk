# SPDK "Reduce" Block Compression Algorithm {#reduce}

## Overview

The SPDK "reduce" block compression scheme is based on using SSDs for storing compressed blocks of
storage and persistent memory for metadata.  This metadata includes mappings of logical blocks
requested by a user to the compressed blocks on SSD.  The scheme described in this document
is generic and not tied to any specific block device framework such as the SPDK block device (bdev)
framework.  This algorithm will be implemented in a library called "libreduce".  Higher-level
software modules can built on top of this library to create and present block devices in a
specific block device framework.  For SPDK, a bdev_reduce module will serve as a wrapper around
the libreduce library, to present the compressed block devices as an SPDK bdev.

This scheme only describes how compressed blocks are stored on an SSD and the metadata for tracking
those compressed blocks.  It relies on the higher-software module to perform the compression
algorithm itself.  For SPDK, the bdev_reduce module will utilize the DPDK compressdev framework
to perform compression and decompression on behalf of the libreduce library.

(Note that in some cases, blocks of storage may not be compressible, or cannot be compressed enough
to realize savings from the compression.  In these cases, the data may be stored uncompressed on
disk.  The phrase "compressed blocks of storage" includes these uncompressed blocks.)

A compressed block device is a logical entity built on top of a similarly-sized backing storage
device.  The backing storage device must be thin-provisioned to realize any savings from
compression for reasons described later in this document.  This algorithm has no direct knowledge
of the implementation of the backing storage device, except that it will always use the
lowest-numbered blocks available on the backing storage device.  This will ensure that when this
algorithm is used on a thin-provisioned backing storage device, blocks will not be allocated until
they are actually needed.

The backing storage device must be sized for the worst case scenario, where no data can be
compressed.  In this case, the size of the backing storage device would be the same as the
compressed block device.  Since this algorithm ensures atomicity by never overwriting data
in place, some additional backing storage is required to temporarily store data for writes in
progress before the associated metadata is updated.

Storage from the backing storage device will be allocated, read, and written to in 4KB units for
best NVMe performance.  These 4KB units are called "backing IO units".  They are indexed from 0 to N-1
with the indices called "backing IO unit indices".  At start, the full set of indices represent the
"free backing IO unit list".

A compressed block device compresses and decompresses data in units of chunks, where a chunk is a
multiple of at least two 4KB backing IO units.  The number of backing IO units per chunk determines
the chunk size and is specified when the compressed block device is created.  A chunk
consumes a number of 4KB backing IO units between 1 and the number of 4KB units in the chunk.  For
example, a 16KB chunk consumes 1, 2, 3 or 4 backing IO units.  The number of backing IO units depends on how
much the chunk was able to be compressed.  The blocks on disk associated with a chunk are stored in a
"chunk map" in persistent memory.  Each chunk map consists of N 64-bit values, where N is the maximum
number of backing IO units in the chunk.  Each 64-bit value corresponds to a backing IO unit index.  A
special value (for example, 2^64-1) is used for backing IO units not needed due to compression.  The
number of chunk maps allocated is equal to the size of the compressed block device divided by its chunk
size, plus some number of extra chunk maps.  These extra chunk maps are used to ensure atomicity on
writes and will be explained later in this document.  At start, all of the chunk maps represent the
"free chunk map list".

Finally, the logical view of the compressed block device is represented by the "logical map".  The
logical map is a mapping of chunk offsets into the compressed block device to the corresponding
chunk map.  Each entry in the logical map is a 64-bit value, denoting the associated chunk map.
A special value (UINT64_MAX) is used if there is no associated chunk map.  The mapping is
determined by dividing the byte offset by the chunk size to get an index, which is used as an
array index into the array of chunk map entries.  At start, all entries in the logical map have no
associated chunk map.  Note that while access to the backing storage device is in 4KB units, the
logical view may allow 4KB or 512B unit access and should perform similarly.

## Example

To illustrate this algorithm, we will use a real example at a very small scale.

The size of the compressed block device is 64KB, with a chunk size of 16KB.  This will
realize the following:

* "Backing storage" will consist of an 80KB thin-provisioned logical volume.  This
  corresponds to the 64KB size of the compressed block device, plus an extra 16KB to handle
  additional write operations under a worst-case compression scenario.
* "Free backing IO unit list" will consist of indices 0 through 19 (inclusive).  These represent
  the 20 4KB IO units in the backing storage.
* A "chunk map" will be 32 bytes in size.  This corresponds to 4 backing IO units per chunk
  (16KB / 4KB), and 8B (64b) per backing IO unit index.
* 5 chunk maps will be allocated in 160B of persistent memory.  This corresponds to 4 chunk maps
  for the 4 chunks in the compressed block device (64KB / 16KB), plus an extra chunk map for use
  when overwriting an existing chunk.
* "Free chunk map list" will consist of indices 0 through 4 (inclusive).  These represent the
  5 allocated chunk maps.
* The "logical map" will be allocated in 32B of persistent memory.  This corresponds to
  4 entries for the 4 chunks in the compressed block device and 8B (64b) per entry.

In these examples, the value "X" will represent the special value (2^64-1) described above.

### Initial Creation

```
                  +--------------------+
  Backing Device  |                    |
                  +--------------------+

  Free Backing IO Unit List  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19

             +------------+------------+------------+------------+------------+
  Chunk Maps |            |            |            |            |            |
             +------------+------------+------------+------------+------------+

  Free Chunk Map List  0, 1, 2, 3, 4

              +---+---+---+---+
  Logical Map | X | X | X | X |
              +---+---+---+---+
```

### Write 16KB at Offset 32KB

* Find the corresponding index into the logical map.  Offset 32KB divided by the chunk size
  (16KB) is 2.
* Entry 2 in the logical map is "X".  This means no part of this 16KB has been written to yet.
* Allocate a 16KB buffer in memory
* Compress the incoming 16KB of data into this allocated buffer
* Assume this data compresses to 6KB.  This requires 2 4KB backing IO units.
* Allocate 2 blocks (0 and 1) from the free backing IO unit list.  Always use the lowest numbered
  entries in the free backing IO unit list - this ensures that unnecessary backing storage
  is not allocated in the thin-provisioned logical volume holding the backing storage.
* Write the 6KB of data to backing IO units 0 and 1.
* Allocate a chunk map (0) from the free chunk map list.
* Write (0, 1, X, X) to the chunk map.  This represents that only 2 backing IO units were used to
  store the 16KB of data.
* Write the chunk map index to entry 2 in the logical map.

```
                  +--------------------+
  Backing Device  |01                  |
                  +--------------------+

  Free Backing IO Unit List  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19

             +------------+------------+------------+------------+------------+
  Chunk Maps | 0 1 X X    |            |            |            |            |
             +------------+------------+------------+------------+------------+

  Free Chunk Map List  1, 2, 3, 4

              +---+---+---+---+
  Logical Map | X | X | 0 | X |
              +---+---+---+---+
```

### Write 4KB at Offset 8KB

* Find the corresponding index into the logical map.  Offset 8KB divided by the chunk size is 0.
* Entry 0 in the logical map is "X".  This means no part of this 16KB has been written to yet.
* The write is not for the entire 16KB chunk, so we must allocate a 16KB chunk-sized buffer for
  source data.
* Copy the incoming 4KB data to offset 8KB of this 16KB buffer.  Zero the rest of the 16KB buffer.
* Allocate a 16KB destination buffer.
* Compress the 16KB source data buffer into the 16KB destination buffer
* Assume this data compresses to 3KB.  This requires 1 4KB backing IO unit.
* Allocate 1 block (2) from the free backing IO unit list.
* Write the 3KB of data to block 2.
* Allocate a chunk map (1) from the free chunk map list.
* Write (2, X, X, X) to the chunk map.
* Write the chunk map index to entry 0 in the logical map.

```
                  +--------------------+
  Backing Device  |012                 |
                  +--------------------+

  Free Backing IO Unit List  3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19

             +------------+------------+------------+------------+------------+
  Chunk Maps | 0 1 X X    | 2 X X X    |            |            |            |
             +------------+------------+------------+------------+------------+

  Free Chunk Map List  2, 3, 4

              +---+---+---+---+
  Logical Map | 1 | X | 0 | X |
              +---+---+---+---+
```

### Read 16KB at Offset 16KB

* Offset 16KB maps to index 1 in the logical map.
* Entry 1 in the logical map is "X".  This means no part of this 16KB has been written to yet.
* Since no data has been written to this chunk, return all 0's to satisfy the read I/O.

### Write 4KB at Offset 4KB

* Offset 4KB maps to index 0 in the logical map.
* Entry 0 in the logical map is "1".  Since we are not overwriting the entire chunk, we must
  do a read-modify-write.
* Chunk map 1 only specifies one backing IO unit (2).  Allocate a 16KB buffer and read block
  2 into it.  This will be called the compressed data buffer.  Note that 16KB is allocated
  instead of 4KB so that we can reuse this buffer to hold the compressed data that will
  be written later back to disk.
* Allocate a 16KB buffer for the uncompressed data for this chunk.  Decompress the data from
  the compressed data buffer into this buffer.
* Copy the incoming 4KB of data to offset 4KB of the uncompressed data buffer.
* Compress the 16KB uncompressed data buffer into the compressed data buffer.
* Assume this data compresses to 5KB.  This requires 2 4KB backing IO units.
* Allocate blocks 3 and 4 from the free backing IO unit list.
* Write the 5KB of data to blocks 3 and 4.
* Allocate chunk map 2 from the free chunk map list.
* Write (3, 4, X, X) to chunk map 2.  Note that at this point, the chunk map is not referenced
  by the logical map.  If there was a power fail at this point, the previous data for this chunk
  would still be fully valid.
* Write chunk map 2 to entry 0 in the logical map.
* Free chunk map 1 back to the free chunk map list.
* Free backing IO unit 2 back to the free backing IO unit list.

```
                  +--------------------+
  Backing Device  |01 34               |
                  +--------------------+

  Free Backing IO Unit List  2, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19

             +------------+------------+------------+------------+------------+
  Chunk Maps | 0 1 X X    |            | 3 4 X X    |            |            |
             +------------+------------+------------+------------+------------+

  Free Chunk Map List  1, 3, 4

              +---+---+---+---+
  Logical Map | 2 | X | 0 | X |
              +---+---+---+---+
```

### Operations that span across multiple chunks

Operations that span a chunk boundary are logically split into multiple operations, each of
which is associated with a single chunk.

Example: 20KB write at offset 4KB

In this case, the write operation is split into a 12KB write at offset 4KB (affecting only
chunk 0 in the logical map) and a 8KB write at offset 16KB (affecting only chunk 1 in the
logical map).  Each write is processed independently using the algorithm described above.
Completion of the 20KB write does not occur until both operations have completed.

### Unmap Operations

Unmap operations on an entire chunk are achieved by removing the chunk map entry (if any) from
the logical map.  The chunk map is returned to the free chunk map list, and any backing IO units
associated with the chunk map are returned to the free backing IO unit list.

Unmap operations that affect only part of a chunk can be treated as writing zeroes to that
region of the chunk.  If the entire chunk is unmapped via several operations, it can be
detected via the uncompressed data equaling all zeroes.  When this occurs, the chunk map entry
may be removed from the logical map.

After an entire chunk has been unmapped, subsequent reads to the chunk will return all zeroes.
This is similar to the "Read 16KB at offset 16KB" example above.

### Write Zeroes Operations

Write zeroes operations are handled similarly to unmap operations.  If a write zeroes
operation covers an entire chunk, we can remove the chunk's entry in the logical map
completely.  Then subsequent reads to that chunk will return all zeroes.

### Restart

An application using libreduce will periodically exit and need to be restarted.  When the
application restarts, it will reload compressed volumes so they can be used again from the
same state as when the application exited.

When the compressed volume is reloaded, the free chunk map list and free backing IO unit list
are reconstructed by walking the logical map.  The logical map will only point to valid
chunk maps, and the valid chunk maps will only point to valid backing IO units.  Any chunk maps
and backing IO units not referenced go into their respective free lists.

This ensures that if a system crashes in the middle of a write operation - i.e. during or
after a chunk map is updated, but before it is written to the logical map - that everything
related to that in-progress write will be ignored after the compressed volume is restarted.

### Overlapping operations on same chunk

Implementations must take care to handle overlapping operations on the same chunk.  For example,
operation 1 writes some data to chunk A, and while this is in progress, operation 2 also writes
some data to chunk A.  In this case, operation 2 should not start until operation 1 has
completed.  Further optimizations are outside the scope of this document.

### Thin provisioned backing storage

Backing storage must be thin provisioned to realize any savings from compression.  This algorithm
will always use (and reuse) backing IO units available closest to offset 0 on the backing device.
This ensures that even though backing storage device may have been sized similarly to the size of
the compressed volume, storage for the backing storage device will not actually be allocated
until the backing IO units are actually needed.
