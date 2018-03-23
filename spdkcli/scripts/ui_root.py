from .ui_node import UINode, UIBdevs, UILvolStores


class UIRoot(UINode):
    def __init__(self, shell, as_root=False):
        UINode.__init__(self, '/', shell=shell)
        self.as_root = as_root

    def refresh(self):
        self._children = set([])
        UIBdevs(self)
        UILvolStores(self)
