from configshell_fb import ConfigNode, ExecutionError
from uuid import UUID
from rpc.client import JSONRPCException
import json


def convert_bytes_to_human(size):
    if not size:
        return ""
    for x in ["bytes", "K", "M", "G", "T"]:
        if size < 1024.0:
            return "%3.1f%s" % (size, x)
        size /= 1024.0


class UINode(ConfigNode):
    def __init__(self, name, parent=None, shell=None):
        ConfigNode.__init__(self, name, parent, shell)

    def refresh(self):
        for child in self.children:
            child.refresh()

    def refresh_node(self):
        self.refresh()

    def ui_command_refresh(self):
        self.refresh()

    def ui_command_ll(self, path=None, depth=None):
        """
        Alias for ls.
        """
        self.ui_command_ls(path, depth)

    def execute_command(self, command, pparams=[], kparams={}):
        try:
            result = ConfigNode.execute_command(self, command,
                                                pparams, kparams)
        except Exception as e:
            raise e
        else:
            self.shell.log.debug("Command %s succeeded." % command)
            return result
        finally:
            if self.shell.interactive and\
                command in ["create", "delete", "delete_all", "add_initiator",
                            "allow_any_host", "bdev_split_create", "add_lun",
                            "iscsi_target_node_add_pg_ig_maps", "remove_target", "add_secret",
                            "bdev_split_delete", "bdev_pmem_delete_pool",
                            "bdev_pmem_create_pool", "delete_secret_all",
                            "delete_initiator", "set_auth", "delete_secret",
                            "iscsi_target_node_remove_pg_ig_maps", "load_config",
                            "load_subsystem_config"]:
                self.get_root().refresh()
                self.refresh_node()


