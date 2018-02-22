from configshell_fb import ConfigNode, ExecutionError
from .bk_end import RTSRoot


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


class UIBdevs(UINode):
    def __init__(self, parent):
        UINode.__init__(self, 'bdevs', parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        RTSRoot.list_bdevs()
        UIMallocBdev(self)
        UIAIOBdev(self)

    def ui_command_refresh(self):
        self.refresh()

    def ui_command_delete(self, name):
        RTSRoot.delete_bdev(name=name)
        self.refresh()


class UIBdev(UINode):
    def __init__(self, name, parent):
        UINode.__init__(self, name, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])

        # pprint.pprint(RTSRoot.current_bdevs)
        # for bdev in RTSRoot.current_bdevs:
        #     if self.name in bdev["product_name"]:
        #         UINode(bdev["name"], self)

        for bdev in RTSRoot.get_bdevs(self.name):
            UIBdevObj(bdev, self)

    def ui_command_delete(self, name):
        RTSRoot.delete_bdev(name=name)
        self.refresh()

    def ui_command_refresh(self):
        self.refresh()

    def ui_command_chld(self):
        for c in self.children:
            print(c, c.bdev.name)


class UIMallocBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "Malloc", parent)

    def summary(self):
        bdevs = len(self.children)
        return "Malloc bdevs: %d" % bdevs, None

    def ui_command_create(self, size=7, block_size=512, name=None, uuid=None):
        self.shell.log.info("AAA")
        RTSRoot.create_malloc_bdev(total_size=size, block_size=block_size,
                                   name=name, uuid=uuid)
        self.refresh()


class UIAIOBdev(UIBdev):
    def __init__(self, parent):
        UIBdev.__init__(self, "AIO", parent)

    def summary(self):
        bdevs = len(self.children)
        return "AIO bdevs: %d" % bdevs, None


class UIVhost(UINode):
    def __init__(self, parent):
        UINode.__init__(self, 'vhost', parent)


class UIBdevObj(UINode):
    def __init__(self, bdev, parent):
        UINode.__init__(self, bdev.name, parent)
        self.bdev = bdev

    def summary(self):
        size = self.bdev.block_size * self.bdev.num_blocks / 1024 / 1024
        in_use = "FREE"
        if bool(self.bdev.claimed):
            in_use = "IN USE"

        return "Size=%sM, %s" % (size, in_use), True
