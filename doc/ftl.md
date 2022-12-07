# Flash Translation Layer {#ftl}

The Flash Translation Layer library provides efficient 4K block device access on top of devices
with >4K write unit size (eg. raid5f bdev) or devices with large indirection units (some
capacity-focused NAND drives), which don't handle 4K writes well. It handles the logical to
physical address mapping and manages the garbage collection process.

## Terminology {#ftl_terminology}

### Logical to physical address map {#ftl_l2p}

- Shorthand: `L2P`

Contains the mapping of the logical addresses (LBA) to their on-disk physical location. The LBAs
are contiguous and in range from 0 to the number of surfaced blocks (the number of spare blocks
are calculated during device formation and are subtracted from the available address space). The
spare blocks account for zones going offline throughout the lifespan of the device as well as
provide necessary buffer for data [garbage collection](#ftl_reloc).

Since the L2P would occupy a significant amount of DRAM (4B/LBA for drives smaller than 16TiB,
8B/LBA for bigger drives), FTL will, by default, store only the 2GiB of most recently used L2P
addresses in memory (the amount is configurable), and page them in and out of the cache device
as necessary.

### Band {#ftl_band}

A band describes a collection of zones, each belonging to a different parallel unit. All writes to
a band follow the same pattern - a batch of logical blocks is written to one zone, another batch
to the next one and so on. This ensures the parallelism of the write operations, as they can be
executed independently on different zones. Each band keeps track of the LBAs it consists of, as
well as their validity, as some of the data will be invalidated by subsequent writes to the same
logical address. The L2P mapping can be restored from the SSD by reading this information in order
from the oldest band to the youngest.

```text
             +--------------+        +--------------+                        +--------------+
    band 1   |   zone 1     +--------+    zone 1    +---- --- --- --- --- ---+     zone 1   |
             +--------------+        +--------------+                        +--------------+
    band 2   |   zone 2     +--------+     zone 2   +---- --- --- --- --- ---+     zone 2   |
             +--------------+        +--------------+                        +--------------+
    band 3   |   zone 3     +--------+     zone 3   +---- --- --- --- --- ---+     zone 3   |
             +--------------+        +--------------+                        +--------------+
             |     ...      |        |     ...      |                        |     ...      |
             +--------------+        +--------------+                        +--------------+
    band m   |   zone m     +--------+     zone m   +---- --- --- --- --- ---+     zone m   |
             +--------------+        +--------------+                        +--------------+
             |     ...      |        |     ...      |                        |     ...      |
             +--------------+        +--------------+                        +--------------+

              parallel unit 1              pu 2                                    pu n
```

The address map (`P2L`) is saved as a part of the band's metadata, at the end of each band:

```text
                        band's data                        tail metadata
    +-------------------+-------------------------------+------------------------+
    |zone 1 |...|zone n |...|...|zone 1 |...|           | ... |zone  m-1 |zone  m|
    |block 1|   |block 1|   |   |block x|   |           |     |block y   |block y|
    +-------------------+-------------+-----------------+------------------------+
```

Bands are written sequentially (in a way that was described earlier). Before a band can be written
to, all of its zones need to be erased. During that time, the band is considered to be in a `PREP`
state. Then the band moves to the `OPEN` state and actual user data can be written to the
band. Once the whole available space is filled, tail metadata is written and the band transitions to
`CLOSING` state. When that finishes the band becomes `CLOSED`.

### Non volatile cache {#ftl_nvcache}

- Shorthand: `nvcache`

Nvcache is a bdev that is used for buffering user writes and storing various metadata.
Nvcache data space is divided into chunks. Chunks are written in sequential manner.
When number of free chunks is below assigned threshold data from fully written chunks
is moved to base_bdev. This process is called chunk compaction.
```text
                      nvcache
    +-----------------------------------------+
    |chunk 1                                  |
    |   +--------------------------------- +  |
    |   |blk 1 + md| blk 2 + md| blk n + md|  |
    |   +----------------------------------|  |
    +-----------------------------------------+
    | ...                                     |
    +-----------------------------------------+
    +-----------------------------------------+
    |chunk N                                  |
    |   +--------------------------------- +  |
    |   |blk 1 + md| blk 2 + md| blk n + md|  |
    |   +----------------------------------|  |
    +-----------------------------------------+
```

### Garbage collection and relocation {#ftl_reloc}

- Shorthand: gc, reloc

Since a write to the same LBA invalidates its previous physical location, some of the blocks on a
band might contain old data that basically wastes space. As there is no way to overwrite an already
written block for a ZNS drive, this data will stay there until the whole zone is reset. This might create a
situation in which all of the bands contain some valid data and no band can be erased, so no writes
can be executed anymore. Therefore a mechanism is needed to move valid data and invalidate whole
bands, so that they can be reused.

```text
                    band                                             band
    +-----------------------------------+            +-----------------------------------+
    | ** *    * ***      *    *** * *   |            |                                   |
    |**  *       *    *    * *     *   *|   +---->   |                                   |
    |*     ***  *      *            *   |            |                                   |
    +-----------------------------------+            +-----------------------------------+
```

Valid blocks are marked with an asterisk '\*'.

Module responsible for data relocation is called `reloc`. When a band is chosen for garbage collection,
the appropriate blocks are marked as required to be moved. The `reloc` module takes a band that has
some of such blocks marked, checks their validity and, if they're still valid, copies them.

Choosing a band for garbage collection depends its validity ratio (proportion of valid blocks to all
user blocks). The lower the ratio, the higher the chance the band will be chosen for gc.

## Metadata {#ftl_metadata}

In addition to the [L2P](#ftl_l2p), FTL will store additional metadata both on the cache, as
well as on the base devices. The following types of metadata are persisted:

- Superblock - stores the global state of FTL; stored on cache, mirrored to the base device

- L2P - see the [L2P](#ftl_l2p) section for details

- Band - stores the state of bands - write pointers, their OPEN/FREE/CLOSE state; stored on cache, mirrored to a different section of the cache device

- Valid map - bitmask of all the valid physical addresses, used for improving [relocation](#ftl_reloc)

- Chunk - stores the state of chunks - write pointers, their OPEN/FREE/CLOSE state; stored on cache, mirrored to a different section of the cache device

- P2L - stores the address mapping (P2L, see [band](#ftl_band)) of currently open bands. This allows for the recovery of open
 bands after dirty shutdown without needing VSS DIX metadata on the base device; stored on the cache device

- Trim - stores information about unmapped (trimmed) LBAs; stored on cache, mirrored to a different section of the cache device

## Dirty shutdown recovery {#ftl_dirty_shutdown}

After power failure, FTL needs to rebuild the whole L2P using the address maps (`P2L`) stored within each band/chunk.
This needs to done, because while individual L2P pages may have been paged out and persisted to the cache device,
there's no way to tell which, if any, pages were dirty before the power failure occurred. The P2L consists of not only
the mapping itself, but also a sequence id (`seq_id`), which describes the relative age of a given logical block
(multiple writes to the same logical block would produce the same amount of P2L entries, only the last one having the current data).

FTL will therefore rebuild the whole L2P by reading the P2L of all closed bands and chunks. For open bands, the P2L is stored on
the cache device, in a separate metadata region (see [the P2L section](#ftl_metadata)). Open chunks can be restored thanks to storing
the mapping in the VSS DIX metadata, which the cache device must be formatted with.

### Shared memory recovery {#ftl_shm_recovery}

In order to shorten the recovery after crash of the target application, FTL also stores its metadata in shared memory (`shm`) - this
allows it to keep track of the dirty-ness state of individual pages and shortens the recovery time dramatically, as FTL will only
need to mark any potential L2P pages which were paging out at the time of the crash as dirty and reissue the writes. There's no need
to read the whole P2L in this case.

### Trim {#ftl_trim}

Due to metadata size constraints and the difficulty of maintaining consistent data returned before and after dirty shutdown, FTL
currently only allows for trims (unmaps) aligned to 4MiB (alignment concerns both the offset and length of the trim command).

## Usage {#ftl_usage}

### Prerequisites {#ftl_prereq}

In order to use the FTL module, a cache device formatted with VSS DIX metadata is required.

### FTL bdev creation {#ftl_create}

Similar to other bdevs, the FTL bdevs can be created either based on JSON config files or via RPC.
Both interfaces require the same arguments which are described by the `--help` option of the
`bdev_ftl_create` RPC call, which are:

- bdev's name
- base bdev's name
- cache bdev's name (cache bdev must support VSS DIX mode - could be emulated by providing SPDK_FTL_VSS_EMU=1 flag to make;
 emulating VSS should be done for testing purposes only, it is not power-fail safe)
- UUID of the FTL device (if the FTL is to be restored from the SSD)

## FTL bdev stack {#ftl_bdev_stack}

In order to create FTL on top of a regular bdev:
1) Create regular bdev e.g. `bdev_nvme`, `bdev_null`, `bdev_malloc`
2) Create second regular bdev for nvcache
3) Create FTL bdev on top of bdev created in step 1 and step 2

Example:
```
$ scripts/rpc.py bdev_nvme_attach_controller -b nvme0 -a 00:05.0 -t pcie
	nvme0n1

$ scripts/rpc.py bdev_nvme_attach_controller -b nvme1 -a 00:06.0 -t pcie
	nvme1n1

$ scripts/rpc.py bdev_ftl_create -b ftl0 -d nvme0n1 -c nvme1n1
{
	"name": "ftl0",
	"uuid": "3b469565-1fa5-4bfb-8341-747ec9f3a9b9"
}
```
