
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
#include "spdk/bdev.h"
#include "spdk/vhost.h"
#include "bdev/null/bdev_null.h"

#define rte_vhost_get_vhost_vring _stub_rte_vhost_get_vhost_vring
#define rte_vhost_get_vhost_ring_inflight _stub_rte_vhost_get_vhost_ring_inflight
#define rte_vhost_get_vring_base _stub_rte_vhost_get_vring_base

#include "rte_vhost.h"
#include "vhost/vhost.c"

int rte_vhost_get_vhost_vring(int vid, uint16_t vring_idx,
			      struct rte_vhost_vring *vring)
{
	return 0;
}

int rte_vhost_get_vhost_ring_inflight(int vid, uint16_t vring_idx,
				      struct rte_vhost_ring_inflight *vring)
{
	return 0;
}

int rte_vhost_get_vring_base(int vid, uint16_t queue_id,
			     uint16_t *last_avail_idx, uint16_t *last_used_idx)
{
	return 0;
}

int vhost_register_unix_socket(const char *path, const char *ctrl_name,
			       uint64_t virtio_features, uint64_t disabled_features, uint64_t protocol_features)
{
	return 0;
}

void vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession)
{
}

int
vhost_get_mem_table(int vid, struct rte_vhost_memory **mem)
{
	return 0;
}

int
vhost_driver_unregister(const char *path)
{
	return 0;
}

int
vhost_get_negotiated_features(int vid, uint64_t *negotiated_features)
{
	return 0;
}

void
vhost_session_mem_unregister(struct rte_vhost_memory *mem)
{
}

void
vhost_session_mem_register(struct rte_vhost_memory *mem)
{
}

enum fake_event_type {
	FAKE_EVENT_NEW_CONNECTION,
	FAKE_EVENT_CREATE_BLK_CONTROLLER,
	FAKE_EVENT_START_DEVICE,
	FAKE_EVENT_HOTREMOVE,
	FAKE_EVENT_REMOVE_BLK_CONTROLLER,
	FAKE_EVENT_STOP_DEVICE,
	FAKE_EVENT_DESTROY_CONNECTION,
	FAKE_EVENT_MAX,
};

struct fake_event {
	enum fake_event_type type;
	int did;
	int vid;
};

#define MAX_NUM_DEVICES 2
volatile bool g_existing_devices[MAX_NUM_DEVICES] = {0};

#define MAX_CONNECTIONS_PER_DEVICE 2
volatile bool g_existing_connections[MAX_NUM_DEVICES * MAX_CONNECTIONS_PER_DEVICE] = {0};
volatile bool g_started_connections[MAX_NUM_DEVICES * MAX_CONNECTIONS_PER_DEVICE] = {0};

#define VID_TO_DID(vid) (vid / MAX_CONNECTIONS_PER_DEVICE)

volatile unsigned int num_dpdk_events = 0;
volatile unsigned int num_dpdk_events_failed = 0;
volatile unsigned int num_init_events = 0;
volatile unsigned int num_init_events_failed = 0;

pthread_t fake_dpdk_thread;
pthread_t init_thread = 0;
struct spdk_thread *spdk_init_thread;

/* Wait on init thread to start up */
volatile bool dpdk_thread_wait = true;

#define DEFAULT_CPU_MASK "0xFFFF"

#define EVENT_CLASS_INIT_THREAD     (1 << 0)
#define EVENT_CLASS_DPDK_THREAD     (1 << 1)

__thread struct fake_event g_event;

#define RANDOM_NAME_LEN 10
static char g_random_blk_names[MAX_NUM_DEVICES][2][RANDOM_NAME_LEN];

#define random_blk_ctrl_name(vid) (g_random_blk_names[vid][0])
#define random_base_name(vid) (g_random_blk_names[vid][1])

int type_properties[FAKE_EVENT_MAX] = {
	[FAKE_EVENT_NEW_CONNECTION] =
	EVENT_CLASS_DPDK_THREAD,
	[FAKE_EVENT_CREATE_BLK_CONTROLLER] =
	EVENT_CLASS_INIT_THREAD,
	[FAKE_EVENT_START_DEVICE] =
	EVENT_CLASS_DPDK_THREAD,
	[FAKE_EVENT_HOTREMOVE] =
	EVENT_CLASS_INIT_THREAD,
	[FAKE_EVENT_REMOVE_BLK_CONTROLLER] =
	EVENT_CLASS_INIT_THREAD,
	[FAKE_EVENT_STOP_DEVICE] =
	EVENT_CLASS_DPDK_THREAD,
	[FAKE_EVENT_DESTROY_CONNECTION] =
	EVENT_CLASS_DPDK_THREAD,
};

