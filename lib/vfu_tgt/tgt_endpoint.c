/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/cpuset.h"
#include "spdk/likely.h"
#include "spdk/vfu_target.h"

#include "tgt_internal.h"

struct tgt_pci_device_ops {
	struct spdk_vfu_endpoint_ops ops;
	TAILQ_ENTRY(tgt_pci_device_ops) link;
};

static struct spdk_cpuset g_tgt_core_mask;
static pthread_mutex_t g_endpoint_lock = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, spdk_vfu_endpoint) g_endpoint = TAILQ_HEAD_INITIALIZER(g_endpoint);
static TAILQ_HEAD(, tgt_pci_device_ops) g_pci_device_ops = TAILQ_HEAD_INITIALIZER(g_pci_device_ops);
static char g_endpoint_path_dirname[PATH_MAX] = "";

static struct spdk_vfu_endpoint_ops *
tgt_get_pci_device_ops(const char *device_type_name)
{
	struct tgt_pci_device_ops *pci_ops, *tmp;
	bool exist = false;

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_FOREACH_SAFE(pci_ops, &g_pci_device_ops, link, tmp) {
		if (!strncmp(device_type_name, pci_ops->ops.name, SPDK_VFU_MAX_NAME_LEN)) {
			exist = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	if (exist) {
		return &pci_ops->ops;
	}
	return NULL;
}

int
spdk_vfu_register_endpoint_ops(struct spdk_vfu_endpoint_ops *ops)
{
	struct tgt_pci_device_ops *pci_ops;
	struct spdk_vfu_endpoint_ops *tmp;

	tmp = tgt_get_pci_device_ops(ops->name);
	if (tmp) {
		return -EEXIST;
	}

	pci_ops = calloc(1, sizeof(*pci_ops));
	if (!pci_ops) {
		return -ENOMEM;
	}
	pci_ops->ops = *ops;

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_INSERT_TAIL(&g_pci_device_ops, pci_ops, link);
	pthread_mutex_unlock(&g_endpoint_lock);

	return 0;
}

static char *
tgt_get_base_path(void)
{
	return g_endpoint_path_dirname;
}

int
spdk_vfu_set_socket_path(const char *basename)
{
	int ret;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(g_endpoint_path_dirname, sizeof(g_endpoint_path_dirname) - 2, "%s", basename);
		if (ret <= 0) {
			return -EINVAL;
		}
		if ((size_t)ret >= sizeof(g_endpoint_path_dirname) - 2) {
			SPDK_ERRLOG("Char dev dir path length %d is too long\n", ret);
			return -EINVAL;
		}

		if (g_endpoint_path_dirname[ret - 1] != '/') {
			g_endpoint_path_dirname[ret] = '/';
			g_endpoint_path_dirname[ret + 1]  = '\0';
		}
	}

	return 0;
}

struct spdk_vfu_endpoint *
spdk_vfu_get_endpoint_by_name(const char *name)
{
	struct spdk_vfu_endpoint *endpoint, *tmp;
	bool exist = false;

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_FOREACH_SAFE(endpoint, &g_endpoint, link, tmp) {
		if (!strncmp(name, endpoint->name, SPDK_VFU_MAX_NAME_LEN)) {
			exist = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	if (exist) {
		return endpoint;
	}
	return NULL;
}

static int
tgt_vfu_ctx_poller(void *ctx)
{
	struct spdk_vfu_endpoint *endpoint = ctx;
	vfu_ctx_t *vfu_ctx = endpoint->vfu_ctx;
	int ret;

	ret = vfu_run_ctx(vfu_ctx);
	if (spdk_unlikely(ret == -1)) {
		if (errno == EBUSY) {
			return SPDK_POLLER_IDLE;
		}

		if (errno == ENOTCONN) {
			spdk_poller_unregister(&endpoint->vfu_ctx_poller);
			if (endpoint->ops.detach_device) {
				endpoint->ops.detach_device(endpoint);
			}
			endpoint->is_attached = false;
			return SPDK_POLLER_BUSY;
		}
	}

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
tgt_accept_poller(void *ctx)
{
	struct spdk_vfu_endpoint *endpoint = ctx;
	int ret;

	if (endpoint->is_attached) {
		return SPDK_POLLER_IDLE;
	}

	ret = vfu_attach_ctx(endpoint->vfu_ctx);
	if (ret == 0) {
		ret = endpoint->ops.attach_device(endpoint);
		if (!ret) {
			SPDK_NOTICELOG("%s: attached successfully\n", spdk_vfu_get_endpoint_id(endpoint));
			/* Polling socket too frequently will cause performance issue */
			endpoint->vfu_ctx_poller = SPDK_POLLER_REGISTER(tgt_vfu_ctx_poller, endpoint, 1000);
			endpoint->is_attached = true;
		}
		return SPDK_POLLER_BUSY;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

static void
tgt_log_cb(vfu_ctx_t *vfu_ctx, int level, char const *msg)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);

	if (level >= LOG_DEBUG) {
		SPDK_DEBUGLOG(vfu, "%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else if (level >= LOG_INFO) {
		SPDK_INFOLOG(vfu, "%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else if (level >= LOG_NOTICE) {
		SPDK_NOTICELOG("%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else if (level >= LOG_WARNING) {
		SPDK_WARNLOG("%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else {
		SPDK_ERRLOG("%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	}
}

static int
tgt_get_log_level(void)
{
	int level;

	if (SPDK_DEBUGLOG_FLAG_ENABLED("vfu")) {
		return LOG_DEBUG;
	}

	level = spdk_log_to_syslog_level(spdk_log_get_level());
	if (level < 0) {
		return LOG_ERR;
	}

	return level;
}

static void
init_pci_config_space(vfu_pci_config_space_t *p, uint16_t ipin)
{
	/* MLBAR */
	p->hdr.bars[0].raw = 0x0;
	/* MUBAR */
	p->hdr.bars[1].raw = 0x0;

	/* vendor specific, let's set them to zero for now */
	p->hdr.bars[3].raw = 0x0;
	p->hdr.bars[4].raw = 0x0;
	p->hdr.bars[5].raw = 0x0;

	/* enable INTx */
	p->hdr.intr.ipin = ipin;
}

static void
tgt_memory_region_add_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	void *map_start, *map_end;
	int ret;

	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(vfu, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	if (info->prot == (PROT_WRITE | PROT_READ)) {
		ret = spdk_mem_register(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region register %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}

	if (endpoint->ops.post_memory_add) {
		endpoint->ops.post_memory_add(endpoint, map_start, map_end);
	}
}

static void
tgt_memory_region_remove_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	void *map_start, *map_end;
	int ret = 0;

	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(vfu, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	if (endpoint->ops.pre_memory_remove) {
		endpoint->ops.pre_memory_remove(endpoint, map_start, map_end);
	}

	if (info->prot == (PROT_WRITE | PROT_READ)) {
		ret = spdk_mem_unregister(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region unregister %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}
}

static int
tgt_device_quiesce_cb(vfu_ctx_t *vfu_ctx)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	int ret;

	assert(endpoint->ops.quiesce_device);
	ret = endpoint->ops.quiesce_device(endpoint);
	if (ret) {
		errno = EBUSY;
		ret = -1;
	}

	return ret;
}

static int
tgt_device_reset_cb(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);

	SPDK_DEBUGLOG(vfu, "Device reset type %u\n", type);

	assert(endpoint->ops.reset_device);
	return endpoint->ops.reset_device(endpoint);
}

static int
tgt_endpoint_realize(struct spdk_vfu_endpoint *endpoint)
{
	int ret;
	uint8_t buf[512];
	struct vsc *vendor_cap;
	ssize_t cap_offset;
	uint16_t vendor_cap_idx, cap_size, sparse_mmap_idx;
	struct spdk_vfu_pci_device pci_dev;
	uint8_t region_idx;

	assert(endpoint->ops.get_device_info);
	ret = endpoint->ops.get_device_info(endpoint, &pci_dev);
	if (ret) {
		SPDK_ERRLOG("%s: failed to get pci device info\n", spdk_vfu_get_endpoint_id(endpoint));
		return ret;
	}

	endpoint->vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, endpoint->uuid, LIBVFIO_USER_FLAG_ATTACH_NB,
					   endpoint, VFU_DEV_TYPE_PCI);
	if (endpoint->vfu_ctx == NULL) {
		SPDK_ERRLOG("%s: error creating libvfio-user context\n", spdk_vfu_get_endpoint_id(endpoint));
		return -EFAULT;
	}
	vfu_setup_log(endpoint->vfu_ctx, tgt_log_cb, tgt_get_log_level());

	ret = vfu_pci_init(endpoint->vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to initialize PCI\n", endpoint->vfu_ctx);
		goto error;
	}

	vfu_pci_set_id(endpoint->vfu_ctx, pci_dev.id.vid, pci_dev.id.did, pci_dev.id.ssvid,
		       pci_dev.id.ssid);
	vfu_pci_set_class(endpoint->vfu_ctx, pci_dev.class.bcc, pci_dev.class.scc, pci_dev.class.pi);

	/* Add Vendor Capabilities */
	for (vendor_cap_idx = 0; vendor_cap_idx < pci_dev.nr_vendor_caps; vendor_cap_idx++) {
		memset(buf, 0, sizeof(buf));
		cap_size = endpoint->ops.get_vendor_capability(endpoint, buf, 256, vendor_cap_idx);
		if (cap_size) {
			vendor_cap = (struct vsc *)buf;
			assert(vendor_cap->hdr.id == PCI_CAP_ID_VNDR);
			assert(vendor_cap->size == cap_size);

			cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, vendor_cap);
			if (cap_offset < 0) {
				SPDK_ERRLOG("vfu_ctx %p failed add vendor capability\n", endpoint->vfu_ctx);
				ret = -EFAULT;
				goto error;
			}
		}
	}

	/* Add Standard PCI Capabilities */
	cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, &pci_dev.pmcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pmcap\n", endpoint->vfu_ctx);
		ret = -EFAULT;
		goto error;
	}
	SPDK_DEBUGLOG(vfu, "%s PM cap_offset %ld\n", spdk_vfu_get_endpoint_id(endpoint), cap_offset);

	cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, &pci_dev.pxcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pxcap\n", endpoint->vfu_ctx);
		ret = -EFAULT;
		goto error;
	}
	SPDK_DEBUGLOG(vfu, "%s PX cap_offset %ld\n", spdk_vfu_get_endpoint_id(endpoint), cap_offset);

	cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, &pci_dev.msixcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add msixcap\n", endpoint->vfu_ctx);
		ret = -EFAULT;
		goto error;
	}
	SPDK_DEBUGLOG(vfu, "%s MSIX cap_offset %ld\n", spdk_vfu_get_endpoint_id(endpoint), cap_offset);

	/* Setup PCI Regions */
	for (region_idx = 0; region_idx < VFU_PCI_DEV_NUM_REGIONS; region_idx++) {
		struct spdk_vfu_pci_region *region = &pci_dev.regions[region_idx];
		struct iovec sparse_mmap[SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS];
		if (!region->len) {
			continue;
		}

		if (region->nr_sparse_mmaps) {
			assert(region->nr_sparse_mmaps <= SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS);
			for (sparse_mmap_idx = 0; sparse_mmap_idx < region->nr_sparse_mmaps; sparse_mmap_idx++) {
				sparse_mmap[sparse_mmap_idx].iov_base = (void *)region->mmaps[sparse_mmap_idx].offset;
				sparse_mmap[sparse_mmap_idx].iov_len = region->mmaps[sparse_mmap_idx].len;
			}
		}

		ret = vfu_setup_region(endpoint->vfu_ctx, region_idx, region->len, region->access_cb, region->flags,
				       region->nr_sparse_mmaps ? sparse_mmap : NULL, region->nr_sparse_mmaps,
				       region->fd, region->offset);
		if (ret) {
			SPDK_ERRLOG("vfu_ctx %p failed to setup region %u\n", endpoint->vfu_ctx, region_idx);
			goto error;
		}
		SPDK_DEBUGLOG(vfu, "%s: region %u, len 0x%"PRIx64", callback %p, nr sparse mmaps %u, fd %d\n",
			      spdk_vfu_get_endpoint_id(endpoint), region_idx, region->len, region->access_cb,
			      region->nr_sparse_mmaps, region->fd);
	}

	ret = vfu_setup_device_dma(endpoint->vfu_ctx, tgt_memory_region_add_cb,
				   tgt_memory_region_remove_cb);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup dma callback\n", endpoint->vfu_ctx);
		goto error;
	}

	if (endpoint->ops.reset_device) {
		ret = vfu_setup_device_reset_cb(endpoint->vfu_ctx, tgt_device_reset_cb);
		if (ret < 0) {
			SPDK_ERRLOG("vfu_ctx %p failed to setup reset callback\n", endpoint->vfu_ctx);
			goto error;
		}
	}

	if (endpoint->ops.quiesce_device) {
		vfu_setup_device_quiesce_cb(endpoint->vfu_ctx, tgt_device_quiesce_cb);
	}

	ret = vfu_setup_device_nr_irqs(endpoint->vfu_ctx, VFU_DEV_INTX_IRQ, pci_dev.nr_int_irqs);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup INTX\n", endpoint->vfu_ctx);
		goto error;
	}

	ret = vfu_setup_device_nr_irqs(endpoint->vfu_ctx, VFU_DEV_MSIX_IRQ, pci_dev.nr_msix_irqs);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup MSIX\n", endpoint->vfu_ctx);
		goto error;
	}

	ret = vfu_realize_ctx(endpoint->vfu_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to realize\n", endpoint->vfu_ctx);
		goto error;
	}

	endpoint->pci_config_space = vfu_pci_get_config_space(endpoint->vfu_ctx);
	assert(endpoint->pci_config_space != NULL);
	init_pci_config_space(endpoint->pci_config_space, pci_dev.intr_ipin);

	assert(cap_offset != 0);
	endpoint->msix = (struct msixcap *)((uint8_t *)endpoint->pci_config_space + cap_offset);

	return 0;

error:
	if (endpoint->vfu_ctx) {
		vfu_destroy_ctx(endpoint->vfu_ctx);
	}
	return ret;
}

static int
vfu_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int rc;
	struct spdk_cpuset negative_vfu_mask;

	if (cpumask == NULL) {
		return -1;
	}

	if (mask == NULL) {
		spdk_cpuset_copy(cpumask, &g_tgt_core_mask);
		return 0;
	}

	rc = spdk_cpuset_parse(cpumask, mask);
	if (rc < 0) {
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -1;
	}

	spdk_cpuset_copy(&negative_vfu_mask, &g_tgt_core_mask);
	spdk_cpuset_negate(&negative_vfu_mask);
	spdk_cpuset_and(&negative_vfu_mask, cpumask);

	if (spdk_cpuset_count(&negative_vfu_mask) != 0) {
		SPDK_ERRLOG("one of selected cpu is outside of core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_tgt_core_mask));
		return -1;
	}

	spdk_cpuset_and(cpumask, &g_tgt_core_mask);

	if (spdk_cpuset_count(cpumask) == 0) {
		SPDK_ERRLOG("no cpu is selected among core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_tgt_core_mask));
		return -1;
	}

	return 0;
}

static void
tgt_endpoint_start_thread(void *arg1)
{
	struct spdk_vfu_endpoint *endpoint = arg1;

	endpoint->accept_poller = SPDK_POLLER_REGISTER(tgt_accept_poller, endpoint, 1000);
	assert(endpoint->accept_poller != NULL);
}

static void
tgt_endpoint_thread_exit(void *arg1)
{
	struct spdk_vfu_endpoint *endpoint = arg1;

	spdk_poller_unregister(&endpoint->accept_poller);
	spdk_poller_unregister(&endpoint->vfu_ctx_poller);

	/* Ensure the attached device is stopped before destorying the vfu context */
	if (endpoint->ops.detach_device) {
		endpoint->ops.detach_device(endpoint);
	}

	if (endpoint->vfu_ctx) {
		vfu_destroy_ctx(endpoint->vfu_ctx);
	}

	endpoint->ops.destruct(endpoint);
	free(endpoint);

	spdk_thread_exit(spdk_get_thread());
}

int
spdk_vfu_create_endpoint(const char *endpoint_name, const char *cpumask_str,
			 const char *dev_type_name)
{
	char *basename;
	char uuid[PATH_MAX] = "";
	struct spdk_cpuset cpumask = {};
	struct spdk_vfu_endpoint *endpoint;
	struct spdk_vfu_endpoint_ops *ops;
	int ret = 0;

	ret = vfu_parse_core_mask(cpumask_str, &cpumask);
	if (ret) {
		return ret;
	}

	if (strlen(endpoint_name) >= SPDK_VFU_MAX_NAME_LEN - 1) {
		return -ENAMETOOLONG;
	}

	if (spdk_vfu_get_endpoint_by_name(endpoint_name)) {
		SPDK_ERRLOG("%s already exist\n", endpoint_name);
		return -EEXIST;
	}

	/* Find supported PCI device type */
	ops = tgt_get_pci_device_ops(dev_type_name);
	if (!ops) {
		SPDK_ERRLOG("Request %s device type isn't registered\n", dev_type_name);
		return -ENOTSUP;
	}

	basename = tgt_get_base_path();
	if (snprintf(uuid, sizeof(uuid), "%s%s", basename, endpoint_name) >= (int)sizeof(uuid)) {
		SPDK_ERRLOG("Resulting socket path for endpoint %s is too long: %s%s\n",
			    endpoint_name, basename, endpoint_name);
		return -EINVAL;
	}

	endpoint = calloc(1, sizeof(*endpoint));
	if (!endpoint) {
		return -ENOMEM;
	}

	endpoint->endpoint_ctx = ops->init(endpoint, basename, endpoint_name);
	if (!endpoint->endpoint_ctx) {
		free(endpoint);
		return -EINVAL;
	}
	endpoint->ops = *ops;
	snprintf(endpoint->name, SPDK_VFU_MAX_NAME_LEN, "%s", endpoint_name);
	snprintf(endpoint->uuid, sizeof(uuid), "%s", uuid);

	SPDK_DEBUGLOG(vfu, "Construct endpoint %s\n", endpoint_name);
	/* Endpoint realize */
	ret = tgt_endpoint_realize(endpoint);
	if (ret) {
		endpoint->ops.destruct(endpoint);
		free(endpoint);
		return ret;
	}

	endpoint->thread = spdk_thread_create(endpoint_name, &cpumask);
	if (!endpoint->thread) {
		endpoint->ops.destruct(endpoint);
		vfu_destroy_ctx(endpoint->vfu_ctx);
		free(endpoint);
		return -EFAULT;
	}

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_INSERT_TAIL(&g_endpoint, endpoint, link);
	pthread_mutex_unlock(&g_endpoint_lock);

	spdk_thread_send_msg(endpoint->thread, tgt_endpoint_start_thread, endpoint);

	return 0;
}

int
spdk_vfu_delete_endpoint(const char *endpoint_name)
{
	struct spdk_vfu_endpoint *endpoint;

	endpoint = spdk_vfu_get_endpoint_by_name(endpoint_name);
	if (!endpoint) {
		SPDK_ERRLOG("%s doesn't exist\n", endpoint_name);
		return -ENOENT;
	}

	SPDK_NOTICELOG("Destruct endpoint %s\n", endpoint_name);

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_REMOVE(&g_endpoint, endpoint, link);
	pthread_mutex_unlock(&g_endpoint_lock);
	spdk_thread_send_msg(endpoint->thread, tgt_endpoint_thread_exit, endpoint);

	return 0;
}

const char *
spdk_vfu_get_endpoint_id(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->uuid;
}

const char *
spdk_vfu_get_endpoint_name(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->name;
}

vfu_ctx_t *
spdk_vfu_get_vfu_ctx(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->vfu_ctx;
}

void *
spdk_vfu_get_endpoint_private(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->endpoint_ctx;
}

bool
spdk_vfu_endpoint_msix_enabled(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->msix->mxc.mxe;
}

bool
spdk_vfu_endpoint_intx_enabled(struct spdk_vfu_endpoint *endpoint)
{
	return !endpoint->pci_config_space->hdr.cmd.id;
}

void *
spdk_vfu_endpoint_get_pci_config(struct spdk_vfu_endpoint *endpoint)
{
	return (void *)endpoint->pci_config_space;
}

void
spdk_vfu_init(spdk_vfu_init_cb init_cb)
{
	uint32_t i;
	size_t len;

	if (g_endpoint_path_dirname[0] == '\0') {
		if (getcwd(g_endpoint_path_dirname, sizeof(g_endpoint_path_dirname) - 2) == NULL) {
			SPDK_ERRLOG("getcwd failed\n");
			return;
		}

		len = strlen(g_endpoint_path_dirname);
		if (g_endpoint_path_dirname[len - 1] != '/') {
			g_endpoint_path_dirname[len] = '/';
			g_endpoint_path_dirname[len + 1] = '\0';
		}
	}

	spdk_cpuset_zero(&g_tgt_core_mask);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_tgt_core_mask, i, true);
	}

	init_cb(0);
}

void *
spdk_vfu_map_one(struct spdk_vfu_endpoint *endpoint, uint64_t addr, uint64_t len, dma_sg_t *sg,
		 struct iovec *iov,
		 int prot)
{
	int ret;

	assert(endpoint != NULL);
	assert(endpoint->vfu_ctx != NULL);
	assert(sg != NULL);
	assert(iov != NULL);

	ret = vfu_addr_to_sgl(endpoint->vfu_ctx, (void *)(uintptr_t)addr, len, sg, 1, prot);
	if (ret < 0) {
		return NULL;
	}

	ret = vfu_sgl_get(endpoint->vfu_ctx, sg, iov, 1, 0);
	if (ret != 0) {
		return NULL;
	}

	assert(iov->iov_base != NULL);
	return iov->iov_base;
}

void
spdk_vfu_unmap_sg(struct spdk_vfu_endpoint *endpoint, dma_sg_t *sg, struct iovec *iov, int iovcnt)
{
	assert(endpoint != NULL);
	assert(endpoint->vfu_ctx != NULL);
	assert(sg != NULL);
	assert(iov != NULL);

	vfu_sgl_put(endpoint->vfu_ctx, sg, iov, iovcnt);
}

void
spdk_vfu_fini(spdk_vfu_fini_cb fini_cb)
{
	struct spdk_vfu_endpoint *endpoint, *tmp;
	struct tgt_pci_device_ops *ops, *ops_tmp;

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_FOREACH_SAFE(ops, &g_pci_device_ops, link, ops_tmp) {
		TAILQ_REMOVE(&g_pci_device_ops, ops, link);
		free(ops);
	}

	TAILQ_FOREACH_SAFE(endpoint, &g_endpoint, link, tmp) {
		TAILQ_REMOVE(&g_endpoint, endpoint, link);
		spdk_thread_send_msg(endpoint->thread, tgt_endpoint_thread_exit, endpoint);
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	fini_cb();
}
SPDK_LOG_REGISTER_COMPONENT(vfu)
