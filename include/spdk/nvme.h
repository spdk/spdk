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

/** \file
 * NVMe driver public API
 */

#ifndef SPDK_NVME_H
#define SPDK_NVME_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_spec.h"

#define SPDK_NVME_DEFAULT_RETRY_COUNT	(4)
extern int32_t		spdk_nvme_retry_count;



/**
 * Opaque handle to a controller. Returned by spdk_nvme_probe()'s attach_cb.
 */
struct spdk_nvme_ctrlr;

/**
 * NVMe controller initialization options.
 *
 * A pointer to this structure will be provided for each probe callback from spdk_nvme_probe() to
 * allow the user to request non-default options, and the actual options enabled on the controller
 * will be provided during the attach callback.
 */
struct spdk_nvme_ctrlr_opts {
	/**
	 * Number of I/O queues to request (used to set Number of Queues feature)
	 */
	uint32_t num_io_queues;

	/**
	 * Enable submission queue in controller memory buffer
	 */
	bool use_cmb_sqs;

	/**
	 * Type of arbitration mechanism
	 */
	enum spdk_nvme_cc_ams arb_mechanism;

	/**
	 * Keep alive timeout in milliseconds (0 = disabled).
	 *
	 * The NVMe library will set the Keep Alive Timer feature to this value and automatically
	 * send Keep Alive commands as needed.  The library user must call
	 * spdk_nvme_ctrlr_process_admin_completions() periodically to ensure Keep Alive commands
	 * are sent.
	 */
	uint32_t keep_alive_timeout_ms;

	/**
	 * Specify the retry number when there is issue with the transport
	 */
	int transport_retry_count;

	/**
	 * The queue depth of each NVMe I/O queue.
	 */
	uint32_t io_queue_size;

	/**
	 * The host NQN to use when connecting to NVMe over Fabrics controllers.
	 *
	 * Unused for local PCIe-attached NVMe devices.
	 */
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];

	/**
	 * The number of requests to allocate for each NVMe I/O queue.
	 *
	 * This should be at least as large as io_queue_size.
	 *
	 * A single I/O may allocate more than one request, since splitting may be necessary to
	 * conform to the device's maximum transfer size, PRP list compatibility requirements,
	 * or driver-assisted striping.
	 */
	uint32_t io_queue_requests;

	/**
	 * Source address for NVMe-oF connections.
	 * Set src_addr and src_svcid to empty strings if no source address should be
	 * specified.
	 */
	char src_addr[SPDK_NVMF_TRADDR_MAX_LEN + 1];

	/**
	 * Source service ID (port) for NVMe-oF connections.
	 * Set src_addr and src_svcid to empty strings if no source address should be
	 * specified.
	 */
	char src_svcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1];

	/**
	 * The host identifier to use when connecting to controllers with 64-bit host ID support.
	 *
	 * Set to all zeroes to specify that no host ID should be provided to the controller.
	 */
	uint8_t host_id[8];

	/**
	 * The host identifier to use when connecting to controllers with extended (128-bit) host ID support.
	 *
	 * Set to all zeroes to specify that no host ID should be provided to the controller.
	 */
	uint8_t extended_host_id[16];

	/**
	 * The I/O command set to select.
	 *
	 * If the requested command set is not supported, the controller
	 * initialization process will not proceed. By default, the NVM
	 * command set is used.
	 */
	enum spdk_nvme_cc_css command_set;
};

/**
 * Get the default options for the creation of a specific NVMe controller.
 *
 * \param[out] opts Will be filled with the default option.
 * \param opts_size Must be set to sizeof(struct spdk_nvme_ctrlr_opts).
 */
void spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size);

/**
 * NVMe library transports
 *
 * NOTE: These are mapped directly to the NVMe over Fabrics TRTYPE values, except for PCIe,
 * which is a special case since NVMe over Fabrics does not define a TRTYPE for local PCIe.
 *
 * Currently, this uses 256 for PCIe which is intentionally outside of the 8-bit range of TRTYPE.
 * If the NVMe-oF specification ever defines a PCIe TRTYPE, this should be updated.
 */
enum spdk_nvme_transport_type {
	/**
	 * PCIe Transport (locally attached devices)
	 */
	SPDK_NVME_TRANSPORT_PCIE = 256,

	/**
	 * RDMA Transport (RoCE, iWARP, etc.)
	 */
	SPDK_NVME_TRANSPORT_RDMA = SPDK_NVMF_TRTYPE_RDMA,

	/**
	 * Fibre Channel (FC) Transport
	 */
	SPDK_NVME_TRANSPORT_FC = SPDK_NVMF_TRTYPE_FC,
};

/**
 * NVMe transport identifier.
 *
 * This identifies a unique endpoint on an NVMe fabric.
 *
 * A string representation of a transport ID may be converted to this type using
 * spdk_nvme_transport_id_parse().
 */
struct spdk_nvme_transport_id {
	/**
	 * NVMe transport type.
	 */
	enum spdk_nvme_transport_type trtype;

	/**
	 * Address family of the transport address.
	 *
	 * For PCIe, this value is ignored.
	 */
	enum spdk_nvmf_adrfam adrfam;

	/**
	 * Transport address of the NVMe-oF endpoint. For transports which use IP
	 * addressing (e.g. RDMA), this should be an IP address. For PCIe, this
	 * can either be a zero length string (the whole bus) or a PCI address
	 * in the format DDDD:BB:DD.FF or DDDD.BB.DD.FF. For FC the string is
	 * formatted as: nn-0xWWNN:pn-0xWWPN” where WWNN is the Node_Name of the
	 * target NVMe_Port and WWPN is the N_Port_Name of the target NVMe_Port.
	 */
	char traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];

	/**
	 * Transport service id of the NVMe-oF endpoint.  For transports which use
	 * IP addressing (e.g. RDMA), this field shoud be the port number. For PCIe,
	 * and FC this is always a zero length string.
	 */
	char trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1];

	/**
	 * Subsystem NQN of the NVMe over Fabrics endpoint. May be a zero length string.
	 */
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
};

