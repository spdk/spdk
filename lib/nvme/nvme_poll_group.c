/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2021 Mellanox Technologies LTD.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "nvme_internal.h"

struct spdk_nvme_poll_group *
spdk_nvme_poll_group_create(void *ctx, struct spdk_nvme_accel_fn_table *table)
{
	struct spdk_nvme_poll_group *group;
	int rc;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	group->accel_fn_table.table_size = sizeof(struct spdk_nvme_accel_fn_table);
	if (table && table->table_size != 0) {
		group->accel_fn_table.table_size = table->table_size;
#define SET_FIELD(field) \
	if (offsetof(struct spdk_nvme_accel_fn_table, field) + sizeof(table->field) <= table->table_size) { \
		group->accel_fn_table.field = table->field; \
	} \

		SET_FIELD(append_crc32c);
		SET_FIELD(append_copy);
		SET_FIELD(finish_sequence);
		SET_FIELD(reverse_sequence);
		SET_FIELD(abort_sequence);
		/* Do not remove this statement, you should always update this statement when you adding a new field,
		 * and do not forget to add the SET_FIELD statement for your added field. */
		SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_accel_fn_table) == 56, "Incorrect size");

#undef SET_FIELD
	}

	/* Make sure either all or none of the sequence manipulation callbacks are implemented */
	if ((group->accel_fn_table.finish_sequence && group->accel_fn_table.reverse_sequence &&
	     group->accel_fn_table.abort_sequence) !=
	    (group->accel_fn_table.finish_sequence || group->accel_fn_table.reverse_sequence ||
	     group->accel_fn_table.abort_sequence)) {
		SPDK_ERRLOG("Invalid accel_fn_table configuration: either all or none of the "
			    "sequence callbacks must be provided\n");
		free(group);
		return NULL;
	}

	/* Make sure that sequence callbacks are implemented if append* callbacks are provided */
	if ((group->accel_fn_table.append_crc32c || group->accel_fn_table.append_copy) &&
	    !group->accel_fn_table.finish_sequence) {
		SPDK_ERRLOG("Invalid accel_fn_table configuration: append_crc32c and/or append_copy require sequence "
			    "callbacks to be provided\n");
		free(group);
		return NULL;
	}

	/* If interrupt is enabled, this fd_group will be used to manage events triggerd on file
	 * descriptors of all the qpairs in this poll group */
	rc = spdk_fd_group_create(&group->fgrp);
	if (rc) {
		/* Ignore this for non-Linux platforms, as fd_groups aren't supported there. */
#if defined(__linux__)
		SPDK_ERRLOG("Cannot create fd group for the nvme poll group\n");
		free(group);
		return NULL;
#endif
	}

	group->disconnect_qpair_fd = -1;
	group->ctx = ctx;
	STAILQ_INIT(&group->tgroups);

	return group;
}

int
spdk_nvme_poll_group_get_fd(struct spdk_nvme_poll_group *group)
{
	if (!group->fgrp) {
		SPDK_ERRLOG("No fd group present for the nvme poll group.\n");
		assert(false);
		return -EINVAL;
	}

	return spdk_fd_group_get_fd(group->fgrp);
}

struct spdk_fd_group *
spdk_nvme_poll_group_get_fd_group(struct spdk_nvme_poll_group *group)
{
	return group->fgrp;
}

int
spdk_nvme_poll_group_set_interrupt_callback(struct spdk_nvme_poll_group *group,
		spdk_nvme_poll_group_interrupt_cb cb_fn, void *cb_ctx)
{
	if (group->interrupt.cb_fn != NULL && cb_fn != NULL) {
		return -EEXIST;
	}

	group->interrupt.cb_fn = cb_fn;
	group->interrupt.cb_ctx = cb_ctx;

	return 0;
}

struct spdk_nvme_poll_group *
spdk_nvme_qpair_get_optimal_poll_group(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;

	tgroup = nvme_transport_qpair_get_optimal_poll_group(qpair->transport, qpair);

	if (tgroup == NULL) {
		return NULL;
	}

	return tgroup->group;
}

#ifdef __linux__
static int
nvme_poll_group_read_disconnect_qpair_fd(void *arg)
{
	struct spdk_nvme_poll_group *group = arg;

	if (group->interrupt.cb_fn != NULL) {
		group->interrupt.cb_fn(group, group->interrupt.cb_ctx);
	}

	return 0;
}

void
nvme_poll_group_write_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
	uint64_t notify = 1;
	int rc;

	if (!group->enable_interrupts) {
		return;
	}

	/* Write to the disconnect qpair fd. This will generate event on the epoll fd of poll
	 * group. We then check for disconnected qpairs either in spdk_nvme_poll_group_wait() or
	 * in transport's poll_group_process_completions() callback.
	 */
	rc = write(group->disconnect_qpair_fd, &notify, sizeof(notify));
	if (rc < 0) {
		SPDK_ERRLOG("failed to write the disconnect qpair fd: %s.\n", strerror(errno));
	}
}

