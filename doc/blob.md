# Blobstore {#blob}

## Introduction

The blobstore is a persistent, power-fail safe block allocator designed to be
used as the local storage system backing a higher level storage service,
typically in lieu of a traditional filesystem. These higher level services can
be local databases or key/value stores (MySQL, RocksDB), they can be dedicated
appliances (SAN, NAS), or distributed storage systems (ex. Ceph, Cassandra). It
is not designed to be a general purpose filesystem, however, and it is
intentionally not POSIX compliant. To avoid confusion, no reference to files or
objects will be made at all, instead using the term 'blob'. The blobstore is
designed to allow asynchronous, uncached, parallel reads and writes to groups
of blocks on a block device called 'blobs'. Blobs are typically large,
measured in at least hundreds of kilobytes, and are always a multiple of the
underlying block size.

The blobstore is designed primarily to run on "next generation" media, which
means the device supports fast random reads _and_ writes, with no required
background garbage collection. However, in practice the design will run well on
NAND too. Absolutely no attempt will be made to make this efficient on spinning
media.

## Design Goals

The blobstore is intended to solve a number of problems that local databases
have when using traditional POSIX filesystems. These databases are assumed to
'own' the entire storage device, to not need to track access times, and to
require only a very simple directory hierarchy. These assumptions allow
significant design optimizations over a traditional POSIX filesystem and block
stack.

