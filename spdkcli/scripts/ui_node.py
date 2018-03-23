from configshell_fb import ConfigNode, ExecutionError
from .bk_end import BKRoot


class UINode(ConfigNode):
    def __init__(self, name, parent=None, shell=None):
        ConfigNode.__init__(self, name, parent, shell)

    def refresh(self):
        for child in self.children:
            child.refresh()

    def ui_command_refresh(self):
        self.refresh()

    def ui_command_ll(self, path=None, depth=None):
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

    def ui_command_refresh(self):
        self.refresh()

    def ui_command_delete(self, name):
        BKRoot.delete_bdev(name=name)
        self.refresh()


class UIBdev(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])

        for bdev in BKRoot.get_bdevs(self.name):
            UIBdevObj(bdev, self)

    def ui_command_delete(self, name):
        BKRoot.delete_bdev(name=name)
        self.refresh()

    def ui_command_refresh(self):
        self.refresh()


class UIMallocBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Malloc", parent)

    def summary(self):
        bdevs = len(self.children)
        return "Malloc bdevs: %d" % bdevs, None

    def ui_command_create(self, size, block_size, name=None, uuid=None):
        # TODO: Need to catch failed calls, modify rpc.client to do that!
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

    def summary(self):
        bdevs = len(self.children)
        return "AIO bdevs: %d" % bdevs, None


class UIBdevObj(UINode):
    def __init__(self, bdev, parent):
        UINode.__init__(self, bdev.name, parent)
        self.bdev = bdev

    def summary(self):
        # TODO: Size always in MB's for now, add util function for that
        # and discover when to use kB's, MB's or GB's
        size = self.bdev.block_size * self.bdev.num_blocks / 1024 / 1024
        in_use = "FREE"
        if bool(self.bdev.claimed):
            in_use = "IN USE"

        return "Size=%sM, %s" % (size, in_use), True
