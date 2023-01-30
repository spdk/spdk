/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include <libflexio/flexio.h>
#include <libflexio/flexio_elf.h>
#include "snap-rdma/src/snap_vrdma.h"
#include "vrdma_dpa.h"
#include "vrdma_dpa_vq.h"
#include "dpa/vrdma_dpa_common.h"
#include "include/spdk/vrdma.h"
#include "stdio.h"

#define DEV_ELF_PATH "dpa/dpa_dev.elf"
#define PRINF_BUF_SZ	(4 * 2048)
extern struct vrdma_vq_ops vrdma_dpa_vq_ops;

static
int extract_dev_elf(const char *dev_elf_fname, void **elf_buf, size_t *elf_size)
{
	int err;

	if (!dev_elf_fname) {
		log_error("No filename/path provided");
		return -EINVAL;
	}

	log_debug("Parsing device ELF file '%s'", dev_elf_fname);
	err = flexio_get_elf_file(dev_elf_fname, elf_buf, elf_size);
	if (err)
		return err;

	log_debug("Device ELF file size is %zdB", *elf_size);
	return 0;
}


#define PRINTF_BUFF_BSIZE (4 * 2048)
int vrdma_dpa_init(const struct vrdma_prov_init_attr *attr, void **out)
{
#define MR_BASE_AND_SIZE_ALIGN 64
	struct vrdma_dpa_ctx *dpa_ctx;
	size_t elf_size;
	int padding;
	int err;

	dpa_ctx = calloc(1, sizeof(*dpa_ctx));
	if (!dpa_ctx) {
		log_error("Failed to allocate dpa_ctx memory");
		return -ENOMEM;
	}
	log_debug("===naliu vrdma_dpa_init begin\n");
	dpa_ctx->core_count = 1;
	err = extract_dev_elf(DEV_ELF_PATH, &dpa_ctx->elf_buf, &elf_size);
	if (err) {
		log_error("Failed to extract dev elf, err(%d)", err);
		goto err_dev_elf;
	}
	log_debug("===naliu vrdma_dpa_init extract_dev_elf done\n");
	err = flexio_process_create(attr->emu_ctx, dpa_ctx->elf_buf, elf_size,
				    NULL, &dpa_ctx->flexio_process);
	if (err) {
		log_error("Failed to create Flex IO process, err(%d)", err);
		goto err_process_create;
	}

	/* For emu manager: UAR to press CQ and QP doorbells via outbox. */
	dpa_ctx->emu_uar = mlx5dv_devx_alloc_uar(attr->emu_ctx,
						 MLX5DV_UAR_ALLOC_TYPE_NC);
	if (!dpa_ctx->emu_uar) {
		log_error("Failed to allocate UAR");
		err = -1;
		goto err_dev_alloc_uar;
	}

	err = flexio_uar_create(dpa_ctx->flexio_process, dpa_ctx->emu_uar,
				&dpa_ctx->flexio_uar);
	if (err) {
		log_error("Failed to create UAR");
		goto err_uar_create;
	}

	/* outbox to press CQ and QP doorbells */
	err = flexio_outbox_create(dpa_ctx->flexio_process, attr->emu_ctx,
				   dpa_ctx->flexio_uar,
				   &dpa_ctx->db_outbox);
	if (err) {
		log_error("Failed to create outbox, err(%d)", err);
		goto err_outbox_create;
	}

	/* window used to get host pi address*/
	err = flexio_window_create(dpa_ctx->flexio_process, attr->emu_pd,
				   &dpa_ctx->window);
	if (err) {
		log_error("Failed to create window, err(%d)", err);
		goto err_window_create;
	}

	/*Init Print environment*/
	err = vrdma_dpa_dev_print_init(dpa_ctx->flexio_process,
					 dpa_ctx->flexio_uar, PRINF_BUF_SZ,
					 stdout, 0, NULL);
	if (err) {
		log_error("Failed to init vrdma dpa dev print, err(%d)", err);
		goto err_print;
	}

	/* size padding allocation of hdata memory = ibv_reg_mr requirement*/
	padding = sizeof(*dpa_ctx->vq_data) + (MR_BASE_AND_SIZE_ALIGN - 1);
	padding -= padding % MR_BASE_AND_SIZE_ALIGN;
	err = posix_memalign((void **)&dpa_ctx->vq_data,
			     MR_BASE_AND_SIZE_ALIGN, padding);
	if (err) {
		log_error("posix_memalign failed, err(%d)", err);
		goto err_print;
	}
	memset(dpa_ctx->vq_data, 0, sizeof(*dpa_ctx->vq_data));

	dpa_ctx->vq_counter_mr = ibv_reg_mr(attr->emu_pd, dpa_ctx->vq_data,
					   padding, IBV_ACCESS_LOCAL_WRITE);
	if (!dpa_ctx->vq_counter_mr) {
		log_error("Failed to register MR, err(%d)", errno);
		goto err_reg_mr;
	}

	*out = (void *)dpa_ctx;
	return 0;

err_reg_mr:
	free(dpa_ctx->vq_data);
err_print:
	flexio_window_destroy(dpa_ctx->window);
err_window_create:
	flexio_outbox_destroy(dpa_ctx->db_outbox);
err_outbox_create:
	flexio_uar_destroy(dpa_ctx->flexio_uar);
err_uar_create:
	mlx5dv_devx_free_uar(dpa_ctx->emu_uar);
err_dev_alloc_uar:
	flexio_process_destroy(dpa_ctx->flexio_process);
err_process_create:
	free(dpa_ctx->elf_buf);
err_dev_elf:
	free(dpa_ctx);
	return err;
}

