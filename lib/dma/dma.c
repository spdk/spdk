/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 *     * Neither the name of Nvidia Corporation nor the names of its
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

#include "spdk/dma.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/likely.h"

pthread_mutex_t g_dma_mutex = PTHREAD_MUTEX_INITIALIZER;
TAILQ_HEAD(, spdk_memory_domain) g_dma_memory_domains = TAILQ_HEAD_INITIALIZER(
			g_dma_memory_domains);

struct spdk_memory_domain {
	enum spdk_dma_device_type type;
	spdk_memory_domain_pull_data_cb pull_cb;
	spdk_memory_domain_push_data_cb push_cb;
	spdk_memory_domain_translate_memory_cb translate_cb;
	TAILQ_ENTRY(spdk_memory_domain) link;
	struct spdk_memory_domain_ctx *ctx;
	char *id;
};

int
spdk_memory_domain_create(struct spdk_memory_domain **_domain, enum spdk_dma_device_type type,
			  struct spdk_memory_domain_ctx *ctx, const char *id)
{
	struct spdk_memory_domain *domain;
	size_t ctx_size;

	if (!_domain) {
		return -EINVAL;
	}

	if (ctx && ctx->size == 0) {
		SPDK_ERRLOG("Context size can't be 0\n");
		return -EINVAL;
	}

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		SPDK_ERRLOG("Failed to allocate memory");
		return -ENOMEM;
	}

	if (id) {
		domain->id = strdup(id);
		if (!domain->id) {
			SPDK_ERRLOG("Failed to allocate memory");
			free(domain);
			return -ENOMEM;
		}
	}

	if (ctx) {
		domain->ctx = calloc(1, sizeof(*domain->ctx));
		if (!domain->ctx) {
			SPDK_ERRLOG("Failed to allocate memory");
			free(domain->id);
			free(domain);
			return -ENOMEM;
		}

		ctx_size = spdk_min(sizeof(*domain->ctx), ctx->size);
		memcpy(domain->ctx, ctx, ctx_size);
		domain->ctx->size = ctx_size;
	}

	domain->type = type;

	pthread_mutex_lock(&g_dma_mutex);
	TAILQ_INSERT_TAIL(&g_dma_memory_domains, domain, link);
	pthread_mutex_unlock(&g_dma_mutex);

	*_domain = domain;

	return 0;
}

void
spdk_memory_domain_set_translation(struct spdk_memory_domain *domain,
				   spdk_memory_domain_translate_memory_cb translate_cb)
{
	if (!domain) {
		return;
	}

	domain->translate_cb = translate_cb;
}

void
spdk_memory_domain_set_pull(struct spdk_memory_domain *domain,
			    spdk_memory_domain_pull_data_cb pull_cb)
{
	if (!domain) {
		return;
	}

	domain->pull_cb = pull_cb;
}

void
spdk_memory_domain_set_push(struct spdk_memory_domain *domain,
			    spdk_memory_domain_push_data_cb push_cb)
{
	if (!domain) {
		return;
	}

	domain->push_cb = push_cb;
}

struct spdk_memory_domain_ctx *
spdk_memory_domain_get_context(struct spdk_memory_domain *domain)
{
	assert(domain);

	return domain->ctx;
}

enum spdk_dma_device_type spdk_memory_domain_get_dma_device_type(struct spdk_memory_domain *domain)
{
	assert(domain);

	return domain->type;
}

const char *
spdk_memory_domain_get_dma_device_id(struct spdk_memory_domain *domain)
{
	assert(domain);

	return domain->id;
}

void
spdk_memory_domain_destroy(struct spdk_memory_domain *domain)
{
	if (!domain) {
		return;
	}

	pthread_mutex_lock(&g_dma_mutex);
	TAILQ_REMOVE(&g_dma_memory_domains, domain, link);
	pthread_mutex_unlock(&g_dma_mutex);

	free(domain->ctx);
	free(domain->id);
	free(domain);
}

int
spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	assert(src_domain);
	assert(src_iov);
	assert(dst_iov);

	if (spdk_unlikely(!src_domain->pull_cb)) {
		return -ENOTSUP;
	}

	return src_domain->pull_cb(src_domain, src_domain_ctx, src_iov, src_iov_cnt, dst_iov, dst_iov_cnt,
				   cpl_cb, cpl_cb_arg);
}

int
spdk_memory_domain_push_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
			     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	assert(dst_domain);
	assert(dst_iov);
	assert(src_iov);

	if (spdk_unlikely(!dst_domain->push_cb)) {
		return -ENOTSUP;
	}

	return dst_domain->push_cb(dst_domain, dst_domain_ctx, dst_iov, dst_iovcnt, src_iov, src_iovcnt,
				   cpl_cb, cpl_cb_arg);
}

int
spdk_memory_domain_translate_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				  struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
				  void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{
	assert(src_domain);
	assert(dst_domain);
	assert(result);

	if (spdk_unlikely(!src_domain->translate_cb)) {
		return -ENOTSUP;
	}

	return src_domain->translate_cb(src_domain, src_domain_ctx, dst_domain, dst_domain_ctx, addr, len,
					result);
}

struct spdk_memory_domain *
spdk_memory_domain_get_first(const char *id)
{
	struct spdk_memory_domain *domain;

	if (!id) {
		pthread_mutex_lock(&g_dma_mutex);
		domain = TAILQ_FIRST(&g_dma_memory_domains);
		pthread_mutex_unlock(&g_dma_mutex);

		return domain;
	}

	pthread_mutex_lock(&g_dma_mutex);
	TAILQ_FOREACH(domain, &g_dma_memory_domains, link) {
		if (!strcmp(domain->id, id)) {
			break;
		}
	}
	pthread_mutex_unlock(&g_dma_mutex);

	return domain;
}

struct spdk_memory_domain *
spdk_memory_domain_get_next(struct spdk_memory_domain *prev, const char *id)
{
	struct spdk_memory_domain *domain;

	if (!prev) {
		return NULL;
	}

	pthread_mutex_lock(&g_dma_mutex);
	domain = TAILQ_NEXT(prev, link);
	pthread_mutex_unlock(&g_dma_mutex);

	if (!id || !domain) {
		return domain;
	}

	pthread_mutex_lock(&g_dma_mutex);
	TAILQ_FOREACH_FROM(domain, &g_dma_memory_domains, link) {
		if (!strcmp(domain->id, id)) {
			break;
		}
	}
	pthread_mutex_unlock(&g_dma_mutex);

	return domain;
}