static void
gen_random_names(void)
{
	int vid;

	for (vid = 0; vid < MAX_NUM_DEVICES; vid++) {
		snprintf(g_random_blk_names[vid][0], RANDOM_NAME_LEN, "%d%s", vid, "ctrl");
		snprintf(g_random_blk_names[vid][1], RANDOM_NAME_LEN, "%d%s", vid, "base");
	}
}

static struct fake_event *
random_event_of_type(enum fake_event_type type)
{
	struct fake_event *e = &g_event;

	e->did = rand() % MAX_NUM_DEVICES;
	e->vid = e->did * MAX_CONNECTIONS_PER_DEVICE + (rand() % MAX_CONNECTIONS_PER_DEVICE);
	e->type = type;

	return e;
}

static struct fake_event *
random_event_of_class(int mask)
{
	enum fake_event_type type;

	do {
		type = rand() % FAKE_EVENT_MAX;
	} while (!(mask & type_properties[type]));

	return random_event_of_type(type);
}

#define random_dpdk_event() random_event_of_class(EVENT_CLASS_DPDK_THREAD)
#define random_init_event() random_event_of_class(EVENT_CLASS_INIT_THREAD)

static void
null_delete_cb(void *ctx, int rc)
{
	int did = (uintptr_t)ctx;

	g_existing_devices[did] = false;
}

static void
delete_null_bdev(struct spdk_bdev *null_bdev, int did)
{
	void *ctx = (void *)(uintptr_t)did;

	bdev_null_delete(null_bdev, null_delete_cb, ctx);
}

static struct spdk_bdev *
create_null_bdev(const char *name)
{
	struct spdk_null_bdev_opts opts = {};
	struct spdk_bdev *bdev;

	opts.name = name;
	opts.uuid = NULL;
	opts.num_blocks = 100 * 1024;
	opts.block_size = 512;
	opts.md_size = 0;
	opts.md_interleave = true;
	opts.dif_type = 0;
	opts.dif_is_head_of_md = false;

	if (bdev_null_create(&bdev, &opts)) {
		return NULL;
	} else {
		return bdev;
	}
}

static void *
fake_dpdk_thread_loop(void *arg)
{
	while (dpdk_thread_wait) {}

	while (1) {
		struct fake_event *e = random_dpdk_event();

		if (g_existing_devices[e->did] == false) {
			continue;
		}

		char *ifname = random_blk_ctrl_name(e->did);
		int vid = e->vid;
		int rc = 0;

		switch (e->type) {
		case FAKE_EVENT_NEW_CONNECTION: {
			if (g_existing_connections[vid]) {
				continue;
			}

			rc = vhost_new_connection_cb(vid, ifname);

			if (rc == 0) {
				g_existing_connections[vid] = true;
			}
		}
		break;
		case FAKE_EVENT_START_DEVICE: {
			if (g_existing_connections[vid] == false || g_started_connections[vid] == true) {
				continue;
			}

			rc = vhost_start_device_cb(vid);

			if (rc == 0) {
				g_started_connections[vid] = true;
			}
		}
		break;
		case FAKE_EVENT_STOP_DEVICE: {
			if (g_started_connections[vid] == false) {
				continue;
			}

			rc = vhost_stop_device_cb(e->vid);
			g_started_connections[vid] = false;
		}
		break;
		case FAKE_EVENT_DESTROY_CONNECTION: {
			if (g_existing_connections[vid] == false) {
				continue;
			}

			rc = vhost_destroy_connection_cb(e->vid);
			g_existing_connections[vid] = false;
		}
		break;
		default:
			assert(false);
		}

		if (rc != 0) {
			num_dpdk_events_failed++;
		}

		num_dpdk_events++;
	}

	return NULL;
}