class UIBdevs(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "bdevs", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        UIMallocBdev(self)
        UIAIOBdev(self)
        UILvolBdev(self)
        UINvmeBdev(self)
        UINullBdev(self)
        UIErrorBdev(self)
        UISplitBdev(self)
        UIPmemBdev(self)
        UIRbdBdev(self)
        UIiSCSIBdev(self)
        UIVirtioBlkBdev(self)
        UIVirtioScsiBdev(self)
        UIRaidBdev(self)


class UILvolStores(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "lvol_stores", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for lvs in self.get_root().bdev_lvol_get_lvstores():
            UILvsObj(lvs, self)

    def delete(self, name, uuid):
        if name is None and uuid is None:
            self.shell.log.error("Please specify one of the identifiers: "
                                 "lvol store name or UUID")
        self.get_root().bdev_lvol_delete_lvstore(lvs_name=name, uuid=uuid)

    def ui_command_create(self, name, bdev_name, cluster_size=None):
        """
        Creates logical volume store on target bdev.

        Arguments:
        name - Friendly name to use alongside with UUID identifier.
        bdev_name - On which bdev to create the lvol store.
        cluster_size - Cluster size to use when creating lvol store, in bytes. Default: 4194304.
        """

        cluster_size = self.ui_eval_param(cluster_size, "number", None)
        self.get_root().bdev_lvol_create_lvstore(lvs_name=name, bdev_name=bdev_name, cluster_sz=cluster_size)

    def ui_command_delete(self, name=None, uuid=None):
        """
        Deletes logical volume store from configuration.
        This will also delete all logical volume bdevs created on this lvol store!

        Arguments:
        name - Friendly name of the logical volume store to be deleted.
        uuid - UUID number of the logical volume store to be deleted.
        """
        self.delete(name, uuid)

    def ui_command_delete_all(self):
        rpc_messages = ""
        for lvs in self._children:
            try:
                self.delete(None, lvs.lvs.uuid)
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def summary(self):
        return "Lvol stores: %s" % len(self.children), None


class UIBdev(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for bdev in self.get_root().bdev_get_bdevs(self.name):
            UIBdevObj(bdev, self)

    def ui_command_get_bdev_iostat(self, name=None):
        ret = self.get_root().bdev_get_iostat(name=name)
        self.shell.log.info(json.dumps(ret, indent=2))

    def ui_command_delete_all(self):
        """Delete all bdevs from this tree node."""
        rpc_messages = ""
        for bdev in self._children:
            try:
                self.delete(bdev.name)
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def summary(self):
        return "Bdevs: %d" % len(self.children), None


class UIMallocBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "malloc", parent)

    def delete(self, name):
        self.get_root().bdev_malloc_delete(name=name)

    def ui_command_create(self, size, block_size, name=None, uuid=None):
        """
        Construct a Malloc bdev.

        Arguments:
        size - Size in megabytes.
        block_size - Integer, block size to use when constructing bdev.
        name - Optional argument. Custom name to use for bdev. If not provided
               then name will be "MallocX" where X is next available ID.
        uuid - Optional parameter. Custom UUID to use. If empty then random
               will be generated.
        """

        size = self.ui_eval_param(size, "number", None)
        block_size = self.ui_eval_param(block_size, "number", None)
        ret_name = self.get_root().create_malloc_bdev(num_blocks=size * 1024 * 1024 // block_size,
                                                      block_size=block_size,
                                                      name=name, uuid=uuid)
        self.shell.log.info(ret_name)

    def ui_command_delete(self, name):
        """
        Deletes malloc bdev from configuration.

        Arguments:
        name - Is a unique identifier of the malloc bdev to be deleted - UUID number or name alias.
        """
        self.delete(name)


class UIAIOBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "aio", parent)

    def delete(self, name):
        self.get_root().bdev_aio_delete(name=name)

    def ui_command_create(self, name, filename, block_size):
        """
        Construct an AIO bdev.
        Backend file must exist before trying to create an AIO bdev.

        Arguments:
        name - Optional argument. Custom name to use for bdev. If not provided
               then name will be "MallocX" where X is next available ID.
        filename - Path to AIO backend.
        block_size - Integer, block size to use when constructing bdev.
        """

        block_size = self.ui_eval_param(block_size, "number", None)
        ret_name = self.get_root().bdev_aio_create(name=name,
                                                   block_size=int(block_size),
                                                   filename=filename)
        self.shell.log.info(ret_name)

    def ui_command_delete(self, name):
        """
        Deletes aio bdev from configuration.

        Arguments:
        name - Is a unique identifier of the aio bdev to be deleted - UUID number or name alias.
        """
        self.delete(name)


class UILvolBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "logical_volume", parent)

    def delete(self, name):
        self.get_root().bdev_lvol_delete(name=name)

    def ui_command_create(self, name, size, lvs, thin_provision=None):
        """
        Construct a Logical Volume bdev.

        Arguments:
        name - Friendly name to use for creating logical volume bdev.
        size - Size in megabytes.
        lvs - Identifier of logical volume store on which the bdev should be
              created. Can be either a friendly name or UUID.
        thin_provision - Whether the bdev should be thick or thin provisioned.
              Default is False, and created bdevs are thick-provisioned.
        """
        uuid = None
        lvs_name = None
        try:
            UUID(lvs)
            uuid = lvs
        except ValueError:
            lvs_name = lvs

        size = self.ui_eval_param(size, "number", None)
        size *= (1024 * 1024)
        thin_provision = self.ui_eval_param(thin_provision, "bool", False)

        ret_uuid = self.get_root().create_lvol_bdev(lvol_name=name, size=size,
                                                    lvs_name=lvs_name, uuid=uuid,
                                                    thin_provision=thin_provision)
        self.shell.log.info(ret_uuid)

    def ui_command_delete(self, name):
        """
        Deletes lvol bdev from configuration.

        Arguments:
        name - Is a unique identifier of the lvol bdev to be deleted - UUID number or name alias.
        """
        self.delete(name)


class UINvmeBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "nvme", parent)

    def delete(self, name):
        self.get_root().bdev_nvme_detach_controller(name=name)

    def ui_command_create(self, name, trtype, traddr,
                          adrfam=None, trsvcid=None, subnqn=None):
        if "rdma" in trtype and None in [adrfam, trsvcid, subnqn]:
            self.shell.log.error("Using RDMA transport type."
                                 "Please provide arguments for adrfam, trsvcid and subnqn.")
        ret_name = self.get_root().create_nvme_bdev(name=name, trtype=trtype,
                                                    traddr=traddr, adrfam=adrfam,
                                                    trsvcid=trsvcid, subnqn=subnqn)
        self.shell.log.info(ret_name)

    def ui_command_delete_all(self):
        rpc_messages = ""
        ctrlrs = [x.name for x in self._children]
        ctrlrs = [x.rsplit("n", 1)[0] for x in ctrlrs]
        ctrlrs = set(ctrlrs)
        for ctrlr in ctrlrs:
            try:
                self.delete(ctrlr)
            except JSONRPCException as e:
                rpc_messages += e.messages
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def ui_command_delete(self, name):
        """
        Deletes NVMe controller from configuration.

        Arguments:
        name - Is a unique identifier of the NVMe controller to be deleted.
        """
        self.delete(name)


class UINullBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "null", parent)

    def delete(self, name):
        self.get_root().bdev_null_delete(name=name)

    def ui_command_create(self, name, size, block_size, uuid=None):
        """
        Construct a Null bdev.

        Arguments:
        name - Name to use for bdev.
        size - Size in megabytes.
        block_size - Integer, block size to use when constructing bdev.
        uuid - Optional parameter. Custom UUID to use. If empty then random
               will be generated.
        """

        size = self.ui_eval_param(size, "number", None)
        block_size = self.ui_eval_param(block_size, "number", None)
        num_blocks = size * 1024 * 1024 // block_size
        ret_name = self.get_root().bdev_null_create(num_blocks=num_blocks,
                                                    block_size=block_size,
                                                    name=name, uuid=uuid)
        self.shell.log.info(ret_name)

    def ui_command_delete(self, name):
        """
        Deletes null bdev from configuration.

        Arguments:
        name - Is a unique identifier of the null bdev to be deleted - UUID number or name alias.
        """
        self.delete(name)


class UIErrorBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "error", parent)

    def delete(self, name):
        self.get_root().bdev_error_delete(name=name)

    def ui_command_create(self, base_name):
        """
        Construct a error injection bdev.

        Arguments:
        base_name - base bdev name on top of which error bdev will be created.
        """

        self.get_root().create_error_bdev(base_name=base_name)

    def ui_command_delete(self, name):
        """
        Deletes error bdev from configuration.

        Arguments:
        name - Is a unique identifier of the error bdev to be deleted - UUID number or name alias.
        """
        self.delete(name)


class UISplitBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "split_disk", parent)

    def delete(self, name):
        pass

    def ui_command_bdev_split_create(self, base_bdev, split_count, split_size_mb=None):
        """
        Create split block devices from a base bdev.

        Arguments:
        base_bdev - Name of bdev to split
        split_count -  Number of split bdevs to create
        split_size_mb- Size of each split volume in MiB (optional)
        """

        split_count = self.ui_eval_param(split_count, "number", None)
        split_size_mb = self.ui_eval_param(split_size_mb, "number", None)

        ret_name = self.get_root().bdev_split_create(base_bdev=base_bdev,
                                                     split_count=split_count,
                                                     split_size_mb=split_size_mb)
        self.shell.log.info(ret_name)

    def ui_command_bdev_split_delete(self, base_bdev):
        """Delete split block devices associated with base bdev.

        Args:
            base_bdev: name of previously split bdev
        """

        self.get_root().bdev_split_delete(base_bdev=base_bdev)


class UIPmemBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "pmemblk", parent)

    def delete(self, name):
        self.get_root().bdev_pmem_delete(name=name)

    def ui_command_bdev_pmem_create_pool(self, pmem_file, total_size, block_size):
        total_size = self.ui_eval_param(total_size, "number", None)
        block_size = self.ui_eval_param(block_size, "number", None)
        num_blocks = int((total_size * 1024 * 1024) / block_size)

        self.get_root().bdev_pmem_create_pool(pmem_file=pmem_file,
                                              num_blocks=num_blocks,
                                              block_size=block_size)

    def ui_command_bdev_pmem_delete_pool(self, pmem_file):
        self.get_root().bdev_pmem_delete_pool(pmem_file=pmem_file)

    def ui_command_bdev_pmem_get_pool_info(self, pmem_file):
        ret = self.get_root().bdev_pmem_get_pool_info(pmem_file=pmem_file)
        self.shell.log.info(json.dumps(ret, indent=2))

    def ui_command_create(self, pmem_file, name):
        ret_name = self.get_root().bdev_pmem_create(pmem_file=pmem_file,
                                                    name=name)
        self.shell.log.info(ret_name)

    def ui_command_delete(self, name):
        """
        Deletes pmem bdev from configuration.

        Arguments:
        name - Is a unique identifier of the pmem bdev to be deleted - UUID number or name alias.
        """
        self.delete(name)


class UIRbdBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "rbd", parent)

    def delete(self, name):
        self.get_root().bdev_rbd_delete(name=name)

    def ui_command_create(self, pool_name, rbd_name, block_size, name=None):
        block_size = self.ui_eval_param(block_size, "number", None)

        ret_name = self.get_root().create_rbd_bdev(pool_name=pool_name,
                                                   rbd_name=rbd_name,
                                                   block_size=block_size,
                                                   name=name)
        self.shell.log.info(ret_name)

    def ui_command_delete(self, name):
        """
        Deletes rbd bdev from configuration.

        Arguments:
        name - Is a unique identifier of the rbd bdev to be deleted - UUID number or name alias.
        """
        self.delete(name)


class UIiSCSIBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "iscsi", parent)

    def delete(self, name):
        self.get_root().bdev_iscsi_delete(name=name)

    def ui_command_create(self, name, url, initiator_iqn):
        """
        Create iSCSI bdev in configuration by connecting to remote
        iSCSI target.

        Arguments:
        name - name to be used as an ID for created iSCSI bdev.
        url - iscsi url pointing to LUN on remote iSCSI target.
              Example: iscsi://127.0.0.1:3260/iqn.2018-06.org.spdk/0.
        initiator_iqn - IQN to use for initiating connection with the target.
        """
        ret_name = self.get_root().create_iscsi_bdev(name=name,
                                                     url=url,
                                                     initiator_iqn=initiator_iqn)
        self.shell.log.info(ret_name)

    def ui_command_delete(self, name):
        """
        Deletes iSCSI bdev from configuration.

        Arguments:
        name - name of the iscsi bdev to be deleted.
        """
        self.delete(name)


class UIVirtioBlkBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "virtioblk_disk", parent)

    def ui_command_create(self, name, trtype, traddr,
                          vq_count=None, vq_size=None):

        vq_count = self.ui_eval_param(vq_count, "number", None)
        vq_size = self.ui_eval_param(vq_size, "number", None)

        ret = self.get_root().create_virtio_dev(name=name,
                                                trtype=trtype,
                                                traddr=traddr,
                                                dev_type="blk",
                                                vq_count=vq_count,
                                                vq_size=vq_size)

        self.shell.log.info(ret)

    def ui_command_delete(self, name):
        """
        Deletes virtio scsi bdev from configuration.

        Arguments:
        name - Is a unique identifier of the virtio scsi bdev to be deleted - UUID number or name alias.
        """
        self.get_root().bdev_virtio_detach_controller(name=name)


class UIVirtioScsiBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "virtioscsi_disk", parent)

    def refresh(self):
        self._children = set([])
        for bdev in self.get_root().bdev_virtio_scsi_get_devices():
            UIVirtioScsiBdevObj(bdev, self)

    def ui_command_create(self, name, trtype, traddr,
                          vq_count=None, vq_size=None):

        vq_count = self.ui_eval_param(vq_count, "number", None)
        vq_size = self.ui_eval_param(vq_size, "number", None)

        ret = self.get_root().create_virtio_dev(name=name,
                                                trtype=trtype,
                                                traddr=traddr,
                                                dev_type="scsi",
                                                vq_count=vq_count,
                                                vq_size=vq_size)

        self.shell.log.info(ret)

    def ui_command_delete(self, name):
        self.get_root().bdev_virtio_detach_controller(name=name)


