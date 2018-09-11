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
        except Exception as msg:
            self.shell.log.error(str(msg))
            pass
        else:
            self.shell.log.debug("Command %s succeeded." % command)
            return result


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


class UILvolStores(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "lvol_stores", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for lvs in self.get_root().get_lvol_stores():
            UILvsObj(lvs, self)

    def ui_command_create(self, name, bdev_name, cluster_size=None):
        """
        Creates logical volume store on target bdev.

        Arguments:
        name - Friendly name to use alongside with UUID identifier.
        bdev_name - On which bdev to create the lvol store.
        cluster_size - Cluster size to use when creating lvol store, in bytes. Default: 4194304.
        """

        cluster_size = self.ui_eval_param(cluster_size, "number", None)

        try:
            self.get_root().create_lvol_store(lvs_name=name, bdev_name=bdev_name, cluster_sz=cluster_size)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name=None, uuid=None):
        """
        Deletes logical volume store from configuration.
        This will also delete all logical volume bdevs created on this lvol store!

        Arguments:
        name - Friendly name of the logical volume store to be deleted.
        uuid - UUID number of the logical volume store to be deleted.
        """
        if name is None and uuid is None:
            self.shell.log.error("Please specify one of the identifiers: "
                                 "lvol store name or UUID")
        self.get_root().delete_lvol_store(lvs_name=name, uuid=uuid)
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return "Lvol stores: %s" % len(self.children), None


