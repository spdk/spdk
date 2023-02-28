/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/log.h"

struct spdk_deprecation {
	const char tag[32];
	const char desc[64];
	const char remove[16];
	TAILQ_ENTRY(spdk_deprecation) link;
	uint64_t hits;
	/* How often (nanoseconds) to log. */
	uint64_t interval;
	/* How many messages were not logged due to rate limiting */
	uint32_t deferred;
	/* CLOCK_MONOTONIC microseconds since g_deprecation_epoch when last warning was logged */
	uint64_t last_log;
};

static TAILQ_HEAD(, spdk_deprecation) g_deprecations = TAILQ_HEAD_INITIALIZER(g_deprecations);
struct timespec g_deprecation_epoch;

static void
__attribute__((constructor))
deprecation_init(void)
{
	clock_gettime(CLOCK_MONOTONIC, &g_deprecation_epoch);
}

static inline uint64_t
get_ns_since_epoch(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - g_deprecation_epoch.tv_sec) * SPDK_SEC_TO_NSEC +
	       now.tv_nsec - g_deprecation_epoch.tv_nsec;
}

int
spdk_log_deprecation_register(const char *tag, const char *description, const char *remove_release,
			      uint32_t rate_limit_seconds, struct spdk_deprecation **depp)
{
	struct spdk_deprecation *dep;

	assert(strnlen(tag, sizeof(dep->tag)) < sizeof(dep->tag));
	assert(strnlen(description, sizeof(dep->desc)) < sizeof(dep->desc));
	assert(strnlen(remove_release, sizeof(dep->remove)) < sizeof(dep->remove));

	dep = calloc(1, sizeof(*dep));
	if (dep == NULL) {
		return -ENOMEM;
	}

	snprintf((char *)dep->tag, sizeof(dep->tag), "%s", tag);
	snprintf((char *)dep->desc, sizeof(dep->desc), "%s", description);
	snprintf((char *)dep->remove, sizeof(dep->remove), "%s", remove_release);
	dep->interval = rate_limit_seconds * SPDK_SEC_TO_NSEC;

	TAILQ_INSERT_TAIL(&g_deprecations, dep, link);
	*depp = dep;
	return 0;
}

/*
 * There is potential for races between pthreads leading to over or under reporting of times that
 * deprecated code was hit. If this function is called in a hot path where that is likely, we care
 * more about performance than accuracy of the error counts. The important thing is that at least
 * one of the racing updates the hits counter to non-zero and the warning is logged at least once.
 */
void
spdk_log_deprecated(struct spdk_deprecation *dep, const char *file, uint32_t line, const char *func)
{
	uint64_t now = get_ns_since_epoch();

	if (dep == NULL) {
		SPDK_ERRLOG("NULL deprecation passed from %s:%u:%s\n", file, line, func);
		assert(false);
		return;
	}

	dep->hits++;

	if (dep->interval != 0) {
		if (dep->last_log != 0 && now < dep->last_log + dep->interval) {
			dep->deferred++;
			return;
		}
	}

	dep->last_log = now;

	spdk_log(SPDK_LOG_WARN, file, line, func, "%s: deprecated feature %s to be removed in %s\n",
		 dep->tag, dep->desc, dep->remove);
	if (dep->deferred != 0) {
		SPDK_WARNLOG("%s: %u messages suppressed\n", dep->tag, dep->deferred);
		dep->deferred = 0;
	}
}

int
spdk_log_for_each_deprecation(void *ctx, spdk_log_for_each_deprecation_fn fn)
{
	struct spdk_deprecation *dep;
	int rc = 0;

	TAILQ_FOREACH(dep, &g_deprecations, link) {
		rc = fn(ctx, dep);
		if (rc != 0) {
			break;
		}
	}

	return rc;
}

const char *
spdk_deprecation_get_tag(const struct spdk_deprecation *deprecation)
{
	return deprecation->tag;
}

const char *
spdk_deprecation_get_description(const struct spdk_deprecation *deprecation)
{
	return deprecation->desc;
}

const char *
spdk_deprecation_get_remove_release(const struct spdk_deprecation *deprecation)
{
	return deprecation->remove;
}

uint64_t
spdk_deprecation_get_hits(const struct spdk_deprecation *deprecation)
{
	return deprecation->hits;
}
