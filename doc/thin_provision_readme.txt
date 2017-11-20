Cluster allocation data path
============================

For write and write zeroes:

This is done after the user request was split on cluster
boundaries - so that this logic does not need to comprehend
I/O that span cluster boundaries.

Each blobstore channel allocates a memory buffer with size
of cluster.  Each channel will only have one outstanding
allocation in progress at any time.

if cluster is not allocated
  if channel already has an allocation in progress
    put request on allocation queue
    return
  allocate cluster (but do not update cluster map)
  allocate cluster-sized memory buffer
  read data from backing blob (or memset to zeroes)
  overwrite part of memory buffer with data from I/O
  write data to disk
  atomic compare and swap new cluster index to cluster map
    (note: this is needed to guard against simultaneous
     allocations for same cluster on different threads)
  if compare and swap was successful
    sync metadata
  else
    free cluster and memory buffer
    retry operation from start
  start next allocation request on queue (if any)

Blobstore sync operations
=========================

Currently blobstore does not support multiple sync
operations at a time on a blob.  This needs to be fixed
for thin provisioning, because there will now be implicit
sync operations after clusters are allocated.  These implicit
sync operations cannot fail - we must have a way to queue them.

We also cannot allow operations that update metadata to occur
while a sync is in progress.  This is because the sync operation
needs to translate the in-memory representation to the on-disk
format, and we cannot have a thread updating the in-memory
representation while another threads is reading it.

So we will add an md_op_in_progress flag to spdk_blob.  Any
metadata operation except sync (resize, set xattr, get xattr,
remove xattr, future ops like "set permissions") will atomic
compare and swap this flag - if it fails, return -EBUSY.

sync operations will also do the compare and swap - if it
passes, continue with the sync operation.  If it fails, put
the request on a lock-protected queue in the spdk_blob.

After the md_op is done, clear the md_op_in_progress flag and
check the lock-protected queue for additional sync operations.
If there are any, start the next one on the list.  If the next
one was submitted on a different thread, send a message to that
thread to start it instead.

Blobstore - metadata sync after allocation
==========================================

After a cluster has been allocated on a thin provisioned blob,
metadata must be synced to disk for the updated cluster map.
Treat this as a "normal" sync operation, leveraging the
procedures listed above to account for metadata operations
already in progress when the sync starts.

lvol snapshots
==============

Users will want to create snapshots at runtime.  In this case,
we want the original blob/lvol to stay the same, and make the
snapshot a new blob/lvol.  blobstore will add a new function
spdk_bs_md_create_snapshot_opts().  

struct spdk_lvol *lvol, *lvol_snapshot;
struct spdk_blob_snapshot_opts opts;

/* open lvol */

lvol_snapshot->uuid = new_uuid;
lvol_snapshot->name = some_name;

spdk_blob_snapshot_opts_init(&opts);
opts.name = "lvol:new_uuid";
opts.xattr_count = 2;
opts.xattr_names = calloc(2, sizeof(char *));
opts.xattr_names[0] = "uuid";
opts.xattr_names[1] = "some_name";
opts.get_xattr_value = lvol_get_xattr_value;

spdk_bs_md_create_blob_snapshot_opts(lvol->blob, &opts, done_fn, lvol_snapshot);

Sequence of operations:
1) create new blob with:
	xattrs as defined in opts
	read_only and md_read_only permissions set
	same cluster map as original blob
	same base name as original blob
	add new metadata descriptor SNAPSHOT_IN_PROGRESS with blobid of original blob
2) sync new blob metadata
3) freeze I/O on original blob
4) change old blob metadata:
	all zeroes in cluster map
	base name = name defined in opts
5) sync original blob metadata
6) change new blob metadata:
	remove SNAPSHOT_IN_PROGRESS metadata descriptor
7) sync new blob metadata
8) thaw I/O on original blob

SNAPSHOT_IN_PROGRESS metadata descriptor is needed in case system crashes during
this sequence of operations.  If there was a crash, we could have two valid
metadata pages referring to the same clusters.  If during blobstore load, the
clean bit is not set, we will need to scan all blobs for the SNAPSHOT_IN_PROGRESS
descriptor.  If it finds one, it must:
	1) copy cluster map to blob with blobid indicated in SNAPSHOT_IN_PROGRESS descriptor
	2) copy base name to blob with blobid indicated in SNAPSHOT_IN_PROGRESS descriptor
	3) delete blob containing SNAPSHOT_IN_PROGRESS descriptor


