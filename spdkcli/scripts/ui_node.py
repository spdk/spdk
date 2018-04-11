from configshell_fb import ConfigNode, ExecutionError
from uuid import UUID
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
        self.get_root().list_bdevs()
        UIMallocBdev(self)
        UIAIOBdev(self)
        UILvolBdev(self)
        UINvmeBdev(self)

    def ui_command_delete(self, name):
        """
        Deletes bdev from configuration.

        Arguments:
        name - Is a unique identifier of the bdev to be deleted - UUID number or name alias.
        """
        self.get_root().delete_bdev(name=name)
        self.refresh()


class UILvolStores(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "lvol_stores", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        self.get_root().list_lvols()
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

        self.get_root().create_lvol_store(lvs_name=name, bdev_name=bdev_name, cluster_sz=cluster_size)
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

    def ui_command_delete(self, name):
        """
        Deletes bdev from configuration.

        Arguments:
        name - Is a unique identifier of the bdev to be deleted - UUID number or name alias.
        """
        self.get_root().delete_bdev(name=name)
        self.refresh()

    def summary(self):
        return "Bdevs: %d" % len(self.children), None


class UIMallocBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Malloc", parent)

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

        ret_name = self.get_root().create_malloc_bdev(total_size=size,
                                                      block_size=block_size,
                                                      name=name, uuid=uuid)
        if name is None:
            name = ret_name
        self.shell.log.info("Created Malloc bdev: %s" % name)
        self.refresh()


class UIAIOBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "AIO", parent)

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

        ret_name = self.get_root().create_aio_bdev(name=name,
                                                   block_size=int(block_size),
                                                   filename=filename)
        if name is None:
            name = ret_name
        self.shell.log.info("Created AIO bdev: %s" % name)
        self.refresh()


class UILvolBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Logical_Volume", parent)

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
        thin_provision = self.ui_eval_param(thin_provision, "bool", False)

        ret_name = self.get_root().create_lvol_bdev(lvol_name=name, size=int(size),
                                                    lvs_name=lvs_name, uuid=uuid,
                                                    thin_provision=thin_provision)
        name, uuid = ret_name
        self.shell.log.info("Created lvol bdev: %s with uuid %s" % (name, uuid))
        self.refresh()


class UINvmeBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "NVMe", parent)

    def ui_command_create(self, name, trtype, traddr,
                          adrfam=None, trsvcid=None, subnqn=None):

        if "rdma" in trtype and None in [adrfam, trsvcid, subnqn]:
            self.shell.log.error("Using RDMA transport type."
                                 "Please provide arguments for adrfam, trsvcid and subnqn.")

        ret_name = self.get_root().create_nvme_bdev(name=name, trtype=trtype,
                                                    traddr=traddr, adrfam=adrfam,
                                                    trsvcid=trsvcid, subnqn=subnqn)
        name = ret_name
        self.shell.log.info("Created NVME bdev: %s" % name)
        self.refresh()


class UIVhosts(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "vhost", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        self.get_root().list_vhost_ctrls()
        UIVhostBlk(self)
        UIVhostScsi(self)

    def ui_command_delete(self, name):
        """
        Delete a Vhost SCSI controller from configuration.

        Arguments:
        name - Controller name.
        """
        self.get_root().remove_vhost_controller(ctrlr=name)
        self.refresh()


class UIVhost(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)
        self.refresh()

    def refresh(self):
        pass

    def ui_command_delete(self, name):
        """
        Delete a Vhost SCSI controller from configuration.

        Arguments:
        name - Controller name.
        """
        self.get_root().remove_vhost_controller(ctrlr=name)
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
        ret_name = self.get_root().create_vhost_blk_controller(ctrlr=name,
                                                               dev_name=bdev,
                                                               cpumask=cpumask,
                                                               readonly=bool(readonly))
        self.shell.log.info("Created Vhost BLK controller: %s" % ret_name)
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
        """
        ret_name = self.get_root().create_vhost_scsi_controller(ctrlr=name,
                                                                cpumask=cpumask)
        self.shell.log.info("Created Vhost SCSI controller: %s" % ret_name)
        self.refresh()


class UIVhostScsiCtrlObj(UINode):
    def __init__(self, ctrlr, parent):
        self.ctrlr = ctrlr
        UINode.__init__(self, self.ctrlr.ctrlr, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for lun in self.ctrlr.backend_specific["scsi"]:
            UIVhostTargetObj(lun, self)

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.ctrlr), indent=2))

    def ui_command_remove_target(self, target_num):
        """
        Remove target node from SCSI controller.

        Arguments:
        target_num - Integer identifier of target node to delete.
        """
        _ = self.get_root().remove_vhost_scsi_target(ctrlr=self.ctrlr.ctrlr,
                                                     scsi_target_num=int(target_num))
        # self.shell.log.info("Removed target % from SCSI controller %s" % (_, self.ctrlr.ctrlr))
        self.refresh()

    def ui_command_add_lun(self, target_num, bdev_name):
        """
        Add LUN to SCSI target node.
        Currently only 1 LUN per target is supported.
        Adding LUN to unexisting target node will create that node.

        Arguments:
        target_num - Integer identifier of target node to modify.
        bdev - Which bdev to add as LUN.
        """

        self.get_root().add_vhost_scsi_lun(ctrlr=self.ctrlr.ctrlr,
                                           scsi_target_num=int(target_num),
                                           bdev_name=bdev_name)
        # TODO: Node won't refresh unless we refresh the parent node. Fix that.
        self.parent().refresh()
        self.refresh()

    def summary(self):
        # TODO: Maybe print socket path instead of ReadOnly or target count?
        cpumask = "CpuMask=%s" % self.ctrlr.cpumask
        msg = "Targets: %s" % len(self.ctrlr.backend_specific["scsi"])
        info = ", ".join([cpumask, msg])
        return info, True


class UIVhostBlkCtrlObj(UINode):
    def __init__(self, ctrlr, parent):
        self.ctrlr = ctrlr
        UINode.__init__(self, self.ctrlr.ctrlr, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        UIVhostLunDevObj(self.ctrlr.backend_specific["block"]["bdev"], self)

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.ctrlr), indent=2))

    def summary(self):
        # TODO: Maybe print socket path instead of ReadOnly or target count?
        cpumask = "CpuMask=%s" % self.ctrlr.cpumask
        ro = None
        if self.ctrlr.backend_specific["block"]["readonly"]:
            ro = "Readonly"
        info = ", ".join(filter(None, [cpumask, ro]))
        return info, True


class UIVhostTargetObj(UINode):
    def __init__(self, target, parent):
        # Next line: configshell does not allow paths with spaces.
        UINode.__init__(self, target["target_name"].replace(" ", "_"), parent)
        self.target = target
        self._children = set([])
        for target in target["luns"]:
            UIVhostLunDevObj(target["bdev_name"], self)

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(self.target, indent=2))

    def summary(self):
        luns = "LUNs: %s" % len(self.target["luns"])
        id = "TargetID: %s" % self.target["id"]
        info = ",".join([luns, id])
        return info, True


class UIVhostLunDevObj(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)


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

        info = ", ".join(filter(None, [alias, size, in_use]))
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