void vrdma_dpa_uninit(void *in)
{
	struct vrdma_dpa_ctx *dpa_ctx;

	dpa_ctx = (struct vrdma_dpa_ctx *)in;
	ibv_dereg_mr(dpa_ctx->vq_counter_mr);
	free(dpa_ctx->vq_data);
	flexio_window_destroy(dpa_ctx->window);
	flexio_outbox_destroy(dpa_ctx->db_outbox);
	flexio_uar_destroy(dpa_ctx->flexio_uar);
	mlx5dv_devx_free_uar(dpa_ctx->emu_uar);
	flexio_process_destroy(dpa_ctx->flexio_process);
	free(dpa_ctx->elf_buf);
	free(dpa_ctx);
}


static int
vrdma_dpa_device_msix_create(struct flexio_process *process,
			       const struct vrdma_prov_emu_dev_init_attr *attr,
			       struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			       int max_msix)
{
	struct vrdma_msix_init_attr msix_attr = {};

	msix_attr.emu_ib_ctx  = attr->emu_ibv_ctx;
	msix_attr.emu_vhca_id = attr->emu_vhca_id;
	msix_attr.sf_ib_ctx   = attr->sf_ibv_ctx;
	msix_attr.sf_vhca_id  = attr->sf_vhca_id;
	msix_attr.msix_vector = attr->msix_config_vector;

	return vrdma_dpa_msix_create(NULL, process, &msix_attr, emu_dev_ctx,
				       max_msix);
}

static void vrdma_dpa_device_msix_destroy(struct flexio_msix *msix, uint16_t msix_vector,
			      struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	vrdma_dpa_msix_destroy(msix, msix_vector, emu_dev_ctx);
}

