# Blobstore Programmer's Guide {#blob}

## In this document {#blob_pg_toc}

* @ref blob_pg_audience
* @ref blob_pg_intro
* @ref blob_pg_theory
* @ref blob_pg_design
* @ref blob_pg_examples
* @ref blob_pg_config
* @ref blob_pg_component

## Target Audience {#blob_pg_audience}

The programmer's guide is intended for developers authoring applications that utilize the SPDK Blobstore. It is
intended to supplement the source code in providing an overall understanding of how to integrate Blobstore into
an application as well as provide some high level insight into how Blobstore works behind the scenes. It is not
intended to serve as a design document or an API reference and in some cases source code snippets and high level
sequences will be discussed; for the latest source code reference refer to the [repo](https://github.com/spdk).

## Introduction {#blob_pg_intro}

Blobstore is a persistent, power-fail safe block allocator designed to be used as the local storage system
backing a higher level storage service, typically in lieu of a traditional filesystem. These higher level services
can be local databases or key/value stores (MySQL, RocksDB), they can be dedicated appliances (SAN, NAS), or
distributed storage systems (ex. Ceph, Cassandra). It is not designed to be a general purpose filesystem, however,
and it is intentionally not POSIX compliant. To avoid confusion, we avoid references to files or objects instead
using the term 'blob'. The Blobstore is designed to allow asynchronous, uncached, parallel reads and writes to
groups of blocks on a block device called 'blobs'. Blobs are typically large, measured in at least hundreds of
kilobytes, and are always a multiple of the underlying block size.

The Blobstore is designed primarily to run on "next generation" media, which means the device supports fast random
reads and writes, with no required background garbage collection. However, in practice the design will run well on
NAND too.

## Theory of Operation {#blob_pg_theory}

### Abstractions

The Blobstore defines a hierarchy of storage abstractions as follows.

* **Logical Block**: Logical blocks are exposed by the disk itself, which are numbered from 0 to N, where N is the
  number of blocks in the disk. A logical block is typically either 512B or 4KiB.
* **Page**: A page is defined to be a fixed number of logical blocks defined at Blobstore creation time. The logical
  blocks that compose a page are always contiguous. Pages are also numbered from the beginning of the disk such
  that the first page worth of blocks is page 0, the second page is page 1, etc. A page is typically 4KiB in size,
  so this is either 8 or 1 logical blocks in practice. The SSD must be able to perform atomic reads and writes of
  at least the page size.
* **Cluster**: A cluster is a fixed number of pages defined at Blobstore creation time. The pages that compose a cluster
  are always contiguous. Clusters are also numbered from the beginning of the disk, where cluster 0 is the first cluster
  worth of pages, cluster 1 is the second grouping of pages, etc. A cluster is typically 1MiB in size, or 256 pages.
* **Blob**: A blob is an ordered list of clusters. Blobs are manipulated (created, sized, deleted, etc.) by the application
  and persist across power failures and reboots. Applications use a Blobstore provided identifier to access a particular blob.
  Blobs are read and written in units of pages by specifying an offset from the start of the blob. Applications can also
  store metadata in the form of key/value pairs with each blob which we'll refer to as xattrs (extended attributes).
* **Blobstore**: An SSD which has been initialized by a Blobstore-based application is referred to as "a Blobstore." A
  Blobstore owns the entire underlying device which is made up of a private Blobstore metadata region and the collection of
  blobs as managed by the application.

```text
+-----------------------------------------------------------------+
|                              Blob                               |
| +-----------------------------+ +-----------------------------+ |
| |           Cluster           | |           Cluster           | |
| | +----+ +----+ +----+ +----+ | | +----+ +----+ +----+ +----+ | |
| | |Page| |Page| |Page| |Page| | | |Page| |Page| |Page| |Page| | |
| | +----+ +----+ +----+ +----+ | | +----+ +----+ +----+ +----+ | |
| +-----------------------------+ +-----------------------------+ |
+-----------------------------------------------------------------+
```

### Atomicity

For all Blobstore operations regarding atomicity, there is a dependency on the underlying device to guarantee atomic
operations of at least one page size. Atomicity here can refer to multiple operations:

* **Data Writes**: For the case of data writes, the unit of atomicity is one page. Therefore if a write operation of
  greater than one page is underway and the system suffers a power failure, the data on media will be consistent at a page
  size granularity (if a single page were in the middle of being updated when power was lost, the data at that page location
  will be as it was prior to the start of the write operation following power restoration.)
* **Blob Metadata Updates**: Each blob has its own set of metadata (xattrs, size, etc). For performance reasons, a copy of
  this metadata is kept in RAM and only synchronized with the on-disk version when the application makes an explicit call to
  do so, or when the Blobstore is unloaded. Therefore, setting of an xattr, for example is not consistent until the call to
  synchronize it (covered later) which is, however, performed atomically.
* **Blobstore Metadata Updates**: Blobstore itself has its own metadata which, like per blob metadata, has a copy in both
  RAM and on-disk. Unlike the per blob metadata, however, the Blobstore metadata region is not made consistent via a blob
  synchronization call, it is only synchronized when the Blobstore is properly unloaded via API. Therefore, if the Blobstore
  metadata is updated (blob creation, deletion, resize, etc.) and not unloaded properly, it will need to perform some extra
  steps the next time it is loaded which will take a bit more time than it would have if shutdown cleanly, but there will be
  no inconsistencies.

### Callbacks

Blobstore is callback driven; in the event that any Blobstore API is unable to make forward progress it will
not block but instead return control at that point and make a call to the callback function provided in the API, along with
arguments, when the original call is completed. The callback will be made on the same thread that the call was made from, more on
threads later. Some API, however, offer no callback arguments; in these cases the calls are fully synchronous. Examples of
asynchronous calls that utilize callbacks include those that involve disk IO, for example, where some amount of polling
is required before the IO is completed.

### Backend Support

Blobstore requires a backing storage device that can be integrated using the `bdev` layer, or by directly integrating a
device driver to Blobstore. The blobstore performs operations on a backing block device by calling function pointers
supplied to it at initialization time. For convenience, an implementation of these function pointers that route I/O
to the bdev layer is available in `bdev_blob.c`.  Alternatively, for example, the SPDK NVMe driver may be directly integrated
bypassing a small amount of `bdev` layer overhead. These options will be discussed further in the upcoming section on examples.

### Metadata Operations

Because Blobstore is designed to be lock-free, metadata operations need to be isolated to a single
thread to avoid taking locks on in memory data structures that maintain data on the layout of definitions of blobs (along
with other data). In Blobstore this is implemented as `the metadata thread` and is defined to be the thread on which the
application makes metadata related calls on. It is up to the application to setup a separate thread to make these calls on
and to assure that it does not mix relevant IO operations with metadata operations even if they are on separate threads.
This will be discussed further in the Design Considerations section.

### Threads

An application using Blobstore with the SPDK NVMe driver, for example, can support a variety of thread scenarios.
The simplest would be a single threaded application where the application, the Blobstore code and the NVMe driver share a
single core. In this case, the single thread would be used to submit both metadata operations as well as IO operations and
it would be up to the application to assure that only one metadata operation is issued at a time and not intermingled with
affected IO operations.

### Channels

Channels are an SPDK-wide abstraction and with Blobstore the best way to think about them is that they are
required in order to do IO.  The application will perform IO to the channel and channels are best thought of as being
associated 1:1 with a thread.

### Blob Identifiers

When an application creates a blob, it does not provide a name as is the case with many other similar
storage systems, instead it is returned a unique identifier by the Blobstore that it needs to use on subsequent APIs to
perform operations on the Blobstore.

## Design Considerations {#blob_pg_design}

### Initialization Options

When the Blobstore is initialized, there are multiple configuration options to consider. The
options and their defaults are:

* **Cluster Size**: By default, this value is 1MB. The cluster size is required to be a multiple of page size and should be
  selected based on the application’s usage model in terms of allocation. Recall that blobs are made up of clusters so when
  a blob is allocated/deallocated or changes in size, disk LBAs will be manipulated in groups of cluster size.  If the
  application is expecting to deal with mainly very large (always multiple GB) blobs then it may make sense to change the
  cluster size to 1GB for example.
* **Number of Metadata Pages**: By default, Blobstore will assume there can be as many clusters as there are metadata pages
  which is the worst case scenario in terms of metadata usage and can be overridden here however the space efficiency is
  not significant.
* **Maximum Simultaneous Metadata Operations**: Determines how many internally pre-allocated memory structures are set
  aside for performing metadata operations. It is unlikely that changes to this value (default 32) would be desirable.
* **Maximum Simultaneous Operations Per Channel**: Determines how many internally pre-allocated memory structures are set
  aside for channel operations. Changes to this value would be application dependent and best determined by both a knowledge
  of the typical usage model, an understanding of the types of SSDs being used and empirical data. The default is 512.
* **Blobstore Type**: This field is a character array to be used by applications that need to identify whether the
  Blobstore found here is appropriate to claim or not. The default is NULL and unless the application is being deployed in
  an environment where multiple applications using the same disks are at risk of inadvertently using the wrong Blobstore, there
  is no need to set this value. It can, however, be set to any valid set of characters.
* **External Snapshot Device Creation Callback**: If the blobstore supports external snapshots this function will be called
  as a blob that clones an external snapshot (an "esnap clone") is opened so that the blobstore consumer can load the external
  snapshot and register a blobstore device that will satisfy read requests. See @ref blob_pg_esnap_and_esnap_clone.

### Sub-page Sized Operations

Blobstore is only capable of doing page sized read/write operations. If the application
requires finer granularity it will have to accommodate that itself.

### Threads

As mentioned earlier, Blobstore can share a single thread with an application or the application
can define any number of threads, within resource constraints, that makes sense.  The basic considerations that must be
followed are:

* Metadata operations (API with MD in the name) should be isolated from each other as there is no internal locking on the
   memory structures affected by these API.
* Metadata operations should be isolated from conflicting IO operations (an example of a conflicting IO would be one that is
  reading/writing to an area of a blob that a metadata operation is deallocating).
* Asynchronous callbacks will always take place on the calling thread.
* No assumptions about IO ordering can be made regardless of how many or which threads were involved in the issuing.

### Data Buffer Memory

As with all SPDK based applications, Blobstore requires memory used for data buffers to be allocated
with SPDK API.

### Error Handling

Asynchronous Blobstore callbacks all include an error number that should be checked; non-zero values
indicate an error. Synchronous calls will typically return an error value if applicable.

### Asynchronous API

Asynchronous callbacks will return control not immediately, but at the point in execution where no
more forward progress can be made without blocking.  Therefore, no assumptions can be made about the progress of
an asynchronous call until the callback has completed.

### Xattrs

Setting and removing of xattrs in Blobstore is a metadata operation, xattrs are stored in per blob metadata.
Therefore, xattrs are not persisted until a blob synchronization call is made and completed. Having a step process for
persisting per blob metadata allows for applications to perform batches of xattr updates, for example, with only one
more expensive call to synchronize and persist the values.

### Synchronizing Metadata

As described earlier, there are two types of metadata in Blobstore, per blob and one global
metadata for the Blobstore itself.  Only the per blob metadata can be explicitly synchronized via API. The global
metadata will be inconsistent during run-time and only synchronized on proper shutdown. The implication, however, of
an improper shutdown is only a performance penalty on the next startup as the global metadata will need to be rebuilt
based on a parsing of the per blob metadata. For consistent start times, it is important to always close down the Blobstore
properly via API.

### Iterating Blobs

Multiple examples of how to iterate through the blobs are included in the sample code and tools.
Worthy to note, however, if walking through the existing blobs via the iter API, if your application finds the blob its
looking for it will either need to explicitly close it (because was opened internally by the Blobstore) or complete walking
the full list.

### The Super Blob

The super blob is simply a single blob ID that can be stored as part of the global metadata to act
as sort of a "root" blob. The application may choose to use this blob to store any information that it needs or finds
relevant in understanding any kind of structure for what is on the Blobstore.

## Examples {#blob_pg_examples}

There are multiple examples of Blobstore usage in the [repo](https://github.com/spdk/spdk):

* **Hello World**: Actually named `hello_blob.c` this is a very basic example of a single threaded application that
  does nothing more than demonstrate the very basic API. Although Blobstore is optimized for NVMe, this example uses
  a RAM disk (malloc) back-end so that it can be executed easily in any development environment. The malloc back-end
  is a `bdev` module thus this example uses not only the SPDK Framework but the `bdev` layer as well.

* **CLI**: The `blobcli.c` example is command line utility intended to not only serve as example code but as a test
  and development tool for Blobstore itself. It is also a simple single threaded application that relies on both the
  SPDK Framework and the `bdev` layer but offers multiple modes of operation to accomplish some real-world tasks. In
  command mode, it accepts single-shot commands which can be a little time consuming if there are many commands to
  get through as each one will take a few seconds waiting for DPDK initialization. It therefore has a shell mode that
  allows the developer to get to a `blob>` prompt and then very quickly interact with Blobstore with simple commands
  that include the ability to import/export blobs from/to regular files. Lastly there is a scripting mode to automate
  a series of tasks, again, handy for development and/or test type activities.

## Configuration {#blob_pg_config}

Blobstore configuration options are described in the initialization options section under @ref blob_pg_design.

## Component Detail {#blob_pg_component}

The information in this section is not necessarily relevant to designing an application for use with Blobstore, but
understanding a little more about the internals may be interesting and is also included here for those wanting to
contribute to the Blobstore effort itself.

### Media Format

The Blobstore owns the entire storage device. The device is divided into clusters starting from the beginning, such
that cluster 0 begins at the first logical block.

```text
LBA 0                                   LBA N
+-----------+-----------+-----+-----------+
| Cluster 0 | Cluster 1 | ... | Cluster N |
+-----------+-----------+-----+-----------+
```

Cluster 0 is special and has the following format, where page 0 is the first page of the cluster:

```text
+--------+-------------------+
| Page 0 | Page 1 ... Page N |
+--------+-------------------+
| Super  |  Metadata Region  |
| Block  |                   |
+--------+-------------------+
```

The super block is a single page located at the beginning of the partition. It contains basic information about
the Blobstore. The metadata region is the remainder of cluster 0 and may extend to additional clusters. Refer
to the latest source code for complete structural details of the super block and metadata region.

Each blob is allocated a non-contiguous set of pages inside the metadata region for its metadata. These pages
form a linked list. The first page in the list will be written in place on update, while all other pages will
be written to fresh locations. This requires the backing device to support an atomic write size greater than
or equal to the page size to guarantee that the operation is atomic. See the section on atomicity for details.

### Blob cluster layout {#blob_pg_cluster_layout}

Each blob is an ordered list of clusters, where starting LBA of a cluster is called extent. A blob can be
thin provisioned, resulting in no extent for some of the clusters. When first write operation occurs
to the unallocated cluster - new extent is chosen. This information is stored in RAM and on-disk.

There are two extent representations on-disk, dependent on `use_extent_table` (default:true) opts used
when creating a blob.

* **use_extent_table=true**: EXTENT_PAGE descriptor is not part of linked list of pages. It contains extents
  that are not run-length encoded. Each extent page is referenced by EXTENT_TABLE descriptor, which is serialized
  as part of linked list of pages.  Extent table is run-length encoding all unallocated extent pages.
  Every new cluster allocation updates a single extent page, in case when extent page was previously allocated.
  Otherwise additionally incurs serializing whole linked list of pages for the blob.

* **use_extent_table=false**: EXTENT_RLE descriptor is serialized as part of linked list of pages.
  Extents pointing to contiguous LBA are run-length encoded, including unallocated extents represented by 0.
  Every new cluster allocation incurs serializing whole linked list of pages for the blob.

### Thin Blobs, Snapshots, and Clones

Each in-use cluster is allocated to blobstore metadata or to a particular blob. Once a cluster is
allocated to a blob it is considered owned by that blob and that particular blob's metadata
maintains a reference to the cluster as a record of ownership. Cluster ownership is transferred
during snapshot operations described later in @ref blob_pg_snapshots.

Through the use of thin provisioning, snapshots, and/or clones, a blob may be backed by clusters it
owns, clusters owned by another blob, or by a zeroes device. The behavior of reads and writes depend
on whether the operation targets blocks that are backed by a cluster owned by the blob or not.

* **read from blocks on an owned cluster**: The read is serviced by reading directly from the
  appropriate cluster.
* **read from other blocks**: The read is passed on to the blob's *back device* and the back
  device services the read. The back device may be another blob or it may be a zeroes device.
* **write to blocks on an owned cluster**: The write is serviced by writing directly to the
  appropriate cluster.
* **write to thin provisioned cluster**: If the back device is the zeroes device and no cluster
  is allocated to the blob the process described in @ref blob_pg_thin_provisioning is followed.
* **write to other blocks**: A copy-on-write operation is triggered. See @ref blob_pg_copy_on_write
  for details.

External snapshots allow some external data source to act as a snapshot. This allows clones to be
created of data that resides outside of the blobstore containing the clone.

#### Thin Provisioning {#blob_pg_thin_provisioning}

As mentioned in @ref blob_pg_cluster_layout, a blob may be thin provisioned. A thin provisioned blob
starts out with no allocated clusters. Clusters are allocated as writes occur. A thin provisioned
blob's back device is a *zeroes device*. A read from a zeroes device fills the read buffer with
zeroes.

When a thin provisioned volume writes to a block that does not have an allocated cluster, the
following steps are performed:

1. Allocate a cluster.
2. Update blob metadata.
3. Perform the write.

#### Snapshots and Clones {#blob_pg_snapshots}

A snapshot is a read-only blob that may have clones. A snapshot may itself be a clone of one other
blob. While the interface gives the illusion of being able to create many snapshots of a blob, under
the covers this results in a chain of snapshots that are clones of the previous snapshot.

When blob1 is snapshotted, a new read-only blob is created and blob1 becomes a clone of this new
blob. That is:

| Step | Action                         | State                                             |
| ---- | ------------------------------ | ------------------------------------------------- |
| 1    | Create blob1                   | `blob1 (rw)`                                      |
| 2    | Create snapshot blob2 of blob1 | `blob1 (rw) --> blob2 (ro)`                       |
| 2a   | Write to blob1                 | `blob1 (rw) --> blob2 (ro)`                       |
| 3    | Create snapshot blob3 of blob1 | `blob1 (rw) --> blob3 (ro) ---> blob2 (ro)`       |

Supposing blob1 was not thin provisioned, step 1 would have allocated clusters needed to perform a
full write of blob1. As blob2 is created in step 2, the ownership of all of blob1's clusters is
transferred to blob2 and blob2 becomes blob1's back device. During step2a, the writes to blob1 cause
one or more clusters to be allocated to blob1. When blob3 is created in step 3, the clusters
allocated in step 2a are given to blob3, blob3's back device becomes blob2, and blob1's back device
becomes blob3.

It is important to understand the chain above when considering strategies to use a golden image from
which many clones are made. The IO path is more efficient if one snapshot is cloned many times than
it is to create a new snapshot for every clone. The following illustrates the difference.

Using a single snapshot means the data originally referenced by the golden image is always one hop
away.

```text
create golden                           golden --> golden-snap
snapshot golden as golden-snap                     ^ ^ ^
clone golden-snap as clone1              clone1 ---+ | |
clone golden-snap as clone2              clone2 -----+ |
clone golden-snap as clone3              clone3 -------+
```

Using a snapshot per clone means that the chain of back devices grows with every new snapshot and
clone pair. Reading a block from clone3 may result in a read from clone3's back device (snap3), from
clone2's back device (snap2), then finally clone1's back device (snap1, the current owner of the
blocks originally allocated to golden).

```text
create golden
snapshot golden as snap1                golden --> snap3 -----> snap2 ----> snap1
clone snap1 as clone1                   clone3----/   clone2 --/  clone1 --/
snapshot golden as snap2
clone snap2 as clone2
snapshot golden as snap3
clone snap3 as clone3
```

A snapshot with no more than one clone can be deleted. When a snapshot with one clone is deleted,
the clone becomes a regular blob. The clusters owned by the snapshot are transferred to the clone or
freed, depending on whether the clone already owns a cluster for a particular block range.

Removal of the last clone leaves the snapshot in place. This snapshot continues to be read-only and
can serve as the snapshot for future clones.

#### Inflating and Decoupling Clones

A clone can remove its dependence on a snapshot with the following operations:

1. Inflate the clone. Clusters backed by any snapshot or a zeroes device are copied into newly
   allocated clusters. The blob becomes a thick provisioned blob.
2. Decouple the clone. Clusters backed by the first back device snapshot are copied into newly
   allocated clusters. If the clone's back device snapshot was itself a clone of another
   snapshot, the clone remains a clone but is now a clone of a different snapshot.
3. Remove the snapshot. This is only possible if the snapshot has one clone. The end result is
   usually the same as decoupling but ownership of clusters is transferred from the snapshot rather
   than being copied. If the snapshot that was deleted was itself a clone of another snapshot, the
   clone remains a clone, but is now a clone of a different snapshot.

#### External Snapshots and Esnap Clones {#blob_pg_esnap_and_esnap_clone}

A blobstore that is loaded with the `esnap_bs_dev_create` callback defined will support external
snapshots (esnaps). An external snapshot is not useful on its own: it needs to be cloned by a blob.
A clone of an external snapshot is referred to as an *esnap clone*. An esnap clone supports IO and
other operations just like any other clone.

An esnap clone can be recognized in various ways:

* **On disk**: the blob metadata has the `SPDK_BLOB_EXTERNAL_SNAPSHOT` (0x8) bit is set in
  `invalid_flags` and an internal XATTR with name `BLOB_EXTERNAL_SNAPSHOT_ID` ("EXTSNAP") exists.
* **In memory**: The `spdk_blob` structure contains the metadata read from disk, `blob->parent_id`
  is set to `SPDK_BLOBID_EXTERNAL_SNAPSHOT`, and `blob->back_bs_dev` references a blobstore device
  which is not a blob in the same blobstore nor a zeroes device.

#### Copy-on-write {#blob_pg_copy_on_write}

A copy-on-write operation is somewhat expensive, with the cost being proportional to the cluster
size. Typical copy-on-write involves the following steps:

1. Allocate a cluster.
2. Allocate a cluster-sized buffer into which data can be read.
3. Trigger a full-cluster read from the back device into the cluster-sized buffer.
4. Write from the cluster-sized buffer into the newly allocated cluster.
5. Update the blob's on-disk metadata to record ownership of the newly allocated cluster. This
   involves at least one page-sized write.
6. Write the new data to the just allocated and copied cluster.

If the source cluster is backed by a zeroes device, steps 2 through 4 are skipped. Alternatively, if
the blobstore resides on a device that can perform the copy on its own, steps 2 through 4 are
offloaded to the device. Neither of these optimizations are available when the back device is an
external snapshot.

### Sequences and Batches

Internally Blobstore uses the concepts of sequences and batches to submit IO to the underlying device in either
a serial fashion or in parallel, respectively. Both are defined using the following structure:

~~~{.sh}
struct spdk_bs_request_set;
~~~

These requests sets are basically bookkeeping mechanisms to help Blobstore efficiently deal with related groups
of IO. They are an internal construct only and are pre-allocated on a per channel basis (channels were discussed
earlier). They are removed from a channel associated linked list when the set (sequence or batch) is started and
then returned to the list when completed.

### Key Internal Structures

`blobstore.h` contains many of the key structures for the internal workings of Blobstore. Only a few notable ones
are reviewed here.  Note that `blobstore.h` is an internal header file, the header file for Blobstore that defines
the public API is `blob.h`.

~~~{.sh}
struct spdk_blob
~~~
This is an in-memory data structure that contains key elements like the blob identifier, its current state and two
copies of the mutable metadata for the blob; one copy is the current metadata and the other is the last copy written
to disk.

~~~{.sh}
struct spdk_blob_mut_data
~~~
This is a per blob structure, included the `struct spdk_blob` struct that actually defines the blob itself. It has the
specific information on size and makeup of the blob (ie how many clusters are allocated for this blob and which ones.)

~~~{.sh}
struct spdk_blob_store
~~~
This is the main in-memory structure for the entire Blobstore. It defines the global on disk metadata region and maintains
information relevant to the entire system - initialization options such as cluster size, etc.

~~~{.sh}
struct spdk_bs_super_block
~~~
The super block is an on-disk structure that contains all of the relevant information that's in the in-memory Blobstore
structure just discussed along with other elements one would expect to see here such as signature, version, checksum, etc.

### Code Layout and Common Conventions

In general, `Blobstore.c` is laid out with groups of related functions blocked together with descriptive comments. For
example,

~~~{.sh}
/* START spdk_bs_md_delete_blob */
< relevant functions to accomplish the deletion of a blob >
/* END spdk_bs_md_delete_blob */
~~~

And for the most part the following conventions are followed throughout:

* functions beginning with an underscore are called internally only
* functions or variables with the letters `cpl` are related to set or callback completions
