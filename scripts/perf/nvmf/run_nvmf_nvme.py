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
extract_dir = os.path.abspath(os.path.join(script_dir, "../../"))
spdk_dir = os.path.abspath(os.path.join(script_dir, "../../../"))
sys.path.append(os.path.join(extract_dir, 'common'))
from test_common import *
now = int(round(time.time() * 1000))
timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now / 1000))
timestamp = timestamp.replace(' ', '_')
timestamp = timestamp.replace(':', '_')




if __name__ == "__main__":
    spdk_zip_path = "/tmp/spdk.zip"
    target_results_dir = "/tmp/results"
    if not os.path.exists("/tmp/results"):
        out = subprocess.check_output("mkdir /tmp/results", shell=True).decode(encoding="utf-8")
    out = subprocess.check_output("mkdir /tmp/results/"+timestamp, shell=True).decode(encoding="utf-8")
    target_results_dir = target_results_dir + "/" + timestamp
    if (len(sys.argv) > 1):
        config_file_path = sys.argv[1]
    else:
        script_full_dir = os.path.dirname(os.path.realpath(__file__))
        config_file_path = os.path.join(script_full_dir, "config.json")

    print("Using config file: %s" % config_file_path)
    with open(config_file_path, "r") as config:
        data = json.load(config)

    log_name = "run_nvme_bdev_" + timestamp + ".log"
    log_file_name = '/home/nvme_bdev_log/' + log_name

    initiators = []
    fio_cases = []

    for k, v in data.items():
        if "target" in k:
            if data[k]["mode"] == "spdk":
                target_obj = SPDKTarget(name=k, **data["general"], **v, log_file_name=log_file_name)
            elif data[k]["mode"] == "kernel":
                target_obj = KernelTarget(name=k, **data["general"], **v, log_file_name=log_file_name)
        elif "initiator" in k:
            if data[k]["mode"] == "spdk":
                init_obj = SPDKInitiator(name=k, **data["general"], **v, log_file_name=log_file_name)
            elif data[k]["mode"] == "kernel":
                init_obj = KernelInitiator(name=k, **data["general"], **v, log_file_name=log_file_name)
            initiators.append(init_obj)
        elif "fio" in k:
            fio_workloads = itertools.product(data[k]["bs"],
                                              data[k]["qd"],
                                              data[k]["rw"])

            fio_rw_mix_read = data[k]["rwmixread"]
            fio_run_time = data[k]["run_time"] if "run_time" in data[k].keys() else 10
            fio_ramp_time = data[k]["ramp_time"] if "ramp_time" in data[k].keys() else 0
            fio_run_num = data[k]["run_num"] if "run_num" in data[k].keys() else None
            fio_num_jobs = data[k]["num_jobs"] if "num_jobs" in data[k].keys() else None
            fio_mem_size_mb = data[k]["mem_size_mb"] if "mem_size_mb" in data[k].keys() else None
            fio_size = data[k]["size"] if "size" in data[k].keys() else "8G"
        else:
            continue

    # Copy and install SPDK on remote initiators
    target_obj.zip_spdk_sources(target_obj.spdk_dir, spdk_zip_path)
    threads = []
    for i in initiators:
        if i.mode == "spdk":
            t = threading.Thread(target=i.install_spdk, args=(spdk_zip_path,))
            threads.append(t)
            t.start()
    for t in threads:
        t.join()

    target_obj.tgt_start()

    # Poor mans threading
    # Run FIO tests
    for block_size, io_depth, rw in fio_workloads:
        threads = []
        configs = []
        for i in initiators:
            if i.mode == "kernel":
                i.kernel_init_connect(i.nic_ips, target_obj.subsys_no)

            cfg = i.gen_fio_config(rw, fio_rw_mix_read, block_size, io_depth, target_obj.subsys_no,
                                   num_jobs=fio_num_jobs, ramp_time=fio_ramp_time, run_time=fio_run_time,
                                   fio_mem_size_mb=fio_mem_size_mb, fio_size=fio_size,
                                   )

            configs.append(cfg)

        for i, cfg in zip(initiators, configs):
            t = threading.Thread(target=i.run_fio, args=(cfg, fio_run_num))
            threads.append(t)
        if target_obj.enable_sar:
            sar_file_name = "_".join([str(block_size), str(rw), str(io_depth), "sar"])
            sar_file_name = ".".join([sar_file_name, "txt"])
            t = threading.Thread(target=target_obj.measure_sar, args=(target_results_dir, sar_file_name))
            threads.append(t)

        for t in threads:
            t.start()
        for t in threads:
            t.join()

        for i in initiators:
            if i.mode == "kernel":
                i.kernel_init_disconnect(i.nic_ips, target_obj.subsys_no)
            i.copy_result_files(target_results_dir)

    target_obj.parse_results(target_results_dir)
