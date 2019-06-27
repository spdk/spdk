/*
 * slab.c
 *
 *  Created on: Jun 18, 2019
 *      Author: root
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/blob.h"
#include "spdk/blob_bdev.h"
#include "spdk/uuid.h"
#include "spdk/bdev_module.h"

#include "spdk_internal/log.h"
#include "spdk/slab.h"
#include "slab_internal.h"

#define SPDK_BDEV_BLOCKSIZE		512
#define CHUNK_ALIGN_BYTES		512
#define SPDK_SLAB_MAX_LCORE		64
#define SPDK_MAX_NUMBER_OF_SLAB_CLASSES (7 + 1)

int slab_num = 32;
int slab_bs_clr_size = 1024 * 1024; // 1MB
int slab_size = 1024 * 1024 * 32; //32MB


typedef struct spdk_slot_item {
	/* slabclass info */
	uint8_t         slabs_clsid;/* which slab class we're in */
	TAILQ_ENTRY(spdk_slot_item) tailq;

	/* ondisk info */
	struct spdk_blob *blob;
	uint64_t blob_blk_offset;
	uint64_t blob_blk_num;

	/* info of item inside record */
	uint8_t         nkey;       /* key length, w/terminating null and padding */
	int             nbytes;     /* size of data */
	int		total_nbytes; /* size of all this item on disk */
} spdk_slot_item_t;

typedef struct spdk_slab {
	TAILQ_ENTRY(spdk_slab) tailq;

	struct spdk_blob *blob;
	struct spdk_slot_item items[0];
} spdk_slab_t;

typedef struct {
	uint32_t size;      /* sizes of items */
	int perslab;   /* how many items per slab */

	TAILQ_HEAD(, spdk_slot_item) item_list; /* list of item ptrs */
	unsigned int slots_nfree;   /* total free items in list */

	unsigned int slabs;     /* how many slabs were allocated for this class */
	struct spdk_slab **slab_list;       /* array of slab pointers */
	unsigned int list_size; /* size of prev array */

	size_t requested; /* The number of requested bytes */
} slabclass_t;

struct spdk_slabs_per_core {
	slabclass_t slabclass[SPDK_MAX_NUMBER_OF_SLAB_CLASSES];
	uint8_t		class_num;

	TAILQ_HEAD(, spdk_slab) avail_slabs;

	struct spdk_io_channel *bs_io_channe; /* used to do blob_io_read/write */
};

struct spdk_slab_manager {
	struct spdk_slabs_per_core percores[SPDK_SLAB_MAX_LCORE];
	TAILQ_HEAD(, spdk_slab) prepared_slabs;

	struct spdk_cpuset *core_mask;

	struct spdk_bdev *bdev;
	struct spdk_bs_dev *bs_dev;
	struct spdk_blob_store *bs;
};

struct spdk_slab_opts;

struct spdk_slab_manager g_slab_mgr;


static int slabs_percore_init(const uint32_t *slab_sizes, int slab_num, int slab_page_size);


static struct spdk_slabs_per_core *
spdk_thread_get_slab_percore(void)
{
	struct spdk_slabs_per_core *percore;
	struct spdk_cpuset *thd_cpumask;
	int cpu_idx;

	thd_cpumask = spdk_thread_get_cpumask(spdk_get_thread());
	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "thd cpumask is %s\n", spdk_cpuset_fmt(thd_cpumask));

	cpu_idx = spdk_cpuset_first_index(thd_cpumask);
	assert(cpu_idx >= 0);
	if (cpu_idx > 3) {
		return NULL;
	}

	percore = &g_slab_mgr.percores[cpu_idx];

	return percore;
}

/* Manager Create Start */
#if 1
struct slab_mgr_create_req {
	spdk_slab_mgr_op_with_handle_complete cb_fn;
	void *cb_arg;

	int slab_prepare_count;
};


static void
__slab_percore_start_cpl(void *ctx)
{
	struct slab_mgr_create_req *req = ctx;

	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "All thread is started\n");
	req->cb_fn(req->cb_arg, 0);
	free(req);
}