int vrdma_dpa_emu_dev_init(const struct vrdma_prov_emu_dev_init_attr *attr,
			     void **out)
{
	struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx;
	struct vrdma_dpa_ctx *dpa_ctx;
	int err;

	emu_dev_ctx = calloc(1, sizeof(*emu_dev_ctx));
	if (!emu_dev_ctx) {
		log_error("Failed to allocate emu_dev_ctx memory");
		return -ENOMEM;
	}
	log_debug("===naliu vrdma_dpa_emu_dev_init num_msix %d\n", attr->num_msix);
	emu_dev_ctx->msix = calloc(attr->num_msix,
				   sizeof(struct vrdma_dpa_msix));
	if (!emu_dev_ctx->msix) {
		log_error("Failed allocating memory to hold msix info");
		err = -ENOMEM;
		goto err_msix_alloc;
	}

	dpa_ctx = attr->dpa_handler;
	emu_dev_ctx->flexio_process = dpa_ctx->flexio_process;

	emu_dev_ctx->sf_uar = dpa_ctx->emu_uar;


	if (!emu_dev_ctx->sf_uar) {
		log_error("Failed to allocate UAR");
		err = -1;
		goto err_alloc_uar;
	}

	err = flexio_uar_create(dpa_ctx->flexio_process, emu_dev_ctx->sf_uar,
				&emu_dev_ctx->flexio_uar);
	if (err) {
		log_error("Failed to create UAR");
		goto err_uar_create;
	}

	/* outbox to press doorbells */
	err = flexio_outbox_create(dpa_ctx->flexio_process, attr->sf_ibv_ctx,
				   emu_dev_ctx->flexio_uar,
				   &emu_dev_ctx->db_sf_outbox);
	if (err) {
		log_error("Failed to create sf outbox, err(%d)", err);
		goto err_outbox_create;
	}

	err = vrdma_dpa_device_msix_create(dpa_ctx->flexio_process, attr,
					     emu_dev_ctx, attr->num_msix);
	if (err) {
		log_error("Failed to create device msix, err(%d)", errno);
		goto err_msix_create;
	}

	*out = emu_dev_ctx;
	return 0;

err_msix_create:
	flexio_outbox_destroy(emu_dev_ctx->db_sf_outbox);
err_outbox_create:
	flexio_uar_destroy(emu_dev_ctx->flexio_uar);
err_uar_create:
	mlx5dv_devx_free_uar(emu_dev_ctx->sf_uar);
err_alloc_uar:
	free(emu_dev_ctx->msix);
err_msix_alloc:
	free(emu_dev_ctx);
	return err;
}

void vrdma_dpa_emu_dev_uninit(void *emu_dev_handler)
{
	struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx = emu_dev_handler;

	vrdma_dpa_device_msix_destroy(emu_dev_ctx->flexio_msix,
					emu_dev_ctx->msix_config_vector,
					emu_dev_ctx);
	flexio_outbox_destroy(emu_dev_ctx->db_sf_outbox);
	flexio_uar_destroy(emu_dev_ctx->flexio_uar);
	mlx5dv_devx_free_uar(emu_dev_ctx->sf_uar);
	free(emu_dev_ctx->msix);
	free(emu_dev_ctx);
}

/*used when device state changed*/
static int vrdma_dpa_device_msix_send(void *handler)
{
	struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx = handler;
	uint32_t outbox_id;
	uint64_t rpc_ret;
	int err;

	outbox_id = flexio_outbox_get_id(emu_dev_ctx->db_sf_outbox);
	err = flexio_process_call(emu_dev_ctx->flexio_process,
		"vrdma_dpa_msix_send_rpc_handler",
		emu_dev_ctx->msix[emu_dev_ctx->msix_config_vector].cqn,
		outbox_id, 0, &rpc_ret);

	if (err)
		log_error("Failed to call rpc, err(%d), rpc_ret(%ld)",
			  err, rpc_ret);
	return err;
}

static struct vrdma_prov_ops vrdma_dpa_prov_ops = {
	.q_ops = &vrdma_dpa_vq_ops,
	.init = vrdma_dpa_init,
	.uninit = vrdma_dpa_uninit,
	.emu_dev_init = vrdma_dpa_emu_dev_init,
	.emu_dev_uninit = vrdma_dpa_emu_dev_uninit,
	.msix_send = vrdma_dpa_device_msix_send,
};

VRDMA_PROV_DECLARE(vrdma_dpa_prov_ops);
