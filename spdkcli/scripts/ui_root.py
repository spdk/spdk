from .ui_node import UINode, UIBdevs, UIVhost


class UIRoot(UINode):
    def __init__(self, shell, as_root=False):
        UINode.__init__(self, '/', shell=shell)
        self.as_root = as_root

    def refresh(self):
        self._children = set([])
        UIBdevs(self)
        UIVhost(self)
