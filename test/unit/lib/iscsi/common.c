#include "iscsi/task.h"
#include "iscsi/iscsi.h"
#include "iscsi/conn.h"

#include "spdk/env.h"
#include "spdk/event.h"

#include "spdk_internal/log.h"

#include "scsi/scsi_internal.h"

SPDK_LOG_REGISTER_TRACE_FLAG("iscsi", SPDK_TRACE_ISCSI)

struct spdk_iscsi_task *
spdk_iscsi_task_get(struct spdk_iscsi_conn *conn,
		    struct spdk_iscsi_task *parent,
		    spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_iscsi_task *task;

	task = calloc(1, sizeof(*task));

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
	if (!pdu)
		return;

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

	pdu = malloc(sizeof(*pdu));
	if (!pdu) {
		return NULL;
	}

	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));
	pdu->ref = 1;

	return pdu;
}

void
spdk_scsi_task_process_null_lun(struct spdk_scsi_task *task)
{
}

void
spdk_scsi_dev_queue_task(struct spdk_scsi_dev *dev,
			 struct spdk_scsi_task *task)
{
}

struct spdk_scsi_port *
spdk_scsi_dev_find_port_by_id(struct spdk_scsi_dev *dev, uint64_t id)
{
	return NULL;
}

void
spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev,
			      struct spdk_scsi_task *task,
			      enum spdk_scsi_task_func func)
{
}

uint32_t
spdk_env_get_current_core(void)
{
	return 0;
}

struct spdk_event *
spdk_event_allocate(uint32_t core, spdk_event_fn fn, void *arg1, void *arg2)
{
	return NULL;
}

struct spdk_scsi_dev *
	spdk_scsi_dev_construct(const char *name, char **lun_name_list,
			int *lun_id_list, int num_luns, uint8_t protocol_id,
			void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			void *hotremove_ctx)
{
	return NULL;
}

void
spdk_scsi_dev_destruct(struct spdk_scsi_dev *dev)
{
}

int
spdk_scsi_dev_add_port(struct spdk_scsi_dev *dev, uint64_t id, const char *name)
{
	return 0;
}

int
spdk_iscsi_drop_conns(struct spdk_iscsi_conn *conn, const char *conn_match,
		      int drop_all)
{
	return 0;
}

void
spdk_shutdown_iscsi_conns(void)
{
}

void
spdk_iscsi_task_cpl(struct spdk_scsi_task *scsi_task)
{

}

void
spdk_iscsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{

}

int
spdk_iscsi_conn_read_data(struct spdk_iscsi_conn *conn, int bytes,
			  void *buf)
{
	return 0;
}

void
spdk_iscsi_conn_logout(struct spdk_iscsi_conn *conn)
{
}

void
spdk_scsi_dev_print(struct spdk_scsi_dev *dev)
{
}

void
spdk_scsi_task_set_status(struct spdk_scsi_task *task, int sc, int sk, int asc, int ascq)
{
}

void
spdk_scsi_task_set_data(struct spdk_scsi_task *task, void *data, uint32_t len)
{
	task->iovs[0].iov_base = data;
	task->iovs[0].iov_len = len;
}
