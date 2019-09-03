/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "bdev_merge.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/conf.h"
#include "spdk_internal/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"




int merge_bdev_config_add_master_bdev(struct merge_base_bdev_config *merge_cfg,
				  const char *master_bdev_name);
int merge_bdev_config_add_slave_bdev(struct merge_base_bdev_config *merge_cfg,
				 const char *slave_bdev_name);
int merge_bdev_create(struct merge_config *merge_config);
int merge_bdev_add_base_devices(struct merge_config *merge_config);

static int merge_bdev_init(void);
static void merge_bdev_exit(void);
static int merge_bdev_get_ctx_size(void);
static void merge_bdev_get_running_config(FILE *fp);

static int merge_bdev_destruct(void *ctxt);
static void merge_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool merge_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static struct spdk_io_channel *merge_bdev_get_io_channel(void *ctx);
static int merge_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
static void merge_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);



static struct spdk_bdev_module g_merge_moudle = {
	.name = "merge",
	.module_init = merge_bdev_init,
	.module_fini = merge_bdev_exit,
	.get_ctx_size = merge_bdev_get_ctx_size,
	/* .examine_config = raid_bdev_examine, */
	.config_text = merge_bdev_get_running_config,
	.async_init = false,
	.async_fini = false,
};



/* g_merge_bdev_fn_table is the function table for merge bdev */
static const struct spdk_bdev_fn_table g_merge_bdev_fn_table = {
	.destruct		= merge_bdev_destruct,
	.submit_request		= merge_bdev_submit_request,
	.io_type_supported	= merge_bdev_io_type_supported,
	.get_io_channel		= merge_bdev_get_io_channel,
	.dump_info_json		= merge_bdev_dump_info_json,
	.write_config_json	= merge_bdev_write_config_json,
};



struct merge_config *g_merge_config;



static void
merge_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* todo */
}


static int
merge_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	/* todo struct merge_bdev *mg_bdev = ctx; */
	return 0;
}


static void
merge_bdev_get_running_config(FILE *fp)
{
	/* todo */
}

static struct spdk_io_channel *
merge_bdev_get_io_channel(void *ctx)
{
	struct merge_bdev *mg_bdev = ctx;

	return spdk_get_io_channel(mg_bdev);
}


static bool
merge_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	/* struct merge_bdev *mg_bdev; */

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP: {
		/* todo check the base bdev support or not. */
		return true;
	}
	default:
		return false;
	}

	return false;
}


static void
merge_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	/* todo */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		break;

	default:
		break;
	}

}



static int
merge_bdev_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}


static void
merge_bdev_destroy_cb(void *io_device, void *ctx_buf)
{

}



static void
merge_bdev_remove_base_bdev(void *ctx)
{
	/* todo */
}


static int
merge_bdev_get_ctx_size(void)
{
	return sizeof(struct merge_bdev_io);
}


