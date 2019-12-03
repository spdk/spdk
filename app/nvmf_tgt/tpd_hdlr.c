#include "tpd_hdlr.h"

#include "spdk/bdev.h"

// Must be public
typedef enum _spdk_nvmf_request_exec_status {
	SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE,
	SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS,
} spdk_nvmf_request_exec_status;


static int
filter_bdev_by_product_name_matches(struct spdk_nvmf_request *req, const char *product_name)
{
  struct spdk_nvmf_subsystem *subsys = spdk_nvmf_request_get_subsystem(req);
 
  // Check if this is for namespace containing a bdev
  struct spdk_nvmf_ns *ns = spdk_nvmf_subsystem_get_first_ns(subsys);
  if (ns == NULL) {
    printf("*** filter_bdev_by_product_name: no namespace found for ss - continuing\n");
    return 0;
  }

  struct spdk_bdev *bdev = spdk_nvmf_ns_get_bdev(ns);
  if (bdev == NULL) {
    printf("*** filter_bdev_by_product_name: no bdev found for ns - continuing\n");
    return 0;
  }

  // The strcmp is not performant, but that may not be a problem for our use cases
  // We could cache the module pointer to speed this up if needed
  if (strcmp(spdk_bdev_get_product_name(bdev), product_name) != 0) {
    printf("*** filter_bdev_by_product_name: not a %s device (got: %s)- continuing\n", product_name, spdk_bdev_get_product_name(bdev));
    return 0;
  }

  return 1;
}

static int 
fixup_identify_ctrlr(struct spdk_nvmf_request *req) {
  void *data;
  uint32_t length;
  spdk_nvmf_request_get_data(req, &data, &length);
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
  
  struct spdk_nvme_ctrlr_data nvmf_cdata;
  memset(&nvmf_cdata, 0, sizeof(nvmf_cdata));
  int rc = spdk_nvmf_ctrlr_identify_ctrlr(ctrlr, &nvmf_cdata);
  assert(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
  // fixup
  struct spdk_nvme_ctrlr_data *nvme_cdata = data;
  // PCI Vendor ID (VID)
  nvmf_cdata.vid = nvme_cdata->vid;
  // PCI Subsystem Vendor ID (SSVID)
  nvmf_cdata.ssvid = nvme_cdata->ssvid;
  // Serial Number (SN)
  memcpy(&nvmf_cdata.sn[0], &nvme_cdata->sn[0], sizeof(nvmf_cdata.sn));
  // Model Number (MN)
  memcpy(&nvmf_cdata.mn[0], &nvme_cdata->mn[0], sizeof(nvmf_cdata.mn));
  // Firmware Revision (FR)
  memcpy(&nvmf_cdata.fr[0], &nvme_cdata->fr[0], sizeof(nvmf_cdata.fr));
  // IEEE OUI Identifier (IEEE)
  memcpy(&nvmf_cdata.ieee[0], &nvme_cdata->ieee[0], sizeof(nvmf_cdata.ieee));
  // FRU Globally Unique Identifier (FGUID)
  memcpy(&nvmf_cdata.fguid[0], &nvme_cdata->fguid[0], sizeof(nvmf_cdata.fguid));
  // Optional Admin Command Support (OACS)
  memcpy(&nvmf_cdata.oacs, &nvme_cdata->oacs, sizeof(nvmf_cdata.oacs));
  // Firmware Updates (FRMW)
  nvmf_cdata.frmw = nvme_cdata->frmw;
  // Maximum Time for Firmware Activation (MTFA)
  nvmf_cdata.mtfa = nvme_cdata->mtfa;
  // Firmware Update Granularity (FWUG)
  nvmf_cdata.fwug = nvme_cdata->fwug;
  // Number of Power States Support (NPSS)?
  // Warning Composite Temperature Threshold (WCTEMP)?
  // Critical Composite Temperature Threshold (CCTEMP)?
  // Minimum Thermal Management Temperature (MNTMT)?
  // Maximum Thermal Management Temperature (MXTMT)?
  // Power State 0 Descriptor (PSD0...31)?
  // Optional NVM Command Support (ONCS)
  nvmf_cdata.oncs = nvme_cdata->oncs;
  // Format NVM Attributes (FNA)
  nvmf_cdata.fna = nvme_cdata->fna;

  // replace
  memcpy(data, &nvmf_cdata, length);
  printf("-->\tfixed\n");
			
	return 0;
}

static int 
fixup_identify_ns(struct spdk_nvmf_request *req) {
  void *data;
  uint32_t length;
  spdk_nvmf_request_get_data(req, &data, &length);
	struct spdk_nvmf_ctrlr *ctrlr = spdk_nvmf_request_get_ctrlr(req);
  struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
  struct spdk_nvme_cpl *rsp = spdk_nvmf_request_get_response(req);
  
  struct spdk_nvme_ns_data nvmf_nsdata;
  memset(&nvmf_nsdata, 0, sizeof(nvmf_nsdata));
  int rc = spdk_nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, &nvmf_nsdata);
  assert(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE); // TODO: CHeck for error in response

  // fixup
  struct spdk_nvme_ns_data *nvme_nsdata = data;

  nvmf_nsdata.nlbaf = nvme_nsdata->nlbaf;
  memcpy(&nvmf_nsdata.lbaf[0], nvme_nsdata->lbaf, sizeof(nvmf_nsdata.lbaf));

   // replace
  memcpy(data, &nvmf_nsdata, length);
  printf("-->\tfixed\n");

  return 0;
}

