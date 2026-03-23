/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_kvmalloc.h"

#include "spdk/stdinc.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/tree.h"
#include "spdk/nvme_spec.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"
#include "spdk/log.h"

/* Default KV parameter values */
#define KVMALLOC_DEFAULT_MAX_KEY_SIZE		16
#define KVMALLOC_DEFAULT_MAX_VALUE_SIZE		(128 * 1024)
#define KVMALLOC_DEFAULT_OPTIMAL_VALUE_GRANULARITY	4096

/* Key-Value entry stored in RB_TREE.
 * key is dynamically allocated because max key size is configurable per-bdev. */
struct kvmalloc_kv_entry {
	RB_ENTRY(kvmalloc_kv_entry) node;
	uint8_t *key;
	uint8_t key_len;
	uint8_t *value;
	uint32_t value_len;
};

/* RB_TREE for storing key-value pairs */
RB_HEAD(kvmalloc_tree, kvmalloc_kv_entry);

static int
kvmalloc_key_cmp(struct kvmalloc_kv_entry *a, struct kvmalloc_kv_entry *b)
{
	if (a->key_len != b->key_len) {
		return (int)a->key_len - (int)b->key_len;
	}

	return memcmp(a->key, b->key, a->key_len);
}

RB_GENERATE_STATIC(kvmalloc_tree, kvmalloc_kv_entry, node, kvmalloc_key_cmp);

struct kvmalloc_disk {
	struct spdk_bdev		disk;
	struct kvmalloc_tree		kv_tree;
	/*
	 * The tree is shared across all threads that submit I/O to the bdev, so every
	 * operation acquires this lock. This is a reference implementation that prioritizes
	 * simplicity over throughput; production KV backends should use a more scalable
	 * scheme (e.g. per-shard locks or RCU).
	 */
	struct spdk_spinlock		kv_tree_lock;

	uint16_t			max_key_size;
	uint32_t			max_value_size;
	uint32_t			optimal_value_granularity;

	TAILQ_ENTRY(kvmalloc_disk)	link;
};

struct kvmalloc_channel {
	/* No per-channel state needed for now */
};

static TAILQ_HEAD(, kvmalloc_disk) g_kvmalloc_disks = TAILQ_HEAD_INITIALIZER(g_kvmalloc_disks);
static int g_kvmalloc_disk_count = 0;

static int bdev_kvmalloc_init(void);
static void bdev_kvmalloc_fini(void);

static struct spdk_bdev_module kvmalloc_if = {
	.name = "kvmalloc",
	.module_init = bdev_kvmalloc_init,
	.module_fini = bdev_kvmalloc_fini,
};

SPDK_BDEV_MODULE_REGISTER(kvmalloc, &kvmalloc_if)

/* Extract key from NVMe command CDW2-3 and CDW14-15. */
static int
kvmalloc_extract_key(const struct spdk_nvme_cmd *cmd, uint16_t max_key_size,
		     uint8_t **key_out, uint8_t *key_len_out)
{
	uint8_t key_len;
	uint8_t *key;
	const uint8_t *key_src;

	key_len = cmd->cdw11_bits.kv.kl;
	if (key_len == 0 || key_len > max_key_size) {
		return -EINVAL;
	}

	key = malloc(key_len);
	if (key == NULL) {
		return -ENOMEM;
	}

	/* First 8 bytes of key are in CDW2-3 */
	key_src = (const uint8_t *)&cmd->cdw2;
	memcpy(key, key_src, spdk_min(key_len, 8));

	/* Remaining bytes (up to 8 more) are in CDW14-15 */
	if (key_len > 8) {
		key_src = (const uint8_t *)&cmd->cdw14;
		memcpy(key + 8, key_src, key_len - 8);
	}

	*key_out = key;
	*key_len_out = key_len;
	return 0;
}

static void
kvmalloc_free_entry(struct kvmalloc_kv_entry *entry)
{
	free(entry->key);
	free(entry->value);
	free(entry);
}

static struct kvmalloc_kv_entry *
kvmalloc_find_entry(struct kvmalloc_tree *tree, const uint8_t *key, uint8_t key_len)
{
	struct kvmalloc_kv_entry search;

	search.key = (uint8_t *)key;
	search.key_len = key_len;

	return RB_FIND(kvmalloc_tree, tree, &search);
}