static void
__slab_percore_start(void *ctx)
{
	struct spdk_slabs_per_core *percore;

	percore = spdk_thread_get_slab_percore();
	if (percore == NULL) {
		return;
	}

	percore->bs_io_channe = spdk_bs_alloc_io_channel(g_slab_mgr.bs);
}

static void
_slab_percore_prepare(struct slab_mgr_create_req *req)
{
	int i, j;
	struct spdk_slab *slab_tmp;
	struct spdk_slabs_per_core *percore;
	int rc;
	int class_num = 8;
	const uint32_t class_sizes[8] = {512, 1024, 2048, 4096, 4096 * 2, 4096 * 3, 4096 * 4, 4096 * 5};
	int thd_count = spdk_thread_get_count();

	assert(thd_count == 4);

	for (i = 0; i < thd_count; i++) {
		percore = &g_slab_mgr.percores[i];

		TAILQ_INIT(&percore->avail_slabs);
		for (j = 0; j < 8; j++) {
			assert(!TAILQ_EMPTY(&g_slab_mgr.prepared_slabs));

			slab_tmp = TAILQ_FIRST(&g_slab_mgr.prepared_slabs);
			TAILQ_REMOVE(&g_slab_mgr.prepared_slabs, slab_tmp, tailq);
			TAILQ_INSERT_TAIL(&percore->avail_slabs, slab_tmp, tailq);
		}


		rc = slabs_percore_init(class_sizes, class_num, i);
		if (rc != 0) {
			goto failed;
		}
	}

	/* Open blobstore io channel for each thread */
	spdk_for_each_thread(__slab_percore_start, req, __slab_percore_start_cpl);
	return;

failed:
	req->cb_fn(req->cb_arg, rc);
	free(req);

	return;
}

static void __slab_create(struct slab_mgr_create_req *req);

static void
__slab_create_open_cb(void *cb_arg, struct spdk_blob *blb, int bserrno)
{
	struct slab_mgr_create_req *req = cb_arg;
	struct spdk_slab *new_slab;

	assert(bserrno == 0);

	new_slab = calloc(1, sizeof(*new_slab));
	assert(new_slab);

	new_slab->blob = blb;
	TAILQ_INSERT_TAIL(&g_slab_mgr.prepared_slabs, new_slab, tailq);

	req->slab_prepare_count++;
	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "Opened blob num is %d\n", req->slab_prepare_count);

	if (req->slab_prepare_count < slab_num) {
		__slab_create(req);
	} else {
		_slab_percore_prepare(req);
	}
}

static void
__slab_create_cb(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	struct slab_mgr_create_req *req = cb_arg;
	struct spdk_blob_store *bs;

	assert(bserrno == 0);
	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "create blobid %lu\n", blobid);

	bs = g_slab_mgr.bs;
	spdk_bs_open_blob(bs, blobid, __slab_create_open_cb, req);
}

static void
__slab_create(struct slab_mgr_create_req *req)
{
	struct spdk_blob_opts opts;
	spdk_blob_opts_init(&opts);
	opts.num_clusters = slab_size / slab_bs_clr_size;

	spdk_bs_create_blob_ext(g_slab_mgr.bs, &opts, __slab_create_cb, req);
}

static void
_slabs_prepare(struct slab_mgr_create_req *req)
{
	int total_clr, free_clr;
	int bs_clr_size;

	total_clr = spdk_bs_total_data_cluster_count(g_slab_mgr.bs);
	free_clr = spdk_bs_free_cluster_count(g_slab_mgr.bs);
	bs_clr_size = spdk_bs_get_cluster_size(g_slab_mgr.bs);
	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "total_clr %d, free_clr %d, bs_clr_size %d\n", total_clr, free_clr,
		      bs_clr_size);

	req->slab_prepare_count = 0;
	__slab_create(req);
}

/* Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module _slab_bdev_module = {
	.name	= "Slab for Memcached Target",
};

static void
mgr_create_cb(void *ctx, struct spdk_blob_store *bs, int bserrno)
{
	struct slab_mgr_create_req *req = ctx;
	struct spdk_bs_type bstype;
	static const struct spdk_bs_type memcd_type = {"MEMCACHED"};
	static const struct spdk_bs_type zeros;

	if (bserrno != 0) {
		assert(0);
		return;
	}

	g_slab_mgr.bs = bs;
	spdk_bs_bdev_claim(g_slab_mgr.bs_dev, &_slab_bdev_module);

	bstype = spdk_bs_get_bstype(bs);
	if (!memcmp(&bstype, &zeros, sizeof(bstype))) {
		SPDK_DEBUGLOG(SPDK_LOG_SLAB, "assigning bstype\n");
		spdk_bs_set_bstype(bs, memcd_type);
	} else if (memcmp(&bstype, &memcd_type, sizeof(bstype))) {
		SPDK_DEBUGLOG(SPDK_LOG_SLAB, "not memcached\n");
		SPDK_LOGDUMP(SPDK_LOG_SLAB, "bstype", &bstype, sizeof(bstype));

		assert(0);
		return;
	}

	_slabs_prepare(req);

	return;
}

int
spdk_slab_mgr_create(const char *bdev_name, struct spdk_cpuset *core_mask,
		     struct spdk_slab_opts *o,
		     spdk_slab_mgr_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *bs_dev;
	struct spdk_bs_opts bs_opt;
	struct slab_mgr_create_req *req;

	req = calloc(1, sizeof(*req));
	assert(req);
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	bdev = spdk_bdev_get_by_name(bdev_name);
	assert(bdev);

	bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	assert(bs_dev);

	g_slab_mgr.bdev = bdev;
	g_slab_mgr.bs_dev = bs_dev;
	g_slab_mgr.core_mask = core_mask;
	TAILQ_INIT(&g_slab_mgr.prepared_slabs);

	spdk_bs_opts_init(&bs_opt);
	bs_opt.cluster_sz = slab_bs_clr_size;
	bs_opt.max_channel_ops *= 64;
	spdk_bs_init(bs_dev, &bs_opt, mgr_create_cb, req);

	return 0;
}
#endif
/* Manager Create End */


/* slabs_percore_init start */
#if 1

static void
do_slab_item_free(struct spdk_slot_item *item, struct spdk_slabs_per_core *slabs_pcore)
{
	int class_id = item->slabs_clsid;
	slabclass_t *p = &slabs_pcore->slabclass[class_id];

	assert(class_id >= 0 && class_id < SPDK_MAX_NUMBER_OF_SLAB_CLASSES);

	/* Operation on slabclass related */
	TAILQ_INSERT_TAIL(&p->item_list, item, tailq);
	p->slots_nfree++;

	return;
}

static void
_split_slab_items_into_freelist(struct spdk_slabs_per_core *slabs_pcore, int class_id,
				struct spdk_slab *slab)
{
	slabclass_t *p = &slabs_pcore->slabclass[class_id];
	struct spdk_slot_item *item;
	int x;

	for (x = 0; x < p->perslab; x++) {
		item = &slab->items[x];
		item->slabs_clsid = class_id;
		do_slab_item_free(item, slabs_pcore);
	}

	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "class_id %d, p->size %u, p->slots_nfree %d\n", class_id, p->size,
		      p->slots_nfree);
}

static int
_map_slab_items_to_blob(struct spdk_slabs_per_core *slabs_pcore, int class_id,
			struct spdk_slab **_new_slab)
{
	slabclass_t *p = &slabs_pcore->slabclass[class_id];
	struct spdk_slab *new_slab, *ori_slab = *_new_slab;
	struct spdk_slot_item *item;
	int i;
	int len;

	len = sizeof(*new_slab) + p->perslab * sizeof(*item);
	new_slab = realloc(ori_slab, len);
	if (new_slab == NULL) {
		return -ENOMEM;
	}