/**
 * Parse the string representation of a transport ID.
 *
 * \param trid Output transport ID structure (must be allocated and initialized by caller).
 * \param str Input string representation of a transport ID to parse.
 *
 * str must be a zero-terminated C string containing one or more key:value pairs
 * separated by whitespace.
 *
 * Key          | Value
 * ------------ | -----
 * trtype       | Transport type (e.g. PCIe, RDMA)
 * adrfam       | Address family (e.g. IPv4, IPv6)
 * traddr       | Transport address (e.g. 0000:04:00.0 for PCIe, 192.168.100.8 for RDMA, or WWN for FC)
 * trsvcid      | Transport service identifier (e.g. 4420)
 * subnqn       | Subsystem NQN
 *
 * Unspecified fields of trid are left unmodified, so the caller must initialize
 * trid (for example, memset() to 0) before calling this function.
 *
 * \return 0 if parsing was successful and trid is filled out, or negated errno
 * values on failure.
 */
int spdk_nvme_transport_id_parse(struct spdk_nvme_transport_id *trid, const char *str);

/**
 * Parse the string representation of a transport ID tranport type.
 *
 * \param trtype Output transport type (allocated by caller).
 * \param str Input string representation of transport type (e.g. "PCIe", "RDMA").
 *
 * \return 0 if parsing was successful and trtype is filled out, or negated errno
 * values on failure.
 */
int spdk_nvme_transport_id_parse_trtype(enum spdk_nvme_transport_type *trtype, const char *str);

/**
 * Look up the string representation of a transport ID transport type.
 *
 * \param trtype Transport type to convert.
 *
 * \return static string constant describing trtype, or NULL if trtype not found.
 */
const char *spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype);

/**
 * Look up the string representation of a transport ID address family.
 *
 * \param adrfam Address family to convert.
 *
 * \return static string constant describing adrfam, or NULL if adrmfam not found.
 */
const char *spdk_nvme_transport_id_adrfam_str(enum spdk_nvmf_adrfam adrfam);

/**
 * Parse the string representation of a tranport ID address family.
 *
 * \param adrfam Output address family (allocated by caller).
 * \param str Input string representation of address family (e.g. "IPv4", "IPv6").
 *
 * \return 0 if parsing was successful and adrfam is filled out, or negated errno
 * values on failure.
 */
int spdk_nvme_transport_id_parse_adrfam(enum spdk_nvmf_adrfam *adrfam, const char *str);

/**
 * Compare two transport IDs.
 *
 * The result of this function may be used to sort transport IDs in a consistent
 * order; however, the comparison result is not guaranteed to be consistent across
 * library versions.
 *
 * This function uses a case-insensitive comparison for string fields, but it does
 * not otherwise normalize the transport ID. It is the caller's responsibility to
 * provide the transport IDs in a consistent format.
 *
 * \param trid1 First transport ID to compare.
 * \param trid2 Second transport ID to compare.
 *
 * \return 0 if trid1 == trid2, less than 0 if trid1 < trid2, greater than 0 if
 * trid1 > trid2.
 */
int spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
				   const struct spdk_nvme_transport_id *trid2);

/**
 * Determine whether the NVMe library can handle a specific NVMe over Fabrics
 * transport type.
 *
 * \param trtype NVMe over Fabrics transport type to check.
 *
 * \return true if trtype is supported or false if it is not supported.
 */
bool spdk_nvme_transport_available(enum spdk_nvme_transport_type trtype);

/**
 * Callback for spdk_nvme_probe() enumeration.
 *
 * \param cb_ctx Opaque value passed to spdk_nvme_probe().
 * \param trid NVMe transport identifier.
 * \param opts NVMe controller initialization options. This structure will be
 * populated with the default values on entry, and the user callback may update
 * any options to request a different value. The controller may not support all
 * requested parameters, so the final values will be provided during the attach
 * callback.
 *
 * \return true to attach to this device.
 */
typedef bool (*spdk_nvme_probe_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
				   struct spdk_nvme_ctrlr_opts *opts);

/**
 * Callback for spdk_nvme_attach() to report a device that has been attached to
 * the userspace NVMe driver.
 *
 * \param cb_ctx Opaque value passed to spdk_nvme_attach_cb().
 * \param trid NVMe transport identifier.
 * \param ctrlr Opaque handle to NVMe controller.
 * \param opts NVMe controller initialization options that were actually used.
 * Options may differ from the requested options from the attach call depending
 * on what the controller supports.
 */
typedef void (*spdk_nvme_attach_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
				    struct spdk_nvme_ctrlr *ctrlr,
				    const struct spdk_nvme_ctrlr_opts *opts);

/**
 * Callback for spdk_nvme_remove() to report that a device attached to the userspace
 * NVMe driver has been removed from the system.
 *
 * The controller will remain in a failed state (any new I/O submitted will fail).
 *
 * The controller must be detached from the userspace driver by calling spdk_nvme_detach()
 * once the controller is no longer in use. It is up to the library user to ensure
 * that no other threads are using the controller before calling spdk_nvme_detach().
 *
 * \param cb_ctx Opaque value passed to spdk_nvme_remove_cb().
 * \param ctrlr NVMe controller instance that was removed.
 */
typedef void (*spdk_nvme_remove_cb)(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr);

