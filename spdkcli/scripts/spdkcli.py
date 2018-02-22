#!/usr/bin/python

from os import getuid
from configshell_fb import ConfigShell

def main():
    """
    Start SPDK CLI
    :return:
    """
    shell = ConfigShell('~/.spdkcli')

    if getuid() != 0:
        shell.con.display("You are not root, disabling privileged commands.\n")

if __name__ == "__main__":
    main()
