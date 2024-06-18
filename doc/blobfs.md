# BlobFS (Blobstore Filesystem) {#blobfs}

## BlobFS Getting Started Guide {#blobfs_getting_started}

## RocksDB Integration {#blobfs_rocksdb}

Clone and build the SPDK repository as per https://github.com/spdk/spdk

~~~{.sh}
git clone https://github.com/spdk/spdk.git
cd spdk
./configure
make
~~~

Clone the RocksDB repository from the SPDK GitHub fork into a separate directory.
Make sure you check out the `6.15.fb` branch.

~~~{.sh}
cd ..
git clone -b 6.15.fb https://github.com/spdk/rocksdb.git
~~~

Build RocksDB.  Only the `db_bench` benchmarking tool is integrated with BlobFS.

~~~{.sh}
cd rocksdb
make db_bench SPDK_DIR=relative_path/to/spdk
~~~

Or you can also add `DEBUG_LEVEL=0` for a release build (need to turn on `USE_RTTI`).

~~~{.sh}
export USE_RTTI=1 && make db_bench DEBUG_LEVEL=0 SPDK_DIR=relative_path/to/spdk
~~~

Create an NVMe section in the configuration file using SPDK's `gen_nvme.sh` script.

~~~{.sh}
scripts/gen_nvme.sh --json-with-subsystems > /usr/local/etc/spdk/rocksdb.json
~~~

Verify the configuration file has specified the correct NVMe SSD.
If there are any NVMe SSDs you do not wish to use for RocksDB/SPDK testing, remove them from the configuration file.

Make sure you have at least 5GB of memory allocated for huge pages.
By default, the SPDK `setup.sh` script only allocates 2GB.
The following will allocate 5GB of huge page memory (in addition to binding the NVMe devices to uio/vfio).

~~~{.sh}
HUGEMEM=5120 scripts/setup.sh
~~~

Create an empty SPDK blobfs for testing.

~~~{.sh}
test/blobfs/mkfs/mkfs /usr/local/etc/spdk/rocksdb.json Nvme0n1
~~~

At this point, RocksDB is ready for testing with SPDK.  Three `db_bench` parameters are used to configure SPDK:

1. `spdk` - Defines the name of the SPDK configuration file.  If omitted, RocksDB will use the default PosixEnv implementation
   instead of SpdkEnv. (Required)
2. `spdk_bdev` - Defines the name of the SPDK block device which contains the BlobFS to be used for testing. (Required)
3. `spdk_cache_size` - Defines the amount of userspace cache memory used by SPDK.  Specified in terms of megabytes (MB).
   Default is 4096 (4GB).  (Optional)

SPDK has a set of scripts which will run `db_bench` against a variety of workloads and capture performance and profiling
data.  The primary script is `test/blobfs/rocksdb/rocksdb.sh`.

## FUSE

BlobFS provides a FUSE plug-in to mount an SPDK BlobFS as a kernel filesystem for inspection or debug purposes.
The FUSE plug-in requires fuse3 and will be built automatically when fuse3 is detected on the system.

~~~{.sh}
test/blobfs/fuse/fuse /usr/local/etc/spdk/rocksdb.json Nvme0n1 /mnt/fuse
~~~

Note that the FUSE plug-in has some limitations - see the list below.

## Limitations

* BlobFS has primarily been tested with RocksDB so far, so any use cases different from how RocksDB uses a filesystem
  may run into issues.  BlobFS will be tested in a broader range of use cases after this initial release.
* Only a synchronous API is currently supported.  An asynchronous API has been developed but not thoroughly tested
  yet so is not part of the public interface yet.  This will be added in a future release.
* File renames are not atomic.  This will be fixed in a future release.
* BlobFS currently supports only a flat namespace for files with no directory support.  Filenames are currently stored
  as xattrs in each blob.  This means that filename lookup is an O(n) operation.  An SPDK btree implementation is
  underway which will be the underpinning for BlobFS directory support in a future release.
* Writes to a file must always append to the end of the file.  Support for writes to any location within the file
  will be added in a future release.
