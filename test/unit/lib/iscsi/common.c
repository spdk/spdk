#include "iscsi/task.h"
#include "iscsi/iscsi.h"
#include "iscsi/conn.h"

#include "spdk/env.h"
#include "spdk/sock.h"
#include "spdk_cunit.h"

#include "spdk/log.h"
#include "spdk_internal/mock.h"

#include "scsi/scsi_internal.h"

SPDK_LOG_REGISTER_COMPONENT(iscsi)

struct spdk_trace_histories *g_trace_histories;
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB_V(spdk_trace_register_owner, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description, (const char *name,
		uint16_t tpoint_id, uint8_t owner_type, uint8_t object_type, uint8_t new_object,
		uint8_t arg1_type, const char *arg1_name));
DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));

TAILQ_HEAD(, spdk_iscsi_pdu) g_write_pdu_list = TAILQ_HEAD_INITIALIZER(g_write_pdu_list);

static bool g_task_pool_is_empty = false;
static bool g_pdu_pool_is_empty = false;

struct spdk_iscsi_task *
iscsi_task_get(struct spdk_iscsi_conn *conn,
	       struct spdk_iscsi_task *parent,
	       spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_iscsi_task *task;

	if (g_task_pool_is_empty) {
		return NULL;
	}

	task = calloc(1, sizeof(*task));
	if (!task) {
		return NULL;
	}

	task->conn = conn;
	task->scsi.cpl_fn = cpl_fn;
	if (parent) {
		parent->scsi.ref++;
		task->parent = parent;
		task->tag = parent->tag;
		task->lun_id = parent->lun_id;
		task->scsi.dxfer_dir = parent->scsi.dxfer_dir;
		task->scsi.transfer_len = parent->scsi.transfer_len;
		task->scsi.lun = parent->scsi.lun;
		task->scsi.cdb = parent->scsi.cdb;
		task->scsi.target_port = parent->scsi.target_port;
		task->scsi.initiator_port = parent->scsi.initiator_port;
		if (conn && (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV)) {
			conn->data_in_cnt++;
		}
	}

	task->scsi.iovs = &task->scsi.iov;
	return task;
}

void
spdk_scsi_task_put(struct spdk_scsi_task *task)
{
	free(task);
}

void
iscsi_put_pdu(struct spdk_iscsi_pdu *pdu)
{
	if (!pdu) {
		return;
	}

	pdu->ref--;
	if (pdu->ref < 0) {
		CU_FAIL("negative ref count");
		pdu->ref = 0;
	}

	if (pdu->ref == 0) {
		if (pdu->data && !pdu->data_from_mempool) {
			free(pdu->data);
		}
		free(pdu);
	}
}

struct spdk_iscsi_pdu *
iscsi_get_pdu(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu;

	assert(conn != NULL);
	if (g_pdu_pool_is_empty) {
		return NULL;
	}

	pdu = malloc(sizeof(*pdu));
	if (!pdu) {
		return NULL;
	}

	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));
	pdu->ref = 1;
	pdu->conn = conn;

	return pdu;
}

DEFINE_STUB_V(spdk_scsi_task_process_null_lun, (struct spdk_scsi_task *task));

DEFINE_STUB_V(spdk_scsi_task_process_abort, (struct spdk_scsi_task *task));

DEFINE_STUB_V(spdk_scsi_dev_queue_task,
	      (struct spdk_scsi_dev *dev, struct spdk_scsi_task *task));

DEFINE_STUB(spdk_scsi_dev_find_port_by_id, struct spdk_scsi_port *,
	    (struct spdk_scsi_dev *dev, uint64_t id), NULL);

DEFINE_STUB_V(spdk_scsi_dev_queue_mgmt_task,
	      (struct spdk_scsi_dev *dev, struct spdk_scsi_task *task));

const char *
spdk_scsi_dev_get_name(const struct spdk_scsi_dev *dev)
{
	if (dev != NULL) {
		return dev->name;
	}

	return NULL;
}

DEFINE_STUB(spdk_scsi_dev_construct, struct spdk_scsi_dev *,
	    (const char *name, const char **bdev_name_list,
	     int *lun_id_list, int num_luns, uint8_t protocol_id,
	     void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
	     void *hotremove_ctx),
	    NULL);

DEFINE_STUB_V(spdk_scsi_dev_destruct,
	      (struct spdk_scsi_dev *dev, spdk_scsi_dev_destruct_cb_t cb_fn, void *cb_arg));

DEFINE_STUB(spdk_scsi_dev_add_port, int,
	    (struct spdk_scsi_dev *dev, uint64_t id, const char *name), 0);

DEFINE_STUB(iscsi_drop_conns, int,
	    (struct spdk_iscsi_conn *conn, const char *conn_match, int drop_all),
	    0);

DEFINE_STUB(spdk_scsi_dev_delete_port, int,
	    (struct spdk_scsi_dev *dev, uint64_t id), 0);

DEFINE_STUB_V(shutdown_iscsi_conns, (void));

DEFINE_STUB_V(iscsi_conns_request_logout, (struct spdk_iscsi_tgt_node *target, int pg_tag));

DEFINE_STUB(iscsi_get_active_conns, int, (struct spdk_iscsi_tgt_node *target), 0);

void
iscsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *iscsi_task;

	if (scsi_task != NULL) {
		iscsi_task = iscsi_task_from_scsi_task(scsi_task);
		if (iscsi_task->parent && (iscsi_task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV)) {
			assert(iscsi_task->conn->data_in_cnt > 0);
			iscsi_task->conn->data_in_cnt--;
		}

		free(iscsi_task);
	}
}

DEFINE_STUB_V(iscsi_task_mgmt_cpl, (struct spdk_scsi_task *scsi_task));

DEFINE_STUB(iscsi_conn_read_data, int,
	    (struct spdk_iscsi_conn *conn, int bytes, void *buf), 0);

DEFINE_STUB(iscsi_conn_readv_data, int,
	    (struct spdk_iscsi_conn *conn, struct iovec *iov, int iovcnt), 0);

void
iscsi_conn_write_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
		     iscsi_conn_xfer_complete_cb cb_fn, void *cb_arg)
{
	TAILQ_INSERT_TAIL(&g_write_pdu_list, pdu, tailq);
}

DEFINE_STUB_V(iscsi_conn_logout, (struct spdk_iscsi_conn *conn));

DEFINE_STUB_V(spdk_scsi_task_set_status,
	      (struct spdk_scsi_task *task, int sc, int sk, int asc, int ascq));

void
spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len)
{
	SPDK_CU_ASSERT_FATAL(task->iovs != NULL);
	task->iovs[0].iov_base = data;
	task->iovs[0].iov_len = len;
}
