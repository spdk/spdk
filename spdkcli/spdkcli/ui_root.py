from ui_node import UINode
from ui_bdevs import UIBdevs

class UIRoot(UINode):
    '''
    The targetcli hierarchy root node.
    '''
    def __init__(self, shell, as_root=False):
        UINode.__init__(self, '/', shell=shell)
        self.as_root = as_root

    def refresh(self):
        '''
        Refreshes the tree of target fabric modules.
        '''
        print("ROOT REFRESH")
        UIBdevs(self)

    def test(self):
        print("AAA")