/* Handle KV STORE command */
static void
kvmalloc_handle_store(struct kvmalloc_disk *kvmalloc, const struct spdk_nvme_cmd *cmd,
		      struct iovec *iovs, int iovcnt, uint32_t data_len,
		      struct spdk_bdev_io *bdev_io)
{
	struct kvmalloc_kv_entry *entry;
	struct spdk_iov_xfer ix;
	uint8_t *key;
	uint8_t key_len;
	uint8_t options;
	int rc;

	rc = kvmalloc_extract_key(cmd, kvmalloc->max_key_size, &key, &key_len);
	if (rc != 0) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_KEY_SIZE);
		return;
	}

	if (data_len > kvmalloc->max_value_size) {
		free(key);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_VALUE_SIZE);
		return;
	}

	options = cmd->cdw11_bits.kv.ro;
	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	spdk_spin_lock(&kvmalloc->kv_tree_lock);

	entry = kvmalloc_find_entry(&kvmalloc->kv_tree, key, key_len);

	if (entry != NULL) {
		/* Key exists */
		if (options & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS) {
			spdk_spin_unlock(&kvmalloc->kv_tree_lock);
			free(key);
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_KEY_EXISTS);
			return;
		}

		/* Update existing entry. A zero length value is legal, and
		 * malloc(0) may or may not return NULL, so don't call it. */
		if (entry->value_len != data_len) {
			uint8_t *new_value = NULL;

			if (data_len > 0) {
				new_value = malloc(data_len);
				if (new_value == NULL) {
					spdk_spin_unlock(&kvmalloc->kv_tree_lock);
					free(key);
					spdk_bdev_io_complete_nvme_status(bdev_io, 0,
									  SPDK_NVME_SCT_GENERIC,
									  SPDK_NVME_SC_CAPACITY_EXCEEDED);
					return;
				}
			}
			free(entry->value);
			entry->value = new_value;
		}
		spdk_iov_xfer_to_buf(&ix, entry->value, data_len);
		entry->value_len = data_len;
		free(key);
	} else {
		/* Key doesn't exist */
		if (options & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS) {
			spdk_spin_unlock(&kvmalloc->kv_tree_lock);
			free(key);
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
			return;
		}

		/* Create new entry */
		entry = calloc(1, sizeof(*entry));
		if (entry == NULL) {
			spdk_spin_unlock(&kvmalloc->kv_tree_lock);
			free(key);
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_CAPACITY_EXCEEDED);
			return;
		}

		entry->key = key;
		entry->key_len = key_len;
		if (data_len > 0) {
			entry->value = malloc(data_len);
			if (entry->value == NULL) {
				spdk_spin_unlock(&kvmalloc->kv_tree_lock);
				kvmalloc_free_entry(entry);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_CAPACITY_EXCEEDED);
				return;
			}
		}
		spdk_iov_xfer_to_buf(&ix, entry->value, data_len);
		entry->value_len = data_len;

		if (RB_INSERT(kvmalloc_tree, &kvmalloc->kv_tree, entry) != NULL) {
			spdk_spin_unlock(&kvmalloc->kv_tree_lock);
			kvmalloc_free_entry(entry);
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_KEY_EXISTS);
			return;
		}
	}

	spdk_spin_unlock(&kvmalloc->kv_tree_lock);
	spdk_bdev_io_complete_nvme_status(bdev_io, 0,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_SUCCESS);
}

/* Handle KV RETRIEVE command */
static void
kvmalloc_handle_retrieve(struct kvmalloc_disk *kvmalloc, const struct spdk_nvme_cmd *cmd,
			 struct iovec *iovs, int iovcnt, uint32_t data_len,
			 struct spdk_bdev_io *bdev_io)
{
	struct kvmalloc_kv_entry *entry;
	struct spdk_iov_xfer ix;
	uint8_t *key;
	uint8_t key_len;
	uint32_t copy_len;
	uint32_t value_len;
	int rc;

