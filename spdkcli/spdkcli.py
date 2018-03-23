#!/usr/bin/env python
import sys
import argparse
from os import getuid
from configshell_fb import ConfigShell
from scripts import UIRoot
from scripts import BKRoot


def main():
    """
    Start SPDK CLI
    :return:
    """
    shell = ConfigShell('~/.scripts')

    if getuid() != 0:
        shell.con.display("You are not root, exiting.\n")
        sys.exit(1)

    parser = argparse.ArgumentParser(description='SPDK command line interface')
    parser.add_argument('-s', dest='socket', help='RPC socket path', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port', help='RPC port', default=5260, type=int)
    parser.add_argument('commands', metavar='command', type=str, nargs='*', default="",
                        help='commands to execute by SPDKCli as one-line command')
    args = parser.parse_args()
    BKRoot.set_rpc_target(args.socket, args.port)

    root_node = UIRoot(shell, as_root=True)
    try:
        root_node.refresh()
    except:
        pass

    if len(args.commands) > 0:
        shell.run_cmdline(" ".join(args.commands))
        sys.exit(0)

    shell.con.display("SPDK CLI v0.1")
    shell.con.display('')
    shell.run_interactive()


if __name__ == "__main__":
    main()
