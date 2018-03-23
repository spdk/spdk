#!/usr/bin/env python
import sys
from os import getuid
from configshell_fb import ConfigShell
from scripts import UIRoot


def main():
    """
    Start SPDK CLI
    :return:
    """
    shell = ConfigShell('~/.scripts')

    if getuid() != 0:
        shell.con.display("You are not root, disabling privileged commands.\n")
        sys.exit(1)

    root_node = UIRoot(shell, as_root=True)
    try:
        root_node.refresh()
    except:
        pass

    shell.con.display("SPDK CLI v0.1")
    shell.con.display('')
    shell.run_interactive()


if __name__ == "__main__":
    main()
