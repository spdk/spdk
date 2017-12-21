# BlobFS Programmer's Guide {#blobfs_pg}

# In this document {#blobfs_pg_toc}

* @ref blobfs_pg_audience
* @ref blobfs_pg_intro
* @ref blobfs_pg_theory
* @ref blobfs_pg_design
* @ref blobfs_pg_examples
* @ref blobfs_pg_config

## Target Audience {#blobfs_pg_audience}

The programmer's guide is intended for developers authoring applications that utilize the SPDK BlobFS. It is
intended to supplement the source code in providing an overall understanding of how to integrate BlobFS into
an application as well as provide some high level insight into how BlobFS works behind the scenes. It is not
intended to serve as a design document or an API reference and in some cases source code snippets and high level
sequences will be discussed. For the latest source code reference refer to the [repo](https://github.com/spdk).

## Introduction {#blobfs_pg_intro}

BlobFS is a persistent, power-fail safe, lightweight user space filesystem which is designed to be used as the local
storage system backing a higher level storage service, typically in lieu of a traditional filesystem. These higher
level services can be local databases or key/value stores (MySQL, RocksDB). They can be dedicated appliances (SAN, NAS),
or distributed storage systems (e.g. Ceph, Cassandra). It is not designed to be a general purpose filesystem, however.
And it is intentionally not POSIX compliant. BlobFS provides public interfaces for file operation such as
"spdk_fs_open_file", "spdk_fs_create_file" etc.

The BlobFS is designed primarily to run on "next generation" media, which means the device supports fast random
reads and writes, with no required background garbage collection. However, in practice the design will run well on
NAND too.

## Theory of Operation {#blobfs_pg_theory}

### Interfaces:

The BlobFS defines a bunch of public file operation interfaces as follows.

* **spdk_fs_init**: Initialize the blobstore filesystem. In this function, the BlobFS will allocate a new blobstore
filesystem, parse the configuration, and most importantly： initialize the blobstore.

* **spdk_fs_load**: Load the existing filesystem from the given blobstore block device. BlobFS will try to load the blobstore 
first, then try to rebuild the filesystem accordingly.

* **spdk_fs_unload**: Unload the filesystem before app exits. It will synchronize the super blob with used bit map metadata page,
used cluster bit map page to the disk and free the resource in BlobFS layer(for example the cache buffer).

* **spdk_fs_file_stat**: Return the file stat. The file stat includes the "blob id" accordingly and the file length.

* **spdk_fs_create_file**: Create one new file. BlobFS will create the "blob" accordingly first. Then the blob will be resized
to one cluster by default. It will also set the default xattr's "name" and "length". This is a synchronous interface.

* **spdk_fs_open_file**: Open one existing file or create one new file. If the file already exists, the interface will just 
open it. Or it will create a new file and keep it as open status. This is a synchronous interface.

* **spdk_file_close**: Close one file. Synchronize the md of the file with disk and close the file. If the file was deleted already,
we execute the "delete" action here. This is a synchronous interface.

* **spdk_fs_rename_file**: Rename one file. If there is no existing file which use the specified name, just rename the file to new 
name. Otherwise, delete the existing file which use the new name and rename the file as new name. This is a synchronous interface.

* **spdk_fs_delete_file**: Delete one file. If the file are not opened by other threads, just delete it and sync the md to disk. 
Otherwise we mark the file as deleted and return. In this case, it will be deleted eventually when the file closes. This is a 
synchronous interface.

* **spdk_file_truncate**: Truncate the file to specified length. The blob behind the file will be resized to the specified length
and the "length" xattr will also be updated. This is a synchronous interface.

* **spdk_file_write**: Write data to the file from specified offset with specified length. The data could be cached in BlobFS
cache buffer or written to blobstore. If there is available space of cache buffer, the data will be cached in buffer. Or if the cache
buffer is full filled, the data will be written to blobstore. This is a synchronous interface.

* **spdk_file_read**: Read data of the file from specified offset with specified length. If this is sequential read, BlobFS will
read ahead data up to double of the cache buffer size, and put the data into cache buffer. Otherwise BlobFS will just read the 
data from the blobstore. This is a synchronous interface.

* **spdk_fs_set_cache_size**: Set the cache size of the BlobFS.

* **spdk_file_set_priority**: Set the priority of the file. There are two levels of the priority, "low" and "high".
The inactive file with "low" priority will be the first choice to release the cache buffer if the cache buffer is insufficient for
the current active file.

* **spdk_file_sync**: Sync the data and md of the file to disk.

* **spdk_fs_iter_first**: Use to get the first file in the BlobFS by the iterator mechanism. Return the first file iterator of
the BlobFS.

* **spdk_fs_iter_next**: Return next file iterator of specific file in the BlobFS.

* **spdk_fs_iter_get_file**: transform the file iterator to file.

### Cache Buffers

BlobFS provides cache for append write. BlobFS will allocate the cache buffers at the very beginning when the application
creates or loads the BlobFS. There are two concepts for cache buffers: cache size and cache buffer size. Cache size is space
for all the cache buffers. Cache buffer size is the space for single cache buffer. Cache size is the sum of multiple cache buffers.
All the cache buffers will be organized in a buffer tree.
For the append write, BlobFS will try to store the data in the cache buffer tree. And the data will be flushed to disk
later when the data length reaches the threshold.
For the read, if the data is cached, the cached data will be returned immediately. otherwise the BlobFS will read the data from disk.

### Backend Support

BlobFS relies on the allocated blobstore. The blobstore organizes the disk as the supper blob, used md pages, used cluster pages,
md pages, and clusters, all data from BlobFS go to the clusters space and all exttrs of files from BlobFS go to the md page space.
Every single "file" map to one blob. It could consist of several md pages and clusters.

### Threads

An application using BlobFS with the SPDK NVMe driver, for example, can support a variety of thread scenarios.
The simplest would be a single threaded application where the application, the Blobstore code and the NVMe driver share a
single core. In this case, the single thread would be used to submit both metadata operations as well as IO operations and
it would be up to the application to assure that only one metadata operation is issued at a time and not intermingled with
affected IO operations.

### Channels

Channels are an SPDK-wide abstraction and with BlobFS the best way to think about them is that they are
required in order to do IO.  The application will perform IO to the channel and channels are best thought of as being
associated 1:1 with a thread.

## Design Considerations {#blobfs_pg_design}

### Initialization Options

When the BlobFS is initialized, there are multiple default steps that need to follow:
* run mkfs tool to build the BlobFS on the target disk. This tool only need to be executed when you want to build a new
filesystem.

* spdk_fs_set_cache_size()
This will set the cache size for the cache buffers.

* spdk_bdev_get_by_name()
* spdk_bdev_create_bs_dev()
These two functions will ceate the low bdev support for blobstore.

* spdk_fs_load()
Load the BlobFS from the disk.

* spdk_allocate_thread()
On each system thread that the user wishes to use with SPDK, they must first call spdk_allocate_thread(). This function 
needs to be invoked on every thread context. It will construct the current thread context in SPDK.

* spdk_fs_alloc_io_channel_sync()
This function will be invoked on every thread context. This will allocate the io channel for every thread.

### Channels

BlobFS provides two types of channels, md_io_channel and sync_io_channel.
md_io_channel is used to submit md requests. sync_io_channel is used to submit IO requests.

## Examples {#blobfs_pg_examples}

There is one example of BlobFS usage in the [repo](https://github.com/spdk/spdk):

* **RocksDB with BlobFS **: The BlobFS have been integrated with RocksDB by providing an individual env layer for
RocksDB, you can refer to env_spdk.cc which is provided by SPDK. It implements one env layer which could used for RocksDB.

## Configuration {#blobfs_pg_config}

The cache buffer size is configurable. You could just set the "CacheBufferShift" item in conf file before you run
the BlobFS. BlobFS will parse this configuration when load the filesystem, the default value is CACHE_BUFFER_SHIFT_DEFAULT.

### Key Structures

`blobfs.c` contains many of the key structures for the BlobFS. Only a few notable ones are listed here.

~~~{.sh}
struct spdk_file
~~~
This is an in-memory data structure for "file" that contains key elements like the blob, name, length etc.

~~~{.sh}
struct spdk_filesystem
~~~
This is the data structure which represents the filesystem. It includes the blobstore which provides the backend support,
list of all the files, md_target, sync_target etc.

~~~{.sh}
struct spdk_fs_cb_args
~~~
This is the generic callback args which used for various spdk_fs_request. The callback function is in the form of
union {...} fn, and the args is in the form of union {...} op. when the request is done, BlobFS will call the callback
function and pass the args accordingly.

~~~{.sh}
struct spdk_fs_request
~~~
The internal data sturcture for various types of requests. The requests could be one of the following types:
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
