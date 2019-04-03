/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 1992-2018 NetApp, Inc.
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

#ifndef NVMF_FC_ADM_API_H
#define NVMF_FC_ADM_API_H

#include "spdk/nvmf.h"
#include "spdk/nvmf_fc_spec.h"

#define SPDK_FC_HW_DUMP_REASON_STR_MAX_SIZE 256
#define SPDK_MAX_NUM_OF_FC_PORTS 32
#define SPDK_NVMF_PORT_ID_MAX_LEN 32

/**
 * Types events from FCT driver to master thread. (see spdk_nvmf_fc_master_enqueue_event)
 */
enum spdk_fc_event {
	SPDK_FC_HW_PORT_INIT,
	SPDK_FC_HW_PORT_ONLINE,
	SPDK_FC_HW_PORT_OFFLINE,
	SPDK_FC_HW_PORT_RESET,
	SPDK_FC_NPORT_CREATE,
	SPDK_FC_NPORT_DELETE,
	SPDK_FC_IT_ADD,    /* PRLI */
	SPDK_FC_IT_DELETE, /* PRLI */
	SPDK_FC_ABTS_RECV,
	SPDK_FC_LINK_BREAK,
	SPDK_FC_HW_PORT_DUMP,
	SPDK_FC_UNRECOVERABLE_ERR,
	SPDK_FC_EVENT_MAX,
};

/**
 * Arguments for to dump assoc id
 */
struct spdk_nvmf_fc_dump_assoc_id_args {
	uint8_t                           pport_handle;
	uint16_t                          nport_handle;
	uint32_t                          assoc_id;
};

/*
 * FC HWQP pointer (from low level FC driver)
 */
typedef void *spdk_nvmf_fc_lld_hwqp_t;

/**
 * Arguments for HW port init event.
 */
struct spdk_nvmf_fc_hw_port_init_args {
	uint32_t                       ls_queue_size;
	spdk_nvmf_fc_lld_hwqp_t        ls_queue;
	uint32_t                       io_queue_size;
	uint32_t                       io_queue_cnt;
	spdk_nvmf_fc_lld_hwqp_t       *io_queues;
	void                          *cb_ctx;
	void                          *port_ctx;
	uint8_t                        port_handle;
	uint8_t                        nvme_aq_index;  /* io_queue used for nvme admin queue */
	uint16_t                       fcp_rq_id; /* Base rq ID of SCSI queue */
};

/**
 * Arguments for HW port link break event.
 */
struct spdk_nvmf_hw_port_link_break_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

/**
 * Arguments for HW port online event.
 */
struct spdk_nvmf_fc_hw_port_online_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

/**
 * Arguments for HW port offline event.
 */
struct spdk_nvmf_fc_hw_port_offline_args {
	uint8_t port_handle;
	void   *cb_ctx;
};

/**
 * Arguments for n-port add event.
 */
struct spdk_nvmf_fc_nport_create_args {
	uint8_t                     port_handle;
	uint16_t                    nport_handle;
	struct spdk_uuid            container_uuid; /* UUID of the nports container */
	struct spdk_uuid            nport_uuid;     /* Unique UUID for the nport */
	uint32_t                    d_id;
	struct spdk_nvmf_fc_wwn fc_nodename;
	struct spdk_nvmf_fc_wwn fc_portname;
	uint32_t                    subsys_id; /* Subsystemid */
	char                        port_id[SPDK_NVMF_PORT_ID_MAX_LEN];
	void                       *cb_ctx;
};

/**
 * Arguments for n-port delete event.
 */
struct spdk_nvmf_fc_nport_delete_args {
	uint8_t  port_handle;
	uint32_t nport_handle;
	uint32_t subsys_id; /* Subsystemid */
	void    *cb_ctx;
};

/**
 * Arguments for I_T add event.
 */
struct spdk_nvmf_fc_hw_i_t_add_args {
	uint8_t                      port_handle;
	uint32_t                     nport_handle;
	uint16_t                     itn_handle;
	uint32_t                     rpi;
	uint32_t                     s_id;
	uint32_t                     initiator_prli_info;
	uint32_t                     target_prli_info; /* populated by the SPDK master */
	struct spdk_nvmf_fc_wwn  fc_nodename;
	struct spdk_nvmf_fc_wwn  fc_portname;
	void                        *cb_ctx;
};

/**
 * Arguments for I_T delete event.
 */
struct spdk_nvmf_fc_hw_i_t_delete_args {
	uint8_t  port_handle;
	uint32_t nport_handle;
	uint16_t itn_handle;    /* Only used by FC LLD driver; unused in SPDK */
	uint32_t rpi;
	uint32_t s_id;
	void    *cb_ctx;
};

/**
 * Arguments for ABTS  event.
 */
struct spdk_nvmf_fc_abts_args {
	uint8_t  port_handle;
	uint32_t nport_handle;
	uint32_t rpi;
	uint16_t oxid, rxid;
	void    *cb_ctx;
};

/**
 * Arguments for link break event.
 */
struct spdk_nvmf_fc_link_break_args {
	uint8_t port_handle;
};

/**
 * Arguments for port reset event.
 */
struct spdk_nvmf_fc_hw_port_reset_args {
	uint8_t    port_handle;
	bool       dump_queues;
	char       reason[SPDK_FC_HW_DUMP_REASON_STR_MAX_SIZE];
	uint32_t **dump_buf;
	void      *cb_ctx;
};

/**
 * Arguments for unrecoverable error event
 */
struct spdk_nvmf_fc_unrecoverable_error_event_args {
};

/**
 * Callback function to the FCT driver.
 */
typedef void (*spdk_nvmf_fc_callback)(uint8_t port_handle,
				      enum spdk_fc_event event_type,
				      void *arg, int err);

/**
 * Enqueue an FCT event to master thread
 *
 * \param event_type Type of the event.
 * \param args Pointer to the argument structure.
 * \param cb_func Callback function into fc driver.
 *
 * \return 0 on success, non-zero on failure.
 */
int
spdk_nvmf_fc_master_enqueue_event(enum spdk_fc_event event_type,
				  void *args,
				  spdk_nvmf_fc_callback cb_func);

/**
  * Print a list of all the FC ports
  *
  * \param arg1 Unused
  * \param arg2 Unused
  */
void spdk_nvmf_fc_print_port_list(void *arg1, void *arg2);

/**
  * Print the contents of an FC port
  *
  * \param arg1 The global id of the FC port.
  * \param arg2 Unused
  */
void spdk_nvmf_fc_print_port(void *arg1, void *arg2);

/**
  * Print the contents of a given FC Nport
  *
  * \param arg1 The global id of the FC port.
  * \param arg2 The id of the Nport on the FC port.
  */
void spdk_nvmf_fc_print_nport(void *arg1, void *arg2);

/**
  * Print the contents of a given HWQP
  *
  * \param arg1 Unused
  * \param arg2 Unused
  */
void spdk_nvmf_fc_print_hwqp(void *arg1, void *arg2);

/**
  * Print the contents of a given association
  *
  * \param arg1 The hwqp id.
  * \param arg2 Unused
  */
void spdk_nvmf_fc_print_assoc(void *arg1, void *arg2);

/**
  * Print the contents of a given connection
  *
  * param arg1 - The association id
  * param arg2 - Unused
  */
void spdk_nvmf_fc_print_conn(void *arg1, void *arg2);

#endif
