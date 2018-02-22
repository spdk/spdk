from ui_node import UINode

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
        self._children = set([])
        if self.shell.prefs['legacy_hba_view']:
            UIBackstoresLegacy(self)
        else:
            UIBackstores(self)

        for fabric_module in RTSRoot().fabric_modules:
            self.shell.log.debug("Using fabric module %s." % fabric_module.name)
            UIFabricModule(fabric_module, self)