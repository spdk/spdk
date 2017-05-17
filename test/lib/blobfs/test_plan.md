# SPDK BlobFS Test Plan

## Current Tests

# Unit tests (asynchronous API)

- Tests BlobFS w/ Blobstore with no dependencies on SPDK bdev layer or event framework.
  Uses simple DRAM buffer to simulate a block device - all block operations are immediately
  completed so no special event handling is required.
- Current tests include:
	- basic fs initialization and unload
	- open non-existent file fails if SPDK_BLOBFS_OPEN_CREATE not specified
	- open non-existent file creates the file if SPDK_BLOBFS_OPEN_CREATE is specified
	- close a file fails if there are still open references
	- closing a file with no open references fails
	- files can be truncated up and down in length
	- three-way rename
	- operations for inserting and traversing buffers in a cache tree
	- allocating and freeing I/O channels

# Unit tests (synchronous API)

- Tests BlobFS w/ Blobstore with no dependencies on SPDK bdev layer or event framework.
  The synchronous API requires a separate thread to handle any asynchronous handoffs such as
  I/O to disk.
	- basic read/write I/O operations
	- appending to a file whose cache has been flushed and evicted

# RocksDB

- Tests BlobFS as the backing store for a RocksDB database.  BlobFS uses the SPDK NVMe driver
  through the SPDK bdev layer as its block device.  Uses RocksDB db_bench utility to drive
  the workloads.  Each workload (after the initial sequential insert) reloads the database
  which validates metadata operations completed correctly in the previous run via the
  RocksDB MANIFEST file.  RocksDB also runs checksums on key/value blocks read from disk,
  verifying data integrity.
	- initialize BlobFS filesystem on NVMe SSD
	- bulk sequential insert of up to 500M keys (16B key, 1000B value)
	- overwrite test - randomly overwrite one of the keys in the database (driving both
	  flush and compaction traffic)
	- readwrite test - one thread randomly overwrites a key in the database, up to 16
	  threads randomly read a key in the database.
	- writesync - same as overwrite, but enables a WAL (write-ahead log)
	- randread - up to 16 threads randomly read a key in the database

## Future tests to add

# Unit tests

- Corrupt data in DRAM buffer, and confirm subsequent operations such as BlobFS load or
  opening a blob fail as expected (no panics, etc.)
- Test synchronous API with multiple synchronous threads.  May be implemented separately
  from existing synchronous unit tests to allow for more sophisticated thread
  synchronization.
- Add tests for out of capacity (no more space on disk for additional blobs/files)
- Pending addition of BlobFS superblob, verify that BlobFS load fails with missing or
  corrupt superblob
- Additional tests to reach 100% unit test coverage

# System/integration tests

- Use fio with BlobFS fuse module for more focused data integrity testing on individual
  files.
- Pending directory support (via an SPDK btree module), use BlobFS fuse module to do
  things like a Linux kernel compilation.  Performance may be poor but this will heavily
  stress the mechanics of BlobFS.
- Run RocksDB tests with varying amounts of BlobFS cache
