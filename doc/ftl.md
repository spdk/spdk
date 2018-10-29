# Flash Translation Layer {#ftl}

The Flash Translation Layer library provides block device access on top of non-block SSDs
implementing Open Channel interface. It handles the logical to physical address mapping, responds to
the asynchronous media management events, and manages the defragmentation process.

# Terminology {#ftl_terminology}

## Logical to physical address map

 * Shorthand: L2P

Contains the mapping of the logical addresses (LBA) to their on-disk physical location (PPA). The
LBAs are contiguous and in range from 0 to the number of surfaced blocks (the number of spare blocks
are calculated during device formation and are subtracted from the available address space). The
spare blocks account for chunks going offline throughout the lifespan of the device as well as
provide necessary buffer for data [defragmentation](#ftl_reloc).

## Band {#ftl_band}

Band describes a collection of chunks, each belonging to a different parallel unit. All writes to
the band follow the same pattern - a batch of logical blocks is written to one chunk, another batch
to the next one and so on. This ensures the parallelism of the write operations, as they can be
executed independently on a different chunks. Each band keeps track of the LBAs it consists of, as
well as their validity, as some of the data will be invalidated by subsequent writes to the same
logical address. The L2P mapping can be restored from the SSD by reading this information in order
from the oldest band to the youngest.

             +--------------+        +--------------+                        +--------------+
    band 1   |   chunk 1    +--------+     chk 1    +---- --- --- --- --- ---+     chk 1    |
             +--------------+        +--------------+                        +--------------+
    band 2   |   chunk 2    +--------+     chk 2    +---- --- --- --- --- ---+     chk 2    |
             +--------------+        +--------------+                        +--------------+
    band 3   |   chunk 3    +--------+     chk 3    +---- --- --- --- --- ---+     chk 3    |
             +--------------+        +--------------+                        +--------------+
             |     ...      |        |     ...      |                        |     ...      |
             +--------------+        +--------------+                        +--------------+
    band m   |   chunk m    +--------+     chk m    +---- --- --- --- --- ---+     chk m    |
             +--------------+        +--------------+                        +--------------+
             |     ...      |        |     ...      |                        |     ...      |
             +--------------+        +--------------+                        +--------------+

              parallel unit 1              pu 2                                    pu n

The address map and valid map are, along with a several other things (e.g. UUID of the device it's
part of, number of surfaced LBAs, band's sequence number, etc.), parts of the band's metadata. The
metadata is split in two parts:
 * the head part, containing information already known when opening the band (device's UUID, band's
   sequence number, etc.), located at the beginning blocks of the band,
 * the tail part, containing the address map and the valid map, located at the end of the band.


       head metadata               band's data               tail metadata
    +-------------------+-------------------------------+----------------------+
    |chk 1|...|chk n|...|...|chk 1|...|                 | ... |chk  m-1 |chk  m|
    |lbk 1|   |lbk 1|   |   |lbk x|   |                 |     |lblk y   |lblk y|
    +-------------------+-------------+-----------------+----------------------+


Bands are being written sequentially (in a way that was described earlier). Before a band can be
written to, all of its chunks need to be erased. During that time, the band is considered to be in a
`PREP` state. After that is done, the band transitions to the `OPENING` state, in which head metadata
is being written. Then the band moves to the `OPEN` state and actual user data can be written to the
band. Once the whole available space is filled, tail metadata is written and the band transitions to
`CLOSING` state. When that finishes the band becomes `CLOSED`.

## Ring write buffer {#ftl_rwb}

 * Shorthand: RWB

Because the smallest write size the SSD may support can be a multiple of block size, in order to
support writes to a single block, the data needs to be buffered. The write buffer is the solution to
this problem. It consists of a number of pre-allocated buffers called batches, each of size allowing
for a single transfer to the SSD. A single batch is divided into block-sized buffer entries.

                 write buffer
    +-----------------------------------+
    |batch 1                            |
    |   +-----------------------------+ |
    |   |rwb    |rwb    | ... |rwb    | |
    |   |entry 1|entry 2|     |entry n| |
    |   +-----------------------------+ |
    +-----------------------------------+
    | ...                               |
    +-----------------------------------+
    |batch m                            |
    |   +-----------------------------+ |
    |   |rwb    |rwb    | ... |rwb    | |
    |   |entry 1|entry 2|     |entry n| |
    |   +-----------------------------+ |
    +-----------------------------------+

When a write is scheduled, it needs to acquire an entry for each of its blocks and copy the data
onto this buffer. Once all blocks are copied, the write can be signalled as completed to the user.
In the meantime, the `rwb` is polled for filled batches and, if one is found, it's sent to the SSD.
After that operation is completed the whole batch can be freed. For the whole time the data is in
the `rwb`, the L2P points at the buffer entry instead of a location on the SSD. This allows for
servicing read requests from the buffer.

## Defragmentation and relocation {#ftl_reloc}

 * Shorthand: defrag, reloc

Since a write to the same LBA invalidates its previous physical location, some of the blocks on a
band might contain old data that basically wastes space. As there is no way to overwrite an already
written block, this data will stay there until the whole chunk is reset. This might create a
situation in which all of the bands contain some valid data and no band can be erased, so no writes
can be executed anymore. Therefore a mechanism is needed to move valid data and invalidate whole
bands, so that they can be reused.

                    band                                             band
    +-----------------------------------+            +-----------------------------------+
    | ** *    * ***      *    *** * *   |            |                                   |
    |**  *       *    *    * *     *   *|   +---->   |                                   |
    |*     ***  *      *            *   |            |                                   |
    +-----------------------------------+            +-----------------------------------+

Valid blocks are marked with an asterisk '\*'.

Another reason for data relocation might be an event from the SSD telling us that the data might
become corrupt if it's not relocated. This might happen due to its old age (if it was written a
long time ago) or due to read disturb (media characteristic, that causes corruption of neighbouring
blocks during a read operation).

Module responsible for data relocation is called `reloc`. When a band is chosen for defragmentation
or an ANM (asynchronous NAND management) event is received, the appropriate blocks are marked as
required to be moved. The `reloc` module takes a band that has some of such blocks marked, checks
their validity and, if they're still valid, copies them.

Choosing a band for defragmentation depends on several factors: its valid ratio (1) (proportion of
valid blocks to all user blocks), its age (2) (when was it written) and its write count / wear level
index of its chunks (3) (how many times the band was written to). The lower the ratio (1), the
higher its age (2) and the lower its write count (3), the higher the chance the band will be chosen
for defrag.
