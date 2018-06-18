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
        UIMallocBdev(self)
        UIAIOBdev(self)
        UILvolBdev(self)
        UINvmeBdev(self)
        UINullBdev(self)
        UIErrorBdev(self)
        UISplitBdev(self)
        UIPmemBdev(self)
        UIRbdBdev(self)

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

    def ui_command_delete(self, name):
        """
        Deletes bdev from configuration.

        Arguments:
        name - Is a unique identifier of the bdev to be deleted - UUID number or name alias.
        """
        self.get_root().delete_bdev(name=name)
        self.get_root().refresh()
        self.refresh()

    def ui_command_get_bdev_iostat(self, name=None):
        if name is None:
            ret = self.get_root().get_bdevs_iostat()
        else:
            ret = self.get_root().get_bdevs_iostat(name=name)
        self.shell.log.info(json.dumps(ret, indent=2))

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

        ret_name = self.get_root().split_bdev(base_bdev=base_bdev,
                                              split_count=split_count,
                                              split_size_mb=split_size_mb)
        self.shell.log.info(ret_name)
        self.parent.refresh()
        self.refresh()

    def ui_command_destruct_split_bdev(self, base_bdev):
        """Destroy split block devices associated with base bdev.

        Args:
            base_bdev: name of previously split bdev
        """
        self.get_root().destruct_split_bdev(base_bdev=base_bdev)
        self.parent.refresh()
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

        ret_name = self.get_root().create_malloc_bdev(num_blocks=size * 1024 * 1024 // block_size,
                                                      block_size=block_size,
                                                      name=name, uuid=uuid)
        self.shell.log.info(ret_name)
        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes malloc bdev from configuration.

        Arguments:
        name - Is a unique identifier of the malloc bdev to be deleted - UUID number or name alias.
        """
        self.get_root().delete_malloc_bdev(name=name)
        self.get_root().refresh()
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
        self.shell.log.info(ret_name)
        self.get_root().refresh()
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
        size *= (1024 * 1024)
        thin_provision = self.ui_eval_param(thin_provision, "bool", False)

        ret_uuid = self.get_root().create_lvol_bdev(lvol_name=name, size=size,
                                                    lvs_name=lvs_name, uuid=uuid,
                                                    thin_provision=thin_provision)
        self.shell.log.info(ret_uuid)
        self.get_root().refresh()
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
        self.shell.log.info(ret_name)
        self.get_root().refresh()
        self.refresh()


class UINullBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Null", parent)

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

        ret_name = self.get_root().create_null_bdev(num_blocks=num_blocks,
                                                    block_size=block_size,
                                                    name=name, uuid=uuid)
        self.shell.log.info(ret_name)
        self.get_root().refresh()
        self.refresh()


class UIErrorBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Error", parent)

    def ui_command_create(self, base_name):
        """
        Construct a error injection bdev.

        Arguments:
        base_name - base bdev name on top of which error bdev will be created.
        """

        self.get_root().create_error_bdev(base_name=base_name)
        self.get_root().refresh()
        self.refresh()


class UISplitBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Split_Disk", parent)


class UIPmemBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "pmemblk", parent)

    def ui_command_create_pmem_pool(self, pmem_file, total_size, block_size):
        total_size = self.ui_eval_param(total_size, "number", None)
        block_size = self.ui_eval_param(block_size, "number", None)
        num_blocks = int((total_size * 1024 * 1024) / block_size)

        self.get_root().create_pmem_pool(pmem_file=pmem_file,
                                         num_blocks=num_blocks,
                                         block_size=block_size)

    def ui_command_delete_pmem_pool(self, pmem_file):
        self.get_root().delete_pmem_pool(pmem_file=pmem_file)

    def ui_command_info_pmem_pool(self, pmem_file):
        ret = self.get_root().delete_pmem_pool(pmem_file=pmem_file)
        self.shell.log.info(ret)

    def ui_command_create(self, pmem_file, name):
        ret_name = self.get_root().create_pmem_bdev(pmem_file=pmem_file,
                                                    name=name)
        self.shell.log.info(ret_name)
        self.get_root().refresh()
        self.refresh()


class UIRbdBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Rbd", parent)

    def ui_command_create(self, pool_name, rbd_name, block_size, name=None):
        block_size = self.ui_eval_param(block_size, "number", None)

        ret_name = self.get_root().create_rbd_bdev(pool_name=pool_name,
                                                   rbd_name=rbd_name,
                                                   block_size=block_size,
                                                   name=name)
        self.shell.log.info(ret_name)
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
