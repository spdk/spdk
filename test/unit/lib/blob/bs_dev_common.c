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

#define DEV_BUFFER_SIZE (64 * 1024 * 1024)
#define DEV_BUFFER_BLOCKLEN (4096)
#define DEV_BUFFER_BLOCKCNT (DEV_BUFFER_SIZE / DEV_BUFFER_BLOCKLEN)
uint8_t *g_dev_buffer;

static struct spdk_io_channel *
dev_create_channel(struct spdk_bs_dev *dev)
{
	return NULL;
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
dev_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	 uint64_t lba, uint32_t lba_count,
	 struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	offset = lba * DEV_BUFFER_BLOCKLEN;
	length = lba_count * DEV_BUFFER_BLOCKLEN;
	SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
	memcpy(payload, &g_dev_buffer[offset], length);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
dev_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	  uint64_t lba, uint32_t lba_count,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	offset = lba * DEV_BUFFER_BLOCKLEN;
	length = lba_count * DEV_BUFFER_BLOCKLEN;
	SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
	memcpy(&g_dev_buffer[offset], payload, length);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
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

	offset = lba * DEV_BUFFER_BLOCKLEN;
	length = lba_count * DEV_BUFFER_BLOCKLEN;
	SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
	__check_iov(iov, iovcnt, length);

	for (i = 0; i < iovcnt; i++) {
		memcpy(iov[i].iov_base, &g_dev_buffer[offset], iov[i].iov_len);
		offset += iov[i].iov_len;
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
dev_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	   struct iovec *iov, int iovcnt,
	   uint64_t lba, uint32_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;
	int i;

	offset = lba * DEV_BUFFER_BLOCKLEN;
	length = lba_count * DEV_BUFFER_BLOCKLEN;
	SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
	__check_iov(iov, iovcnt, length);

	for (i = 0; i < iovcnt; i++) {
		memcpy(&g_dev_buffer[offset], iov[i].iov_base, iov[i].iov_len);
		offset += iov[i].iov_len;
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
dev_flush(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
dev_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	  uint64_t lba, uint32_t lba_count,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	offset = lba * DEV_BUFFER_BLOCKLEN;
	length = lba_count * DEV_BUFFER_BLOCKLEN;
	SPDK_CU_ASSERT_FATAL(offset + length <= DEV_BUFFER_SIZE);
	memset(&g_dev_buffer[offset], 0, length);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
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
	dev->blockcnt = DEV_BUFFER_BLOCKCNT;
	dev->blocklen = DEV_BUFFER_BLOCKLEN;

	return dev;
}