/**
 * Enumerate the bus indicated by the transport ID and attach the userspace NVMe
 * driver to each device found if desired.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively using any NVMe devices.
 *
 * If called from a secondary process, only devices that have been attached to
 * the userspace driver in the primary process will be probed.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK NVMe driver will be reported.
 *
 * To stop using the the controller and release its associated resources,
 * call spdk_nvme_detach() with the spdk_nvme_ctrlr instance from the attach_cb()
 * function.
 *
 * \param trid The transport ID indicating which bus to enumerate. If the trtype
 * is PCIe or trid is NULL, this will scan the local PCIe bus. If the trtype is
 * RDMA, the traddr and trsvcid must point at the location of an NVMe-oF discovery
 * service.
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per NVMe device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once that NVMe controller has been attached to the userspace driver.
 * \param remove_cb will be called for devices that were attached in a previous
 * spdk_nvme_probe() call but are no longer attached to the system. Optional;
 * specify NULL if removal notices are not desired.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvme_probe(const struct spdk_nvme_transport_id *trid,
		    void *cb_ctx,
		    spdk_nvme_probe_cb probe_cb,
		    spdk_nvme_attach_cb attach_cb,
		    spdk_nvme_remove_cb remove_cb);

/**
 * Connect the NVMe driver to the device located at the given transport ID.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively using this NVMe device.
 *
 * If called from a secondary process, only the device that has been attached to
 * the userspace driver in the primary process will be connected.
 *
 * If connecting to multiple controllers, it is suggested to use spdk_nvme_probe()
 * and filter the requested controllers with the probe callback. For PCIe controllers,
 * spdk_nvme_probe() will be more efficient since the controller resets will happen
 * in parallel.
 *
 * To stop using the the controller and release its associated resources, call
 * spdk_nvme_detach() with the spdk_nvme_ctrlr instance returned by this function.
 *
 * \param trid The transport ID indicating which device to connect. If the trtype
 * is PCIe, this will connect the local PCIe bus. If the trtype is RDMA, the traddr
 * and trsvcid must point at the location of an NVMe-oF service.
 * \param opts NVMe controller initialization options. Default values will be used
 * if the user does not specify the options. The controller may not support all
 * requested parameters.
 * \param opts_size Must be set to sizeof(struct spdk_nvme_ctrlr_opts), or 0 if
 * opts is NULL.
 *
 * \return pointer to the connected NVMe controller or NULL if there is any failure.
 *
 */
struct spdk_nvme_ctrlr *spdk_nvme_connect(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size);

/**
 * Detach specified device returned by spdk_nvme_probe()'s attach_cb from the
 * NVMe driver.
 *
 * On success, the spdk_nvme_ctrlr handle is no longer valid.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Perform a full hardware reset of the NVMe controller.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * Any pointers returned from spdk_nvme_ctrlr_get_ns() and spdk_nvme_ns_get_data()
 * may be invalidated by calling this function. The number of namespaces as returned
 * by spdk_nvme_ctrlr_get_num_ns() may also change.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the identify controller data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return pointer to the identify controller data.
 */
const struct spdk_nvme_ctrlr_data *spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller CSTS (Status) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller CSTS (Status) register.
 */
union spdk_nvme_csts_register spdk_nvme_ctrlr_get_regs_csts(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller CAP (Capabilities) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller CAP (Capabilities) register.
 */
union spdk_nvme_cap_register spdk_nvme_ctrlr_get_regs_cap(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the NVMe controller VS (Version) register.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the NVMe controller VS (Version) register.
 */
union spdk_nvme_vs_register spdk_nvme_ctrlr_get_regs_vs(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the number of namespaces for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the
 * controller is attached to the SPDK NVMe driver.
 *
 * This is equivalent to calling spdk_nvme_ctrlr_get_data() to get the
 * spdk_nvme_ctrlr_data and then reading the nn field.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the number of namespaces.
 */
uint32_t spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the PCI device of a given NVMe controller.
 *
 * This only works for local (PCIe-attached) NVMe controllers; other transports
 * will return NULL.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return PCI device of the NVMe controller, or NULL if not available.
 */
struct spdk_pci_device *spdk_nvme_ctrlr_get_pci_device(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get the maximum data transfer size of a given NVMe controller.
 *
 * \return Maximum data transfer size of the NVMe controller in bytes.
 *
 * The I/O command helper functions, such as spdk_nvme_ns_cmd_read(), will split
 * large I/Os automatically; however, it is up to the user to obey this limit for
 * commands submitted with the raw command functions, such as spdk_nvme_ctrlr_cmd_io_raw().
 */
uint32_t spdk_nvme_ctrlr_get_max_xfer_size(const struct spdk_nvme_ctrlr *ctrlr);

/**
 * Check whether the nsid is an active nv for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param nsid Namespace id.
 *
 * \return true if nsid is an active ns, or false otherwise.
 */
bool spdk_nvme_ctrlr_is_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid);

/**
 * Get the nsid of the first active namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return the nsid of the first active namespace, 0 if there are no active namespaces.
 */
uint32_t spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr);

/**
 * Get next active namespace given the previous nsid.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param prev_nsid Namespace id.
 *
 * \return a next active namespace given the previous nsid, 0 when there are no
 * more active namespaces.
 */
uint32_t spdk_nvme_ctrlr_get_next_active_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t prev_nsid);

/**
 * Determine if a particular log page is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_log_page().
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param log_page Log page to query.
 *
 * \return true if supported, or false otherwise.
 */
bool spdk_nvme_ctrlr_is_log_page_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page);

/**
 * Determine if a particular feature is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature().
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param feature_code Feature to query.
 *
 * \return true if supported, or false otherwise.
 */
bool spdk_nvme_ctrlr_is_feature_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature_code);

/**
 * Signature for callback function invoked when a command is completed.
 *
 * \param spdk_nvme_cpl Completion queue entry that coontains the completion status.
 */
typedef void (*spdk_nvme_cmd_cb)(void *, const struct spdk_nvme_cpl *);

