from . import app
from . import bdev
from . import iscsi
from . import log
from . import lvol
from . import nbd
from . import net
from . import nvmf
from . import pmem
from . import subsystem
from . import vhost


def get_rpc_methods(args):
    return args.client.call('get_rpc_methods')
