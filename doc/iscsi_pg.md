# SPDK iSCSI Programmer's Guide {#iscsi_pg}

* @ref iscsi_pg_audience
* @ref iscsi_pg_intro
* @ref iscsi_pg_theory
* @ref iscsi_pg_design
* @ref iscsi_pg_examples
* @ref iscsi_pg_config
* @ref iscsi_pg_component

## Target Audience {#iscsi_pg_audience}

The programmer's guide is intended for developers authoring applications that utilize the SPDK iSCSI.
It is intended to supplement the source code in providing an overall understanding of how to
integrate iSCSI into an application as well as provide some high level insight into how iSCSI works
behind the scenes. It is not intended to serve as a design document or an API reference and in some
cases source code snippets and high level sequences will be discussed; for the latest source code
reference refer to the [repo](https://github.com/spdk).

## Introduction {#iscsi_pg_intro}

The iSCSI subsystem in SPDK library implements the iSCSI server logic according to iSCSI protocol
(i.e., RFC 3720) based on the application framework inside SPDK. Generally the related code can be
divided into several parts, i.e., (1)iSCSI target side initialization, which includes target side
parameter configuration, memory pool initialization, target node and group configuration, connection
configuration and etc; (2) iSCSI target side iSCSI command handling; (3) Asynchronous I/O handling
mechanism among network, iSCSI main components and bdev layers.

## Theory of Operation {#iscsi_pg_theory}

The purpose of iSCSI library is to provide users the related API and implementions for constructing
a high performance iSCSI target reference solution based on other components in spdk, e.g.,
application framework, storage service layer (i.e., bdev), low level device drivers (e.g., user space
NVMe driver). After iSCSI programing users understand the whole iSCSI handling logic in this lib,
they can add their own functions or refactor the lib according to their purpose.

## Design Considerations {#iscsi_pg_design}

Following aspects are considered to design and implement a high performance and efficient iSCSI
targets with basic functionalities.

### Per CPU core Performanance

The purpose is to improve the per cpu core performance in the following aspects (e.g., IOPS, latency)
compared with commercial iSCSI target, e.g., Linux IO target, iSCSI tgt. To achieve that,
following methods are used:

1 Pin iSCSI connections to dedicated CPU core: Each iSCSI connection should be aways handled by one
CPU core, but not be handled by different cores. The only case is connection migration for load
balance purpose.

2 Asynchronous I/O programing: Leverage application framework (i.e., reactor, poller, event, I/O
channel) to implement asynchronous I/O programing methods. Also for network communication, non
blocking TCP/IP is selected.

3 Zero copy support: For iSCSI read commands, we leverage the zero copy methods which means that
buffers are allocated in SPDK bdev layer, and this buffer will be freed after the iSCSI datain
response pdus are sent to iSCSI initiator. During all the iSCSI read handling process, there is no
data copy from storage module to network module.

4 Leverage SPDK user space NVMe drivers if the backend storage device are PCIe NVMe SSDs or NVMe SSDs
exported by remote NVMe-oF target.

### Functionality Consideration.

1 At least, we implement the basic iSCSI supports described in RFC 3720. Currently SPDK iSCSI target
implements ERL (Error recovery level) 0 and some support in ERL 1.

2 Pass the basic iSCSI/SCSI compliance test. We use UNH, calsoft, lib-iscsi test suites to verify
the iSCSI/SCSI compliance tests.

3 Support both legacy Linux and windows iSCSI initaitor for basic tests. For example, SPDK iSCSI
target works with iscsiadm in Linux.

## Examples {#iscsi_pg_examples}

**iscsi_tgt:** Actually named iscsi_tgt.c, which is a iSCSI tgt application based on SPDK librararies
including iSCSI, SCSI, bdev, NVMe driver lib, app framework and etc. It is an example demonstrates
how to glue SPDK iSCSI lib and others to construct a high performance iSCSI target implemention.
For more details on how to configure and evaluate the iSCSI target provided by SPDK, you can refer
the iSCSI target user guide.

## Configuration {#iscsi_pg_config}

For configuration details on SPDK iSCSI target, you can refer the information in ISCSI target user
guide.

## Component Detail {#iscsi_pg_component}

Following shows the detailed API of files in lib/iscsi folder:

###File: acceptor.h/c

The two files are used to handle the iSCSI connection requests from network.

Exported APIs:

**spdk_iscsi_acceptor_start:** Register portal acceptor poller (i.e., function: spdk_iscsi_portal_accept).

**spdk_iscsi_acceptor_stop:** Unrgister the poller. spdk_iscsi_port_accept is used to accept the iSCSI
connetion request. If connection is  accepted, it will call spdk_iscsi_conn_construct to conduct the
following connection handling.

### File: conn.h/c

The two files are used to handle the iSCSI connection, and the following shows
the detailed info:

**struct spdk_iscsi_conn: This structure contains lots of useful information for a iSCSI connection as
showed in the following but not limited:

(1) Connection belongs to which portal.

(2) Connection states: Invalid, running, logged_out, exiting, exited.

(3) Pdu related info: e.g., pdu in progress, write pdu list, snack pdu list.

(4) Tasks related: r2t_tasks, datain_tasks.

(5) Session/connection related parameter state.

(6) Initiator and target related address info (e.g., ip, port)

(7) Lun/bdev related with this connection.

Exported APIs:

**spdk_initialize_iscsi_conns:** Initialize all iSCSI connections

**spdk_shutdown_iscsi_conns:** Clean all iSCSI connections

**spdk_iscsi_conn_construct:** Construct a connection when the portal accepts the iSCSI connection.
It allocates the connection from the iSCSI connection pools. After that, it calls spdk_register_poller
with the function.

**spdk_iscsi_conn_login_do_work:** Handle the login related SCSI commands related with  this connection.

**spdk_iscsi_conn_destruct:** Destruct the connection. And this function does the following work: (1) Cleanup
all the task belonging to the backend lun operated by the connection; (2) clean up all r2t related
tasks: i.e., outstanding r2t tasks, active r2t tasks, queued r2t tasks; (3) clean up all data in related
tasks; (4) clean up write_pdu_list, snack_pdu_list;(5) Check the tasks related with this connection,
if all tasks are freed, it calls spdk_iscsi_conn_stop_poller, otherwise registering a shutdown timer
poller with function spdk_iscsi_conn_check_shutdown to check the remaining task belonging to this connection.
By checking tasks periodically, it will finally call spdk_iscsi_conn_stop_poller again.

**spdk_iscsi_conn_logout:** Set the connection to logout status and handle the logout event with a timer
poller, i.e., logout_timeout. Currently, the default timer is 5s.

**spdk_iscsi_drop_conns:** Drop the connection with provided info.
spdk_iscsi_conn_set_min_per_core: Set the min connection count which a cpu core will handle, which
is used for the load blance purpose.

**spdk_iscsi_set_min_conn_idle_interval:** Set the minimal idle interval for the conn.

**spdk_iscsi_conn_read_data:** Read the data with buf from the socket binding to the conn.

**spdk_iscsi_conn_free_pdu:** Free the pdu belongling to the connection, which may be related with
handling data_in_cnt if the returned iscsi cmd is ISCSI_OP_SCSI_DATAIN, and it will also free the
pdu related task.

### File: init_grp.h/c

The two files are used to manage the iSCSI initator groups, more detailed info are listed:

**struct spdk_iscsi_initiator_name:** Manage the name of iscsi initator. A valid name can be "ANY",
which means allowing any initiator names.

**struct spdk_iscsi_initiator_netmask:** manage the network master of initiator. A valid network mask
can be "192.168.2.0/24".

**struct spdk_iscsi_init_grp:** Manage the group of iSCSI initiator. In a group, it contains several
instance of spddk_iscsi_initiator_name, and several instance of spdk_iscsi_initiator_mask.

Exported APIs:

**spdk_iscsi_init_grp_release:** Relase a initiator group and it will call spdk_iscsi_init_grp_destroy.

**spdk_iscsi_init_grp_find_by_tag:** Find a initiator group by a tag.

**spdk_iscsi_init_grp_register:** Register an initator group into group list in instance from struct
spdk_iscsi_globals.

**spdk_iscsi_init_grp_array_create:** create initator group arrays from all sections in configuration file.

**spdk_iscsi_init_grp_array_destroy:** destroy all initiator groups.

### File: iscsi.h/c

The two files are used to handle the iSCSI protocols, serveral functions and structures are defined
to handle the pdu, tasks. More details are showed in the following:

**struct spdk_iscsi_pdu:** Define the pdu info which contains iSCSI head related info including basic
head, addtional head, data info and also the related digest for integrity checking.

**enum iscsi_connection_state:** Define the iscsi conneciton state.

**enum iscsi_chap_phase:** Define the phase for iSCSI chap authentication.

**enum session_type:** Define the session type

**struct iscsi_chap_auth:** Define all chap authentication info.

**struct spdk_iscsi_sess:** Define all the contained info for iSCSI session.

**struct spdk_iscsi_globals:** Define all the needed info for the iSCSI global target: e.g., portal,
portal group, initator. group, target node, session, all the related memory pools for pdu, session,
task.

Exported APIs(for handling iSCSI protocols):

**spdk_iscsi_send_nopin:** Used to send the nopin ISCSI response to the initiator.

**spdk_iscsi_task_response:** Convert the non management task owned by the conn and generate
response pdu contained the ISCSI command to the initiator.

**spdk_iscsi_execute:** Parse the ISCSI command in the pdu and execute, which includes: ISCSI_OP_NOPUT,
ISCSI_OP_SCSI, ISCSI_OP_TASK, ISCSI_OP_LOGIN, ISCSI_OP_TEXT, ISCSI_OP_SCSI_DATAOUT, ISCSI_OP_LOGOUT,
ISCSI_OP_SNACK and etc.

**spdk_iscsi_build_iovecs:** Convert the data contained in the pdu to io vectors, which is used to write
the data into network.

**spdk_iscsi_read_pdu:** Read the data from the network and construct it into a ISCSI pdu.

**spdk_iscsi_task_mgmt_response:** Convert the management task owned by the conn and generate response
pdu contained the ISCSI command to the initiator spdk_iscsi_conn_params_init: Initialze the
connections related parameters according to predefined conn_param_table

**ispdk_iscsi_sess_params_init:** Initialize session related paramters according to predefined
sess_param_table.

**spdk_free_sess:** Free the session and put it into the session pool.
spdk_clear_all_transfer_task: Clear all the r2t related task owned by a connection including
outstanding, pending and active r2t tasks.

**spdk_del_transfer_task:** Delete the r2t transfer task according to the task tag

**spdk_iscsi_is_deferred_free_pdu:** Judge whether a pdu should be freed in a deferred manner. Those r2t
and datain related pdu should be handled in this way.

**spdk_iscsi_negotiate_params:** Negotiat the params between the params sent from ISCSI initiator and
the params from iSCSI target side.

**spdk_iscsi_copy_param2var:** Update the corresponding fields owned by a conn according to the key value
contained in param pointer.

**spdk_iscsi_task_cpl:** iSCSI non management task call back completion function.

**spdk_iscsi_task_mgmt_cpl:** ISCSI management task call back completion function.

**spdk_iscsi_conn_handle_queued_tasks:** Handle the queued datain tasks.

**spdk_get_immediate_data_buffer_size:** Get immiediate data buffer size, which is used to put the
incoming ISCSI commands related info.

**spdk_get_data_out_buffer_size:** Get the data out buffer size, which is used to put the data for
reponse ISCSI commands with data.

## File: iscsi_rpc.c

This file exports several rpc functions to manage the iSCSI targets, all the supported functions are
registered int the following mananers:

SPDK_RPC_REGISTER(method_name, func), e.g., SPDK_RPC_REGISTER("get_initiator_group",
spdk_rpc_get_initiator_group).

Exported APIs:

**spdk_rpc_get_initiator_groups:** Get the info of ISCSI initiator groups.

**spdk_rpc_add_initiator_group:** Add a iSCSI initiator group.

**spdk_rpc_delete_initiator_group:** Delete a iSCSI initiator group.

**spdk_rpc_get_target_nodes:** Get all iSCSI target node info.

**spdk_rpc_construct_target_node:** construct iSCSI target nod info

**spdk_rpc_add_pg_ig_maps:** Add iSCSI portal group to initiator group mapping info.

**spdk_rpc_delete_pg_ig_maps:** Delete iSCSI portal group to initiator group mapping info.

**spdk_rpc_delete_target_node:** Delete a ISCSI target node.

**spdk_rpc_get_portal_groups:** Get all iSCSI portal group info.

**spdk_rpc_add_portal_group:** Add a iSCSI portal group.

**spdk_rpc_delete_portal_group:** Delete a iSCSI portal group.

**spdk_rpc_get_iscsi_connections:** List useful info of all iSCSI connections.

**spdk_rpc_get_iscsi_global_params:** Get all iSCSI global parameters of the configured iSCSI target.

### File: iscsi_subsystem.c

This file is used to initialize iSCSI subsystem.

Exported APIs:

**spdk_iscsi_init:** Initialize SPDK defined iSCSI subsystem. The major work is to pass the configuration
files, initialize all the memory related pools, initialize all the target nodes, initialize all the
connections, and register the poller to open the portal for listening the connection from the network.

**spdk_iscsi_fini_cb:** Callback function passed to destroy iSCSI subsystem.

**spdk_iscsi_fini:** Destroy iSCSI subsystem.

**spdk_iscsi_fini_done:** Destroy all the resources for iSCSI subsystem.

**spdk_iscsi_config_text:** Show how to configure iSCSI subsystem.

**spdk_put_pdu:** Put the pdu into pdu memory pool.

**spdk_get_pdu:** Get the pdu from pdu memory pool.

### File: md5.h/c

The two files are used to privide and wrap the md5 related functions which is
used for iSCSI.

Exported APIs:

**spdk_md5init:** Wrap MD5_Init function.

**spdk_md5final:** Wrap MD5_Final function.

**spdk_md5update:** Wrap MD5_Update function.

### File: param.h/c

The two files define some structs and functions which are used for the parameter negoitation between
iSCSI initiator and target.

**enum iscsi_param_type:** Define the type of parameter. For example, if the type is ISPT_NUMERICAL_MIN,
it means that we should select the mini value between the value from initiator and target. For more
details, you can refer the related section in iSCSI protocol 3720. struct iscsi_param: Define the
info of a iSCSI param.

Exported APIs:

**spdk_iscsi_param_free:** Free the iSCSI param. If it is a param list, will free all the param list.

**spdk_iscsi_param_find:** Search the param list. And return the param with the key value.

**spdk_iscsi_param_del:** Delete the param with the key value in the provided param list.

**spdk_iscsi_param_add:** Add a new param into the param list. If there is already a param with the key,
the function will delete it first. Then it will add the param into the end of param list.

**spdk_iscsi_param_set:** Set the new value with char type of a param with the key. It will search the
param list with the key to find the param, then it will set the value of the finding param.

**spdk_iscsi_param_set_int:** Set the new value with int type of a param with the key. It will search the
param list with the key to find the param, then it will set the value of the finding param.

**spdk_iscsi_parse_params:** parse the the param with a string like "KEY=VAL<NUL>KEY=VAL<NUL>..." and
update the provided params.

**spdk_iscsi_param_get_val:** Get the value of the provided key. It searchs the param list and find the
param with the key, then it return the related value.

**spdk_iscsi_param_eq_val:** Get the value by calling spdk_iscsi_param_get_val, and compare the return
value with the input value. This function return 1, if the two values are equal.

### File: portal_grp.h/c

The two files are used to manange the portal group of iSCSI target.

**struct spdk_iscsi_portal:** Define the iSCSI portal info. It contains the host,
port, sock, cpu mask related info. Also it indicates the portal group it belongs
to. struct spdk_iscsi_portal_grp: This struct defines eht portal group.
It contains several portals defined by struct spdk_iscsi_portal.

Exported APIs:

**spdk_iscsi_portal_create:** Create a iscsi_portal according to the parameters, and it will be added
into global portal list (i.e., g_spdk_iscsi.portal_head) if it is successfully created.

**spdk_iscsi_portal_destroy:** Destroy a portal group and it will be remove from the global portal list.

**spdk_iscsi_portal_grp_create:** Create a portal group with tag. A tag is a unique identifier for a portal
group. Duplicated tag is not allowed.

**spdk_iscsi_portal_grp_create_from_portal_list:** Create a portal group from a set of portals.

**spdk_iscsi_portal_grp_release:** Release a portal group.

**spdk_iscsi_portal_grp_array_create:** Create portal group from various PortalGroup sections in
configuration file.

**spdk_iscsi_portal_grp_array_destroy:** Destroy all the portal groups.

**spdk_iscsi_portal_grp_find_by_tag:** Find a portal group with a unique tag.

**spdk_iscsi_portal_grp_open_all:** For each portal group, open each portal in the portal group.
Opening the portal means that listening the socket pointed by host and port, then register the
spdk_iscsi_portal_accept poller.

**spdk_iscsi_portal_grp_close_all:** For each portal group, close each portal in the portal group.
Closing the portal means that registering the spdk_iscsi_accept_stop poller and close the opened socket.

### File: task.h/c

The two files define the iSCSI task for ISCSI connections, which defines the struct of iSCSI tasks,
and related operations on the iSCSI task.

**struct spdk_iscsi_task:** Define spdk ISCSI task, it contains pointers to SCSI task, iSCSI connection,
iSCSI task. It also contains the related fields for ISCSI R2T, DATAIN related operations. Also an
iSCSI task may be a primary task or a subtask. For primary ISCSI task, it may have a list of subtasks.

Exported APIs:

**spdk_iscsi_task_put:** Free an iSCSI task. It calls scsi task put function.

**spdk_iscsi_task_get_pdu:** Get the pdu associated with this task.

**spdk_iscsi_task_set_pdu:** Associate a pdu to this task.

**spdk_iscsi_task_get_bhs:** Get iSCSI basic header from this task.

**spdk_iscsi_task_associate_pdu:** Associate a pdu to this task, and also increases the reference of this pdu.

**spdk_iscsi_task_disassociate_pdu:** Disassociate the pdu from a task. It will call spdk_put_pdu function
to free the pdu associated to the task, and clear the pdu pointer to this task.

**spdk_iscsi_task_is_immediate:** Check whether the task is an immediate task according to the basic
header from this task.

**spdk_iscsi_task_is_read:** Check whether it is a read task by the basic ISCSI head info from this task.

**spdk_iscsi_task_get_cmdsn:** Get the iSCSI command serial number from the pdu associated to the task.

**spdk_iscsi_task_get:** Create a subtask according to the parent iSCSI task with the SCSI task
completion call back function.

**spdk_iscsi_task_from_scsi_task:** Get ISCSI task from a SCSI task pointer.

**spdk_iscsi_task_get_primary:** Get the primary ISCSI task of a given ISCSI task. If the task does not
have a parent, the function returns itself, otherwise it returns the parent task.

### File: tgt_node.h/c

The two files manage the iSCSI target node, which involes several operations on the target node with
iSCSI portal groups and initiator groups.

**struct spdk_iscsi_tgt_node:** Define the ISCSI target node info, which includes the name, the
authentication methods for accessing the target node, also points to instance of spdk_scsi_dev,
several portal group to initiator group mapping info (i.e., spdk_iscsi_pg_map). In detail,
spdk_scsi_dev contains several luns associated to spdk bdev instances.

**struct spdk_iscsi_pg_map:** Define the portal group mapping to a initiator group list. Gnerally, it is
a 1 to N mapping.

**struct spdk_iscsi_ig_map:** Contains the iSCSI initiator group info.

Exported APIs:

**spdk_iscsi_init_tgt_nodes:** Parse the various TargetNode sections in
configuration file, and compose available target node list.

**spdk_iscsi_shutdown_tgt_nodes:** Destroy all target nodes.

**spdk_iscsi_shutdown_tgt_node_by_name:** Shutdown the target node by name. spdk_iscsi_send_tgts: Find
the target by provided tiqn, and check whether the iSCSI intiator with info iaddr can be access to
this target node. If it can, write the target related info into the provided buffer.

**spdk_iscsi_tgt_node_construct:** Construct a target node with the provided info.

**spdk_iscsi_tgt_node_add_pg_ig_maps:** Create portal group to initiator group mapping info.

**spdk_iscsi_tgt_node_delete_pg_ig_maps:** Delete the portal group to initiator group mapping info.

**spdk_iscsi_tgt_node_access:** Check whether the conneciton which contains the initiator info can
access the target node. If can, it return true, otherwise it returns false.

**spdk_iscsi_find_tgt_node:** Find the target node with a target name.

**spdk_iscsi_tgt_node_reset:** Reset the lun owned by the target.

**spdk_iscsi_tgt_node_cleanup_luns:** Reset all the luns owned by the target by creating reset
management tasks from the conn.

**spdk_iscsi_tgt_node_delete_map:** Delete the pg-ig map from the provided portal group info and
initiator group info.