/**
 * Signature for callback function invoked when an asynchronous error request
 * command is completed.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param aer_cb_arg Context specified by spdk_nvme_register_aer_callback().
 * \param spdk_nvme_cpl Completion queue entry that contains the completion status
 * of the asynchronous event request that was completed.
 */
typedef void (*spdk_nvme_aer_cb)(void *aer_cb_arg,
				 const struct spdk_nvme_cpl *);

/**
 * Register callback function invoked when an AER command is completed for the
 * given NVMe controller.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param aer_cb_fn Callback function invoked when an asynchronous error request
 * command is completed.
 * \param aer_cb_arg Argument passed to callback function.
 */
void spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_aer_cb aer_cb_fn,
		void *aer_cb_arg);

/**
 * Opaque handle to a queue pair.
 *
 * I/O queue pairs may be allocated using spdk_nvme_ctrlr_alloc_io_qpair().
 */
struct spdk_nvme_qpair;

/**
 * Signature for the callback function invoked when a timeout is detected on a
 * request.
 *
 * For timeouts detected on the admin queue pair, the qpair returned here will
 * be NULL.  If the controller has a serious error condition and is unable to
 * communicate with driver via completion queue, the controller can set Controller
 * Fatal Status field to 1, then reset is required to recover from such error.
 * Users may detect Controller Fatal Status when timeout happens.
 *
 * \param cb_arg Argument passed to callback funciton.
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair Opaque handle to a queue pair.
 * \param cid Command ID.
 */
typedef void (*spdk_nvme_timeout_cb)(void *cb_arg,
				     struct spdk_nvme_ctrlr *ctrlr,
				     struct spdk_nvme_qpair *qpair,
				     uint16_t cid);

/**
 * Register for timeout callback on a controller.
 *
 * The application can choose to register for timeout callback or not register
 * for timeout callback.
 *
 * \param ctrlr NVMe controller on which to monitor for timeout.
 * \param timeout_us Timeout value in microseconds.
 * \param cb_fn A function pointer that points to the callback function.
 * \param cb_arg Argument to the callback function.
 */
void spdk_nvme_ctrlr_register_timeout_callback(struct spdk_nvme_ctrlr *ctrlr,
		uint64_t timeout_us, spdk_nvme_timeout_cb cb_fn, void *cb_arg);

/**
 * NVMe I/O queue pair initialization options.
 *
 * These options may be passed to spdk_nvme_ctrlr_alloc_io_qpair() to configure queue pair
 * options at queue creation time.
 *
 * The user may retrieve the default I/O queue pair creation options for a controller using
 * spdk_nvme_ctrlr_get_default_io_qpair_opts().
 */
struct spdk_nvme_io_qpair_opts {
	/**
	 * Queue priority for weighted round robin arbitration.  If a different arbitration
	 * method is in use, pass 0.
	 */
	enum spdk_nvme_qprio qprio;

	/**
	 * The queue depth of this NVMe I/O queue. Overrides spdk_nvme_ctrlr_opts::io_queue_size.
	 */
	uint32_t io_queue_size;

	/**
	 * The number of requests to allocate for this NVMe I/O queue.
	 *
	 * Overrides spdk_nvme_ctrlr_opts::io_queue_requests.
	 *
	 * This should be at least as large as io_queue_size.
	 *
	 * A single I/O may allocate more than one request, since splitting may be
	 * necessary to conform to the device's maximum transfer size, PRP list
	 * compatibility requirements, or driver-assisted striping.
	 */
	uint32_t io_queue_requests;
};

/**
 * Get the default options for I/O qpair creation for a specific NVMe controller.
 *
 * \param ctrlr NVMe controller to retrieve the defaults from.
 * \param[out] opts Will be filled with the default options for
 * spdk_nvme_ctrlr_alloc_io_qpair().
 * \param opts_size Must be set to sizeof(struct spdk_nvme_io_qpair_opts).
 */
void spdk_nvme_ctrlr_get_default_io_qpair_opts(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size);

/**
 * Allocate an I/O queue pair (submission and completion queue).
 *
 * Each queue pair should only be used from a single thread at a time (mutual
 * exclusion must be enforced by the user).
 *
 * \param ctrlr NVMe controller for which to allocate the I/O queue pair.
 * \param opts I/O qpair creation options, or NULL to use the defaults as returned
 * by spdk_nvme_ctrlr_alloc_io_qpair().
 * \param opts_size Must be set to sizeof(struct spdk_nvme_io_qpair_opts), or 0
 * if opts is NULL.
 *
 * \return a pointer to the allocated I/O queue pair.
 */
struct spdk_nvme_qpair *spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size);

/**
 * Free an I/O queue pair that was allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 *
 * \param qpair I/O queue pair to free.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair);

/**
 * Send the given NVM I/O command to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the spdk_nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair I/O qpair to submit command.
 * \param cmd NVM I/O command to submit.
 * \param buf Virtual memory address of a single physically contiguous buffer.
 * \param len Size of buffer.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success, negated errno on failure.
 */
int spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			       struct spdk_nvme_qpair *qpair,
			       struct spdk_nvme_cmd *cmd,
			       void *buf, uint32_t len,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Send the given NVM I/O command with metadata to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the spdk_nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param qpair I/O qpair to submit command.
 * \param cmd NVM I/O command to submit.
 * \param buf Virtual memory address of a single physically contiguous buffer.
 * \param len Size of buffer.
 * \param md_buf Virtual memory address of a single physically contiguous metadata
 * buffer.
 * \param cb_fn Callback function invoked when the I/O command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success, negated errno on failure.
 */