class UIBdevObj(UINode):
    def __init__(self, bdev, parent):
        self.bdev = bdev
        # Using bdev name also for lvol bdevs, which results in displying
        # UUID instead of alias. This is because alias naming convention
        # (lvol_store_name/lvol_bdev_name) conflicts with configshell paths
        # ("/" as separator).
        # Solution: show lvol alias in "summary field" for now.
        # TODO: Possible next steps:
        # - Either change default separator in tree for smth else
        # - or add a UI command which would be able to autocomplete
        #   "cd" command based on objects alias and match is to the
        #   "main" bdev name.
        UINode.__init__(self, self.bdev.name, parent)

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.bdev), indent=2))

    def summary(self):
        size = convert_bytes_to_human(self.bdev.block_size * self.bdev.num_blocks)
        size = "=".join(["Size", size])

        in_use = "Not claimed"
        if bool(self.bdev.claimed):
            in_use = "Claimed"

        alias = None
        if self.bdev.aliases:
            alias = self.bdev.aliases[0]

        info = ", ".join([_f for _f in [alias, size, in_use] if _f])
        return info, True


class UIVirtioScsiBdevObj(UIBdevObj):
    def __init__(self, bdev, parent):
        UIBdevObj.__init__(self, bdev, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for bdev in self.get_root().bdev_get_bdevs("virtio_scsi_disk"):
            if self.bdev.name in bdev.name:
                UIBdevObj(bdev, self)

    def summary(self):
        if "socket" in list(self.bdev.virtio.keys()):
            info = self.bdev.virtio["socket"]
        if "pci_address" in list(self.bdev.virtio.keys()):
            info = self.bdev.virtio["pci_address"]
        return info, True


class UILvsObj(UINode):
    def __init__(self, lvs, parent):
        UINode.__init__(self, lvs.name, parent)
        self.lvs = lvs

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.lvs), indent=2))

    def summary(self):
        size = convert_bytes_to_human(self.lvs.total_data_clusters * self.lvs.cluster_size)
        free = convert_bytes_to_human(self.lvs.free_clusters * self.lvs.cluster_size)
        if not free:
            free = "0"
        size = "=".join(["Size", size])
        free = "=".join(["Free", free])
        info = ", ".join([str(size), str(free)])
        return info, True


class UIVhosts(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "vhost", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        self.get_root().list_vhost_ctrls()
        UIVhostBlk(self)
        UIVhostScsi(self)


class UIVhost(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)
        self.refresh()

    def ui_command_delete(self, name):
        """
        Delete a Vhost controller from configuration.

        Arguments:
        name - Controller name.
        """
        self.get_root().vhost_delete_controller(ctrlr=name)


class UIVhostBlk(UIVhost):
    def __init__(self, parent):
        UIVhost.__init__(self, "block", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for ctrlr in self.get_root().vhost_get_controllers(ctrlr_type=self.name):
            UIVhostBlkCtrlObj(ctrlr, self)

    def ui_command_create(self, name, bdev, cpumask=None, readonly=False):
        """
        Create a Vhost BLK controller.

        Arguments:
        name - Controller name.
        bdev - Which bdev to attach to the controller.
        cpumask - Optional. Integer to specify mask of CPUs to use.
                  Default: 1.
        readonly - Whether controller should be read only or not.
                   Default: False.
        """
        self.get_root().vhost_create_blk_controller(ctrlr=name,
                                                    dev_name=bdev,
                                                    cpumask=cpumask,
                                                    readonly=bool(readonly))


class UIVhostScsi(UIVhost):
    def __init__(self, parent):
        UIVhost.__init__(self, "scsi", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for ctrlr in self.get_root().vhost_get_controllers(ctrlr_type=self.name):
            UIVhostScsiCtrlObj(ctrlr, self)

    def ui_command_create(self, name, cpumask=None):
        """
        Create a Vhost SCSI controller.

        Arguments:
        name - Controller name.
        cpumask - Optional. Integer to specify mask of CPUs to use.
                  Default: 1.
        """
        self.get_root().vhost_create_scsi_controller(ctrlr=name,
                                                     cpumask=cpumask)


class UIVhostCtrl(UINode):
    # Base class for SCSI and BLK controllers, do not instantiate
    def __init__(self, ctrlr, parent):
        self.ctrlr = ctrlr
        UINode.__init__(self, self.ctrlr.ctrlr, parent)
        self.refresh()

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.ctrlr), indent=2))

    def ui_command_set_coalescing(self, delay_base_us, iops_threshold):
        delay_base_us = self.ui_eval_param(delay_base_us, "number", None)
        iops_threshold = self.ui_eval_param(iops_threshold, "number", None)

        self.get_root().vhost_controller_set_coalescing(ctrlr=self.ctrlr.ctrlr,
                                                        delay_base_us=delay_base_us,
                                                        iops_threshold=iops_threshold)


