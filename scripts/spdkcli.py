#!/usr/bin/env python3
import sys
import argparse
import configshell_fb
import threading
from os import getuid
from rpc.client import JSONRPCException
from configshell_fb import ConfigShell, shell, ExecutionError
from spdkcli import UIRoot, UIBdevObj, Bdev
import rpc.client
from pyparsing import (alphanums, Optional, Suppress, Word, Regex,
                       removeQuotes, dblQuotedString, OneOrMore)


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


class NotificationThread(threading.Thread):

    def __init__(self, args, root_node):
        threading.Thread.__init__(self)
        self.args = args
        self.root_node = root_node
        self.is_notify_listen = False
        self.verbose = False

    """
    Supported notification list
    
    def _notify_<notification_method>(self, notification)

    """
    def _notify_delete_malloc_bdev(self, notification):

        if self.verbose:
            print("Received notification:")
            print(notification['params'])

        node = self.root_node.get_node("/bdevs/malloc/" + notification['params']['name'])
        node.parent.remove_child(node)

    def _notify_construct_malloc_bdev(self, notification):

        if self.verbose:
            print("Received notification:")
            print(notification['params'])

        node = self.root_node.get_node("/bdevs/malloc/")

        # FIXIT: This is stub, propably notification should be extended with
        #        undefined parameters or we should to fetch full information
        #        about bdev with get_bdevs(name)
        notification['params']['claimed'] = False
        notification['params']['aliases'] = []

        UIBdevObj(Bdev(notification['params']), node)

    def process_notification(self, notification):
        with self.root_node.lock:
            try:
                getattr(self, "_notify_" + notification['method'])(notification)
            except AttributeError as e:
                print("Unsupported notification: %s" % notification)
            except Exception as e:
                print(e)

    def run(self):
        global is_notify_listen
        notify_ids = set()
        self.is_notify_listen = True
        with rpc.client.JSONRPCClient(self.args.socket) as client:
            notify_ids.add(client.add_request("get_notifications", {'max_count': 10, 'timeout_ms': 10000}))
            while self.is_notify_listen:
                notify_ids.add(client.add_request("get_notifications", {'max_count': 10, 'timeout_ms': 5000}))
                client.flush()
                try:
                    response = client.recv()
                    if response['id'] not in notify_ids:
                        # We didn't should to receive here responses for other
                        # requests than get_notifications
                        print("Wrong notification id %s" % response['id'])
                        if self.verbose:
                            print(response)
                    else:
                        for notification in response['result']:
                            self.process_notification(notification)
                    # TODO: remove id from the list
                except Exception as e:
                    print(e)


def main():
    """
    Start SPDK CLI
    :return:
    """
    spdk_shell = ConfigShell("~/.scripts")
    spdk_shell.interactive = True
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
        try:
            spdk_shell.interactive = False
            spdk_shell.run_cmdline(" ".join(args.commands))
        except Exception as e:
            sys.stderr.write("%s\n" % e)
            sys.exit(1)
        sys.exit(0)

    # Start notification thread
    thread = NotificationThread(args, root_node)
    thread.verbose = args.verbose
    thread.start()

    spdk_shell.con.display("SPDK CLI v0.1")
    spdk_shell.con.display("")
    while not spdk_shell._exit:
        try:
            spdk_shell.run_interactive()
        except (JSONRPCException, ExecutionError) as e:
            spdk_shell.log.error("%s" % e)

    thread.is_notify_listen = False
    thread.join()


if __name__ == "__main__":
    main()
