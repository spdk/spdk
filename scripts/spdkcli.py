#!/usr/bin/env python3
import sys
import argparse
import configshell_fb
from os import getuid
from configshell_fb import ConfigShell, shell
from spdkcli import UIRoot
from pyparsing import (alphanums, Optional, Suppress, Word,
                       removeQuotes, dblQuotedString, OneOrMore)


def add_quotes_to_shell(spdk_shell):
    value = dblQuotedString.addParseAction(removeQuotes)
    value_word = Word(alphanums + ';,=_\+/.<>()~@:-%[]')
    keyword = Word(alphanums + '_\-')
    kparam = shell.locatedExpr(keyword + Suppress('=') + \
                               Optional(value | value_word, default=''))('kparams*')
    pparam = shell.locatedExpr(value | value_word)('pparams*')
    parameters = OneOrMore(kparam | pparam)
    spdk_shell._parser = spdk_shell._parser + Optional(parameters)


def main():
    """
    Start SPDK CLI
    :return:
    """
    spdk_shell = ConfigShell("~/.scripts")
    add_quotes_to_shell(spdk_shell)

    parser = argparse.ArgumentParser(description="SPDK command line interface")
    parser.add_argument("-s", dest="socket", help="RPC socket path", default="/var/tmp/spdk.sock")
    parser.add_argument("-v", dest="verbose", help="Print request/response JSON for configuration calls",
                        default=False, action="store_true")
    parser.add_argument("commands", metavar="command", type=str, nargs="*", default="",
                        help="commands to execute by SPDKCli as one-line command")
    args = parser.parse_args()

    root_node = UIRoot(args.socket, spdk_shell)
    root_node.verbose = args.verbose
    try:
        root_node.refresh()
    except BaseException:
        pass

    if len(args.commands) > 0:
        shell.run_cmdline(" ".join(args.commands))
        sys.exit(0)

    spdk_shell.con.display("SPDK CLI v0.1")
    spdk_shell.con.display("")
    spdk_shell.run_interactive()


if __name__ == "__main__":
    main()
