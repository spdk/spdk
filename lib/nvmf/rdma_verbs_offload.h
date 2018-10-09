#ifndef RDMA_VERBS_OFFLOAD_H
#define RDMA_VERBS_OFFLOAD_H

struct ibv_mr_sg {
	struct ibv_mr   *mr;
	union {
		void		*addr;
		uint64_t	 offset;
	};
	uint64_t	len;
};

enum ibv_nvmf_offload_ops {
	IBV_NVMF_OPS_WRITE            = 1 << 0,
	IBV_NVMF_OPS_READ             = 1 << 1,
	IBV_NVMF_OPS_FLUSH            = 1 << 2,
	IBV_NVMF_OPS_READ_WRITE       = IBV_NVMF_OPS_READ | IBV_NVMF_OPS_WRITE,
	IBV_NVMF_OPS_READ_WRITE_FLUSH = IBV_NVMF_OPS_READ_WRITE | IBV_NVMF_OPS_FLUSH
};

const int IBV_SRQ_INIT_ATTR_NVMF = 1 << 5;
const int IBV_SRQT_NVMF = 3;

struct ibv_nvmf_attrs {
	enum ibv_nvmf_offload_ops offload_ops
	;	    /* Which NVMe-oF operations to offload, combination should be supported according to caps */
	uint32_t		max_namespaces;	  /* Maximum allowed front-facing namespaces */
	uint8_t		nvme_log_page_sz; /* Page size of NVMe backend controllers, log, 4KB units */
	uint32_t		ioccsz;		  /* IO command capsule size, 16B units (NVMe-oF standard) */
	uint16_t		icdoff;		  /* In-capsule data offset, 16B units (NVMe-oF standard) */
	uint32_t		max_io_sz;	  /* Max IO transfer per NVMf transaction */
	uint16_t		nvme_queue_depth; /* Number of elements in queues of NVMe backend controllers */
	struct ibv_mr_sg	staging_buf;	  /* Memory for a staging buffer space */
};

struct ibv_srq_init_attr_ext {
	void		       *srq_context;
	struct ibv_srq_attr	attr;

	uint32_t		comp_mask;
	enum ibv_srq_type	srq_type;
	struct ibv_pd	       *pd;
	struct ibv_xrcd	       *xrcd;
	struct ibv_cq	       *cq;
	struct ibv_tm_cap	tm_cap;
	struct ibv_nvmf_attrs	nvmf_attr;
};

struct nvme_ctrl_attrs {
	struct ibv_mr_sg	sq_buf;	     /* The NVMe submit queue */
	struct ibv_mr_sg	cq_buf;	     /* The NVMe completion queue */
	struct ibv_mr_sg	sqdb;	     /* The NVMe submit queue doorbell, must be 4 bytes*/
	struct ibv_mr_sg	cqdb;	     /* The NVMe completion queue doorbell, must be 4 bytes*/
	uint16_t		sqdb_ini;	     /* NVMe SQ doorbell initial value */
	uint16_t		cqdb_ini;	     /* NVMe CQ doorbell initial value */
	uint16_t		cmd_timeout_ms; /* Command timeout */
	uint32_t		comp_mask;     /* For future extention */
};

struct ibv_nvme_ctrl {
	struct ibv_srq		*srq;
	struct nvme_ctrl_attrs	attrs;
};

enum {
	IBV_QP_NVMF_ATTR_FLAG_ENABLE     = 1 << 0,
};

static int ibv_query_device_ext(struct ibv_context *context,
				const struct ibv_query_device_ex_input *input,
				struct ibv_device_attr_ex *attr)
{
	int rc;
	rc = ibv_query_device_ex(context, NULL, attr);
	attr->nvmf_caps.offload_type_rc = IBV_NVMF_READ_WRITE_FLUSH_OFFLOAD;
	return rc;
}

static struct ibv_srq *ibv_create_srq_ext(struct ibv_context *context,
		struct ibv_srq_init_attr_ext *srq_init_attr_ext)
{
	struct ibv_srq_init_attr_ex srq_init_attr_ex;

	if (!context || !srq_init_attr_ext) {
		return NULL;
	}

	memset(&srq_init_attr_ex, 0, sizeof(struct ibv_srq_init_attr_ex));
	memcpy(&srq_init_attr_ex, srq_init_attr_ext, sizeof(struct ibv_srq_init_attr_ex));
	srq_init_attr_ex.srq_type = IBV_SRQT_BASIC;
	srq_init_attr_ex.comp_mask = srq_init_attr_ext->comp_mask & (~IBV_SRQ_INIT_ATTR_NVMF);
	return ibv_create_srq_ex(context, &srq_init_attr_ex);
}

static struct ibv_nvme_ctrl *ibv_srq_create_nvme_ctrl(struct ibv_srq *srq,
		struct nvme_ctrl_attrs *nvme_attrs)
{
	struct ibv_nvme_ctrl *ctrlr;
	if (!srq || !nvme_attrs) {
		return NULL;
	}

	ctrlr = calloc(1, sizeof(struct ibv_nvme_ctrl));
	if (ctrlr) {
		ctrlr->srq = srq;
		memcpy(&ctrlr->attrs, nvme_attrs, sizeof(ctrlr->attrs));
	}
	return ctrlr;
}

static int ibv_srq_remove_nvme_ctrl(struct ibv_srq *srq,
				    struct ibv_nvme_ctrl *nvme_ctrl)
{
	if (!nvme_ctrl) {
		return -1;
	}
	free(nvme_ctrl);
	return 0;
}

static int ibv_map_nvmf_nsid(struct ibv_nvme_ctrl *nvme_ctrl,
			     uint32_t fe_nsid,
			     uint16_t lba_data_size,
			     uint32_t nvme_nsid)
{
	if (!nvme_ctrl) {
		return -1;
	}
	return 0;
}

static int ibv_unmap_nvmf_nsid(struct ibv_nvme_ctrl *nvme_ctrl,
			       uint32_t fe_nsid)
{
	if (!nvme_ctrl) {
		return -1;
	}
	return 0;
}

static int ibv_qp_set_nvmf(struct ibv_qp *qp, unsigned int flags)
{
	if (!qp) {
		return -1;
	}
	return 0;
}

#endif /* RDMA_VERBS_OFFLOAD_H */
