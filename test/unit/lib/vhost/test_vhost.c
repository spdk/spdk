/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include "CUnit/Basic.h"
#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "spdk/io_channel.h"

struct spdk_conf_section {
	struct spdk_conf_section *next;
	char *name;
	int num;
	struct spdk_conf_item *item;
};

DEFINE_STUB(spdk_ring_enqueue, size_t, (struct spdk_ring *ring, void **objs, size_t count), 0);
DEFINE_STUB(spdk_ring_dequeue, size_t, (struct spdk_ring *ring, void **objs, size_t count), 0);
DEFINE_STUB(spdk_vhost_vq_get_desc, int, (struct spdk_vhost_dev *vdev,
		struct spdk_vhost_virtqueue *vq, uint16_t req_idx, struct vring_desc **desc,
		struct vring_desc **desc_table, uint32_t *desc_table_size), 0);
DEFINE_STUB(spdk_vhost_vring_desc_is_wr, bool, (struct vring_desc *cur_desc), false);
DEFINE_STUB(spdk_vhost_vring_desc_to_iov, int, (struct spdk_vhost_dev *vdev, struct iovec *iov,
		uint16_t *iov_index, const struct vring_desc *desc), 0);
DEFINE_STUB_V(spdk_vhost_vq_used_ring_enqueue, (struct spdk_vhost_dev *vdev,
		struct spdk_vhost_virtqueue *vq, uint16_t id, uint32_t len));
DEFINE_STUB(spdk_vhost_vring_desc_get_next, int, (struct vring_desc **desc,
		struct vring_desc *desc_table, uint32_t desc_table_size), 0);
DEFINE_STUB(spdk_vhost_vq_avail_ring_get, uint16_t, (struct spdk_vhost_virtqueue *vq,
		uint16_t *reqs, uint16_t reqs_len), 0);
DEFINE_STUB(spdk_vhost_vq_used_signal, int, (struct spdk_vhost_dev *vdev,
		struct spdk_vhost_virtqueue *virtqueue), 0);
DEFINE_STUB_V(spdk_vhost_dev_used_signal, (struct spdk_vhost_dev *vdev));
DEFINE_STUB_V(spdk_vhost_dev_mem_register, (struct spdk_vhost_dev *vdev));
DEFINE_STUB_P(spdk_vhost_dev_find, struct spdk_vhost_dev, (const char *ctrlr_name), {0});
DEFINE_STUB_V(spdk_ring_free, (struct spdk_ring *ring));
DEFINE_STUB_P(spdk_conf_first_section, struct spdk_conf_section, (struct spdk_conf *cp), {0});
DEFINE_STUB(spdk_conf_section_match_prefix, bool, (const struct spdk_conf_section *sp,
		const char *name_prefix), false);
DEFINE_STUB_P(spdk_conf_next_section, struct spdk_conf_section, (struct spdk_conf_section *sp), {0});
DEFINE_STUB_P(spdk_conf_section_get_name, const char, (const struct spdk_conf_section *sp), {0});
DEFINE_STUB(spdk_conf_section_get_boolval, bool, (struct spdk_conf_section *sp, const char *key,
		bool default_val), false);
DEFINE_STUB_P(spdk_conf_section_get_nmval, char, (struct spdk_conf_section *sp, const char *key,
		int idx1, int idx2), {0});
DEFINE_STUB_V(spdk_vhost_dev_mem_unregister, (struct spdk_vhost_dev *vdev));
DEFINE_STUB(spdk_vhost_event_send, int, (struct spdk_vhost_dev *vdev, spdk_vhost_event_fn cb_fn,
		void *arg, unsigned timeout_sec, const char *errmsg), 0);
DEFINE_STUB(spdk_env_get_socket_id, uint32_t, (uint32_t core), 0);
DEFINE_STUB_V(spdk_vhost_dev_backend_event_done, (void *event_ctx, int response));
DEFINE_STUB_V(spdk_vhost_lock, (void));
DEFINE_STUB_V(spdk_vhost_unlock, (void));
DEFINE_STUB(spdk_env_get_current_core, uint32_t, (void), 0);
DEFINE_STUB_V(spdk_vhost_call_external_event, (const char *ctrlr_name, spdk_vhost_event_fn fn,
		void *arg));
DEFINE_STUB(spdk_vhost_vring_desc_has_next, bool, (struct vring_desc *cur_desc), false);
DEFINE_STUB_VP(spdk_vhost_gpa_to_vva, (struct spdk_vhost_dev *vdev, uint64_t addr), {0});
DEFINE_STUB(spdk_scsi_dev_get_id, int, (const struct spdk_scsi_dev *dev), {0});
DEFINE_STUB(spdk_json_write_null, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_bool, int, (struct spdk_json_write_ctx *w, bool val), 0);
DEFINE_STUB(spdk_json_write_name, int, (struct spdk_json_write_ctx *w, const char *name), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_uint32, int, (struct spdk_json_write_ctx *w, uint32_t val), 0);
DEFINE_STUB(spdk_json_write_int32, int, (struct spdk_json_write_ctx *w, int32_t val), 0);
DEFINE_STUB(spdk_json_write_string, int, (struct spdk_json_write_ctx *w, const char *val), 0);
DEFINE_STUB(spdk_json_write_array_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_end, int, (struct spdk_json_write_ctx *w), 0);

/* This sets spdk_vhost_dev_unregister to either to fail or success */
DEFINE_STUB(spdk_vhost_dev_unregister_fail, bool, (void), false);
/* This sets spdk_vhost_dev_register to either to fail or success */
DEFINE_STUB(spdk_vhost_dev_register_fail, bool, (void), false);

static struct spdk_vhost_dev *g_spdk_vhost_device;
int
spdk_vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			const struct spdk_vhost_dev_backend *backend)
{
	if (spdk_vhost_dev_register_fail()) {
		return -1;
	}

	vdev->backend = backend;
	g_spdk_vhost_device = vdev;
	vdev->registered = true;
	return 0;
}

int
spdk_vhost_dev_unregister(struct spdk_vhost_dev *vdev)
{
	if (spdk_vhost_dev_unregister_fail()) {
		return -1;
	}

	free(vdev->name);
	g_spdk_vhost_device = NULL;
	return 0;
}