	rc = kvmalloc_extract_key(cmd, kvmalloc->max_key_size, &key, &key_len);
	if (rc != 0) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_KEY_SIZE);
		return;
	}

	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	spdk_spin_lock(&kvmalloc->kv_tree_lock);

	entry = kvmalloc_find_entry(&kvmalloc->kv_tree, key, key_len);
	free(key);

	if (entry == NULL) {
		spdk_spin_unlock(&kvmalloc->kv_tree_lock);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
		return;
	}

	copy_len = spdk_min(data_len, entry->value_len);
	spdk_iov_xfer_from_buf(&ix, entry->value, copy_len);
	value_len = entry->value_len;

	spdk_spin_unlock(&kvmalloc->kv_tree_lock);
	spdk_bdev_io_complete_nvme_status(bdev_io, value_len,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_SUCCESS);
}

/* Handle KV DELETE command */
static void
kvmalloc_handle_delete(struct kvmalloc_disk *kvmalloc, const struct spdk_nvme_cmd *cmd,
		       struct spdk_bdev_io *bdev_io)
{
	struct kvmalloc_kv_entry *entry;
	uint8_t *key;
	uint8_t key_len;
	int rc;

	rc = kvmalloc_extract_key(cmd, kvmalloc->max_key_size, &key, &key_len);
	if (rc != 0) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_KEY_SIZE);
		return;
	}

	spdk_spin_lock(&kvmalloc->kv_tree_lock);

	entry = kvmalloc_find_entry(&kvmalloc->kv_tree, key, key_len);
	free(key);

	if (entry == NULL) {
		spdk_spin_unlock(&kvmalloc->kv_tree_lock);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
		return;
	}

	RB_REMOVE(kvmalloc_tree, &kvmalloc->kv_tree, entry);
	kvmalloc_free_entry(entry);

	spdk_spin_unlock(&kvmalloc->kv_tree_lock);
	spdk_bdev_io_complete_nvme_status(bdev_io, 0,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_SUCCESS);
}

/* Handle KV EXIST command */
static void
kvmalloc_handle_exist(struct kvmalloc_disk *kvmalloc, const struct spdk_nvme_cmd *cmd,
		      struct spdk_bdev_io *bdev_io)
{
	struct kvmalloc_kv_entry *entry;
	uint8_t *key;
	uint8_t key_len;
	uint32_t value_len;
	int rc;

	rc = kvmalloc_extract_key(cmd, kvmalloc->max_key_size, &key, &key_len);
	if (rc != 0) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_KEY_SIZE);
		return;
	}

	spdk_spin_lock(&kvmalloc->kv_tree_lock);

	entry = kvmalloc_find_entry(&kvmalloc->kv_tree, key, key_len);
	free(key);

	if (entry == NULL) {
		spdk_spin_unlock(&kvmalloc->kv_tree_lock);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST);
		return;
	}

	value_len = entry->value_len;

	spdk_spin_unlock(&kvmalloc->kv_tree_lock);
	spdk_bdev_io_complete_nvme_status(bdev_io, value_len,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_SUCCESS);
}

/*
 * Find the first entry in the tree whose key is >= the given search key.
 * Returns the first entry in the tree if search_key is NULL.
 */
static struct kvmalloc_kv_entry *
kvmalloc_find_start(struct kvmalloc_tree *tree, const uint8_t *search_key, uint8_t search_key_len)
{
	struct kvmalloc_kv_entry probe;

	if (search_key == NULL) {
		return RB_MIN(kvmalloc_tree, tree);
	}

	probe.key = (uint8_t *)search_key;
	probe.key_len = search_key_len;
	return RB_NFIND(kvmalloc_tree, tree, &probe);
}

/* Handle KV LIST command.
 *
 * The list response buffer format per the NVMe KV spec (Figures 15-16):
 *   Bytes 0-3:  Number of keys returned (uint32_t)
 *   Then for each key entry (padded to 4-byte boundary):
 *     Bytes 0-1: Key length (uint16_t)
 *     Bytes 2-N: Key data
 *     Padding to next 4-byte boundary
 */
