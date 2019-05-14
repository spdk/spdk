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

#include "spdk/thread.h"
#include "bs_scheduler.c"


#define DEV_BUFFER_SIZE (64 * 1024 * 1024)
#define DEV_BUFFER_BLOCKLEN (4096)
#define DEV_BUFFER_BLOCKCNT (DEV_BUFFER_SIZE / DEV_BUFFER_BLOCKLEN)
uint8_t *g_dev_buffer;
uint64_t g_dev_write_bytes;
uint64_t g_dev_read_bytes;

uint64_t g_power_failure_general_counter;
uint64_t g_power_failure_read_counter;
uint64_t g_power_failure_write_counter;
uint64_t g_power_failure_unmap_counter;
uint64_t g_power_failure_write_zero_counter;
uint64_t g_power_failure_flush_counter;

uint64_t g_power_failure_general_treshold;
uint64_t g_power_failure_read_treshold;
uint64_t g_power_failure_write_treshold;
uint64_t g_power_failure_unmap_treshold;
uint64_t g_power_failure_write_zero_treshold;
uint64_t g_power_failure_flush_treshold;

uint64_t g_power_failure_rc;

void dev_reset_power_failure_event(void);
void dev_set_power_failure_event(uint64_t general_treshold,
				 uint64_t read_treshold,
				 uint64_t write_treshold,
				 uint64_t unmap_treshold,
				 uint64_t write_zero_treshold,
				 uint64_t flush_treshold);


void
dev_reset_power_failure_event(void)
{
	g_power_failure_general_counter = 0;
	g_power_failure_read_counter = 0;
	g_power_failure_write_counter = 0;
	g_power_failure_unmap_counter = 0;
	g_power_failure_write_zero_counter = 0;
	g_power_failure_flush_counter = 0;

	g_power_failure_general_treshold = 0;
	g_power_failure_read_treshold = 0;
	g_power_failure_write_treshold = 0;
	g_power_failure_unmap_treshold = 0;
	g_power_failure_write_zero_treshold = 0;
	g_power_failure_flush_treshold = 0;

	g_power_failure_rc = 0;
}

/**
 * Set power failure event. Power failure will occur after given number
 * of IO operations. It may occure after number of particular operations
 * (read, write, unmap, write zero or flush) or after given number of
 * any IO operations (general_treshold). Value 0 means that the treshold
 * is disabled. Any other value is the number of operation starting from
 * which power failure event will happen.
 */
void
dev_set_power_failure_event(uint64_t general_treshold,
			    uint64_t read_treshold,
			    uint64_t write_treshold,
			    uint64_t unmap_treshold,
			    uint64_t write_zero_treshold,
			    uint64_t flush_treshold)
{
	g_power_failure_general_treshold = general_treshold;
	g_power_failure_read_treshold = read_treshold;
	g_power_failure_write_treshold = write_treshold;
	g_power_failure_unmap_treshold = unmap_treshold;
	g_power_failure_write_zero_treshold = write_zero_treshold;
	g_power_failure_flush_treshold = flush_treshold;
}

/* Define here for UT only. */
struct spdk_io_channel g_io_channel;

static struct spdk_io_channel *
dev_create_channel(struct spdk_bs_dev *dev)
{
	return &g_io_channel;
}

static void
dev_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
}

static void
dev_destroy(struct spdk_bs_dev *dev)
{
	free(dev);
}


static void
dev_complete_cb(void *arg)
{
	struct spdk_bs_dev_cb_args *cb_args = arg;

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, g_power_failure_rc);
}

static void
dev_complete(void *arg)
{
	_bs_send_msg(dev_complete_cb, arg, NULL);
}

static void
dev_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	 uint64_t lba, uint32_t lba_count,
	 struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	if (g_power_failure_read_treshold != 0) {
		g_power_failure_read_counter++;
	}

	if (g_power_failure_general_treshold != 0) {
		g_power_failure_general_counter++;
	}

	if ((g_power_failure_read_treshold == 0 ||
	     g_power_failure_read_counter < g_power_failure_read_treshold) &&
	    (g_power_failure_general_treshold == 0 ||
	     g_power_failure_general_counter < g_power_failure_general_treshold)) {
		offset = lba * dev->blocklen;
		length = lba_count * dev->blocklen;
		SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);

		memcpy(payload, &g_dev_buffer[offset], length);
		g_dev_read_bytes += length;
	} else {
		g_power_failure_rc = -EIO;
	}

	spdk_thread_send_msg(spdk_get_thread(), dev_complete, cb_args);
}

static void
dev_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	  uint64_t lba, uint32_t lba_count,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	if (g_power_failure_write_treshold != 0) {
		g_power_failure_write_counter++;
	}

	if (g_power_failure_general_treshold != 0) {
		g_power_failure_general_counter++;
	}

	if ((g_power_failure_write_treshold == 0 ||
	     g_power_failure_write_counter < g_power_failure_write_treshold) &&
	    (g_power_failure_general_treshold == 0 ||
	     g_power_failure_general_counter < g_power_failure_general_treshold)) {
		offset = lba * dev->blocklen;
		length = lba_count * dev->blocklen;
		SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);

		memcpy(&g_dev_buffer[offset], payload, length);
		g_dev_write_bytes += length;
	} else {
		g_power_failure_rc = -EIO;
	}

	spdk_thread_send_msg(spdk_get_thread(), dev_complete, cb_args);
}