static int
nvme_poll_group_add_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
	struct spdk_event_handler_opts opts = {};
	int fd;

	fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0) {
		return fd;
	}

	assert(group->disconnect_qpair_fd == -1);
	group->disconnect_qpair_fd = fd;

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
	opts.fd_type = SPDK_FD_TYPE_EVENTFD;

	return SPDK_FD_GROUP_ADD_EXT(group->fgrp, fd, nvme_poll_group_read_disconnect_qpair_fd,
				     group, &opts);
}

#else

void
nvme_poll_group_write_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
}

static int
nvme_poll_group_add_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
	return -ENOTSUP;
}

#endif

int
spdk_nvme_poll_group_add(struct spdk_nvme_poll_group *group, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	const struct spdk_nvme_transport *transport;
	int rc;

	if (nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTED) {
		return -EINVAL;
	}

	if (!group->enable_interrupts_is_valid) {
		group->enable_interrupts_is_valid = true;
		group->enable_interrupts = qpair->ctrlr->opts.enable_interrupts;
		if (group->enable_interrupts) {
			rc = nvme_poll_group_add_disconnect_qpair_fd(group);
			if (rc != 0) {
				return rc;
			}
		}
	} else if (qpair->ctrlr->opts.enable_interrupts != group->enable_interrupts) {
		NVME_QPAIR_ERRLOG(qpair, "Queue pair %s interrupts cannot be added to poll group\n",
				  qpair->ctrlr->opts.enable_interrupts ? "without" : "with");
		return -EINVAL;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			break;
		}
	}

	/* See if a new transport has been added (dlopen style) and we need to update the poll group */
	if (!tgroup) {
		transport = nvme_get_first_transport();
		while (transport != NULL) {
			if (transport == qpair->transport) {
				tgroup = nvme_transport_poll_group_create(transport);
				if (tgroup == NULL) {
					return -ENOMEM;
				}
				tgroup->group = group;
				STAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);
				break;
			}
			transport = nvme_get_next_transport(transport);
		}
	}

	return tgroup ? nvme_transport_poll_group_add(tgroup, qpair) : -ENODEV;
}

int
spdk_nvme_poll_group_remove(struct spdk_nvme_poll_group *group, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			return nvme_transport_poll_group_remove(tgroup, qpair);
		}
	}

	return -ENODEV;
}

static int
nvme_qpair_process_completion_wrapper(void *arg)
{
	struct spdk_nvme_qpair *qpair = arg;

	return spdk_nvme_qpair_process_completions(qpair, 0);
}

static int
nvme_poll_group_add_qpair_fd(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_poll_group *group;
	struct spdk_event_handler_opts opts = {
		.opts_size = SPDK_SIZEOF(&opts, fd_type),
	};
	int fd;

	group = qpair->poll_group->group;
	if (group->enable_interrupts == false) {
		return 0;
	}

	fd = spdk_nvme_qpair_get_fd(qpair, &opts);
	if (fd < 0) {
		NVME_QPAIR_ERRLOG(qpair, "Cannot get fd for the qpair: %d\n", fd);
		return -EINVAL;
	}

	return SPDK_FD_GROUP_ADD_EXT(group->fgrp, fd, nvme_qpair_process_completion_wrapper,
				     qpair, &opts);
}

static void
nvme_poll_group_remove_qpair_fd(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_poll_group *group;
	int fd;

	group = qpair->poll_group->group;
	if (group->enable_interrupts == false) {
		return;
	}

	fd = spdk_nvme_qpair_get_fd(qpair, NULL);
	if (fd < 0) {
		NVME_QPAIR_ERRLOG(qpair, "Cannot get fd for the qpair: %d\n", fd);
		assert(false);
		return;
	}

	spdk_fd_group_remove(group->fgrp, fd);
}

int
nvme_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	int rc;

	rc = nvme_transport_poll_group_connect_qpair(qpair);
	if (rc != 0) {
		return rc;
	}

	rc = nvme_poll_group_add_qpair_fd(qpair);
	if (rc != 0) {
		nvme_transport_poll_group_disconnect_qpair(qpair);
		return rc;
	}

	return 0;
}

int
nvme_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	nvme_poll_group_remove_qpair_fd(qpair);

	return nvme_transport_poll_group_disconnect_qpair(qpair);
}

int
spdk_nvme_poll_group_wait(struct spdk_nvme_poll_group *group,
			  spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	int num_events, timeout = -1;

	if (disconnected_qpair_cb == NULL) {
		return -EINVAL;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		nvme_transport_poll_group_check_disconnected_qpairs(tgroup, disconnected_qpair_cb);
	}

	num_events = spdk_fd_group_wait(group->fgrp, timeout);

	return num_events;
}