static int
handle_identify(struct spdk_nvmf_request *req)
{
  struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
  struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);

  uint8_t cns = cmd->cdw10 & 0xFF;
	printf("*** fixup: IDENTIFY: cns=%02x\n", cns);
	if (cns != SPDK_NVME_IDENTIFY_CTRLR && cns != SPDK_NVME_IDENTIFY_NS) {
    return -1; // continue
  }

  // We only do a special identify for NVMe disk devices
  if (! filter_bdev_by_product_name_matches(req, "NVMe disk")) {
    return -1; // continue
  }
  
  // Forward to first namespace
  struct spdk_bdev *bdev;
  struct spdk_bdev_desc *desc;
  struct spdk_io_channel *ch;
  int rc = spdk_nvmf_request_get_bdev_info(1, req, &bdev, &desc, &ch);
	if (rc) {
    printf("*** no bdev info found for ss:\n");
   	response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
  }

  return spdk_nvmf_bdev_nvme_passthru_admin(bdev, desc, ch, req, 
      cns == SPDK_NVME_IDENTIFY_CTRLR ? fixup_identify_ctrlr : fixup_identify_ns);
}

static int 
handle_sesd_passthru(struct spdk_nvmf_request *req)
{
  struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
  struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);

  // Null disk has some Sesd handling
  if (! filter_bdev_by_product_name_matches(req, "Null disk")) {
    return -1; // continue
  }

  // Enforce that we have a nsid
  if (cmd->nsid != 1) {
    printf("*** No namespace specified for sesd subsystem\n");
   	response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE; 
  }

  struct spdk_bdev *bdev;
  struct spdk_bdev_desc *desc;
  struct spdk_io_channel *ch;
  int rc = spdk_nvmf_request_get_bdev_info(cmd->nsid, req, &bdev, &desc, &ch);
	if (rc) {
    printf("*** No bdev info found for sesd subsystem\n");
   	response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
  }

  return spdk_nvmf_bdev_nvme_passthru_admin(bdev, desc, ch, req, NULL);
}

static int 
handle_firmware(struct spdk_nvmf_request *req)
{
  struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
  struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);

  // Null disk has some firmware handling
  if (! filter_bdev_by_product_name_matches(req, "Null disk")) {
    return -1; // continue
  }

  // Get the bdev ... assumes one bdev in this subsystem
  struct spdk_bdev *bdev;
  struct spdk_bdev_desc *desc;
  struct spdk_io_channel *ch;
  int rc = spdk_nvmf_request_get_bdev_info(1, req, &bdev, &desc, &ch);
	if (rc) {
    printf("*** handle_firmware_download: No bdev info found for sesd subsystem\n");
   	response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
  }

  return spdk_nvmf_bdev_nvme_passthru_admin(bdev, desc, ch, req, NULL);
}

static int 
handle_format(struct spdk_nvmf_request *req)
{
  struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
  struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);

  // Only supported w/ NVMe disks
  if (! filter_bdev_by_product_name_matches(req, "NVMe disk")) {
    return -1; // continue
  }

  // Get the bdev ... assumes one bdev in this subsystem
  struct spdk_bdev *bdev;
  struct spdk_bdev_desc *desc;
  struct spdk_io_channel *ch;
  int rc = spdk_nvmf_request_get_bdev_info(1, req, &bdev, &desc, &ch);
	if (rc) {
    printf("*** handle_format: No bdev info found for sesd subsystem\n");
   	response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
  }

  printf("\t-->sending format command\n");

  return spdk_nvmf_bdev_nvme_passthru_admin(bdev, desc, ch, req, NULL);
}

int
tpd_admin_hdlr(struct spdk_nvmf_request *req)
{
  struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);

  printf("*** tpd_admin_hdlr\n");
  switch(cmd->opc) {
    case SPDK_NVME_OPC_IDENTIFY:
      return handle_identify(req);
    case 0xC1: // sesd_send
    case 0xC2: // sesd_recv
      return handle_sesd_passthru(req);
    case SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD:
    case SPDK_NVME_OPC_FIRMWARE_COMMIT:
      return handle_firmware(req); 
    case SPDK_NVME_OPC_FORMAT_NVM:
      return handle_format(req);
    default:
      return -1; // continue
  }
  return -1;
}