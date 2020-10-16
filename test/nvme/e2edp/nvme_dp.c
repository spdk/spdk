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

/*
 * NVMe end-to-end data protection test
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/crc16.h"
#include "spdk/endian.h"
#include "spdk/memory.h"

#define MAX_DEVS 64

#define DATA_PATTERN 0x5A

struct dev {
	struct spdk_nvme_ctrlr			*ctrlr;
	char					name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int io_complete_flag = 0;

struct io_request {
	void *contig;
	void *metadata;
	bool use_extended_lba;
	bool use_sgl;
	uint32_t sgl_offset;
	uint32_t buf_size;
	uint64_t lba;
	uint32_t lba_count;
	uint16_t apptag_mask;
	uint16_t apptag;
};

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		io_complete_flag = 2;
	} else {
		io_complete_flag = 1;
	}
}

static void
ns_data_buffer_reset(struct spdk_nvme_ns *ns, struct io_request *req, uint8_t data_pattern)
{
	uint32_t md_size, sector_size;
	uint32_t i, offset = 0;
	uint8_t *buf;

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);

	for (i = 0; i < req->lba_count; i++) {
		if (req->use_extended_lba) {
			offset = (sector_size + md_size) * i;
		} else {
			offset = sector_size * i;
		}

		buf = (uint8_t *)req->contig + offset;
		memset(buf, data_pattern, sector_size);
	}
}

static void nvme_req_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	struct io_request *req = (struct io_request *)cb_arg;

	req->sgl_offset = sgl_offset;
	return;
}

static int nvme_req_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;
	void *payload;

	payload = req->contig + req->sgl_offset;
	*address = payload;

	*length = req->buf_size - req->sgl_offset;

	return 0;
}

/* CRC-16 Guard checked for extended lba format */
static uint32_t dp_guard_check_extended_lba_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	struct spdk_nvme_protection_info *pi;
	uint32_t md_size, sector_size;

	req->lba_count = 2;

	/* extended LBA only for the test case */
	if (!(spdk_nvme_ns_supports_extended_lba(ns))) {
		return 0;
	}

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = spdk_zmalloc((sector_size + md_size) * req->lba_count, 0x1000, NULL,
				   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	assert(req->contig);

	req->lba = 0;
	req->use_extended_lba = true;
	req->use_sgl = true;
	req->buf_size = (sector_size + md_size) * req->lba_count;
	req->metadata = NULL;
	ns_data_buffer_reset(ns, req, DATA_PATTERN);
	pi = (struct spdk_nvme_protection_info *)(req->contig + sector_size + md_size - 8);
	/* big-endian for guard */
	to_be16(&pi->guard, spdk_crc16_t10dif(0, req->contig, sector_size));

	pi = (struct spdk_nvme_protection_info *)(req->contig + (sector_size + md_size) * 2 - 8);
	to_be16(&pi->guard, spdk_crc16_t10dif(0, req->contig + sector_size + md_size, sector_size));

	*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD;

	return req->lba_count;
}

/*
 * No protection information with PRACT setting to 1,
 *  both extended LBA format and separate metadata can
 *  run the test case.
 */
static uint32_t dp_with_pract_test(struct spdk_nvme_ns *ns, struct io_request *req,
				   uint32_t *io_flags)
{
	uint32_t md_size, sector_size, data_len;

	req->lba_count = 8;

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	if (md_size == 8) {
		/* No additional metadata buffer provided */
		data_len = sector_size * req->lba_count;
	} else {
		data_len = (sector_size + md_size) * req->lba_count;
	}
	req->contig = spdk_zmalloc(data_len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				   SPDK_MALLOC_DMA);
	assert(req->contig);

	req->metadata = spdk_zmalloc(md_size * req->lba_count, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				     SPDK_MALLOC_DMA);
	assert(req->metadata);

	switch (spdk_nvme_ns_get_pi_type(ns)) {
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE3:
		*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD | SPDK_NVME_IO_FLAGS_PRACT;
		break;
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE1:
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE2:
		*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD | SPDK_NVME_IO_FLAGS_PRCHK_REFTAG |
			    SPDK_NVME_IO_FLAGS_PRACT;
		break;
	default:
		*io_flags = 0;
		break;
	}

	req->lba = 0;
	req->use_extended_lba = false;

	return req->lba_count;
}

/* Block Reference Tag checked for TYPE1 and TYPE2 with PRACT setting to 0 */
static uint32_t dp_without_pract_extended_lba_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	struct spdk_nvme_protection_info *pi;
	uint32_t md_size, sector_size;

	req->lba_count = 2;

	switch (spdk_nvme_ns_get_pi_type(ns)) {
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE3:
		return 0;
	default:
		break;
	}

	/* extended LBA only for the test case */
	if (!(spdk_nvme_ns_supports_extended_lba(ns))) {
		return 0;
	}

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = spdk_zmalloc((sector_size + md_size) * req->lba_count, 0x1000, NULL,
				   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	assert(req->contig);

	req->lba = 0;
	req->use_extended_lba = true;
	req->metadata = NULL;
	pi = (struct spdk_nvme_protection_info *)(req->contig + sector_size + md_size - 8);
	/* big-endian for reference tag */
	to_be32(&pi->ref_tag, (uint32_t)req->lba);

	pi = (struct spdk_nvme_protection_info *)(req->contig + (sector_size + md_size) * 2 - 8);
	/* is incremented for each subsequent logical block */
	to_be32(&pi->ref_tag, (uint32_t)(req->lba + 1));

	*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;

	return req->lba_count;
}