int64_t
spdk_nvme_poll_group_process_completions(struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	int64_t error_reason = 0, num_completions = 0;

	if (spdk_unlikely(disconnected_qpair_cb == NULL)) {
		return -EINVAL;
	}

	if (spdk_unlikely(group->in_process_completions)) {
		return 0;
	}
	group->in_process_completions = true;

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		int64_t local_completions;

		local_completions = nvme_transport_poll_group_process_completions(tgroup, completions_per_qpair,
				    disconnected_qpair_cb);
		if (spdk_unlikely(local_completions < 0)) {
			if (!error_reason) {
				error_reason = local_completions;
			}
		} else {
			num_completions += local_completions;
			/* Just to be safe */
			assert(num_completions >= 0);
		}
	}
	group->in_process_completions = false;

	return error_reason ? error_reason : num_completions;
}

int
spdk_nvme_poll_group_all_connected(struct spdk_nvme_poll_group *group)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	struct spdk_nvme_qpair *qpair;
	int rc = 0;

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (!STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
			/* Treat disconnected qpairs as highest priority for notification.
			 * This means we can just return immediately here.
			 */
			return -EIO;
		}
		STAILQ_FOREACH(qpair, &tgroup->connected_qpairs, poll_group_stailq) {
			if (nvme_qpair_get_state(qpair) < NVME_QPAIR_CONNECTING) {
				return -EIO;
			} else if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
				rc = -EAGAIN;
				/* Break so that we can check the remaining transport groups,
				 * in case any of them have a disconnected qpair.
				 */
				break;
			}
		}
	}

	return rc;
}

void *
spdk_nvme_poll_group_get_ctx(struct spdk_nvme_poll_group *group)
{
	return group->ctx;
}

int
spdk_nvme_poll_group_destroy(struct spdk_nvme_poll_group *group)
{
	struct spdk_nvme_transport_poll_group *tgroup, *tmp_tgroup;
	struct spdk_fd_group *fgrp = group->fgrp;

	STAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp_tgroup) {
		STAILQ_REMOVE(&group->tgroups, tgroup, spdk_nvme_transport_poll_group, link);
		if (nvme_transport_poll_group_destroy(tgroup) != 0) {
			STAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);
			return -EBUSY;
		}

	}

	if (fgrp) {
		if (group->enable_interrupts) {
			spdk_fd_group_remove(fgrp, group->disconnect_qpair_fd);
			close(group->disconnect_qpair_fd);
		}
		spdk_fd_group_destroy(fgrp);
	}

	free(group);

	return 0;
}

int
spdk_nvme_poll_group_get_stats(struct spdk_nvme_poll_group *group,
			       struct spdk_nvme_poll_group_stat **stats)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	struct spdk_nvme_poll_group_stat *result;
	uint32_t transports_count = 0;
	/* Not all transports used by this poll group may support statistics reporting */
	uint32_t reported_stats_count = 0;
	int rc;

	assert(group);
	assert(stats);

	result = calloc(1, sizeof(*result));
	if (!result) {
		SPDK_ERRLOG("Failed to allocate memory for poll group statistics\n");
		return -ENOMEM;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		transports_count++;
	}

	result->transport_stat = calloc(transports_count, sizeof(*result->transport_stat));
	if (!result->transport_stat) {
		SPDK_ERRLOG("Failed to allocate memory for poll group statistics\n");
		free(result);
		return -ENOMEM;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		rc = nvme_transport_poll_group_get_stats(tgroup, &result->transport_stat[reported_stats_count]);
		if (rc == 0) {
			reported_stats_count++;
		}
	}

	if (reported_stats_count == 0) {
		free(result->transport_stat);
		free(result);
		SPDK_DEBUGLOG(nvme, "No transport statistics available\n");
		return -ENOTSUP;
	}

	result->num_transports = reported_stats_count;
	*stats = result;

	return 0;
}

void
spdk_nvme_poll_group_free_stats(struct spdk_nvme_poll_group *group,
				struct spdk_nvme_poll_group_stat *stat)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	uint32_t i;
	uint32_t freed_stats __attribute__((unused)) = 0;

	assert(group);
	assert(stat);

	for (i = 0; i < stat->num_transports; i++) {
		STAILQ_FOREACH(tgroup, &group->tgroups, link) {
			if (nvme_transport_get_trtype(tgroup->transport) == stat->transport_stat[i]->trtype) {
				nvme_transport_poll_group_free_stats(tgroup, stat->transport_stat[i]);
				freed_stats++;
				break;
			}
		}
	}

	assert(freed_stats == stat->num_transports);

	free(stat->transport_stat);
	free(stat);
}
