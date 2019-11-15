#!/usr/bin/env python3
import logging
import os
import re
import sys
import json
import paramiko
import zipfile
import threading
import subprocess
import itertools
import time
import uuid
import rpc
import rpc.client
from common import *

script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
extract_dir = os.path.dirname(script_dir)
sys.path.append(os.path.join(extract_dir, 'common'))
from test_common import *
now = int(round(time.time() * 1000))
timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now / 1000))
timestamp = timestamp.replace(' ', '_')
timestamp = timestamp.replace(':', '_')





def get_config():
    global config_file_path
    if (len(sys.argv) > 1):
        config_file_path = sys.argv[1]
    else:
        script_full_dir = os.path.dirname(os.path.realpath(__file__))
        config_file_path = os.path.join(script_full_dir, "config.json")
    print("Using config file: %s" % config_file_path)
    return config_file_path


if __name__ == '__main__':
    print(1111111111111111111)
    config_file_path = get_config()

    with open(config_file_path, "r") as config:
        data = json.load(config)

    log_name = "filesys_" + timestamp + ".log"
    log_file_name = '/home/filesystem_log/'+log_name

    initiators = []

    for k, v in data.items():
        if "target" in k:
            target_obj = SPDKTarget(name=k, **data["general"], **v, log_file_name=log_file_name)

        elif "initiator" in k:
            init_obj = SPDKInitiator(name=k, **data["general"], **v, log_file_name=log_file_name)
            initiators.append(init_obj)
        else:
            continue

    target_obj.tgt_start()

    threads = []
    print(target_obj.subsys_no,666666666666666666666)
    for i in initiators:
        n = 1
        t = threading.Thread(target=i.discover_connect, args=(i.nic_ips, target_obj.subsys_no, n, len(initiators)))
        threads.append(t)
        t.start()
        n += 1
    for t in threads:
        t.join()

    filesys_threads = []
    for i in initiators:
        t = threading.Thread(target=i.filesys_test, args=(1,))
        filesys_threads.append(t)
        t.start()
    for t in threads:
        t.join()
    #time.sleep(100000)