	for (i = 0; i < p->perslab; i++) {
		item = &new_slab->items[i];
		item->blob = new_slab->blob;
		item->blob_blk_offset = p->size * i / SPDK_BDEV_BLOCKSIZE;
		item->blob_blk_num = p->size / SPDK_BDEV_BLOCKSIZE;
	}

	/* Since ori_slab is unlinked already, so it is not necessary to change its link */
	*_new_slab = new_slab;

	return 0;
}

static int
_get_new_slab_local(struct spdk_slabs_per_core *slabs_pcore, struct spdk_slab **_new_slab)
{
	struct spdk_slab *new_slab;

	if (TAILQ_EMPTY(&slabs_pcore->avail_slabs)) {
		return -1;
	}

	new_slab = TAILQ_FIRST(&slabs_pcore->avail_slabs);
	TAILQ_REMOVE(&g_slab_mgr.prepared_slabs, new_slab, tailq);
	*_new_slab = new_slab;

	return 0;
}

static int
_grow_slab_list(slabclass_t *p)
{
	if (p->slabs == p->list_size) {
		size_t new_size = (p->list_size != 0) ? p->list_size * 2 : 16;
		void *new_list = realloc(p->slab_list, new_size * sizeof(void *));

		if (new_list == NULL) {
			return -ENOMEM;
		}

		p->list_size = new_size;
		p->slab_list = new_list;
	}

	return 0;
}

static int
do_slabclass_newslab(struct spdk_slabs_per_core *slabs_pcore, int class_id)
{
	slabclass_t *p = &slabs_pcore->slabclass[class_id];
	struct spdk_slab *new_slab;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "Add new slab for class %d\n", class_id);
	rc = _grow_slab_list(p);
	assert(rc == 0);

	//TODO: its result should be returned in its callback, here only assume blob is already assigned to percore.
	rc = _get_new_slab_local(slabs_pcore, &new_slab);
	assert(rc == 0);

	rc = _map_slab_items_to_blob(slabs_pcore, class_id, &new_slab);
	assert(rc == 0);

	_split_slab_items_into_freelist(slabs_pcore, class_id, new_slab);

	p->slab_list[p->slabs++] = new_slab;

	return rc;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
static int
slabs_percore_init(const uint32_t *class_sizes, int class_num, int core_idx)
{
	struct spdk_slabs_per_core *slabs_pcore = &g_slab_mgr.percores[core_idx];
	slabclass_t *slabclass = slabs_pcore->slabclass;
	int i;
	int rc = 0;

	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "Percore init for core %d\n", core_idx);
	if (class_num > SPDK_MAX_NUMBER_OF_SLAB_CLASSES || class_num < 0) {
		return -EINVAL;
	}

	memset(slabs_pcore->slabclass, 0, sizeof(slabs_pcore->slabclass));
	slabs_pcore->class_num = class_num;

	for (i = 0; i < class_num; i++) {
		int size;

		/* align each class size with blocksize */
		size = class_sizes[i];
		if (size % CHUNK_ALIGN_BYTES != 0) {
			size = (size + CHUNK_ALIGN_BYTES - 1) / CHUNK_ALIGN_BYTES * CHUNK_ALIGN_BYTES;
			SPDK_WARNLOG("Unaligned slab size %d, aligned it to be %d\n", class_sizes[i], size);
		}

		slabclass[i].size = size;
		slabclass[i].perslab = slab_size / slabclass[i].size;

		SPDK_DEBUGLOG(SPDK_LOG_SLAB, "slab class %3d: chunk size %9u perslab %7u\n",
			      i, slabclass[i].size, slabclass[i].perslab);

		TAILQ_INIT(&slabclass[i].item_list);
	}

	for (i = 0; i < class_num; i++) {
		rc = do_slabclass_newslab(slabs_pcore, i);
		if (rc) {
			SPDK_WARNLOG("Error while adding new slab to slabclass %d\n", i);
			assert(rc);
		}
	}
	SPDK_DEBUGLOG(SPDK_LOG_SLAB, "core %d is initialized\n", core_idx);

	return rc;
}
#endif
/* slabs_percore_init end */


