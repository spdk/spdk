# BlobFS Programmer's Guide {#blobfs_pg}

# In this document {#blobfs_pg_toc}

* @ref blobfs_pg_audience
* @ref blobfs_pg_intro
* @ref blobfs_pg_theory
* @ref blobfs_pg_design
* @ref blobfs_pg_examples
* @ref blobfs_pg_config

## Target Audience {#blobfs_pg_audience}

The programmer's guide is intended for developers authoring applications that utilize the SPDK Blobfs. It is
intended to supplement the source code in providing an overall understanding of how to integrate Blobfs into
an application as well as provide some high level insight into how Blobfs works behind the scenes. It is not
intended to serve as a design document or an API reference and in some cases source code snippets and high level
sequences will be discussed, for the latest source code reference refer to the [repo](https://github.com/spdk).

## Introduction {#blobfs_pg_intro}

Blobfs is a persistent, power-fail safe, lightweight user space filesystem which designed to be used as the local
storage system backing a higher level storage service, typically in lieu of a traditional filesystem. These higher
level services can be local databases or key/value stores (MySQL, RocksDB), they can be dedicated appliances (SAN, NAS),
or distributed storage systems (ex. Ceph, Cassandra). It is not designed to be a general purpose filesystem, however,
and it is intentionally not POSIX compliant. Blobfs provide public interfaces for file operation such as
"spdk_fs_open_file", "spdk_fs_create_file" etc.

The Blobfs is designed primarily to run on "next generation" media, which means the device supports fast random
reads and writes, with no required background garbage collection. However, in practice the design will run well on
NAND too.

## Theory of Operation {#blobfs_pg_theory}

### Interfaces:

The Blobfs defines a bunch of public file operation interfaces as follows.

* **spdk_fs_init**: Initialization the file sysmtem. In this function, the blobfs will allocate new spdk_filesystem,
parse the configuration, and most important： Initialization the blobstore.

* **spdk_fs_load**: Load the existing filesystem from the disk. Blobfs will try to load the blobstore first, then try
to rebuild the filesystem accordingly.

* **spdk_fs_unload**: Unload the file sysmtem before app exit. It will sync the supper blob, used bit map md page,
used cluster bit map page to the disk and free the resource in blobfs layer(for example the cache buffer).

* **spdk_fs_file_stat**: Return the file stat. The file stat include the "blob id" accordingly and the file length.

* **spdk_fs_create_file**: Create one new file. Blobfs will create the "blob" accordingly first, then the blob will be resize
as one cluster by default, it will also set the defult xattr "name" and "length". This is a synchronous interface.

* **spdk_fs_open_file**: Open one existing file or create one new file. If the file already exist, the interface just open it,
or it will create a new file and keep it as open status. This is a synchronous interface.

* **spdk_file_close**: Close one file. Sync the md of the file to disk and close the file, if the file was deleted prior,
we excute the "delete" action here. This is a synchronous interface.

* **spdk_fs_rename_file**: Rename one file. If there is no existing file which use the new name, just rename the file as new name,
or delete the existing file which use the new name and rename the file as new name. This is a synchronous interface.

* **spdk_fs_delete_file**: Delete one file. If the file are not opened by other threads, just delete it and sync the md to disk,
or we mark the file as deleted and return, it will be deleted eventually when the file closed. This is a synchronous interface.

* **spdk_file_truncate**: Truncate the file by specific length. The blob which behind the file will be resized as the specific length
and the "length" xattr will also be updated. This is a synchronous interface.

* **spdk_file_write**: Write data to the file from specific offset with specific length. The data could be cached in blobfs
cache buffer or flush to disk, if there is available space of cache buffer, the data will be cached in buffer, or if the cache
buffer full filled, the data will be flushed to disk. This is a synchronous interface.

* **spdk_file_read**: Read data of the file from specific offset with specific length. If this is sequential read, we will
read ahead data of double length of cache buffer size, and insert the data into cache buffer, or we will read data directly from disk.
This is a synchronous interface.

* **spdk_fs_set_cache_size**: Set the cache size of the blobfs.

* **spdk_file_set_priority**: Set the priority of the file. There are two level of the priority: "low" and "high",
the inactive file with "low" priority will be the first choice to release the cache buffer if the cache buffer insufficient for
the current active file.

* **spdk_file_sync**: Sync the data and md of the file to disk.

* **spdk_fs_iter_first**: Return the first file iter of the blobfs.

* **spdk_fs_iter_next**: Return next file iter of specific file in the blobfs.

* **spdk_fs_iter_get_file**: transform the file iter to file.

### Cache Buffers

Blobfs provide cache for append write, Blobfs will allocate the cache buffers at the very beginning when the application
create or load the blobfs. There are two concept for cache buffers: cache size and cache buffer size, cache size is space
for all the cache buffers, cache buffer size is the space for single cache buffer, cache size could be divided into multiple
cache buffer, all the cache buffers will be organized in a buffer tree.
For the append write, blobfs will try to buffer the data in the cache buffer tree, and the data will be flush to disk
later when the data length hit the threshold.
For the read, if the data is cached, it will return the cached data immediately, or the blobfs will read the data from disk.

### Backend Support

Blobfs rely on the allocator blobstore, the blobstore organize the disk as the supper blob, used md pages, used cluster pages,
md pages, and clusters, all data from blobfs goto the clusters space and all exttrs of files from blobfs goto the md pages space.
every single "file" map to one blob, it could consist of several md pages and clusters.

### Threads

An application using Blobfs with the SPDK NVMe driver, for example, can support a variety of thread scenarios.
The simplest would be a single threaded application where the application, the Blobstore code and the NVMe driver share a
single core. In this case, the single thread would be used to submit both metadata operations as well as IO operations and
it would be up to the application to assure that only one metadata operation is issued at a time and not intermingled with
affected IO operations.

### Channels

Channels are an SPDK-wide abstraction and with Blobfs the best way to think about them is that they are
required in order to do IO.  The application will perform IO to the channel and channels are best thought of as being
associated 1:1 with a thread.

## Design Considerations {#blobfs_pg_design}

### Initialization Options

When the Blobfs is initialized, there are multiple default steps need to follow:
* run mkfs tool to build the blobfs on the target disk. This tool only need to be excuted when you want to build a new
filesystem.

* spdk_fs_set_cache_size()
This will set the cache size for the cache buffers.

* spdk_bdev_get_by_name()
* spdk_bdev_create_bs_dev()
These two functions will ceate the low bdev support for blobstore.

* spdk_fs_load()
Load the blobfs from the disk.

* spdk_allocate_thread()
This function will invoked on evry thread context, it will construct the current thread context in SPDK.

* spdk_fs_alloc_io_channel_sync()
This function will invoked on evry thread context, This will allcate the io channel for every threads.

### Channels

Blobfs provide two types of channel which include md_io_channel, sync_io_channel, md_io_channel used for submit
of md requests, sync_io_channel used for submit of IO requests.

## Examples {#blobfs_pg_examples}

There are one example of Blobfs usage in the [repo](https://github.com/spdk/spdk):

* **RocksDB with blobfs **: The Blobfs have been integrated with RocksDB by providing an individual env layer for
RocksDB, you can refer to env_spdk.cc which provide by SPDK, it implements one env layer which could used for RocksDB.

## Configuration {#blobfs_pg_config}

The cache buffer size is configurable, you could just set the "CacheBufferShift" item in conf file before you run
the blobfs, blobfs will parse this configuration when load the filesystem, the default value is CACHE_BUFFER_SHIFT_DEFAULT.

### Key Structures

`blobfs.c` contains many of the key structures for the Blobfs. Only a few notable ones are reviewed here.

~~~{.sh}
struct spdk_file
~~~
This is an in-memory data structure for "file" that contains key elements like the blob, name, length etc.

~~~{.sh}
struct spdk_filesystem
~~~
This is the data structure which represent the filesystem, it include the blobstore which provide the backend support,
list of all the files, md_target, sync_target etc.

~~~{.sh}
struct spdk_fs_cb_args
~~~
This is the generic callback args which used for various spdk_fs_request, the callback function is in the form of
union {...} fn, and the args is in the form of union {...} op, when the request done, blobfs will call the callback
function and pass the args accordingly.

~~~{.sh}
struct spdk_fs_request
~~~
The internal data sturcture for various type of requests, the requests could be one of following type:
fs_load
truncate
rw
rename
flush
readahead
sync
resize
open
create
delete
stat

### Code Layout and Common Conventions

For the most part the following conventions are followed throughout:
* functions beginning with an underscore are called internally only
* functions with the letters `md` in them are related to metadata operations