int spdk_nvme_ctrlr_cmd_io_raw_with_md(struct spdk_nvme_ctrlr *ctrlr,
				       struct spdk_nvme_qpair *qpair,
				       struct spdk_nvme_cmd *cmd,
				       void *buf, uint32_t len, void *md_buf,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Process any outstanding completions for I/O submitted on a queue pair.
 *
 * This call is non-blocking, i.e. it only processes completions that are ready
 * at the time of this function call. It does not wait for outstanding commands
 * to finish.
 *
 * For each completed command, the request's callback function will be called if
 * specified as non-NULL when the request was submitted.
 *
 * The caller must ensure that each queue pair is only used from one thread at a
 * time.
 *
 * This function may be called at any point while the controller is attached to
 * the SPDK NVMe driver.
 *
 * \sa spdk_nvme_cmd_cb
 *
 * \param qpair Queue pair to check for completions.
 * \param max_completions Limit the number of completions to be processed in one
 * call, or 0 for unlimited.
 *
 * \return number of completions processed (may be 0) or negated on error.
 */
int32_t spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);

/**
 * Send the given admin command to the NVMe controller.
 *
 * This is a low level interface for submitting admin commands directly. Prefer
 * the spdk_nvme_ctrlr_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param cmd NVM admin command to submit.
 * \param buf Virtual memory address of a single physically contiguous buffer.
 * \param len Size of buffer.
 * \param cb_fn Callback function invoked when the admin command completes.
 * \param cb_arg Argument passed to callback function.
 *
 * \return 0 on success, negated errno on failure.
 */
int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_cmd *cmd,
				  void *buf, uint32_t len,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Process any outstanding completions for admin commands.
 *
 * This will process completions for admin commands submitted on any thread.
 *
 * This call is non-blocking, i.e. it only processes completions that are ready
 * at the time of this function call. It does not wait for outstanding commands
 * to finish.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return number of completions processed (may be 0) or negated on error.
 */
int32_t spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr);


/**
 * Opaque handle to a namespace. Obtained by calling spdk_nvme_ctrlr_get_ns().
 */
struct spdk_nvme_ns;

/**
 * Get a handle to a namespace for the given controller.
 *
 * Namespaces are numbered from 1 to the total number of namespaces. There will
 * never be any gaps in the numbering. The number of namespaces is obtained by
 * calling spdk_nvme_ctrlr_get_num_ns().
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param ns_id Namespace id.
 *
 * \return a pointer to the namespace.
 */
struct spdk_nvme_ns *spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t ns_id);

/**
 * Get a specific log page from the NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_is_log_page_supported()
 *
 * \param ctrlr Opaque handle to NVMe controller.
 * \param log_page The log page identifier.
 * \param nsid Depending on the log page, this may be 0, a namespace identifier,
 * or SPDK_NVME_GLOBAL_NS_TAG.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param offset Offset in bytes within the log page to start retrieving log page
 * data. May only be non-zero if the controller supports extended data for Get Log
 * Page as reported in the controller data log page attributes.
 * \param cb_fn Callback function to invoke when the log page has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request.
 */
int spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr,
				     uint8_t log_page, uint32_t nsid,
				     void *payload, uint32_t payload_size,
				     uint64_t offset,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Abort a specific previously-submitted NVMe command.
 *
 * \sa spdk_nvme_ctrlr_register_timeout_callback()
 *
 * \param ctrlr NVMe controller to which the command was submitted.
 * \param qpair NVMe queue pair to which the command was submitted. For admin
 *  commands, pass NULL for the qpair.
 * \param cid Command ID of the command to abort.
 * \param cb_fn Callback function to invoke when the abort has completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno value otherwise.
 */
int spdk_nvme_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_qpair *qpair,
			      uint16_t cid,
			      spdk_nvme_cmd_cb cb_fn,
			      void *cb_arg);

/**
 * Set specific feature for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature().
 *
 * \param ctrlr NVMe controller to manipulate.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param cdw12 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been set.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request.
 */
int spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11, uint32_t cdw12,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get specific feature from given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_set_feature()
 *
 * \param ctrlr NVMe controller to query.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request.
 */
int spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Get specific feature from given NVMe controller.
 *
 * \param ctrlr NVMe controller to query.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 * \param ns_id The namespace identifier.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_set_feature_ns()
 */
int spdk_nvme_ctrlr_cmd_get_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				       uint32_t cdw11, void *payload, uint32_t payload_size,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t ns_id);

/**
 * Set specific feature for the given NVMe controller and namespace ID.
 *
 * \param ctrlr NVMe controller to manipulate.
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param cdw12 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been set.
 * \param cb_arg Argument to pass to the callback function.
 * \param ns_id The namespace identifier.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature_ns()
 */
int spdk_nvme_ctrlr_cmd_set_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				       uint32_t cdw11, uint32_t cdw12, void *payload,
				       uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				       void *cb_arg, uint32_t ns_id);

/**
 * Receive security protocol data from controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \param ctrlr NVMe controller to use for security receive command submission.
 * \param secp Security Protocol that is used.
 * \param spsp Security Protocol Specific field.
 * \param nssf NVMe Security Specific field. Indicate RPMB target when using Security
 * Protocol EAh.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the security receive has completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
int spdk_nvme_ctrlr_cmd_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp, uint16_t spsp,
		uint8_t nssf, void *payload, uint32_t payload_size,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Send security protocol data to controller.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \param ctrlr NVMe controller to use for security send command submission.
 * \param secp Security Protocol that is used.
 * \param spsp Security Protocol Specific field.
 * \param nssf NVMe Security Specific field. Indicate RPMB target when using Security
 * Protocol EAh.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the security send has completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be allocated
 * for this request.
 */
int spdk_nvme_ctrlr_cmd_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp, uint16_t spsp,
				      uint8_t nssf, void *payload, uint32_t payload_size,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Attach the specified namespace to controllers.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \param ctrlr NVMe controller to use for command submission.
 * \param nsid Namespace identifier for namespace to attach.
 * \param payload The pointer to the controller list.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request.
 */