static void
kvmalloc_handle_list(struct kvmalloc_disk *kvmalloc, const struct spdk_nvme_cmd *cmd,
		     struct iovec *iovs, int iovcnt, uint32_t data_len,
		     struct spdk_bdev_io *bdev_io)
{
	struct kvmalloc_kv_entry *entry;
	struct spdk_iov_xfer ix;
	uint8_t *start_key = NULL;
	uint8_t start_key_len = 0;
	uint32_t key_count = 0;
	uint32_t remaining;
	uint8_t *buf;
	uint8_t *out_ptr;
	int rc;

	if (iovcnt == 0 || data_len < 4) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_FIELD);
		return;
	}

	if (cmd->cdw11_bits.kv.kl > 0) {
		rc = kvmalloc_extract_key(cmd, kvmalloc->max_key_size, &start_key, &start_key_len);
		if (rc != 0) {
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_INVALID_KEY_SIZE);
			return;
		}
	}

	/* Build the response into a contiguous buffer. The count at offset 0
	 * is only known after the loop runs, and writing it last keeps the
	 * iov-scatter at the end simple. */
	buf = calloc(1, data_len);
	if (buf == NULL) {
		free(start_key);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	}

	out_ptr = buf + 4;
	remaining = data_len - 4;

	spdk_spin_lock(&kvmalloc->kv_tree_lock);

	entry = kvmalloc_find_start(&kvmalloc->kv_tree, start_key, start_key_len);
	for (; entry != NULL; entry = RB_NEXT(kvmalloc_tree, &kvmalloc->kv_tree, entry)) {
		uint32_t entry_len = SPDK_ALIGN_CEIL(2 + (uint32_t)entry->key_len, 4);

		if (remaining < entry_len) {
			break;
		}

		*(uint16_t *)out_ptr = entry->key_len;
		memcpy(out_ptr + 2, entry->key, entry->key_len);
		out_ptr += entry_len;
		remaining -= entry_len;

		key_count++;
	}

	spdk_spin_unlock(&kvmalloc->kv_tree_lock);

	free(start_key);

	*(uint32_t *)buf = key_count;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	spdk_iov_xfer_from_buf(&ix, buf, data_len);

	free(buf);

	spdk_bdev_io_complete_nvme_status(bdev_io, 0,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_SUCCESS);
}

static void
kvmalloc_handle_admin_identify(struct kvmalloc_disk *kvmalloc, struct spdk_bdev_io *bdev_io)
{
	struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	void *buf = bdev_io->u.nvme_passthru.buf;
	uint32_t nbytes = bdev_io->u.nvme_passthru.nbytes;
	uint8_t cns = cmd->cdw10_bits.identify.cns;
	uint8_t csi = cmd->cdw11_bits.identify.csi;

	if (csi != SPDK_NVME_CSI_KV) {
		goto invalid;
	}

	switch (cns) {
	case SPDK_NVME_IDENTIFY_NS_IOCS: {
		struct spdk_nvme_kv_ns_data *nsdata;
		const struct spdk_uuid *uuid;

		memset(buf, 0, nbytes);
		nsdata = buf;

		/* In-memory KV store has no fixed capacity. Report an unbounded
		 * namespace size and leave nuse zero so capacity-aware hosts
		 * don't reject the namespace.
		 * TODO: nuse should reflect the bytes currently stored. Track
		 * live usage in the tree and report it here. */
		nsdata->nsze = UINT64_MAX;
		nsdata->nuse = 0;

		nsdata->nkvf = 0;
		nsdata->kvfc.kvfi = 0;
		nsdata->novg = kvmalloc->optimal_value_granularity;
		nsdata->kvf[0].kvkml = kvmalloc->max_key_size;
		nsdata->kvf[0].kvvml = kvmalloc->max_value_size;

		uuid = spdk_bdev_get_uuid(&kvmalloc->disk);
		SPDK_STATIC_ASSERT(sizeof(nsdata->nguid) == sizeof(*uuid), "size mismatch");
		memcpy(nsdata->nguid, uuid, sizeof(nsdata->nguid));

		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_SUCCESS);
		return;
	}
	case SPDK_NVME_IDENTIFY_CTRLR_IOCS: {
		struct spdk_nvme_kv_ctrlr_data *cdata;

		memset(buf, 0, nbytes);
		cdata = buf;
		cdata->ver = SPDK_NVME_KV_SPEC_VER;

		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_SUCCESS);
		return;
	}
	default:
		break;
	}

