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

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/util.h"

#define MAX_DEVS 64

#define MAX_IOVS 128

#define DATA_PATTERN 0x5A

#define BASE_LBA_START 0x100000

struct dev {
	struct spdk_nvme_ctrlr			*ctrlr;
	char 					name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

//static int io_complete_flag = 0;

struct buffer_io_request {
	uint32_t num_blocks;
	uint32_t max_blocks_per_io;
	uint32_t block_size;
	size_t num_bytes;
	uint32_t small_ops_complete;
	int split_io_complete;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_ns *ns;
	char *read_buf;
	char *write_buf;
};

struct sgl_element {
	void *base;
	size_t offset;
	size_t len;
};

struct sgl_io_request {
	uint32_t current_iov_index;
	uint32_t current_iov_bytes_left;
	struct sgl_element iovs[MAX_IOVS];
	uint32_t nseg;
	uint32_t misalign;
};

static void
free_req(struct buffer_io_request *req)
{

	spdk_dma_free(req->write_buf);
	spdk_dma_free(req->read_buf);
	if (req->qpair) {
		spdk_nvme_ctrlr_free_io_qpair(req->qpair);
	}
	free(req);
}

/*
 * Fill buffer with usable, random data.
 *
 */
static void
fill_random(char *buf, size_t num_bytes)
{
	size_t	i;

	srand((unsigned) time(NULL));
	for (i = 0; i < num_bytes; i++) {
		buf[i] = rand() % 0x100;
	}
}

/*
 * Each small io, be that a read or write, should increment ops_complete by 1.
 * This value should be used by the main loop to determine the completion of all
 * requests.
 *
 */
static void
small_io_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct buffer_io_request *req;
	req = arg;
	req->small_ops_complete++;
}

static void
write_complete_start_small_reads(void *arg, const struct spdk_nvme_cpl *completion)
{
	uint32_t block_offset, blocks_in_io;
	size_t byte_offset;
	struct buffer_io_request *req;

	req = arg;
	block_offset = 0;
	byte_offset = 0;
	for (block_offset = 0; block_offset < req->num_blocks; block_offset += req->max_blocks_per_io) {
		byte_offset = (size_t)block_offset * (size_t)req->block_size;
		blocks_in_io = spdk_min(req->max_blocks_per_io, req->num_blocks - block_offset);
		spdk_nvme_ns_cmd_read(req->ns, req->qpair, req->read_buf + byte_offset, block_offset, blocks_in_io,
				      small_io_complete, (void *)req, 0);
	}
	req->split_io_complete = 1;
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct buffer_io_request *req;

	req = arg;
	req->split_io_complete = 1;
}

/*
 * send an nvme read or write command to the namespace that is large enough to require an asynchronous split.
 * In the case of a split write command, the single large write command is issued first,
 * then the contents of the namespace are read back to a second buffer using a series of small read requests (unsplit).
 * In the case of a read, a series of small write requests are sent from the write buffer, and then a single large read
 * is sent and the results are stored in the read buffer.
 * either way, the contents of the two buffers are compared to ensure that the asynchronous write split worked properly.
 *
 */
static int
async_split_no_sgl(struct spdk_nvme_ns *ns, int split_read)
{
	struct buffer_io_request *req;
	uint32_t ns_blk_cnt, block_offset, blocks_in_io;
	size_t byte_offset;
	int split_read_flag = split_read;

	req = calloc(sizeof(struct buffer_io_request), 1);
	if (!req) {
		printf("Cannot allocate io request\n");
		return 0;
	}

	req->ns = ns;
	req->block_size = spdk_nvme_ns_get_sector_size(ns);
	req->max_blocks_per_io = spdk_nvme_ns_get_max_sectors_per_io(ns);
	ns_blk_cnt = spdk_nvme_ns_get_num_sectors(ns);
	req->num_blocks = req->max_blocks_per_io * 10;
	req->num_bytes = (size_t)req->num_blocks * (size_t)req->block_size;
	if (req->num_blocks > ns_blk_cnt) {
		printf("namespace is not large enough to perform test.\n");
		return 1;
	}

	req->read_buf = spdk_dma_zmalloc(req->num_bytes, 0x1000, NULL);
	req->write_buf = spdk_dma_zmalloc(req->num_bytes, 0x1000, NULL);

	if (!req->read_buf || !req->write_buf) {
		free_req(req);
		printf("Unable to allocate buffers.\n");
		return 1;
	}

	req->qpair = spdk_nvme_ctrlr_alloc_io_qpair(spdk_nvme_ns_get_ctrlr(ns), NULL, 0);
	if (!req->qpair) {
		free_req(req);
		return -1;
	}

	fill_random(req->write_buf, req->num_bytes);

	if (!memcmp(req->write_buf, req->read_buf, req->num_bytes)) {
		printf("buffer not properly written.\n");
		free_req(req);
		return 1;
	}

	if (split_read) {
		for (block_offset = 0; block_offset < req->num_blocks; block_offset += req->max_blocks_per_io) {
			byte_offset = (size_t)block_offset * (size_t)req->block_size;
			blocks_in_io = spdk_min(req->max_blocks_per_io, req->num_blocks - block_offset);
			spdk_nvme_ns_cmd_write(req->ns, req->qpair, req->write_buf + byte_offset, block_offset,
					       blocks_in_io, small_io_complete, (void *)req, 0);
		}
	} else {
		spdk_nvme_ns_cmd_write(req->ns, req->qpair, req->write_buf, 0, req->num_blocks,
				       write_complete_start_small_reads, (void *)req, 0);
	}


	while (req->small_ops_complete < 10 || !req->split_io_complete) {
		if (req->small_ops_complete == 10 && split_read_flag) {
			spdk_nvme_ns_cmd_read(req->ns, req->qpair, req->read_buf, 0, req->num_blocks, read_complete,
					      (void *)req, 0);
			split_read_flag = 0;
		}
		spdk_nvme_qpair_process_completions(req->qpair, 0);
	}

	if (memcmp(req->write_buf, req->read_buf, req->num_bytes)) {
		printf("Test failed, blocks do not match.\n");
		free_req(req);
		return 1;
	}
	split_read == 1 ? printf("Read test passed, blocks match.\n") :
	printf("Write test passed, blocks match.\n");
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
	int			i;
	int status;
	struct spdk_env_opts opts;
	struct spdk_nvme_ns *ns;

	spdk_env_opts_init(&opts);
	opts.name = "nvme_sgl";
	opts.core_mask = "0x1";
	opts.shm_id = 0;
	spdk_env_init(&opts);

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "nvme_probe() failed\n");
		exit(1);
	}

	status = 0;
	foreach_dev(iter) {
		ns = spdk_nvme_ctrlr_get_ns(iter->ctrlr, 1);
		if (!ns) {
			continue;
		} else {
			printf("testing dev: %s\n", iter->name);
			status += async_split_no_sgl(ns, 0);
			status += async_split_no_sgl(ns, 1);
		}
	}

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];

		spdk_nvme_detach(dev->ctrlr);
	}
	if (status) {
		return 1;
	}
	return 0;
}
