#!/usr/bin/env python

import json
import linecache
from subprocess import check_output
import sys
import re
from types import *

rpc_param = {
    'malloc_bdev_size': 64,
    'malloc_block_size': 512,
    'ctrlr': unicode('vhost.0'),
    'scsi_devs': [],
    'cpu_mask': unicode('0x1'),
    'scsi_dev_num': 0,
    'claimed': False
}




if __name__ == "__main__":

    rpc_py = sys.argv[1]
# todo
