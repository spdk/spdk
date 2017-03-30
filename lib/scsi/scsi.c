/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "scsi_internal.h"

#include "spdk/conf.h"

#include "spdk_internal/event.h"

#define DEFAULT_MAX_UNMAP_LBA_COUNT			4194304
#define DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT	1
#define DEFAULT_OPTIMAL_UNMAP_GRANULARITY		0
#define DEFAULT_UNMAP_GRANULARITY_ALIGNMENT		0
#define DEFAULT_UGAVALID				0
#define DEFAULT_MAX_WRITE_SAME_LENGTH			512

struct spdk_scsi_globals g_spdk_scsi;

static void
spdk_set_default_scsi_parameters(void)
{
	g_spdk_scsi.scsi_params.max_unmap_lba_count = DEFAULT_MAX_UNMAP_LBA_COUNT;
	g_spdk_scsi.scsi_params.max_unmap_block_descriptor_count =
		DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT;
	g_spdk_scsi.scsi_params.optimal_unmap_granularity =
		DEFAULT_OPTIMAL_UNMAP_GRANULARITY;
	g_spdk_scsi.scsi_params.unmap_granularity_alignment =
		DEFAULT_UNMAP_GRANULARITY_ALIGNMENT;
	g_spdk_scsi.scsi_params.ugavalid = DEFAULT_UGAVALID;
	g_spdk_scsi.scsi_params.max_write_same_length = DEFAULT_MAX_WRITE_SAME_LENGTH;
}

static int
spdk_read_config_scsi_parameters(void)
{
	struct spdk_conf_section *sp;
	const char *val;

	sp = spdk_conf_find_section(NULL, "Scsi");
	if (sp == NULL) {
		spdk_set_default_scsi_parameters();
		return 0;
	}

	val = spdk_conf_section_get_val(sp, "MaxUnmapLbaCount");
	g_spdk_scsi.scsi_params.max_unmap_lba_count = (val == NULL) ?
			DEFAULT_MAX_UNMAP_LBA_COUNT : strtoul(val, NULL, 10);

	val = spdk_conf_section_get_val(sp, "MaxUnmapBlockDescriptorCount");
	g_spdk_scsi.scsi_params.max_unmap_block_descriptor_count = (val == NULL) ?
			DEFAULT_MAX_UNMAP_BLOCK_DESCRIPTOR_COUNT : strtoul(val, NULL, 10);
	val = spdk_conf_section_get_val(sp, "OptimalUnmapGranularity");
	g_spdk_scsi.scsi_params.optimal_unmap_granularity = (val == NULL) ?
			DEFAULT_OPTIMAL_UNMAP_GRANULARITY : strtoul(val, NULL, 10);

	val = spdk_conf_section_get_val(sp, "UnmapGranularityAlignment");
	g_spdk_scsi.scsi_params.unmap_granularity_alignment = (val == NULL) ?
			DEFAULT_UNMAP_GRANULARITY_ALIGNMENT : strtoul(val, NULL, 10);

	g_spdk_scsi.scsi_params.ugavalid = spdk_conf_section_get_boolval(sp, "Ugavalid", DEFAULT_UGAVALID);

	val = spdk_conf_section_get_val(sp, "MaxWriteSameLength");
	g_spdk_scsi.scsi_params.max_write_same_length = (val == NULL) ?
			DEFAULT_MAX_WRITE_SAME_LENGTH : strtoul(val, NULL, 10);

	return 0;
}

static int
spdk_scsi_subsystem_init(void)
{
	int rc;

	rc = pthread_mutex_init(&g_spdk_scsi.mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("mutex_init() failed\n");
		return -1;
	}

	rc = spdk_read_config_scsi_parameters();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_scsi_parameters() failed\n");
		return -1;
	}

	return rc;
}

static int
spdk_scsi_subsystem_fini(void)
{
	pthread_mutex_destroy(&g_spdk_scsi.mutex);
	return 0;
}

SPDK_TRACE_REGISTER_FN(scsi_trace)
{
	spdk_trace_register_owner(OWNER_SCSI_DEV, 'd');
	spdk_trace_register_object(OBJECT_SCSI_TASK, 't');
	spdk_trace_register_description("SCSI TASK DONE", "", TRACE_SCSI_TASK_DONE,
					OWNER_SCSI_DEV, OBJECT_SCSI_TASK, 0, 0, 0, "");
	spdk_trace_register_description("SCSI TASK START", "", TRACE_SCSI_TASK_START,
					OWNER_SCSI_DEV, OBJECT_SCSI_TASK, 0, 0, 0, "");
}

SPDK_SUBSYSTEM_REGISTER(scsi, spdk_scsi_subsystem_init, spdk_scsi_subsystem_fini, NULL)
SPDK_SUBSYSTEM_DEPEND(scsi, bdev)

SPDK_LOG_REGISTER_TRACE_FLAG("scsi", SPDK_TRACE_SCSI)