int
merge_bdev_config_add_master_bdev(struct merge_base_bdev_config *merge_cfg,
				  const char *master_bdev_name)
{
	struct merge_base_bdev_config *tmp;

	/* for rpc method , to check master node exist */
	TAILQ_FOREACH(tmp, &g_merge_config->merge_base_bdev_config_head, link) {
		if (tmp->type == MERGE_BDEV_TYPE_MASTER && tmp->merge_bdev != NULL) {
			SPDK_ERRLOG("Already contain master node : %s\n", tmp->name);
			return -EEXIST;
		}
	}

	merge_cfg->merge_bdev = NULL;
	merge_cfg->type = MERGE_BDEV_TYPE_MASTER;
	merge_cfg->name = strdup(master_bdev_name);
	if (merge_cfg->name == NULL) {
		SPDK_ERRLOG("Unable to allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}


int
merge_bdev_config_add_slave_bdev(struct merge_base_bdev_config *merge_cfg,
				 const char *slave_bdev_name)
{
	/* todo check slave number , now we only need one slave */
	merge_cfg->name = strdup(slave_bdev_name);
	merge_cfg->type = MERGE_BDEV_TYPE_SLAVE;
	if (merge_cfg->name == NULL) {
		SPDK_ERRLOG("Unable to allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}


static int
merge_bdev_destruct(void *ctxt)
{
	/* todo struct merge_bdev *merge_bdev = ctxt; */


	return 0;
}

int
merge_bdev_create(struct merge_config *merge_config)
{
	struct merge_bdev *merge_bdev;
	struct spdk_bdev *merge_bdev_gen;
	struct merge_base_bdev_config *mb_config;

	merge_bdev = calloc(1, sizeof(*merge_bdev));
	if (!merge_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for merge bdev\n");
		return -ENOMEM;
	}

	merge_bdev->config = merge_config;
	merge_bdev->state = MERGE_BDEV_STATE_CONFIGURING;
	merge_bdev_gen = &merge_bdev->bdev;

	merge_bdev_gen->name = strdup(merge_config->name);
	if (!merge_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for merg\n");
		/* todo free config and all alloced bdev */
		return -ENOMEM;
	}

	merge_bdev_gen->product_name = "Merge Volume";
	merge_bdev_gen->ctxt = merge_bdev;
	merge_bdev_gen->fn_table = &g_merge_bdev_fn_table;
	merge_bdev_gen->module = &g_merge_moudle;
	merge_bdev_gen->write_cache = 0;

	TAILQ_FOREACH(mb_config, &merge_config->merge_base_bdev_config_head, link) {
		mb_config->merge_bdev = merge_bdev;
	}


	merge_config->merge_bdev = merge_bdev;

	return 0;
}


static int
merge_bdev_add_base_device(struct merge_base_bdev_config *mb_config, struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *desc;
	int rc;

	/* register claim */
	rc = spdk_bdev_open(bdev, true, merge_bdev_remove_base_bdev, bdev, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", bdev->name);
		return rc;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_merge_moudle);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "bdev %s is claimed\n", bdev->name);

	mb_config->base_bdev_info.bdev = bdev;
	mb_config->base_bdev_info.desc = desc;

	return rc;
}

int
merge_bdev_add_base_devices(struct merge_config *merge_config)
{
	struct merge_bdev *merge_bdev;
	struct merge_base_bdev_config *mb_config;
	struct spdk_bdev	*base_bdev;
	int			rc = 0, _rc;

	uint64_t		min_blockcnt = UINT64_MAX;
	uint32_t		blocklen = 0;

	TAILQ_FOREACH(mb_config, &merge_config->merge_base_bdev_config_head, link) {
		base_bdev = spdk_bdev_get_by_name(mb_config->name);
		if (base_bdev == NULL) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "base bdev %s doesn't exist now\n", mb_config->name);
			continue;
		}

		/* config merge bdev blockcnt */
		min_blockcnt = min_blockcnt < base_bdev->blockcnt ? min_blockcnt : base_bdev->blockcnt;

		/* todo make sure blocklen */
		blocklen = base_bdev->blocklen;

		_rc = merge_bdev_add_base_device(mb_config, base_bdev);
		if (_rc != 0) {
			SPDK_ERRLOG("Failed to add base bdev %s to MERGE bdev %s: %s\n",
				    mb_config->name, merge_config->name,
				    spdk_strerror(-_rc));
			if (rc == 0) {
				rc = _rc;
			}
		}
	}

	merge_bdev = merge_config->merge_bdev;

	/* register io_device */
	if (merge_bdev->state == MERGE_BDEV_STATE_CONFIGURING) {
		merge_bdev->state = MERGE_BDEV_STATE_ONLINE;
		spdk_io_device_register(merge_bdev,
					merge_bdev_create_cb,
					merge_bdev_destroy_cb,
					sizeof(struct merge_bdev_io_channel),
					merge_bdev->config->name);
		merge_bdev->bdev.blockcnt = min_blockcnt;
		merge_bdev->bdev.blocklen = blocklen;

		rc = spdk_bdev_register(&merge_bdev->bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to register merge bdev and stay at configuring state\n");
			spdk_io_device_unregister(merge_bdev, NULL);
			merge_bdev->state = MERGE_BDEV_STATE_ERROR;
			return rc;
		}

	} else {
		/* todo clean all base bdev */
		SPDK_ERRLOG("Merge bdev state error.\n");
		return -1;
	}


	return rc;
}

/*
 * brief:
 * merge_bdev_parse_config is used to parse the merge bdev from config file based on
 * pre-defined merge bdev format in config file.
 * Format of config file:
 *   [Merge1]
 *   Name merge1
 *   MasterStripSize 4
 *   SlaveStripSize 1096
 *   Master Nvme1n1
 *   Slave Nvme2n1
 *
 *   [Merge2]
 *   Name merge2
 *   MasterStripSize 4
 *   SlaveStripSize 1096
 *   Master Nvme3n1
 *   Slave Nvme4n1
 *
 * params:
 * conf_section - pointer to config section
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
merge_bdev_parse_config(struct spdk_conf_section *conf_section)
{
	const char *merge_name;
	const char *master_name, *slave_name;
	struct merge_base_bdev_config *merge_cfg;
	int rc, val;

	merge_name = spdk_conf_section_get_val(conf_section, "Name");
	if (merge_name == NULL) {
		SPDK_ERRLOG("merge_name is null\n");
		return -EINVAL;
	}

	g_merge_config = calloc(1, sizeof(*g_merge_config));
	g_merge_config->name = "merge";
	TAILQ_INIT(&g_merge_config->merge_base_bdev_config_head);

	/* now , ali only need one slave , and i think most of situation is one */
	g_merge_config->total_merge_slave_bdev = 1;

	/* parse the strip size */
	val = spdk_conf_section_get_intval(conf_section, "MasterStripSize");
	if (val < 0) {
		SPDK_ERRLOG("MasterStripSize must bigger than 0\n");
		return -EINVAL;
	}
	g_merge_config->master_strip_size = val;

	val = spdk_conf_section_get_intval(conf_section, "SlaveStripSize");
	if (val < 0) {
		SPDK_ERRLOG("SlaveStripSize must bigger than 0\n");
		return -EINVAL;
	}
	g_merge_config->slave_strip_size = val;

	/* parse the master bdev */
	master_name = spdk_conf_section_get_val(conf_section, "Master");
	if (merge_name == NULL) {
		SPDK_ERRLOG("Master name is null\n");
		return -EINVAL;
	}

	merge_cfg = calloc(1, sizeof(*merge_cfg));
	merge_cfg->strip_size = g_merge_config->master_strip_size;
	rc = merge_bdev_config_add_master_bdev(merge_cfg, master_name);
	if (rc != 0) {
		/* todo free the config  */
		SPDK_ERRLOG("Failed to add base bdev to merge bdev config\n");
		return rc;
	}
	TAILQ_INSERT_TAIL(&g_merge_config->merge_base_bdev_config_head, merge_cfg, link);

	/* parse the slave bdev */
	slave_name = spdk_conf_section_get_val(conf_section, "Slave");
	if (slave_name == NULL) {
		SPDK_ERRLOG("Slave name is null\n");
		return -EINVAL;
	}

	merge_cfg = calloc(1, sizeof(*merge_cfg));
	merge_cfg->strip_size = g_merge_config->slave_strip_size;
	merge_bdev_config_add_slave_bdev(merge_cfg, slave_name);
	if (rc != 0) {
		/* todo free the config  */
		SPDK_ERRLOG("Failed to add base bdev to merge bdev config\n");
		return rc;
	}
	TAILQ_INSERT_TAIL(&g_merge_config->merge_base_bdev_config_head, merge_cfg, link);

	/* create bdevs */
	rc = merge_bdev_create(g_merge_config);
	if (rc != 0) {
		/* todo clean up */
		SPDK_ERRLOG("Failed to create merge bdev\n");
		return rc;
	}

	rc = merge_bdev_add_base_devices(g_merge_config);
	if (rc != 0) {
		/* todo clean up */
		SPDK_ERRLOG("Failed to add any base bdev to merge bdev\n");
	}

	return 0;
}


/* todo deal [Merge1] ... [Merge2] now , only support one section */
static int
merge_bdev_parse_config_root(void)
{
	int ret;
	struct spdk_conf_section *conf_section;
	/* multi [Merge]
	conf_section = spdk_conf_first_section(NULL);
	while (conf_section != NULL) {
		if (spdk_conf_section_match_prefix(conf_section, "Merge")) {
			ret = merge_bdev_parse_config(conf_section);
			if (ret < 0) {
				SPDK_ERRLOG("Unable to parse merge bdev section\n");
				return ret;
			}
		}
		conf_section = spdk_conf_next_section(conf_section);
	} */

	conf_section = spdk_conf_find_section(NULL, "Merge");
	if (conf_section != NULL) {
		ret = merge_bdev_parse_config(conf_section);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse merge bdev section\n");
			return ret;
		}
	}
	return 0;
}





static void
merge_bdev_exit(void)
{
	/* todo free g_merge_config */
}


static int
merge_bdev_init(void)
{
	int ret;

	ret = merge_bdev_parse_config_root();
	if (ret < 0) {
		SPDK_ERRLOG("merge bdev init failed parsing\n");
		merge_bdev_exit();
		return ret;
	}

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_MERGE, "merge_bdev_init completed successfully\n");

	return 0;

}

SPDK_BDEV_MODULE_REGISTER(merge, &g_merge_moudle)
SPDK_LOG_REGISTER_COMPONENT("bdev_merge", SPDK_LOG_BDEV_MERGE)
