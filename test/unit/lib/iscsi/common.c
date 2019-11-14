#include "iscsi/task.h"
#include "iscsi/iscsi.h"
#include "iscsi/conn.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/sock.h"
#include "spdk_cunit.h"

#include "spdk_internal/log.h"
#include "spdk_internal/mock.h"

#include "scsi/scsi_internal.h"

SPDK_LOG_REGISTER_COMPONENT("iscsi", SPDK_LOG_ISCSI)

TAILQ_HEAD(, spdk_iscsi_pdu) g_write_pdu_list = TAILQ_HEAD_INITIALIZER(g_write_pdu_list);

static bool g_task_pool_is_empty = false;
static bool g_pdu_pool_is_empty = false;

struct spdk_iscsi_task *
spdk_iscsi_task_get(struct spdk_iscsi_conn *conn,
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
	task->scsi.iovs = &task->scsi.iov;
	return task;
}

void
spdk_scsi_task_put(struct spdk_scsi_task *task)
{
	free(task);
}

void
spdk_put_pdu(struct spdk_iscsi_pdu *pdu)
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
spdk_get_pdu(void)
{
	struct spdk_iscsi_pdu *pdu;

	if (g_pdu_pool_is_empty) {
		return NULL;
	}

	pdu = malloc(sizeof(*pdu));
	if (!pdu) {
		return NULL;
	}

	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));
	pdu->ref = 1;

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

DEFINE_STUB(spdk_iscsi_drop_conns, int,
	    (struct spdk_iscsi_conn *conn, const char *conn_match, int drop_all),
	    0);

DEFINE_STUB(spdk_scsi_dev_delete_port, int,
	    (struct spdk_scsi_dev *dev, uint64_t id), 0);

DEFINE_STUB_V(spdk_shutdown_iscsi_conns, (void));

DEFINE_STUB_V(spdk_iscsi_conns_request_logout, (struct spdk_iscsi_tgt_node *target));

DEFINE_STUB(spdk_iscsi_get_active_conns, int, (struct spdk_iscsi_tgt_node *target), 0);

void
spdk_iscsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *iscsi_task;

	if (scsi_task != NULL) {
		iscsi_task = spdk_iscsi_task_from_scsi_task(scsi_task);
		free(iscsi_task);
	}
}

DEFINE_STUB_V(spdk_iscsi_task_mgmt_cpl, (struct spdk_scsi_task *scsi_task));

DEFINE_STUB(spdk_iscsi_conn_read_data, int,
	    (struct spdk_iscsi_conn *conn, int bytes, void *buf), 0);

DEFINE_STUB(spdk_iscsi_conn_readv_data, int,
	    (struct spdk_iscsi_conn *conn, struct iovec *iov, int iovcnt), 0);

void
spdk_iscsi_conn_write_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	TAILQ_INSERT_TAIL(&g_write_pdu_list, pdu, tailq);
}

DEFINE_STUB_V(spdk_iscsi_conn_logout, (struct spdk_iscsi_conn *conn));

DEFINE_STUB_V(spdk_scsi_task_set_status,
	      (struct spdk_scsi_task *task, int sc, int sk, int asc, int ascq));

void
spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len)
{
	SPDK_CU_ASSERT_FATAL(task->iovs != NULL);
	task->iovs[0].iov_base = data;
	task->iovs[0].iov_len = len;
}
