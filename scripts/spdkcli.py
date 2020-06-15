#!/usr/bin/env python3
import sys
import argparse
from configshell_fb import ConfigShell, shell, ExecutionError
from pyparsing import (alphanums, Optional, Suppress, Word, Regex,
                       removeQuotes, dblQuotedString, OneOrMore)
from rpc.client import JSONRPCException, JSONRPCClient
from spdkcli import UIRoot


def add_quotes_to_shell(spdk_shell):
    command = shell.locatedExpr(Word(alphanums + '_'))('command')
    value = dblQuotedString.addParseAction(removeQuotes)
    value_word = Word(alphanums + r';,=_\+/.<>()~@:-%[]')
    keyword = Word(alphanums + r'_\-')
    kparam = shell.locatedExpr(keyword + Suppress('=') +
                               Optional(value | value_word, default=''))('kparams*')
    pparam = shell.locatedExpr(value | value_word)('pparams*')
    parameters = OneOrMore(kparam | pparam)
    bookmark = Regex(r'@([A-Za-z0-9:_.]|-)+')
    pathstd = Regex(r'([A-Za-z0-9:_.\[\]]|-)*' + '/' + r'([A-Za-z0-9:_.\[\]/]|-)*') \
        | '..' | '.'
    path = shell.locatedExpr(bookmark | pathstd | '*')('path')
    spdk_shell._parser = Optional(path) + Optional(command) + Optional(parameters)


def main():
    """
    Start SPDK CLI
    :return:
    """
    spdk_shell = ConfigShell("~/.scripts")
    spdk_shell.interactive = True
    add_quotes_to_shell(spdk_shell)

    parser = argparse.ArgumentParser(description="SPDK command line interface")
    parser.add_argument('-s', dest='server_addr',
                        help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=None, type=int)
    parser.add_argument("-v", dest="verbose", help="Print request/response JSON for configuration calls",
                        default=False, action="store_true")
    parser.add_argument("commands", metavar="command", type=str, nargs="*", default="",
                        help="commands to execute by SPDKCli as one-line command")
    args = parser.parse_args()

    try:
        client = JSONRPCClient(args.server_addr, port=args.port)
    except JSONRPCException as e:
        spdk_shell.log.error("%s. SPDK not running?" % e)
        sys.exit(1)

    with client:
        root_node = UIRoot(client, spdk_shell)
        root_node.verbose = args.verbose
        try:
            root_node.refresh()
        except BaseException:
            pass

        if args.commands:
            try:
                spdk_shell.interactive = False
                spdk_shell.run_cmdline(" ".join(args.commands))
            except Exception as e:
                sys.stderr.write("%s\n" % e)
                sys.exit(1)
            sys.exit(0)

        spdk_shell.con.display("SPDK CLI v0.1")
        spdk_shell.con.display("")

        while not spdk_shell._exit:
            try:
                spdk_shell.run_interactive()
            except (JSONRPCException, ExecutionError) as e:
                spdk_shell.log.error("%s" % e)
            except BrokenPipeError as e:
                spdk_shell.log.error("Lost connection with SPDK: %s" % e)


if __name__ == "__main__":
    main()
