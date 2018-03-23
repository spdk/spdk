from configshell_fb import ConfigNode, ExecutionError
from .bk_end import BKRoot
from uuid import UUID


def convert_bytes_to_human(size):
    if not size:
        return ""
    for x in ['bytes', 'K', 'M', 'G', 'T']:
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
        UINode.__init__(self, 'bdevs', parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        BKRoot.list_bdevs()
        UIMallocBdev(self)
        UIAIOBdev(self)
        UILvolBdev(self)

    def ui_command_refresh(self):
        self.refresh()

    def ui_command_delete(self, name):
        """
        Deletes bdev from configuration.

        Arguments:
        name - Is a unique identifier of the bdev to be deleted - UUID number or name alias.
        """
        BKRoot.delete_bdev(name=name)
        self.refresh()


class UILvolStores(UINode):
    def __init__(self, parent):
        UINode.__init__(self, 'lvol_stores', parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        BKRoot.list_lvols()
        for lvs in BKRoot.get_lvol_stores():
            UILvsObj(lvs, self)

    def ui_command_create(self, name, bdev_name, cluster_size=None):
        """
        Creates logical volume store on target bdev.

        Arguments:
        name - Friendly name to use alongside with UUID identifier.
        bdev_name - On which bdev to create the lvol store.
        cluster_size - Cluster size to use when creating lvol store, in bytes. Default: 4194304.
        """
        if cluster_size:
            cluster_size = int(cluster_size)
        BKRoot.create_lvol_store(lvs_name=name, bdev_name=bdev_name, cluster_sz=cluster_size)
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
        BKRoot.delete_lvol_store(lvs_name=name, uuid=uuid)
        self.refresh()

    def summary(self):
        return "Lvol stores: %s" % len(self.children), None


class UIBdev(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for bdev in BKRoot.get_bdevs(self.name):
            UIBdevObj(bdev, self)

    def ui_command_delete(self, name):
        """
        Deletes bdev from configuration.

        Arguments:
        name - Is a unique identifier of the bdev to be deleted - UUID number or name alias.
        """
        BKRoot.delete_bdev(name=name)
        self.refresh()

    def ui_command_refresh(self):
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
        ret_name = BKRoot.create_malloc_bdev(total_size=int(size),
                                             block_size=int(block_size),
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
        block_size - Integer, block size to use when constructing bdev.
        filename - Path to AIO backend.
        """
        ret_name = BKRoot.create_aio_bdev(name=name,
                                          block_size=int(block_size),
                                          filename=filename)
        if name is None:
            name = ret_name
        self.shell.log.info("Created Malloc bdev: %s" % name)
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
        except:
            lvs_name = lvs

        if thin_provision:
            thin_provision = True

        ret_name = BKRoot.create_lvol_bdev(lvol_name=name, size=int(size),
                                           lvs_name=lvs_name, uuid=uuid,
                                           thin_provision=thin_provision)
        name, uuid = ret_name
        self.shell.log.info("Created lvol bdev: %s with uuid %s" % (name, uuid))
        self.refresh()


class UIBdevObj(UINode):
    def __init__(self, bdev, parent):
        name = bdev.name
        if len(bdev.aliases):
            name = bdev.aliases[0]
        UINode.__init__(self, name, parent)
        self.bdev = bdev

    def summary(self):
        size = convert_bytes_to_human(self.bdev.block_size * self.bdev.num_blocks)
        in_use = "Not claimed"
        if bool(self.bdev.claimed):
            in_use = "Claimed"
        return "Size=%s, %s" % (size, in_use), True


class UILvsObj(UINode):
    def __init__(self, lvs, parent):
        UINode.__init__(self, lvs.name, parent)
        self.lvs = lvs

    def summary(self):
        size = convert_bytes_to_human(self.lvs.total_data_clusters * self.lvs.cluster_size)
        free = convert_bytes_to_human(self.lvs.free_clusters * self.lvs.cluster_size)
        size = "=".join(["Size", size])
        free = "=".join(["Free", free])
        info = ", ".join([str(size), str(free)])
        return info, True
