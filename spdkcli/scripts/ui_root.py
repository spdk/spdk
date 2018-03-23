from .ui_node import UINode, UIBdevs, UILvolStores


class UIRoot(UINode):
    """
    Root node for CLI menu tree structure. Refreshes running config on startup.
    """
    def __init__(self, shell, as_root=False):
        UINode.__init__(self, '/', shell=shell)
        self.as_root = as_root

    def refresh(self):
        self._children = set([])
        UIBdevs(self)
        UILvolStores(self)