int spdk_nvme_ctrlr_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			      struct spdk_nvme_ctrlr_list *payload);

/**
 * Detach the specified namespace from controllers.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \param ctrlr NVMe controller to use for command submission.
 * \param nsid Namespace ID to detach.
 * \param payload The pointer to the controller list.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request
 */
int spdk_nvme_ctrlr_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			      struct spdk_nvme_ctrlr_list *payload);

/**
 * Create a namespace.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to create namespace on.
 * \param payload The pointer to the NVMe namespace data.
 *
 * \return Namespace ID (>= 1) if successfully created, or 0 if the request failed.
 */
uint32_t spdk_nvme_ctrlr_create_ns(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_ns_data *payload);

/**
 * Delete a namespace.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * Call spdk_nvme_ctrlr_process_admin_completions() to poll for completion of
 * commands submitted through this function.
 *
 * \param ctrlr NVMe controller to delete namespace from.
 * \param nsid The namespace identifier.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated
 * for this request
 */
int spdk_nvme_ctrlr_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid);

/**
 * Format NVM.
 *
 * This function requests a low-level format of the media.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to format.
 * \param nsid The namespace identifier. May be SPDK_NVME_GLOBAL_NS_TAG to format
 * all namespaces.
 * \param format The format information for the command.
 *
 * \return 0 if successfully submitted, negated errno if resources could not be
 * allocated for this request
 */
int spdk_nvme_ctrlr_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			   struct spdk_nvme_format *format);

/**
 * Download a new firmware image.
 *
 * This function is thread safe and can be called at any point after spdk_nvme_probe().
 *
 * \param ctrlr NVMe controller to perform firmware operation on.
 * \param payload The data buffer for the firmware image.
 * \param size The data size will be downloaded.
 * \param slot The slot that the firmware image will be committed to.
 * \param commit_action The action to perform when firmware is committed.
 * \param completion_status output parameter. Contains the completion status of
 * the firmware commit operation.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated
 * for this request, -1 if the size is not multiple of 4.
 */
int spdk_nvme_ctrlr_update_firmware(struct spdk_nvme_ctrlr *ctrlr, void *payload, uint32_t size,
				    int slot, enum spdk_nvme_fw_commit_action commit_action,
				    struct spdk_nvme_status *completion_status);

/**
 * Allocate an I/O buffer from the controller memory buffer (Experimental).
 *
 * This function allocates registered memory which belongs to the Controller
 * Memory Buffer (CMB) of the specified NVMe controller. Note that the CMB has
 * to support the WDS and RDS capabilities for the allocation to be successful.
 * Also, due to vtophys contraints the CMB must be at least 4MiB in size. Free
 * memory allocated with this function using spdk_nvme_ctrlr_free_cmb_io_buffer().
 *
 * \param ctrlr Controller from which to allocate memory buffer.
 * \param size Size of buffer to allocate in bytes.
 *
 * \return Pointer to controller memory buffer allocation, or NULL if allocation
 * was not possible.
 */
void *spdk_nvme_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size);

/**
 * Free a controller memory I/O buffer (Experimental).
 *
 * Note this function is currently a NOP which is one reason why this and
 * spdk_nvme_ctrlr_alloc_cmb_io_buffer() are currently marked as experimental.
 *
 * \param ctrlr Controller from which the buffer was allocated.
 * \param buf Buffer previously allocated by spdk_nvme_ctrlr_alloc_cmb_io_buffer().
 * \param size Size of buf in bytes.
 */
void spdk_nvme_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size);

/**
 * Get the identify namespace data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the namespace data.
 */
const struct spdk_nvme_ns_data *spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns);

/**
 * Get the namespace id (index number) from the given namespace handle.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return namespace id.
 */
uint32_t spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns);

/**
 * Get the controller with which this namespace is associated.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace.
 *
 * \return a pointer to the controller.
 */
struct spdk_nvme_ctrlr *spdk_nvme_ns_get_ctrlr(struct spdk_nvme_ns *ns);

/**
 * Determine whether a namespace is active.
 *
 * Inactive namespaces cannot be the target of I/O commands.
 *
 * \param ns Namespace to query.
 *
 * \return true if active, or false if inactive.
 */
bool spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns);

/**
 * Get the maximum transfer size, in bytes, for an I/O sent to the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the maximum transfer size in bytes.
 */
uint32_t spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns);

/**
 * Get the sector size, in bytes, of the given namespace.
 *
 * This function returns the size of the data sector only.  It does not
 * include metadata size.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * /return the sector size in bytes.
 */
uint32_t spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns);

/**
 * Get the extended sector size, in bytes, of the given namespace.
 *
 * This function returns the size of the data sector plus metadata.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * /return the extended sector size in bytes.
 */
uint32_t spdk_nvme_ns_get_extended_sector_size(struct spdk_nvme_ns *ns);

/**
 * Get the number of sectors for the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the number of sectors.
 */
uint64_t spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns);

/**
 * Get the size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the size of the given namespace in bytes.
 */
uint64_t spdk_nvme_ns_get_size(struct spdk_nvme_ns *ns);

/**
 * Get the end-to-end data protection information type of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the end-to-end data protection information type.
 */
enum spdk_nvme_pi_type spdk_nvme_ns_get_pi_type(struct spdk_nvme_ns *ns);

/**
 * Get the metadata size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the metadata size of the given namespace in bytes.
 */
uint32_t spdk_nvme_ns_get_md_size(struct spdk_nvme_ns *ns);

/**
 * Check whether if the namespace can support extended LBA when end-to-end data
 * protection enabled.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return true if the namespace can support extended LBA when end-to-end data
 * protection enabled, or false otherwise.
 */
