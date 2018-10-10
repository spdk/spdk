#!/usr/bin/env python3

from subprocess import check_output
import os
import sys
import argparse
import itertools
import time
from common import *

global script_dir
global spdk_dir

# SPDK Initiator functions
def spdk_init_gen_bdev_conf(address_list=None):
    address_list = address_list.split(",")
    header = "[Nvme]"
    row_template = """  TransportId "trtype:RDMA adrfam:IPv4 traddr:{ip} trsvcid:{svc} subnqn:{nqn}" Nvme{i}"""
    subsystems = discover_subsystems(address_list)

    bdev_rows = [row_template.format(svc=x[0],
                                     nqn=x[1],
                                     ip=x[2],
                                     i=i) for i, x in enumerate(subsystems)]
    bdev_rows = "\n".join(bdev_rows)
    bdev_section = "\n".join([header, bdev_rows])

    return bdev_section


def spdk_init_gen_fio_conf(address_list, num_cpus=None):
    address_list = address_list.split(",")
    subsystems = discover_subsystems(address_list)
    subsystems = [str(x) for x in range(0, len(subsystems))]

    if num_cpus and num_cpus not in "all":
        threads = range(0, int(num_cpus))
    else:
        threads = range(0, len(subsystems))
    n = int(len(subsystems) / len(threads))

    filename_section = ""
    for t in threads:
        header = "[filename%s]" % t
        disks = "\n".join(["filename=Nvme%sn1" % x for x in subsystems[n*t:n+n*t]])
        filename_section = "\n".join([filename_section, header, disks])

    return filename_section


def spdk_init_gen_config(rw, rwmixread, block_size, iodepth, ramp_time, run_time, address, cpus_num):
    fio_conf_template = """
[global]
ioengine=examples/bdev/fio_plugin/fio_plugin
spdk_conf=bdev.conf
thread=1
group_reporting=1
direct=1

norandommap=1
rw={rw}
rwmixread={rwmixread}
bs={block_size}
iodepth={iodepth}
time_based=1
ramp_time={ramp_time}
runtime={run_time}

        """
    with open("fio.conf", "w") as fh:
        fh.write(fio_conf_template.format(rw=rw,
                                          rwmixread=rwmixread,
                                          block_size=block_size,
                                          iodepth=iodepth,
                                          ramp_time=ramp_time,
                                          run_time=run_time))

    bdevs = spdk_init_gen_bdev_conf(address_list=address)
    with open("bdev.conf", "w") as fh:
        fh.write(bdevs)

    fio = spdk_init_gen_fio_conf(address_list=address, num_cpus=cpus_num)
    with open("fio.conf", "a") as fh:
        fh.write(fio)


def spdk_init_start(args):
    block_sizes = args.block_size.split(",")
    iodepths = args.iodepth.split(",")
    rws = args.rw.split(",")
    if args.cpus_num:
        cpus_num = args.cpus_num.split(",")
    else:
        cpus_num = None

    for block_size, iodepth, rw, cpu_num in itertools.product(block_sizes, iodepths, rws, cpus_num):
        print("Running config:", block_size, iodepth, rw, cpu_num, args.rwmixread, args.ramp_time, args.run_time)
        spdk_init_gen_config(rw=rw,
                             rwmixread=args.rwmixread,
                             block_size=block_size,
                             iodepth=iodepth,
                             ramp_time=args.ramp_time,
                             run_time=args.run_time,
                             address=args.address,
                             cpus_num=cpu_num)
        run_fio(block_size, iodepth, rw, args.rwmixread, cpu_num, args.run_num, args.output_dir)
        parse_results(args.output_dir)

# Kernel Initiator functions

def kernel_init_connect(address_list):
    subsystems = discover_kernel_subsystems(address_list)
    print("Below connection attempts may result in error messages, this is expected!")
    for subsystem in subsystems:
        print("Trying to connect %s %s %s" % subsystem)
        cmd = "nvme connect -t rdma -s %s -n %s -a %s" % subsystem
        output = check_output(cmd, shell=True).decode(encoding="utf-8")
        time.sleep(1)


def kernel_init_disconnect(address_list):
    subsystems = discover_kernel_subsystems(address_list)
    for subsystem in subsystems:
        cmd = "nvme disconnect -n %s" % subsystem[1]
        output = check_output(cmd, shell=True).decode(encoding="utf-8")


def kernel_init_gen_fio_conf(rw, rwmixread, block_size, iodepth, ramp_time, run_time):
    fio_conf_template = """[global]
ioengine=libaio
thread=1
group_reporting=1
direct=1

norandommap=1
rw={rw}
rwmixread={rwmixread}
bs={block_size}
iodepth={iodepth}
time_based=1
ramp_time={ramp_time}
runtime={run_time}
  """

    fio_conf = fio_conf_template.format(rw=rw,
                                        rwmixread=rwmixread,
                                        block_size=block_size,
                                        iodepth=iodepth,
                                        ramp_time=ramp_time,
                                        run_time=run_time)

    nvme_list = get_nvme_devices()
    for i, nvme in enumerate(nvme_list):
        fio_conf = "\n".join([fio_conf,
                              "[filename%s]" % i,
                              "filename=%s" % nvme])

    with open("fio.conf", "w") as fh:
        fh.write(fio_conf)


