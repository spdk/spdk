from configshell import ConfigNode, ExecutionError

class UINode(ConfigNode):
    '''
    Our targetcli basic UI node.
    '''
    def __init__(self, name, parent=None, shell=None):
        ConfigNode.__init__(self, name, parent, shell)

    def refresh(self):
        '''
        Refreshes and updates the objects tree from the current path.
        '''
        print("NODE REFRESHING")

    def ui_command_test(self):
        print("AAABB")