static void
__check_iov(struct iovec *iov, int iovcnt, uint64_t length)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		length -= iov[i].iov_len;
	}

	CU_ASSERT(length == 0);
}

static void
dev_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	  struct iovec *iov, int iovcnt,
	  uint64_t lba, uint32_t lba_count,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;
	int i;

	if (g_power_failure_read_treshold != 0) {
		g_power_failure_read_counter++;
	}

	if (g_power_failure_general_treshold != 0) {
		g_power_failure_general_counter++;
	}

	if ((g_power_failure_read_treshold == 0 ||
	     g_power_failure_read_counter < g_power_failure_read_treshold) &&
	    (g_power_failure_general_treshold == 0 ||
	     g_power_failure_general_counter < g_power_failure_general_treshold)) {
		offset = lba * dev->blocklen;
		length = lba_count * dev->blocklen;
		SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
		__check_iov(iov, iovcnt, length);

		for (i = 0; i < iovcnt; i++) {
			memcpy(iov[i].iov_base, &g_dev_buffer[offset], iov[i].iov_len);
			offset += iov[i].iov_len;
		}

		g_dev_read_bytes += length;
	} else {
		g_power_failure_rc = -EIO;
	}

	spdk_thread_send_msg(spdk_get_thread(), dev_complete, cb_args);
}

static void
dev_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	   struct iovec *iov, int iovcnt,
	   uint64_t lba, uint32_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;
	int i;

	if (g_power_failure_write_treshold != 0) {
		g_power_failure_write_counter++;
	}

	if (g_power_failure_general_treshold != 0) {
		g_power_failure_general_counter++;
	}

	if ((g_power_failure_write_treshold == 0 ||
	     g_power_failure_write_counter < g_power_failure_write_treshold)  &&
	    (g_power_failure_general_treshold == 0 ||
	     g_power_failure_general_counter < g_power_failure_general_treshold)) {
		offset = lba * dev->blocklen;
		length = lba_count * dev->blocklen;
		SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
		__check_iov(iov, iovcnt, length);

		for (i = 0; i < iovcnt; i++) {
			memcpy(&g_dev_buffer[offset], iov[i].iov_base, iov[i].iov_len);
			offset += iov[i].iov_len;
		}

		g_dev_write_bytes += length;
	} else {
		g_power_failure_rc = -EIO;
	}

	spdk_thread_send_msg(spdk_get_thread(), dev_complete, cb_args);
}

static void
dev_flush(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	if (g_power_failure_flush_treshold != 0) {
		g_power_failure_flush_counter++;
	}

	if (g_power_failure_general_treshold != 0) {
		g_power_failure_general_counter++;
	}

	if ((g_power_failure_flush_treshold != 0 &&
	     g_power_failure_flush_counter >= g_power_failure_flush_treshold)  ||
	    (g_power_failure_general_treshold != 0 &&
	     g_power_failure_general_counter >= g_power_failure_general_treshold)) {
		g_power_failure_rc = -EIO;
	}

	spdk_thread_send_msg(spdk_get_thread(), dev_complete, cb_args);
}

static void
dev_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	  uint64_t lba, uint32_t lba_count,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	if (g_power_failure_unmap_treshold != 0) {
		g_power_failure_unmap_counter++;
	}

	if (g_power_failure_general_treshold != 0) {
		g_power_failure_general_counter++;
	}

	if ((g_power_failure_unmap_treshold == 0 ||
	     g_power_failure_unmap_counter < g_power_failure_unmap_treshold)  &&
	    (g_power_failure_general_treshold == 0 ||
	     g_power_failure_general_counter < g_power_failure_general_treshold)) {
		offset = lba * dev->blocklen;
		length = lba_count * dev->blocklen;
		SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
		memset(&g_dev_buffer[offset], 0, length);
	} else {
		g_power_failure_rc = -EIO;
	}

	spdk_thread_send_msg(spdk_get_thread(), dev_complete, cb_args);
}

static void
dev_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		 uint64_t lba, uint32_t lba_count,
		 struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	if (g_power_failure_write_zero_treshold != 0) {
		g_power_failure_write_zero_counter++;
	}

	if (g_power_failure_general_treshold != 0) {
		g_power_failure_general_counter++;
	}

	if ((g_power_failure_write_zero_treshold == 0 ||
	     g_power_failure_write_zero_counter < g_power_failure_write_zero_treshold)  &&
	    (g_power_failure_general_treshold == 0 ||
	     g_power_failure_general_counter < g_power_failure_general_treshold)) {
		offset = lba * dev->blocklen;
		length = lba_count * dev->blocklen;
		SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
		memset(&g_dev_buffer[offset], 0, length);
		g_dev_write_bytes += length;
	} else {
		g_power_failure_rc = -EIO;
	}

	spdk_thread_send_msg(spdk_get_thread(), dev_complete, cb_args);
}

static struct spdk_bs_dev *
init_dev(void)
{
	struct spdk_bs_dev *dev = calloc(1, sizeof(*dev));

	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->create_channel = dev_create_channel;
	dev->destroy_channel = dev_destroy_channel;
	dev->destroy = dev_destroy;
	dev->read = dev_read;
	dev->write = dev_write;
	dev->readv = dev_readv;
	dev->writev = dev_writev;
	dev->flush = dev_flush;
	dev->unmap = dev_unmap;
	dev->write_zeroes = dev_write_zeroes;
	dev->blockcnt = DEV_BUFFER_BLOCKCNT;
	dev->blocklen = DEV_BUFFER_BLOCKLEN;

	return dev;
}
