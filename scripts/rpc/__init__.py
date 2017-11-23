import app
import bdev
import client
import hist
import iscsi
import log
import lvol
import nbd
import net
import nvmf
import pmem
import vhost


def get_rpc_methods(args):
    print_dict(args.client.call('get_rpc_methods'))