class UIBdev(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for bdev in self.get_root().get_bdevs(self.name):
            UIBdevObj(bdev, self)

    def ui_command_get_bdev_iostat(self, name=None):
        try:
            ret = self.get_root().get_bdevs_iostat(name=name)
            self.shell.log.info(json.dumps(ret, indent=2))
        except JSONRPCException as e:
            self.shell.log.error(e.message)

    def summary(self):
        return "Bdevs: %d" % len(self.children), None


class UIMallocBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "malloc", parent)

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

        try:
            ret_name = self.get_root().create_malloc_bdev(num_blocks=size * 1024 * 1024 // block_size,
                                                          block_size=block_size,
                                                          name=name, uuid=uuid)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes malloc bdev from configuration.

        Arguments:
        name - Is a unique identifier of the malloc bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().delete_malloc_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UIAIOBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "aio", parent)

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

        try:
            ret_name = self.get_root().create_aio_bdev(name=name,
                                                       block_size=int(block_size),
                                                       filename=filename)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes aio bdev from configuration.

        Arguments:
        name - Is a unique identifier of the aio bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().delete_aio_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UILvolBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "logical_volume", parent)

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

        try:
            ret_uuid = self.get_root().create_lvol_bdev(lvol_name=name, size=size,
                                                        lvs_name=lvs_name, uuid=uuid,
                                                        thin_provision=thin_provision)
            self.shell.log.info(ret_uuid)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes lvol bdev from configuration.

        Arguments:
        name - Is a unique identifier of the lvol bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().destroy_lvol_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UINvmeBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "nvme", parent)

    def ui_command_create(self, name, trtype, traddr,
                          adrfam=None, trsvcid=None, subnqn=None):

        if "rdma" in trtype and None in [adrfam, trsvcid, subnqn]:
            self.shell.log.error("Using RDMA transport type."
                                 "Please provide arguments for adrfam, trsvcid and subnqn.")

        try:
            ret_name = self.get_root().create_nvme_bdev(name=name, trtype=trtype,
                                                        traddr=traddr, adrfam=adrfam,
                                                        trsvcid=trsvcid, subnqn=subnqn)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes NVMe controller from configuration.

        Arguments:
        name - Is a unique identifier of the NVMe controller to be deleted.
        """
        try:
            self.get_root().delete_nvme_controller(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UINullBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "null", parent)

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

        try:
            ret_name = self.get_root().create_null_bdev(num_blocks=num_blocks,
                                                        block_size=block_size,
                                                        name=name, uuid=uuid)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes null bdev from configuration.

        Arguments:
        name - Is a unique identifier of the null bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().delete_null_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UIErrorBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "error", parent)

    def ui_command_create(self, base_name):
        """
        Construct a error injection bdev.

        Arguments:
        base_name - base bdev name on top of which error bdev will be created.
        """

        try:
            self.get_root().create_error_bdev(base_name=base_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes error bdev from configuration.

        Arguments:
        name - Is a unique identifier of the error bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().delete_error_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UISplitBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "split_disk", parent)

    def ui_command_split_bdev(self, base_bdev, split_count, split_size_mb=None):
        """
        Construct split block devices from a base bdev.

        Arguments:
        base_bdev - Name of bdev to split
        split_count -  Number of split bdevs to create
        split_size_mb- Size of each split volume in MiB (optional)
        """

        split_count = self.ui_eval_param(split_count, "number", None)
        split_size_mb = self.ui_eval_param(split_size_mb, "number", None)

        try:
            ret_name = self.get_root().split_bdev(base_bdev=base_bdev,
                                                  split_count=split_count,
                                                  split_size_mb=split_size_mb)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.parent.refresh()
        self.refresh()

    def ui_command_destruct_split_bdev(self, base_bdev):
        """Destroy split block devices associated with base bdev.

        Args:
            base_bdev: name of previously split bdev
        """

        try:
            self.get_root().destruct_split_bdev(base_bdev=base_bdev)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.parent.refresh()
        self.refresh()


class UIPmemBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "pmemblk", parent)

    def ui_command_create_pmem_pool(self, pmem_file, total_size, block_size):
        total_size = self.ui_eval_param(total_size, "number", None)
        block_size = self.ui_eval_param(block_size, "number", None)
        num_blocks = int((total_size * 1024 * 1024) / block_size)

        try:
            self.get_root().create_pmem_pool(pmem_file=pmem_file,
                                             num_blocks=num_blocks,
                                             block_size=block_size)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

    def ui_command_delete_pmem_pool(self, pmem_file):
        try:
            self.get_root().delete_pmem_pool(pmem_file=pmem_file)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

    def ui_command_info_pmem_pool(self, pmem_file):
        try:
            ret = self.get_root().delete_pmem_pool(pmem_file=pmem_file)
            self.shell.log.info(ret)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

    def ui_command_create(self, pmem_file, name):
        try:
            ret_name = self.get_root().create_pmem_bdev(pmem_file=pmem_file,
                                                        name=name)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes pmem bdev from configuration.

        Arguments:
        name - Is a unique identifier of the pmem bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().delete_pmem_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UIRbdBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "rbd", parent)

    def ui_command_create(self, pool_name, rbd_name, block_size, name=None):
        block_size = self.ui_eval_param(block_size, "number", None)

        try:
            ret_name = self.get_root().create_rbd_bdev(pool_name=pool_name,
                                                       rbd_name=rbd_name,
                                                       block_size=block_size,
                                                       name=name)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes rbd bdev from configuration.

        Arguments:
        name - Is a unique identifier of the rbd bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().delete_rbd_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UIiSCSIBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "iscsi", parent)

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
        try:
            ret_name = self.get_root().create_iscsi_bdev(name=name,
                                                         url=url,
                                                         initiator_iqn=initiator_iqn)
            self.shell.log.info(ret_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes iSCSI bdev from configuration.

        Arguments:
        name - name of the iscsi bdev to be deleted.
        """
        try:
            self.get_root().delete_iscsi_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()


class UIVirtioBlkBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "virtioblk_disk", parent)

    def ui_command_create(self, name, trtype, traddr,
                          vq_count=None, vq_size=None):

        vq_count = self.ui_eval_param(vq_count, "number", None)
        vq_size = self.ui_eval_param(vq_size, "number", None)

        try:
            ret = self.get_root().create_virtio_dev(name=name,
                                                    trtype=trtype,
                                                    traddr=traddr,
                                                    dev_type="blk",
                                                    vq_count=vq_count,
                                                    vq_size=vq_size)

            self.shell.log.info(ret)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes virtio scsi bdev from configuration.

        Arguments:
        name - Is a unique identifier of the virtio scsi bdev to be deleted - UUID number or name alias.
        """
        try:
            self.get_root().remove_virtio_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh()


class UIVirtioScsiBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "virtioscsi_disk", parent)

    def refresh(self):
        self._children = set([])
        for bdev in self.get_root().get_virtio_scsi_devs():
            UIVirtioScsiBdevObj(bdev, self)

    def ui_command_create(self, name, trtype, traddr,
                          vq_count=None, vq_size=None):

        vq_count = self.ui_eval_param(vq_count, "number", None)
        vq_size = self.ui_eval_param(vq_size, "number", None)

        try:
            ret = self.get_root().create_virtio_dev(name=name,
                                                    trtype=trtype,
                                                    traddr=traddr,
                                                    dev_type="scsi",
                                                    vq_count=vq_count,
                                                    vq_size=vq_size)

            self.shell.log.info(ret)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        try:
            self.get_root().remove_virtio_bdev(name=name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()


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
        for bdev in self.get_root().get_bdevs("virtio_scsi_disk"):
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
        self.get_root().remove_vhost_controller(ctrlr=name)
        self.get_root().refresh()
        self.refresh()


class UIVhostBlk(UIVhost):
    def __init__(self, parent):
        UIVhost.__init__(self, "block", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for ctrlr in self.get_root().get_vhost_ctrlrs(self.name):
            UIVhostBlkCtrlObj(ctrlr, self)

    def ui_command_create(self, name, bdev, cpumask=None, readonly=False):
        """
        Construct a Vhost BLK controller.

        Arguments:
        name - Controller name.
        bdev - Which bdev to attach to the controller.
        cpumask - Optional. Integer to specify mask of CPUs to use.
                  Default: 1.
        readonly - Whether controller should be read only or not.
                   Default: False.
        """
        try:
            self.get_root().create_vhost_blk_controller(ctrlr=name,
                                                        dev_name=bdev,
                                                        cpumask=cpumask,
                                                        readonly=bool(readonly))
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()


class UIVhostScsi(UIVhost):
    def __init__(self, parent):
        UIVhost.__init__(self, "scsi", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for ctrlr in self.get_root().get_vhost_ctrlrs(self.name):
            UIVhostScsiCtrlObj(ctrlr, self)

    def ui_command_create(self, name, cpumask=None):
        """
        Construct a Vhost SCSI controller.

        Arguments:
        name - Controller name.
        cpumask - Optional. Integer to specify mask of CPUs to use.
                  Default: 1.
        """
        try:
            self.get_root().create_vhost_scsi_controller(ctrlr=name,
                                                         cpumask=cpumask)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()


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

        try:
            self.get_root().set_vhost_controller_coalescing(ctrlr=self.ctrlr.ctrlr,
                                                            delay_base_us=delay_base_us,
                                                            iops_threshold=iops_threshold)
        except JSONRPCException as e:
            self.shell.log.error(e.message)


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
        try:
            self.get_root().remove_vhost_scsi_target(ctrlr=self.ctrlr.ctrlr,
                                                     scsi_target_num=int(target_num))
            for ctrlr in self.get_root().get_vhost_ctrlrs("scsi"):
                if ctrlr.ctrlr == self.ctrlr.ctrlr:
                    self.ctrlr = ctrlr
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()
        self.get_root().refresh()

    def ui_command_add_lun(self, target_num, bdev_name):
        """
        Add LUN to SCSI target node.
        Currently only one LUN (which is LUN ID 0) per target is supported.
        Adding LUN to not existing target node will create that node.

        Arguments:
        target_num - Integer identifier of target node to modify.
        bdev - Which bdev to add as LUN.
        """
        try:
            self.get_root().add_vhost_scsi_lun(ctrlr=self.ctrlr.ctrlr,
                                               scsi_target_num=int(target_num),
                                               bdev_name=bdev_name)
            for ctrlr in self.get_root().get_vhost_ctrlrs("scsi"):
                if ctrlr.ctrlr == self.ctrlr.ctrlr:
                    self.ctrlr = ctrlr
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

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
