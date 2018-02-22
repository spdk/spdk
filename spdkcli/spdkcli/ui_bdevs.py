from ui_node import UINode

class UIBdevs(UINode):
    def __init__(self, parent):
        UINode.__init__(self, 'bdevs', parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        print("BDEV REFRESH")