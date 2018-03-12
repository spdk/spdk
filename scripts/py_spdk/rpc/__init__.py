import app
import bdev
from client import print_dict
import iscsi
import log
import lvol
import nbd
import net
import nvmf
import pmem
import subsystem
import vhost


def get_rpc_methods(args):
    print_dict(args.client.call('get_rpc_methods'))