/* LBA + Metadata without data protection bits setting */
static uint32_t dp_without_flags_extended_lba_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	uint32_t md_size, sector_size;

	req->lba_count = 16;

	/* extended LBA only for the test case */
	if (!(spdk_nvme_ns_supports_extended_lba(ns))) {
		return 0;
	}

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = spdk_zmalloc((sector_size + md_size) * req->lba_count, 0x1000, NULL,
				   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	assert(req->contig);

	req->lba = 0;
	req->use_extended_lba = true;
	req->metadata = NULL;
	*io_flags = 0;

	return req->lba_count;
}

/* Block Reference Tag checked for TYPE1 and TYPE2 with PRACT setting to 0 */
static uint32_t dp_without_pract_separate_meta_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	struct spdk_nvme_protection_info *pi;
	uint32_t md_size, sector_size;

	req->lba_count = 2;

	switch (spdk_nvme_ns_get_pi_type(ns)) {
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE3:
		return 0;
	default:
		break;
	}

	/* separate metadata payload for the test case */
	if (spdk_nvme_ns_supports_extended_lba(ns)) {
		return 0;
	}

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = spdk_zmalloc(sector_size * req->lba_count, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				   SPDK_MALLOC_DMA);
	assert(req->contig);

	req->metadata = spdk_zmalloc(md_size * req->lba_count, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				     SPDK_MALLOC_DMA);
	assert(req->metadata);

	req->lba = 0;
	req->use_extended_lba = false;

	/* last 8 bytes if the metadata size bigger than 8 */
	pi = (struct spdk_nvme_protection_info *)(req->metadata + md_size - 8);
	/* big-endian for reference tag */
	to_be32(&pi->ref_tag, (uint32_t)req->lba);

	pi = (struct spdk_nvme_protection_info *)(req->metadata + md_size * 2 - 8);
	/* is incremented for each subsequent logical block */
	to_be32(&pi->ref_tag, (uint32_t)(req->lba + 1));

	*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;

	return req->lba_count;
}

/* Application Tag checked with PRACT setting to 0 */
static uint32_t dp_without_pract_separate_meta_apptag_test(struct spdk_nvme_ns *ns,
		struct io_request *req,
		uint32_t *io_flags)
{
	struct spdk_nvme_protection_info *pi;
	uint32_t md_size, sector_size;

	req->lba_count = 1;

	/* separate metadata payload for the test case */
	if (spdk_nvme_ns_supports_extended_lba(ns)) {
		return 0;
	}

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = spdk_zmalloc(sector_size * req->lba_count, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				   SPDK_MALLOC_DMA);
	assert(req->contig);

	req->metadata = spdk_zmalloc(md_size * req->lba_count, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				     SPDK_MALLOC_DMA);
	assert(req->metadata);

	req->lba = 0;
	req->use_extended_lba = false;
	req->apptag_mask = 0xFFFF;
	req->apptag = req->lba_count;

	/* last 8 bytes if the metadata size bigger than 8 */
	pi = (struct spdk_nvme_protection_info *)(req->metadata + md_size - 8);
	to_be16(&pi->app_tag, req->lba_count);

	*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_APPTAG;

	return req->lba_count;
}

/*
 * LBA + Metadata without data protection bits setting,
 *  separate metadata payload for the test case.
 */
static uint32_t dp_without_flags_separate_meta_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	uint32_t md_size, sector_size;

	req->lba_count = 16;

	/* separate metadata payload for the test case */
	if (spdk_nvme_ns_supports_extended_lba(ns)) {
		return 0;
	}

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = spdk_zmalloc(sector_size * req->lba_count, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				   SPDK_MALLOC_DMA);
	assert(req->contig);

	req->metadata = spdk_zmalloc(md_size * req->lba_count, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
				     SPDK_MALLOC_DMA);
	assert(req->metadata);

	req->lba = 0;
	req->use_extended_lba = false;
	*io_flags = 0;

	return req->lba_count;
}

typedef uint32_t (*nvme_build_io_req_fn_t)(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *lba_count);

static void
free_req(struct io_request *req)
{
	if (req == NULL) {
		return;
	}

	if (req->contig) {
		spdk_free(req->contig);
	}

	if (req->metadata) {
		spdk_free(req->metadata);
	}

	spdk_free(req);
}

