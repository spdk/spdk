/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk/stdinc.h"
#include "bdev/raid/bdev_raid.h"

struct spdk_bdev_desc {
	struct spdk_bdev *bdev;
};

typedef enum spdk_dif_type spdk_dif_type_t;

spdk_dif_type_t
spdk_bdev_get_dif_type(const struct spdk_bdev *bdev)
{
	if (bdev->md_len != 0) {
		return bdev->dif_type;
	} else {
		return SPDK_DIF_DISABLE;
	}
}

enum raid_params_md_type {
	RAID_PARAMS_MD_NONE,
	RAID_PARAMS_MD_SEPARATE,
	RAID_PARAMS_MD_INTERLEAVED,
};

struct raid_params {
	uint8_t num_base_bdevs;
	uint64_t base_bdev_blockcnt;
	uint32_t base_bdev_blocklen;
	uint32_t strip_size;
	enum raid_params_md_type md_type;
};

int raid_test_params_alloc(size_t count);
void raid_test_params_free(void);
void raid_test_params_add(struct raid_params *params);
struct raid_bdev *raid_test_create_raid_bdev(struct raid_params *params,
		struct raid_bdev_module *module);
void raid_test_delete_raid_bdev(struct raid_bdev *raid_bdev);
struct raid_bdev_io_channel *raid_test_create_io_channel(struct raid_bdev *raid_bdev);
void raid_test_destroy_io_channel(struct raid_bdev_io_channel *raid_ch);
void raid_test_bdev_io_init(struct raid_bdev_io *raid_io, struct raid_bdev *raid_bdev,
			    struct raid_bdev_io_channel *raid_ch,
			    enum spdk_bdev_io_type type, uint64_t offset_blocks,
			    uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf);

/* needs to be implemented in module unit test files */
void raid_test_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status);

struct raid_params *g_params;
size_t g_params_count;
size_t g_params_size;

#define ARRAY_FOR_EACH(a, e) \
	for (e = a; e < a + SPDK_COUNTOF(a); e++)

#define RAID_PARAMS_FOR_EACH(p) \
	for (p = g_params; p < g_params + g_params_count; p++)

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->bdev;
}

int
raid_test_params_alloc(size_t count)
{
	assert(g_params == NULL);

	g_params_size = count;
	g_params_count = 0;
	g_params = calloc(count, sizeof(*g_params));

	return g_params ? 0 : -ENOMEM;
}

void
raid_test_params_free(void)
{
	g_params_count = 0;
	g_params_size = 0;
	free(g_params);
}

void
raid_test_params_add(struct raid_params *params)
{
	assert(g_params_count < g_params_size);

	memcpy(g_params + g_params_count, params, sizeof(*params));
	g_params_count++;
}

struct raid_bdev *
raid_test_create_raid_bdev(struct raid_params *params, struct raid_bdev_module *module)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	SPDK_CU_ASSERT_FATAL(spdk_u32_is_pow2(params->base_bdev_blocklen));

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	SPDK_CU_ASSERT_FATAL(raid_bdev != NULL);

	raid_bdev->module = module;
	raid_bdev->level = module->level;
	raid_bdev->num_base_bdevs = params->num_base_bdevs;

	switch (raid_bdev->module->base_bdevs_constraint.type) {
	case CONSTRAINT_MAX_BASE_BDEVS_REMOVED:
		raid_bdev->min_base_bdevs_operational = raid_bdev->num_base_bdevs -
							raid_bdev->module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL:
		raid_bdev->min_base_bdevs_operational = raid_bdev->module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_UNSET:
		raid_bdev->min_base_bdevs_operational = raid_bdev->num_base_bdevs;
		break;
	default:
		CU_FAIL_FATAL("unsupported raid constraint type");
	};

	raid_bdev->bdev.blocklen = params->base_bdev_blocklen;
	raid_bdev->bdev.md_len = (params->md_type == RAID_PARAMS_MD_NONE ? 0 : 16);
	raid_bdev->bdev.md_interleave = (params->md_type == RAID_PARAMS_MD_INTERLEAVED);
	if (raid_bdev->bdev.md_interleave) {
		raid_bdev->bdev.blocklen += raid_bdev->bdev.md_len;
	}

	raid_bdev->strip_size = params->strip_size;
	raid_bdev->strip_size_kb = params->strip_size * params->base_bdev_blocklen / 1024;
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);

	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	SPDK_CU_ASSERT_FATAL(raid_bdev->base_bdev_info != NULL);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		struct spdk_bdev *bdev;
		struct spdk_bdev_desc *desc;

		bdev = calloc(1, sizeof(*bdev));
		SPDK_CU_ASSERT_FATAL(bdev != NULL);
		bdev->ctxt = base_info;
		bdev->blockcnt = params->base_bdev_blockcnt;
		bdev->blocklen = raid_bdev->bdev.blocklen;
		bdev->md_len = raid_bdev->bdev.md_len;
		bdev->md_interleave = raid_bdev->bdev.md_interleave;

		desc = calloc(1, sizeof(*desc));
		SPDK_CU_ASSERT_FATAL(desc != NULL);
		desc->bdev = bdev;

		base_info->raid_bdev = raid_bdev;
		base_info->desc = desc;
		base_info->data_offset = 0;
		base_info->data_size = bdev->blockcnt;
	}

	return raid_bdev;
}