Asynchronous I/O can be an order of magnitude or more faster than synchronous
I/O, and so solutions like
[libaio](https://git.fedorahosted.org/cgit/libaio.git/) have become popular.
However, libaio is [not actually
asynchronous](http://www.scylladb.com/2016/02/09/qualifying-filesystems/) in
all cases. The blobstore will provide truly asynchronous operations in all
cases without any hidden locks or stalls.

With the advent of NVMe, storage devices now have a hardware interface that
allows for highly parallel I/O submission from many threads with no locks.
Unfortunately, placement of data on a device requires some central coordination
to avoid conflicts. The blobstore will separate operations that require
coordination from operations that do not, and allow users to explictly
associate I/O with channels. Operations on different channels happen in
parallel, all the way down to the hardware, with no locks or coordination.

As media access latency improves, strategies for in-memory caching are changing
and often the kernel page cache is a bottleneck. Many databases have moved to
opening files only in O_DIRECT mode, avoiding the page cache entirely, and
writing their own caching layer. With the introduction of next generation media
and its additional expected latency reductions, this strategy will become far
more prevalent. To support this, the blobstore will perform no in-memory
caching of data at all, essentially making all blob operations conceptually
equivalent to O_DIRECT. This means the blobstore has similar restrictions to
O_DIRECT where data can only be read or written in units of pages (4KiB),
although memory alignment requirements are much less strict than O_DIRECT (the
pages can even be composed of scattered buffers). We fully expect that DRAM
caching will remain critical to performance, but leave the specifics of the
cache design to higher layers.

Storage devices pull data from host memory using a DMA engine, and those DMA
engines operate on physical addresses and often introduce alignment
restrictions. Further, to avoid data corruption, the data must not be paged out
by the operating system while it is being transferred to disk. Traditionally,
operating systems solve this problem either by copying user data into special
kernel buffers that were allocated for this purpose and the I/O operations are
performed to/from there, or taking locks to mark all user pages as locked and
unmovable. Historically, the time to perform the copy or locking was
inconsequential relative to the I/O time at the storage device, but that is
simply no longer the case. The blobstore will instead provide zero copy,
lockless read and write access to the device. To do this, memory to be used for
blob data must be registered with the blobstore up front, preferably at
application start and out of the I/O path, so that it can be pinned, the
physical addresses can be determined, and the alignment requirements can be
verified.

Hardware devices are necessarily limited to some maximum queue depth. For NVMe
devices that can be quite large (the spec allows up to 64k!), but is typically
much smaller (128 - 1024 per queue). Under heavy load, databases may generate
enough requests to exceed the hardware queue depth, which requires queueing in
software. For operating systems this is often done in the generic block layer
and may cause unexpected stalls or require locks. The blobstore will avoid this
by simply failing requests with an appropriate error code when the queue is
full. This allows the blobstore to easily stick to its commitment to never
block, but may require the user to provide their own queueing layer.

## The Basics

The blobstore defines a hierarchy of three units of disk space. The smallest are
the *logical blocks* exposed by the disk itself, which are numbered from 0 to N,
where N is the number of blocks in the disk. A logical block is typically
either 512B or 4KiB.

The blobstore defines a *page* to be a fixed number of logical blocks defined
at blobstore creation time. The logical blocks that compose a page are
contiguous. Pages are also numbered from the beginning of the disk such that
the first page worth of blocks is page 0, the second page is page 1, etc. A
page is typically 4KiB in size, so this is either 8 or 1 logical blocks in
practice. The device must be able to perform atomic reads and writes of at
least the page size.

The largest unit is a *cluster*, which is a fixed number of pages defined at
blobstore creation time. The pages that compose a cluster are contiguous.
Clusters are also numbered from the beginning of the disk, where cluster 0 is
the first cluster worth of pages, cluster 1 is the second grouping of pages,
etc. A cluster is typically 1MiB in size, or 256 pages.

On top of these three basic units, the blobstore defines three primitives. The
most fundamental is the blob, where a blob is an ordered list of clusters plus
an identifier. Blobs persist across power failures and reboots. The set of all
blobs described by shared metadata is called the blobstore. I/O operations on
blobs are submitted through a channel. Channels are tied to threads, but
multiple threads can simultaneously submit I/O operations to the same blob on
their own channels.

Blobs are read and written in units of pages by specifying an offset in the
virtual blob address space. This offset is translated by first determining
which cluster(s) are being accessed, and then translating to a set of logical
blocks. This translation is done trivially using only basic math - there is no
mapping data structure. Unlike read and write, blobs are resized in units of
clusters.

Blobs are described by their metadata which consists of a discontiguous set of
pages stored in a reserved region on the disk. Each page of metadata is
referred to as a *metadata page*. Blobs do not share metadata pages with other
blobs, and in fact the design relies on the backing storage device supporting
an atomic write unit greater than or equal to the page size. Most devices
backed by NAND and next generation media support this atomic write capability,
but often magnetic media does not.

The metadata region is fixed in size and defined upon creation of the
blobstore. The size is configurable, but by default one page is allocated for
each cluster. For 1MiB clusters and 4KiB pages, that results in 0.4% metadata
overhead.

## Conventions

Data formats on the device are specified in [Backus-Naur
Form](https://en.wikipedia.org/wiki/Backus%E2%80%93Naur_Form). All data is
stored on media in little-endian format. Unspecified data must be zeroed.

## Media Format

The blobstore owns the entire storage device. The device is divided into
clusters starting from the beginning, such that cluster 0 begins at the first
logical block.

    LBA 0                                   LBA N
    +-----------+-----------+-----+-----------+
    | Cluster 0 | Cluster 1 | ... | Cluster N |
    +-----------+-----------+-----+-----------+

Or in formal notation:

    <media-format> ::= <cluster0> <cluster>*


Cluster 0 is special and has the following format, where page 0
is the first page of the cluster:

    +--------+-------------------+
    | Page 0 | Page 1 ... Page N |
    +--------+-------------------+
    | Super  |  Metadata Region  |
    | Block  |                   |
    +--------+-------------------+

Or formally:

    <cluster0> ::= <super-block> <metadata-region>

The super block is a single page located at the beginning of the partition.
It contains basic information about the blobstore. The metadata region
is the remainder of cluster 0 and may extend to additional clusters.

    <super-block> ::= <sb-version> <sb-len> <sb-super-blob> <sb-params>
                      <sb-metadata-start> <sb-metadata-len>
    <sb-version> ::= u32
    <sb-len> ::= u32 # Length of this super block, in bytes. Starts from the
                     # beginning of this structure.
    <sb-super-blob> ::= u64 # Special blobid set by the user that indicates where
                            # their starting metadata resides.

    <sb-md-start> ::= u64 # Metadata start location, in pages
    <sb-md-len> ::= u64 # Metadata length, in pages

The `<sb-params>` data contains parameters specified by the user when the blob
store was initially formatted.

    <sb-params> ::= <sb-page-size> <sb-cluster-size>
    <sb-page-size> ::= u32 # page size, in bytes.
                           # Must be a multiple of the logical block size.
                           # The implementation today requires this to be 4KiB.
    <sb-cluster-size> ::= u32 # Cluster size, in bytes.
                              # Must be a multiple of the page size.

Each blob is allocated a non-contiguous set of pages inside the metadata region
for its metadata. These pages form a linked list. The first page in the list
will be written in place on update, while all other pages will be written to
fresh locations. This requires the backing device to support an atomic write
size greater than or equal to the page size to guarantee that the operation is
atomic. See the section on atomicity for details.

Each page is defined as:

    <metadata-page> ::= <blob-id> <blob-sequence-num> <blob-descriptor>*
                        <blob-next> <blob-crc>
    <blob-id> ::= u64 # The blob guid
    <blob-sequence-num> ::= u32 # The sequence number of this page in the linked
                                # list.

    <blob-descriptor> ::= <blob-descriptor-type> <blob-descriptor-length>
                            <blob-descriptor-data>
    <blob-descriptor-type> ::= u8 # 0 means padding, 1 means "extent", 2 means
                                  # xattr. The type
                                  # describes how to interpret the descriptor data.
    <blob-descriptor-length> ::= u32 # Length of the entire descriptor

    <blob-descriptor-data-padding> ::= u8

    <blob-descriptor-data-extent> ::= <extent-cluster-id> <extent-cluster-count>
    <extent-cluster-id> ::= u32 # The cluster id where this extent starts
    <extent-cluster-count> ::= u32 # The number of clusters in this extent

    <blob-descriptor-data-xattr> ::= <xattr-name-length> <xattr-value-length>
                                     <xattr-name> <xattr-value>
    <xattr-name-length> ::= u16
    <xattr-value-length> ::= u16
    <xattr-name> ::= u8*
    <xattr-value> ::= u8*

    <blob-next> ::= u32 # The offset into the metadata region that contains the
                        # next page of metadata. 0 means no next page.
    <blob-crc> ::= u32 # CRC of the entire page


Descriptors cannot span metadata pages.

## Atomicity

Metadata in the blobstore is cached and must be explicitly synced by the user.
Data is not cached, however, so when a write completes the data can be
considered durable if the metadata is synchronized. Metadata does not often
change, and in fact only must be synchronized after these explicit operations:

* resize
* set xattr
* remove xattr

Any other operation will not dirty the metadata. Further, the metadata for each
blob is independent of all of the others, so a synchronization operation is
only needed on the specific blob that is dirty.

The metadata consists of a linked list of pages. Updates to the metadata are
done by first writing page 2 through N to a new location, writing page 1 in
place to atomically update the chain, and then erasing the remainder of the old
chain. The vast majority of the time, blobs consist of just a single metadata
page and so this operation is very efficient. For this scheme to work the write
to the first page must be atomic, which requires hardware support from the
backing device. For most, if not all, NVMe SSDs, an atomic write unit of 4KiB
can be expected. Devices specify their atomic write unit in their NVMe identify
data - specifically in the AWUN field.