class UIVhostScsiCtrlObj(UIVhostCtrl):
    def refresh(self):
        self._children = set([])
        for lun in self.ctrlr.backend_specific["scsi"]:
            UIVhostTargetObj(lun, self)

    def ui_command_remove_target(self, target_num):
        """
        Remove target node from SCSI controller.

        Arguments:
        target_num - Integer identifier of target node to delete.
        """
        self.get_root().vhost_scsi_controller_remove_target(ctrlr=self.ctrlr.ctrlr,
                                                            scsi_target_num=int(target_num))
        for ctrlr in self.get_root().vhost_get_controllers(ctrlr_type="scsi"):
            if ctrlr.ctrlr == self.ctrlr.ctrlr:
                self.ctrlr = ctrlr

    def ui_command_add_lun(self, target_num, bdev_name):
        """
        Add LUN to SCSI target node.
        Currently only one LUN (which is LUN ID 0) per target is supported.
        Adding LUN to not existing target node will create that node.

        Arguments:
        target_num - Integer identifier of target node to modify.
        bdev - Which bdev to add as LUN.
        """
        self.get_root().vhost_scsi_controller_add_target(ctrlr=self.ctrlr.ctrlr,
                                                         scsi_target_num=int(target_num),
                                                         bdev_name=bdev_name)
        for ctrlr in self.get_root().vhost_get_controllers(ctrlr_type="scsi"):
            if ctrlr.ctrlr == self.ctrlr.ctrlr:
                self.ctrlr = ctrlr

    def summary(self):
        info = self.ctrlr.socket
        return info, True


class UIVhostBlkCtrlObj(UIVhostCtrl):
    def refresh(self):
        self._children = set([])
        UIVhostLunDevObj(self.ctrlr.backend_specific["block"]["bdev"], self)

    def summary(self):
        ro = None
        if self.ctrlr.backend_specific["block"]["readonly"]:
            ro = "Readonly"
        info = ", ".join([_f for _f in [self.ctrlr.socket, ro] if _f])
        return info, True


class UIVhostTargetObj(UINode):
    def __init__(self, target, parent):
        self.target = target
        # Next line: configshell does not allow paths with spaces.
        UINode.__init__(self, target["target_name"].replace(" ", "_"), parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for target in self.target["luns"]:
            UIVhostLunDevObj(target["bdev_name"], self)

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(self.target, indent=2))

    def summary(self):
        luns = "LUNs: %s" % len(self.target["luns"])
        id = "TargetID: %s" % self.target["scsi_dev_num"]
        info = ",".join([luns, id])
        return info, True


class UIVhostLunDevObj(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)


class UIRaidBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "raid_volume", parent)

    def delete(self, name):
        self.get_root().bdev_raid_delete(name=name)

    def ui_command_create(self, name, raid_level, base_bdevs, strip_size_kb):
        """
        Creates a raid bdev of the provided base_bdevs

        Arguments:
        name - raid bdev name
        raid_level - raid level, supported values 0
        base_bdevs - base bdevs name, whitespace separated list in quotes
        strip_size_kb - strip size of raid bdev in KB, supported values like 8, 16, 32, 64, 128, 256, etc
        """
        base_bdevs_array = []
        for u in base_bdevs.strip().split(" "):
            base_bdevs_array.append(u)

        strip_size_kb = self.ui_eval_param(strip_size_kb, "number", None)

        ret_name = self.get_root().bdev_raid_create(name=name,
                                                    raid_level=raid_level,
                                                    base_bdevs=base_bdevs_array,
                                                    strip_size_kb=strip_size_kb)
        self.shell.log.info(ret_name)

    def ui_command_delete(self, name):
        """
        Deletes this raid bdev object

        Arguments:
        name - raid bdev name
        """
        self.delete(name)