/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */
static int slabs_clsid(uint32_t size)
{
	struct spdk_slabs_per_core *slabs_pcore = spdk_thread_get_slab_percore();
	slabclass_t *slabclass = slabs_pcore->slabclass;
	uint32_t class_num = slabs_pcore->class_num;
	int i;

	if (size == 0 || size > slabclass[class_num - 1].size) {
		return -EINVAL;
	}

	for (i = 0; i < slabs_pcore->class_num; i++) {
		if (size < slabclass[i].size) {
			return i;
		}
	}

	assert(0);
	return 0;
}

int
spdk_slab_get_item(int size, struct spdk_slot_item **_item)
{
	struct spdk_slabs_per_core *percore;
	uint32_t class_id = slabs_clsid(size);

	percore = spdk_thread_get_slab_percore();
	if (percore == NULL) {
		return -EIO;
	}

	slabclass_t *p;
	struct spdk_slot_item *item;
	int rc = -1;

	p = &percore->slabclass[class_id];

	if (p->slots_nfree == 0) {
		//TODO: add newslab in runtime
		assert(0);
		do_slabclass_newslab(percore, class_id);
	}

	if (p->slots_nfree != 0) {
		item = TAILQ_FIRST(&p->item_list);
		TAILQ_REMOVE(&p->item_list, item, tailq);

		p->slots_nfree--;
		*_item = item;

		rc = 0;
	}

	return rc;
}

int
spdk_slab_put_item(struct spdk_slot_item *item)
{
	struct spdk_slabs_per_core *percore;
	int rc = 0;

	percore = spdk_thread_get_slab_percore();

	do_slab_item_free(item, percore);

	return rc;
}

bool spdk_slab_item_is_valid(struct spdk_slot_item *item)
{
	return false;
}

int spdk_slab_item_get_data_size(struct spdk_slot_item *item)
{
	return 0;

}

struct slab_rw_op_complete_req {
	spdk_slab_item_rw_cb cb;
	void *cb_arg;
};

static void
slab_item_rw_op_cpl(void *cb_arg, int bserrno)
{
	struct slab_rw_op_complete_req *req = cb_arg;

	req->cb(req->cb_arg, bserrno);
	free(req);
}

static int
_slab_item_rw(struct spdk_slot_item *item, char *buf, uint32_t len,
	      spdk_slab_item_rw_cb cb, void *cb_arg, bool obtain)
{
	struct spdk_slabs_per_core *percore = spdk_thread_get_slab_percore();
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	uint64_t offset;
	uint32_t blk_num;
	struct slab_rw_op_complete_req *req;

	req = calloc(1, sizeof(req));
	assert(req);

	blob = item->blob;
	channel = percore->bs_io_channe;
	offset = item->blob_blk_offset;
	blk_num = item->blob_blk_num;

	assert(len <= blk_num * 512);

	req->cb = cb;
	req->cb_arg = cb_arg;

	if (obtain == true) {
		spdk_blob_io_read(blob, channel, buf, offset, blk_num,
				  slab_item_rw_op_cpl, req);
	} else {
		spdk_blob_io_write(blob, channel, buf, offset, blk_num,
				   slab_item_rw_op_cpl, req);
	}

	return 0;
}

int spdk_slab_item_store(struct spdk_slot_item *item, const char *buf, uint32_t len,
			 spdk_slab_item_rw_cb cb, void *cb_arg)
{
	return _slab_item_rw(item, (char *)buf, len, cb, cb_arg, false);
}

int spdk_slab_item_obtain(struct spdk_slot_item *item, char *buf, uint32_t len,
			  spdk_slab_item_rw_cb cb, void *cb_arg)
{
	return _slab_item_rw(item, buf, len, cb, cb_arg, true);
}

SPDK_LOG_REGISTER_COMPONENT("slab", SPDK_LOG_SLAB)

