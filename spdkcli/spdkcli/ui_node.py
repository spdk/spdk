from configshell import ConfigNode, ExecutionError

class UINode(ConfigNode):
    '''
    Our targetcli basic UI node.
    '''
    def __init__(self, name, parent=None, shell=None):
        ConfigNode.__init__(self, name, parent, shell)