bool spdk_nvme_ns_supports_extended_lba(struct spdk_nvme_ns *ns);

/**
 * Determine the value returned when reading deallocated blocks.
 *
 * If deallocated blocks return 0, the deallocate command can be used as a more
 * efficient alternative to the write_zeroes command, especially for large requests.
 *
 * \param ns Namespace.
 *
 * \return the logical block read value.
 */
enum spdk_nvme_dealloc_logical_block_read_value spdk_nvme_ns_get_dealloc_logical_block_read_value(
	struct spdk_nvme_ns *ns);

/**
 * Get the optimal I/O boundary, in blocks, for the given namespace.
 *
 * Read and write commands should not cross the optimal I/O boundary for best
 * performance.
 *
 * \param ns Namespace to query.
 *
 * \return Optimal granularity of I/O commands, in blocks, or 0 if no optimal
 * granularity is reported.
 */
uint32_t spdk_nvme_ns_get_optimal_io_boundary(struct spdk_nvme_ns *ns);

/**
 * Get the UUID for the given namespace.
 *
 * \param ns Namespace to query.
 *
 * \return a pointer to namespace UUID, or NULL if ns does not have a UUID.
 */
const struct spdk_uuid *spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns);

/**
 * \brief Namespace command support flags.
 */
enum spdk_nvme_ns_flags {
	SPDK_NVME_NS_DEALLOCATE_SUPPORTED	= 0x1, /**< The deallocate command is supported */
	SPDK_NVME_NS_FLUSH_SUPPORTED		= 0x2, /**< The flush command is supported */
	SPDK_NVME_NS_RESERVATION_SUPPORTED	= 0x4, /**< The reservation command is supported */
	SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED	= 0x8, /**< The write zeroes command is supported */
	SPDK_NVME_NS_DPS_PI_SUPPORTED		= 0x10, /**< The end-to-end data protection is supported */
	SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED	= 0x20, /**< The extended lba format is supported,
							      metadata is transferred as a contiguous
							      part of the logical block that it is associated with */
};

/**
 * Get the flags for the given namespace.
 *
 * See spdk_nvme_ns_flags for the possible flags returned.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ns Namespace to query.
 *
 * \return the flags for the given namespace.
 */
uint32_t spdk_nvme_ns_get_flags(struct spdk_nvme_ns *ns);

/**
 * Restart the SGL walk to the specified offset when the command has scattered payloads.
 *
 * \param cb_arg Argument passed to readv/writev.
 * \param offset Offset for SGL.
 */
typedef void (*spdk_nvme_req_reset_sgl_cb)(void *cb_arg, uint32_t offset);

/**
 * Fill out *address and *length with the current SGL entry and advance to the next
 * entry for the next time the callback is invoked.
 *
 * The described segment must be physically contiguous.
 *
 * \param cb_arg Argument passed to readv/writev.
 * \param address Virtual address of this segment.
 * \param length Length of this physical segment.
 */
typedef int (*spdk_nvme_req_next_sge_cb)(void *cb_arg, void **address, uint32_t *length);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param lba Starting LBA to write the data.
 * \param lba_count Length (in sectors) for the write operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request
 */
int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			   void *cb_arg, uint32_t io_flags);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to write the data.
 * \param lba_count Length (in sectors) for the write operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_writev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    uint64_t lba, uint32_t lba_count,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param qpair I/O queue pair to submit the request
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param apptag_mask application tag mask.
 * \param apptag application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_writev_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				    uint64_t lba, uint32_t lba_count,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				    spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				    uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a write I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param metadata Virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param lba Starting LBA to write the data.
 * \param lba_count Length (in sectors) for the write operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_write_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   void *payload, void *metadata,
				   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				   void *cb_arg, uint32_t io_flags,
				   uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a write zeroes I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the write zeroes I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA for this command.
 * \param lba_count Length (in sectors) for the write zero operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined by the SPDK_NVME_IO_FLAGS_* entries in
 * spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_write_zeroes(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  uint64_t lba, uint32_t lba_count,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				  uint32_t io_flags);

/**
 * \brief Submits a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the read I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param lba Starting LBA to read the data.
 * \param lba_count Length (in sectors) for the read operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg, uint32_t io_flags);

/**
 * Submit a read I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the read I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to read the data.
 * \param lba_count Length (in sectors) for the read operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_readv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			   spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Submit a read I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param qpair I/O queue pair to submit the request
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 * \param metadata virtual address pointer to the metadata payload, the length
 *	           of metadata is specified by spdk_nvme_ns_get_md_size()
 * \param apptag_mask application tag mask.
 * \param apptag application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any given time.
 */
int spdk_nvme_ns_cmd_readv_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				   uint64_t lba, uint32_t lba_count,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
				   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
				   spdk_nvme_req_next_sge_cb next_sge_fn, void *metadata,
				   uint16_t apptag_mask, uint16_t apptag);

/**
 * Submits a read I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param qpair I/O queue pair to submit the request
 * \param payload virtual address pointer to the data payload
 * \param metadata virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param lba starting LBA to read the data.
 * \param lba_count Length (in sectors) for the read operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_read_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				  void *payload, void *metadata,
				  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				  void *cb_arg, uint32_t io_flags,
				  uint16_t apptag_mask, uint16_t apptag);

/**
 * Submit a data set management request to the specified NVMe namespace. Data set
 * management operations are designed to optimize interaction with the block
 * translation layer inside the device. The most common type of operation is
 * deallocate, which is often referred to as TRIM or UNMAP.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * This is a convenience wrapper that will automatically allocate and construct
 * the correct data buffers. Therefore, ranges does not need to be allocated from
 * pinned memory and can be placed on the stack. If a higher performance, zero-copy
 * version of DSM is required, simply build and submit a raw command using
 * spdk_nvme_ctrlr_cmd_io_raw().
 *
 * \param ns NVMe namespace to submit the DSM request
 * \param type A bit field constructed from \ref spdk_nvme_dsm_attribute.
 * \param qpair I/O queue pair to submit the request
 * \param ranges An array of \ref spdk_nvme_dsm_range elements describing the LBAs
 * to operate on.
 * \param num_ranges The number of elements in the ranges array.
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negated POSIX errno values otherwise.
 */
