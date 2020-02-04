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

# Usage {#ftl_usage}

## Prerequisites {#ftl_prereq}

In order to use the FTL module, an Open Channel SSD is required. The easiest way to obtain one is to
emulate it using QEMU. The QEMU with the patches providing Open Channel support can be found on the
SPDK's QEMU fork on [spdk-3.0.0](https://github.com/spdk/qemu/tree/spdk-3.0.0) branch.

## Configuring QEMU {#ftl_qemu_config}

To emulate an Open Channel device, QEMU expects parameters describing the characteristics and
geometry of the SSD:

 - `serial` - serial number,
 - `lver` - version of the OCSSD standard (0 - disabled, 1 - "1.2", 2 - "2.0"), libftl only supports
   2.0,
 - `lba_index` - default LBA format. Possible values (libftl only supports lba_index >= 3):
        |lba_index| data| metadata|
        |---------|-----|---------|
        |    0    | 512B|    0B   |
        |    1    | 512B|    8B   |
        |    2    | 512B|   16B   |
        |    3    |4096B|    0B   |
        |    4    |4096B|   64B   |
        |    5    |4096B|  128B   |
        |    6    |4096B|   16B   |
 - `lnum_ch` - number of groups,
 - `lnum_lun` - number of parallel units
 - `lnum_pln` - number of planes (logical blocks from all planes constitute a chunk)
 - `lpgs_per_blk` - number of pages (smallest programmable unit) per chunk
 - `lsecs_per_pg` - number of sectors in a page
 - `lblks_per_pln` - number of chunks in a parallel unit
 - `laer_thread_sleep` - timeout in ms between asynchronous events requesting the host to relocate
   the data based on media feedback
 - `lmetadata` - metadata file

For more detailed description of the available options, consult the `hw/block/nvme.c` file in
the QEMU repository.

Example:

```
$ /path/to/qemu [OTHER PARAMETERS] -drive format=raw,file=/path/to/data/file,if=none,id=myocssd0
        -device nvme,drive=myocssd0,serial=deadbeef,lver=2,lba_index=3,lnum_ch=1,lnum_lun=8,lnum_pln=4,
        lpgs_per_blk=1536,lsecs_per_pg=4,lblks_per_pln=512,lmetadata=/path/to/md/file
```

In the above example, a device is created with 1 channel, 8 parallel units, 512 chunks per parallel
unit, 24576 (`lnum_pln` * `lpgs_per_blk` * `lsecs_per_pg`) logical blocks in each chunk with logical
block being 4096B. Therefore the data file needs to be at least 384G (8 * 512 * 24576 * 4096B) of
size and can be created with the following command:

```
$ fallocate -l 384G /path/to/data/file
```

## Configuring SPDK {#ftl_spdk_config}

To verify that the drive is emulated correctly, one can check the output of the NVMe identify app
(assuming that `scripts/setup.sh` was called before and the driver has been changed for that
device):

```
$ examples/nvme/identify/identify
=====================================================
NVMe Controller at 0000:00:0a.0 [1d1d:1f1f]
=====================================================
Controller Capabilities/Features
================================
Vendor ID:                             1d1d
Subsystem Vendor ID:                   1af4
Serial Number:                         deadbeef
Model Number:                          QEMU NVMe Ctrl

... other info ...

Namespace OCSSD Geometry
=======================
OC version: maj:2 min:0

... other info ...

Groups (channels): 1
PUs (LUNs) per group: 8
Chunks per LUN: 512
Logical blks per chunk: 24576

... other info ...

```

Similarly to other bdevs, the FTL bdevs can be created either based on config files or via RPC. Both
interfaces require the same arguments which are described by the `--help` option of the
`bdev_ftl_create` RPC call, which are:

 - bdev's name
 - transport type of the device (e.g. PCIe)
 - transport address of the device (e.g. `00:0a.0`)
 - parallel unit range
 - UUID of the FTL device (if the FTL is to be restored from the SSD)

Example config:

```
[Ftl]
 TransportID "trtype:PCIe traddr:00:0a.0" nvme0 "0-3" 00000000-0000-0000-0000-000000000000
 TransportID "trtype:PCIe traddr:00:0a.0" nvme1 "4-5" e9825835-b03c-49d7-bc3e-5827cbde8a88
```

The above will result in creation of two devices:

 - `nvme0` on `00:0a.0` using parallel units 0-3, created from scratch
 - `nvme1` on the same device using parallel units 4-5, restored from the SSD using the UUID
   provided

The same can be achieved with the following two RPC calls:

```
$ scripts/rpc.py bdev_ftl_create -b nvme0 -l 0-3 -a 00:0a.0
{
        "name": "nvme0",
        "uuid": "b4624a89-3174-476a-b9e5-5fd27d73e870"
}
$ scripts/rpc.py bdev_ftl_create -b nvme1 -l 0-3 -a 00:0a.0 -u e9825835-b03c-49d7-bc3e-5827cbde8a88
{
        "name": "nvme1",
        "uuid": "e9825835-b03c-49d7-bc3e-5827cbde8a88"
}
```
