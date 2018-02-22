#!/usr/bin/python

from os import getuid
from configshell_fb import ConfigShell
from ui_root import UIRoot

def main():
    """
    Start SPDK CLI
    :return:
    """
    shell = ConfigShell('~/.spdkcli')

    if getuid() != 0:
        shell.con.display("You are not root, disabling privileged commands.\n")

    root_node = UIRoot(shell, as_root=True)
    root_node.refresh()

    shell.con.display("SPDK CLI v0.1")
    shell.con.display('')
    shell.run_interactive()

if __name__ == "__main__":
    main()