invalid:
	spdk_bdev_io_complete_nvme_status(bdev_io, 0,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_INVALID_FIELD);
}

static void
kvmalloc_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct kvmalloc_disk *kvmalloc = bdev_io->bdev->ctxt;
	struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	struct iovec single_iov;
	struct iovec *iovs = NULL;
	uint32_t data_len = 0;
	int iovcnt = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		if (cmd->opc == SPDK_NVME_OPC_IDENTIFY) {
			kvmalloc_handle_admin_identify(kvmalloc, bdev_io);
			return;
		}
		goto invalid;
	case SPDK_BDEV_IO_TYPE_NVME_IO:
		/* Buffer-style passthru. Wrap buf in a single-element iov so
		 * the I/O handlers can use the iov interface uniformly. */
		data_len = bdev_io->u.nvme_passthru.nbytes;
		if (bdev_io->u.nvme_passthru.buf != NULL) {
			single_iov.iov_base = bdev_io->u.nvme_passthru.buf;
			single_iov.iov_len = data_len;
			iovs = &single_iov;
			iovcnt = 1;
		}
		break;
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		data_len = bdev_io->u.nvme_passthru.nbytes;
		iovs = bdev_io->u.nvme_passthru.iovs;
		iovcnt = bdev_io->u.nvme_passthru.iovcnt;
		break;
	default:
		goto invalid;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_KV_STORE:
		kvmalloc_handle_store(kvmalloc, cmd, iovs, iovcnt, data_len, bdev_io);
		return;
	case SPDK_NVME_OPC_KV_RETRIEVE:
		kvmalloc_handle_retrieve(kvmalloc, cmd, iovs, iovcnt, data_len, bdev_io);
		return;
	case SPDK_NVME_OPC_KV_DELETE:
		kvmalloc_handle_delete(kvmalloc, cmd, bdev_io);
		return;
	case SPDK_NVME_OPC_KV_EXIST:
		kvmalloc_handle_exist(kvmalloc, cmd, bdev_io);
		return;
	case SPDK_NVME_OPC_KV_LIST:
		kvmalloc_handle_list(kvmalloc, cmd, iovs, iovcnt, data_len, bdev_io);
		return;
	default:
		break;
	}

invalid:
	spdk_bdev_io_complete_nvme_status(bdev_io, 0,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_INVALID_OPCODE);
}

static bool
kvmalloc_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
kvmalloc_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_kvmalloc_disks);
}

static int
kvmalloc_destruct(void *ctx)
{
	struct kvmalloc_disk *kvmalloc = ctx;
	struct kvmalloc_kv_entry *entry, *next;

	TAILQ_REMOVE(&g_kvmalloc_disks, kvmalloc, link);

	/* Free all key-value entries */
	RB_FOREACH_SAFE(entry, kvmalloc_tree, &kvmalloc->kv_tree, next) {
		RB_REMOVE(kvmalloc_tree, &kvmalloc->kv_tree, entry);
		kvmalloc_free_entry(entry);
	}

	spdk_spin_destroy(&kvmalloc->kv_tree_lock);
	free(kvmalloc->disk.name);
	free(kvmalloc);

	return 0;
}

static void
kvmalloc_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct kvmalloc_disk *kvmalloc = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_kvmalloc_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "max_key_size", kvmalloc->max_key_size);
	spdk_json_write_named_uint32(w, "max_value_size", kvmalloc->max_value_size);
	spdk_json_write_named_uint32(w, "optimal_value_granularity", kvmalloc->optimal_value_granularity);

	spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table kvmalloc_fn_table = {
	.destruct		= kvmalloc_destruct,
	.submit_request		= kvmalloc_submit_request,
	.io_type_supported	= kvmalloc_io_type_supported,
	.get_io_channel		= kvmalloc_get_io_channel,
	.write_config_json	= kvmalloc_write_json_config,
};

static int
kvmalloc_create_channel_cb(void *io_device, void *ctx)
{
	return 0;
}

static void
kvmalloc_destroy_channel_cb(void *io_device, void *ctx)
{
}