static int
ns_data_buffer_compare(struct spdk_nvme_ns *ns, struct io_request *req, uint8_t data_pattern)
{
	uint32_t md_size, sector_size;
	uint32_t i, j, offset = 0;
	uint8_t *buf;

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);

	for (i = 0; i < req->lba_count; i++) {
		if (req->use_extended_lba) {
			offset = (sector_size + md_size) * i;
		} else {
			offset = sector_size * i;
		}

		buf = (uint8_t *)req->contig + offset;
		for (j = 0; j < sector_size; j++) {
			if (buf[j] != data_pattern) {
				return -1;
			}
		}
	}

	return 0;
}

static int
write_read_e2e_dp_tests(struct dev *dev, nvme_build_io_req_fn_t build_io_fn, const char *test_name)
{
	int rc = 0;
	uint32_t lba_count;
	uint32_t io_flags = 0;

	struct io_request *req;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	const struct spdk_nvme_ns_data *nsdata;

	ns = spdk_nvme_ctrlr_get_ns(dev->ctrlr, 1);
	if (!ns) {
		printf("Null namespace\n");
		return 0;
	}

	if (!(spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED)) {
		return 0;
	}

	nsdata = spdk_nvme_ns_get_data(ns);
	if (!nsdata || !spdk_nvme_ns_get_sector_size(ns)) {
		fprintf(stderr, "Empty nsdata or wrong sector size\n");
		return -EINVAL;
	}

	req = spdk_zmalloc(sizeof(*req), 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	assert(req);

	/* IO parameters setting */
	lba_count = build_io_fn(ns, req, &io_flags);
	if (!lba_count) {
		printf("%s: %s bypass the test case\n", dev->name, test_name);
		free_req(req);
		return 0;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(dev->ctrlr, NULL, 0);
	if (!qpair) {
		free_req(req);
		return -1;
	}

	ns_data_buffer_reset(ns, req, DATA_PATTERN);
	if (req->use_extended_lba && req->use_sgl) {
		rc = spdk_nvme_ns_cmd_writev(ns, qpair, req->lba, lba_count, io_complete, req, io_flags,
					     nvme_req_reset_sgl, nvme_req_next_sge);
	} else if (req->use_extended_lba) {
		rc = spdk_nvme_ns_cmd_write(ns, qpair, req->contig, req->lba, lba_count,
					    io_complete, req, io_flags);
	} else {
		rc = spdk_nvme_ns_cmd_write_with_md(ns, qpair, req->contig, req->metadata, req->lba, lba_count,
						    io_complete, req, io_flags, req->apptag_mask, req->apptag);
	}

	if (rc != 0) {
		fprintf(stderr, "%s: %s write submit failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	io_complete_flag = 0;

	while (!io_complete_flag) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s write exec failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	/* reset completion flag */
	io_complete_flag = 0;

	ns_data_buffer_reset(ns, req, 0);
	if (req->use_extended_lba && req->use_sgl) {
		rc = spdk_nvme_ns_cmd_readv(ns, qpair, req->lba, lba_count, io_complete, req, io_flags,
					    nvme_req_reset_sgl, nvme_req_next_sge);

	} else if (req->use_extended_lba) {
		rc = spdk_nvme_ns_cmd_read(ns, qpair, req->contig, req->lba, lba_count,
					   io_complete, req, io_flags);
	} else {
		rc = spdk_nvme_ns_cmd_read_with_md(ns, qpair, req->contig, req->metadata, req->lba, lba_count,
						   io_complete, req, io_flags, req->apptag_mask, req->apptag);
	}

	if (rc != 0) {
		fprintf(stderr, "%s: %s read failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	while (!io_complete_flag) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s read failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	rc = ns_data_buffer_compare(ns, req, DATA_PATTERN);
	if (rc < 0) {
		fprintf(stderr, "%s: %s write/read success, but memcmp Failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	printf("%s: %s test passed\n", dev->name, test_name);
	spdk_nvme_ctrlr_free_io_qpair(qpair);
	free_req(req);
	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];

	dev->ctrlr = ctrlr;

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);
}

int main(int argc, char **argv)
{
	struct dev		*iter;
	int			rc;
	struct spdk_env_opts	opts;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_env_opts_init(&opts);
	opts.name = "nvme_dp";
	opts.core_mask = "0x1";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("NVMe Write/Read with End-to-End data protection test\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "nvme_probe() failed\n");
		exit(1);
	}

	rc = 0;
	foreach_dev(iter) {
#define TEST(x) write_read_e2e_dp_tests(iter, x, #x)
		if (TEST(dp_with_pract_test)
		    || TEST(dp_guard_check_extended_lba_test)
		    || TEST(dp_without_pract_extended_lba_test)
		    || TEST(dp_without_flags_extended_lba_test)
		    || TEST(dp_without_pract_separate_meta_test)
		    || TEST(dp_without_pract_separate_meta_apptag_test)
		    || TEST(dp_without_flags_separate_meta_test)) {
#undef TEST
			rc = 1;
			printf("%s: failed End-to-End data protection tests\n", iter->name);
		}
	}

	printf("Cleaning up...\n");

	foreach_dev(iter) {
		spdk_nvme_detach_async(iter->ctrlr, &detach_ctx);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}

	return rc;
}
