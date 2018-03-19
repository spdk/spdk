import app
import bdev
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
    return args.client.call('get_rpc_methods')