static void
start_fake_dpdk_thread(void)
{
	pthread_create(&fake_dpdk_thread, NULL, fake_dpdk_thread_loop, NULL);
}

static int
remove_controller(const char *ctrl_name)
{
	struct spdk_vhost_dev *vdev;
	int rc;

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_find(ctrl_name);
	if (vdev == NULL) {
		spdk_vhost_unlock();
		return ENODEV;
	}

	rc = spdk_vhost_dev_remove(vdev);
	spdk_vhost_unlock();

	return rc;
}

static int
send_random_init_event(void)
{
	struct fake_event *e = random_init_event();
	struct spdk_bdev *null_bdev;
	char *base_name = random_base_name(e->did);
	char *ctrl_name = random_blk_ctrl_name(e->did);
	int rc = 0;

	switch (e->type) {
	case FAKE_EVENT_CREATE_BLK_CONTROLLER: {
		if (g_existing_devices[e->did]) {
			goto skip;
		} else {
			g_existing_devices[e->did] = true;
		}

		null_bdev = create_null_bdev(base_name);
		if (null_bdev == NULL) {
			break;
		}

		rc = spdk_vhost_blk_construct(ctrl_name,
					      DEFAULT_CPU_MASK,
					      base_name,
					      false);
		if (rc) {
			delete_null_bdev(null_bdev, e->did);
		}
	}
	break;
	case FAKE_EVENT_HOTREMOVE: {
		struct spdk_bdev *null_bdev = spdk_bdev_get_by_name(base_name);
		if (null_bdev) {
			delete_null_bdev(null_bdev, e->did);
		} else {
			g_existing_devices[e->did] = false;
			goto skip;
		}
	}
	break;
	case FAKE_EVENT_REMOVE_BLK_CONTROLLER: {
		if (g_existing_devices[e->did] == false) {
			goto skip;
		}

		rc = remove_controller(ctrl_name);
		if (rc > 0) {
			g_existing_devices[e->did] = false;
			break;
		}

		null_bdev = spdk_bdev_get_by_name(base_name);
		if (null_bdev) {
			delete_null_bdev(null_bdev, e->did);
		} else {
			g_existing_devices[e->did] = false;
		}
	}
	break;
	default:
		assert(false);
	};

	return rc > 0 ? -rc : rc;
skip:
	rc = 1;
	return rc;
}

uint64_t shutdown_timestamp;

static void
init_thread_loop(void *arg)
{
	int rc;

	num_init_events++;
	while ((rc = send_random_init_event())) {
		if (rc < 0) {
			num_init_events++;
			num_init_events_failed++;
			break;
		}
	}

	if (spdk_get_ticks() > shutdown_timestamp) {
		spdk_app_stop(0);
		return;
	}

	spdk_thread_send_msg(spdk_init_thread, init_thread_loop, NULL);
}

static void
threading_logfunc(int level, const char *file, const int line,
		  const char *func, const char *format, va_list args)
{
}

static void
vhost_init_cb(int status)
{
	assert(status == 0);

	shutdown_timestamp = spdk_get_ticks() + 60 * spdk_get_ticks_hz();

	init_thread = pthread_self();
	spdk_init_thread = spdk_get_thread();
	spdk_thread_send_msg(spdk_init_thread, init_thread_loop, NULL);

	dpdk_thread_wait = false;
}

static void
app_start_cb(void *arg)
{
	spdk_vhost_init(vhost_init_cb);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {0};

	srand(0);
	gen_random_names();
	start_fake_dpdk_thread();

	spdk_app_opts_init(&opts);
	spdk_log_open(threading_logfunc);

	opts.name = "vhost-fuzz-app";
	opts.reactor_mask = DEFAULT_CPU_MASK;

	spdk_app_start(&opts, app_start_cb, NULL);

	printf("INIT thread events count = %d\n", num_init_events);
	printf("DPDK thread events count = %d\n", num_dpdk_events);
	printf("Failed INIT thread events = %d (%0.2f%%)\n", num_init_events_failed,
	       (num_init_events_failed) / ((float) num_init_events) * 100);
	printf("Failed DPDK thread events = %d (%0.2f%%)\n", num_dpdk_events_failed,
	       (num_dpdk_events_failed) / ((float) num_dpdk_events) * 100);

	spdk_app_fini();
	return 0;
}