int
create_kvmalloc_disk(struct spdk_bdev **bdev, const struct kvmalloc_bdev_opts *opts)
{
	struct kvmalloc_disk *kvmalloc;
	int rc;

	if (opts->max_key_size > 16) {
		SPDK_ERRLOG("max_key_size must not exceed 16\n");
		return -EINVAL;
	}

	if (opts->max_value_size > 0 && opts->optimal_value_granularity > opts->max_value_size) {
		SPDK_ERRLOG("optimal_value_granularity must not exceed max_value_size\n");
		return -EINVAL;
	}

	kvmalloc = calloc(1, sizeof(*kvmalloc));
	if (!kvmalloc) {
		SPDK_ERRLOG("kvmalloc calloc() failed\n");
		return -ENOMEM;
	}

	RB_INIT(&kvmalloc->kv_tree);
	spdk_spin_init(&kvmalloc->kv_tree_lock);

	if (opts->name) {
		kvmalloc->disk.name = strdup(opts->name);
	} else {
		kvmalloc->disk.name = spdk_sprintf_alloc("KVMalloc%d", g_kvmalloc_disk_count);
		g_kvmalloc_disk_count++;
	}
	if (!kvmalloc->disk.name) {
		spdk_spin_destroy(&kvmalloc->kv_tree_lock);
		free(kvmalloc);
		return -ENOMEM;
	}

	kvmalloc->disk.product_name = "KVMalloc disk";

	/*
	 * KV devices don't have a block layout, but the bdev layer requires
	 * non-zero blocklen and blockcnt. Use 1-byte blocks and report an
	 * unbounded capacity to match the KV identify-NS nsze.
	 */
	kvmalloc->disk.blocklen = 1;
	kvmalloc->disk.blockcnt = UINT64_MAX;

	kvmalloc->disk.ctxt = kvmalloc;
	kvmalloc->disk.fn_table = &kvmalloc_fn_table;
	kvmalloc->disk.module = &kvmalloc_if;

	kvmalloc->disk.csi = SPDK_NVME_CSI_KV;

	kvmalloc->max_key_size = opts->max_key_size > 0 ? opts->max_key_size :
				 KVMALLOC_DEFAULT_MAX_KEY_SIZE;
	kvmalloc->max_value_size = opts->max_value_size > 0 ? opts->max_value_size :
				   KVMALLOC_DEFAULT_MAX_VALUE_SIZE;
	kvmalloc->optimal_value_granularity = opts->optimal_value_granularity > 0 ?
					      opts->optimal_value_granularity :
					      KVMALLOC_DEFAULT_OPTIMAL_VALUE_GRANULARITY;

	if (!spdk_uuid_is_null(&opts->uuid)) {
		spdk_uuid_copy(&kvmalloc->disk.uuid, &opts->uuid);
	}

	kvmalloc->disk.numa.id_valid = 1;
	kvmalloc->disk.numa.id = opts->numa_id;

	rc = spdk_bdev_register(&kvmalloc->disk);
	if (rc) {
		free(kvmalloc->disk.name);
		spdk_spin_destroy(&kvmalloc->kv_tree_lock);
		free(kvmalloc);
		return rc;
	}

	*bdev = &(kvmalloc->disk);

	TAILQ_INSERT_TAIL(&g_kvmalloc_disks, kvmalloc, link);
	SPDK_DEBUGLOG(bdev_kvmalloc, "KV bdev %s created (max_key=%u max_value=%u opt_gran=%u)\n",
		      spdk_bdev_get_name(*bdev), kvmalloc->max_key_size,
		      kvmalloc->max_value_size, kvmalloc->optimal_value_granularity);

	return 0;
}

void
delete_kvmalloc_disk(const char *name, delete_kvmalloc_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &kvmalloc_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}

static int
bdev_kvmalloc_init(void)
{
	spdk_io_device_register(&g_kvmalloc_disks, kvmalloc_create_channel_cb,
				kvmalloc_destroy_channel_cb, sizeof(struct kvmalloc_channel),
				"bdev_kvmalloc");
	return 0;
}

static void
bdev_kvmalloc_fini(void)
{
	spdk_io_device_unregister(&g_kvmalloc_disks, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_kvmalloc)