int spdk_nvme_ns_cmd_dataset_management(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
					uint32_t type,
					const struct spdk_nvme_dsm_range *ranges,
					uint16_t num_ranges,
					spdk_nvme_cmd_cb cb_fn,
					void *cb_arg);

/**
 * Submit a flush request to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the flush request.
 * \param qpair I/O queue pair to submit the request.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a reservation register to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation register request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the reservation register data.
 * \param ignore_key '1' the current reservation key check is disabled.
 * \param action Specifies the registration action.
 * \param cptpl Change the Persist Through Power Loss state.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_reservation_register(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_reservation_register_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_register_action action,
		enum spdk_nvme_reservation_register_cptpl cptpl,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submits a reservation release to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation release request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to current reservation key.
 * \param ignore_key '1' the current reservation key check is disabled.
 * \param action Specifies the reservation release action.
 * \param type Reservation type for the namespace.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_reservation_release(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_reservation_key_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_release_action action,
		enum spdk_nvme_reservation_type type,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submits a reservation acquire to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation acquire request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to reservation acquire data.
 * \param ignore_key '1' the current reservation key check is disabled.
 * \param action Specifies the reservation acquire action.
 * \param type Reservation type for the namespace.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_reservation_acquire(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_reservation_acquire_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_acquire_action action,
		enum spdk_nvme_reservation_type type,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a reservation report to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the reservation report request.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer for reservation status data.
 * \param len Length bytes for reservation status data structure.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_reservation_report(struct spdk_nvme_ns *ns,
					struct spdk_nvme_qpair *qpair,
					void *payload, uint32_t len,
					spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * Submit a compare I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the compare I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param lba Starting LBA to compare the data.
 * \param lba_count Length (in sectors) for the compare operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_compare(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			     uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			     void *cb_arg, uint32_t io_flags);

/**
 * Submit a compare I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the compare I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param lba Starting LBA to compare the data.
 * \param lba_count Length (in sectors) for the compare operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param reset_sgl_fn Callback function to reset scattered payload.
 * \param next_sge_fn Callback function to iterate each scattered payload memory
 * segment.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_comparev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint64_t lba, uint32_t lba_count,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			      spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			      spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * Submit a compare I/O to the specified NVMe namespace.
 *
 * The command is submitted to a qpair allocated by spdk_nvme_ctrlr_alloc_io_qpair().
 * The user must ensure that only one thread submits I/O on a given qpair at any
 * given time.
 *
 * \param ns NVMe namespace to submit the compare I/O.
 * \param qpair I/O queue pair to submit the request.
 * \param payload Virtual address pointer to the data payload.
 * \param metadata Virtual address pointer to the metadata payload, the length
 * of metadata is specified by spdk_nvme_ns_get_md_size().
 * \param lba Starting LBA to compare the data.
 * \param lba_count Length (in sectors) for the compare operation.
 * \param cb_fn Callback function to invoke when the I/O is completed.
 * \param cb_arg Argument to pass to the callback function.
 * \param io_flags Set flags, defined in nvme_spec.h, for this I/O.
 * \param apptag_mask Application tag mask.
 * \param apptag Application tag to use end-to-end protection information.
 *
 * \return 0 if successfully submitted, negated errno if an nvme_request structure
 * cannot be allocated for the I/O request.
 */
int spdk_nvme_ns_cmd_compare_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				     void *payload, void *metadata,
				     uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
				     void *cb_arg, uint32_t io_flags,
				     uint16_t apptag_mask, uint16_t apptag);

/**
 * \brief Inject an error for the next request with a given opcode.
 *
 * \param ctrlr NVMe controller.
 * \param qpair I/O queue pair to add the error command,
 *              NULL for Admin queue pair.
 * \param opc Opcode for Admin or I/O commands.
 * \param do_not_submit True if matching requests should not be submitted
 *                      to the controller, but instead completed manually
 *                      after timeout_in_us has expired.  False if matching
 *                      requests should be submitted to the controller and
 *                      have their completion status modified after the
 *                      controller completes the request.
 * \param timeout_in_us Wait specified microseconds when do_not_submit is true.
 * \param err_count Number of matching requests to inject errors.
 * \param sct Status code type.
 * \param sc Status code.
 *
 * \return 0 if successfully enabled, ENOMEM if an error command
 *	     structure cannot be allocated.
 *
 * The function can be called multiple times to inject errors for different
 * commands.  If the opcode matches an existing entry, the existing entry
 * will be updated with the values specified.
 */
int spdk_nvme_qpair_add_cmd_error_injection(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair,
		uint8_t opc,
		bool do_not_submit,
		uint64_t timeout_in_us,
		uint32_t err_count,
		uint8_t sct, uint8_t sc);

/**
 * \brief Clear the specified NVMe command with error status.
 *
 * \param ctrlr NVMe controller.
 * \param qpair I/O queue pair to remove the error command,
 * \            NULL for Admin queue pair.
 * \param opc Opcode for Admin or I/O commands.
 *
 * The function will remove specified command in the error list.
 */
void spdk_nvme_qpair_remove_cmd_error_injection(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair,
		uint8_t opc);


#ifdef __cplusplus
}
#endif

#endif