def kernel_init_run(args):
    block_sizes = args.block_size.split(",")
    iodepths = args.iodepth.split(",")
    rws = args.rw.split(",")
    address_list = args.address.split(",")

    try:
        kernel_init_connect(address_list)
        for block_size, iodepth, rw in itertools.product(block_sizes, iodepths, rws):
            print("Running config:", block_size, iodepth, rw, args.rwmixread, args.ramp_time, args.run_time)
            kernel_init_gen_fio_conf(rw=rw,
                                     rwmixread=args.rwmixread,
                                     block_size=block_size,
                                     iodepth=iodepth,
                                     ramp_time=args.ramp_time,
                                     run_time=args.run_time)
            run_fio(block_size, iodepth, rw, args.rwmixread, "all", args.run_num, args.output_dir)
    finally:
        kernel_init_disconnect(address_list)

    parse_results(args.output_dir)



script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
spdk_dir = os.path.abspath(os.path.join(script_dir, "../../../"))

parser = argparse.ArgumentParser()
subparsers = parser.add_subparsers()
subparser_spdk_init = subparsers.add_parser('spdk_init', help="Create Bdev and Fio config files and"
                                                              "run SPDK NVMeOF Initiator.")

subparser_spdk_init.add_argument("-a", "--address", type=str,
                                 help="Comma separated list of IP addresses of subsystems"
                                      "to which initiator should connect to.")
subparser_spdk_init.add_argument("-b", "--block-size", type=str, default="4k",
                                 help="Comma separated list of Block sizes to use in FIO workload.")
subparser_spdk_init.add_argument("-i", "--iodepth", type=str, default="8",
                                 help="Comma separated list of  IODepth values to use in FIO workload.")
subparser_spdk_init.add_argument("-r", "--rw", type=str, default="1",
                                 help="Comma separated list of RW modes to use in FIO workload.")
subparser_spdk_init.add_argument("-m", "--rwmixread", type=str, default="1",
                                 help="Percentage of read operations to use in readwrite operations.")
subparser_spdk_init.add_argument("-t", "--run-time", type=str, default="10",
                                 help="Run time (in seconds) to run FIO workload.")
subparser_spdk_init.add_argument("-T", "--ramp-time", type=str, default="10",
                                 help="Ramp time (In seconds) to run FIO workload.")
subparser_spdk_init.add_argument("-c", "--cpus-num", type=str, default="all",
                                 help="Comma separated list of max number of CPUs to use for FIO workload"
                                      "By default each discovered remote subsystem will get it's own CPU.")
subparser_spdk_init.add_argument("-n", "--run-num", type=int, default=1,
                                 help="Number of times to run given workload. If >1 then average results will be"
                                      "calculated.")
subparser_spdk_init.add_argument("-o", "--output-dir", type=str, default="",
                                 help="Output directory to save output files.")
subparser_spdk_init.set_defaults(func=spdk_init_start)


subparser_kernel_init = subparsers.add_parser('kernel_init', help="Create Bdev and Fio config files and"
                                                                  "run SPDK NVMeOF Initiator.")
subparser_kernel_init.add_argument("-a", "--address", type=str,
                                   help="Comma separated list of IP addresses of subsystems"
                                        "to which initiator should connect to.")
subparser_kernel_init.add_argument("-b", "--block-size", type=str, default="4k",
                                   help="Comma separated list of Block sizes to use in FIO workload.")
subparser_kernel_init.add_argument("-i", "--iodepth", type=str, default="8",
                                   help="Comma separated list of  IODepth values to use in FIO workload.")
subparser_kernel_init.add_argument("-r", "--rw", type=str, default="randread",
                                   help="Comma separated list of RW modes to use in FIO workload.")
subparser_kernel_init.add_argument("-m", "--rwmixread", type=str, default="70",
                                   help="Percentage of read operations to use in readwrite operations.")
subparser_kernel_init.add_argument("-t", "--run-time", type=str, default="10",
                                   help="Run time (in seconds) to run FIO workload.")
subparser_kernel_init.add_argument("-T", "--ramp-time", type=str, default="10",
                                   help="Ramp time (In seconds) to run FIO workload.")
subparser_kernel_init.add_argument("-n", "--run-num", type=int, default=1,
                                   help="Number of times to run given workload."
                                        "If >1 then average results will be calculated.")
subparser_kernel_init.add_argument("-o", "--output-dir", type=str, default="",
                                   help="Output directory to save output files.")
subparser_kernel_init.set_defaults(func=kernel_init_run)


args = parser.parse_args()
args.func(args)