void
raid_test_delete_raid_bdev(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		free(base_info->desc->bdev);
		free(base_info->desc);
	}
	free(raid_bdev->base_bdev_info);
	free(raid_bdev);
}

struct raid_bdev_io_channel {
	struct spdk_io_channel **_base_channels;
	struct spdk_io_channel *_module_channel;
};

struct spdk_io_channel *
raid_bdev_channel_get_base_channel(struct raid_bdev_io_channel *raid_ch, uint8_t idx)
{
	return raid_ch->_base_channels[idx];
}

void *
raid_bdev_channel_get_module_ctx(struct raid_bdev_io_channel *raid_ch)
{
	return spdk_io_channel_get_ctx(raid_ch->_module_channel);
}

struct raid_bdev_io_channel *
raid_test_create_io_channel(struct raid_bdev *raid_bdev)
{
	struct raid_bdev_io_channel *raid_ch;
	uint8_t i;

	raid_ch = calloc(1, sizeof(*raid_ch));
	SPDK_CU_ASSERT_FATAL(raid_ch != NULL);

	raid_ch->_base_channels = calloc(raid_bdev->num_base_bdevs, sizeof(struct spdk_io_channel *));
	SPDK_CU_ASSERT_FATAL(raid_ch->_base_channels != NULL);

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		raid_ch->_base_channels[i] = (void *)1;
	}

	if (raid_bdev->module->get_io_channel) {
		raid_ch->_module_channel = raid_bdev->module->get_io_channel(raid_bdev);
		SPDK_CU_ASSERT_FATAL(raid_ch->_module_channel != NULL);
	}

	return raid_ch;
}

void
raid_test_destroy_io_channel(struct raid_bdev_io_channel *raid_ch)
{
	free(raid_ch->_base_channels);

	if (raid_ch->_module_channel) {
		spdk_put_io_channel(raid_ch->_module_channel);
		poll_threads();
	}

	free(raid_ch);
}

void
raid_test_bdev_io_init(struct raid_bdev_io *raid_io, struct raid_bdev *raid_bdev,
		       struct raid_bdev_io_channel *raid_ch,
		       enum spdk_bdev_io_type type, uint64_t offset_blocks,
		       uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf)
{
	memset(raid_io, 0, sizeof(*raid_io));

	raid_io->raid_bdev = raid_bdev;
	raid_io->raid_ch = raid_ch;

	raid_io->type = type;
	raid_io->offset_blocks = offset_blocks;
	raid_io->num_blocks = num_blocks;
	raid_io->iovs = iovs;
	raid_io->iovcnt = iovcnt;
	raid_io->md_buf = md_buf;

	raid_bdev_io_set_default_status(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	if (raid_io->completion_cb != NULL) {
		raid_io->completion_cb(raid_io, status);
	} else {
		raid_test_bdev_io_complete(raid_io, status);
	}
}

bool
raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
			   enum spdk_bdev_io_status status)
{
	SPDK_CU_ASSERT_FATAL(raid_io->base_bdev_io_remaining >= completed);
	raid_io->base_bdev_io_remaining -= completed;

	if (status != raid_io->base_bdev_io_status_default) {
		raid_io->base_bdev_io_status = status;
	}

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

struct raid_base_bdev_info *
raid_bdev_channel_get_base_info(struct raid_bdev_io_channel *raid_ch, struct spdk_bdev *base_bdev)
{
	return base_bdev->ctxt;
}
