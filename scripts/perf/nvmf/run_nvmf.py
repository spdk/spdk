#!/usr/bin/env python3

from subprocess import check_call, call, check_output, Popen, PIPE
import subprocess
import os
import sys
import re
import argparse


# TODO: Common function with perf/nvme. Move to some common module.
def get_nvme_devices_count():
    output = check_output("lspci | grep -i Non | wc -l", shell=True)
    return int(output)


# TODO: Common function with perf/nvme. Move to some common module.
def get_nvme_devices_bdf():
    print("Getting BDFs for NVMe section")
    output = check_output("lspci | grep -i Non | awk '{print $1}'", shell=True)
    output = [str(x, encoding="utf-8") for x in output.split()]
    print("Done getting BDFs")
    return output


def add_nvme_to_spdk_tgt_conf(req_num_disks=None):
    print("Adding NVMe to config")
    header = """[Nvme]
  RetryCount 4
  Timeout 0
  ActionOnTimeout None
  AdminPollRate 100000
  HotplugEnable No
  """

    row_template = """  TransportId "trtype:PCIe traddr:{pci_addr}" Nvme{i}"""

    bdfs = get_nvme_devices_bdf()
    bdfs = [b.replace(":", ".") for b in bdfs]

    if req_num_disks:
        if req_num_disks > len(bdfs):
            print("ERROR: Requested number of disks is more than available %s" % len(bdfs))
            sys.exit(1)
        else:
            bdfs = bdfs[0:req_num_disks]

    nvme_section = [row_template.format(pci_addr=b, i=i) for i, b in enumerate(bdfs)]
    # join header and all NVMe entries into one string
    nvme_section = ("\n".join([header, "\n".join(nvme_section), "\n"]))

    print("Done adding NVMe to config")
    return str(nvme_section)


def add_subsystems_to_spdk_tgt_conf(address=None, req_num_disks=None):
    print("Adding subsystems to config")
    ips = address.split(",")
    if not req_num_disks:
        req_num_disks = get_nvme_devices_count()

    subsystem_template = """[Subsystem{sys_no}]
  NQN nqn.2018-09.io.spdk:cnode{sys_no}
  Listen RDMA {ip}:4420
  AllowAnyHost Yes
  SN SPDK000{sys_no}
  Namespace Nvme{nvme_no}n1 1
    """
    # Split disks between NIC IP's
    num_disks = range(1, req_num_disks + 1)
    disks_per_ip = int(len(num_disks) / len(ips))
    disk_chunks = [num_disks[i*disks_per_ip:disks_per_ip + disks_per_ip*i] for i in range(0, len(ips))]

    subsystems_section = ""
    for ip, chunk in zip(ips, disk_chunks):
        subsystems_chunk = [subsystem_template.format(sys_no=x,
                                                      nvme_no=x-1,
                                                      ip=ip) for x in chunk]
        subsystems_chunk = "\n".join(subsystems_chunk)
        subsystems_section = "\n".join([subsystems_section, subsystems_chunk])
    return str(subsystems_section)


def gen_spdk_tgt_config(address, core_mask="0x1"):
    print("Generating SPDK Target config file")

    # TODO: Improve core mask param
    # Rework to make it "num cores" and dynamically distribute between NUMA nodes if any used NICs or NVMes are not on the same node
    with open("scripts/perf/nvmf/spdk_tgt_template.conf", "r") as fh:
        global_section = fh.read()
        global_section = global_section.format(core_mask=core_mask)

    nvme_section = add_nvme_to_spdk_tgt_conf()
    subsystems_section = add_subsystems_to_spdk_tgt_conf(address)
    with open("./spdk_tgt.conf", "w") as config_fh:
        config_fh.write(global_section)
        config_fh.write(nvme_section)
        config_fh.write(subsystems_section)
    print("Done generating SPDK Target config file")


def start_spdk_nvmf_tgt(args):
    nvmf_app_path = "app/nvmf_tgt/nvmf_tgt"
    command = ["sudo", nvmf_app_path, "-c", "spdk_tgt.conf"]
    command = " ".join(command)

    gen_spdk_tgt_config(args.address, core_mask=args.core_mask)
    proc = subprocess.Popen(command, shell=True)
    proc.wait()


def discover_subsystems(address_list):
    nvme_discover_cmd = ["sudo", "nvme", "discover", "-t rdma", "-s 4420", "-a %s" % address_list[0]]
    nvme_discover_cmd = " ".join(nvme_discover_cmd)
    nvme_discover_output = check_output(nvme_discover_cmd, shell=True)

    # Call nvme discover and get trsvcid, traddr and subnqn fields. Filter out any not matching IP entries
    subsystems = re.findall(r'trsvcid:\s(\d+)\s+'  # get svcid number
                            r'subnqn:\s+([a-zA-Z0-9\.\-\:]+)\s+'  # get NQN id
                            r'traddr:\s+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})',  # get IP address
                            str(nvme_discover_output, encoding="utf-8"))  # from nvme discovery output

    # Filter out entries which ip's don't mach expected
    # x[-1] because last field is ip address
    subsystems = filter(lambda x: x[-1] in address_list, subsystems)
    subsystems = list(subsystems)

    return subsystems


def gen_spdk_bdev_init_config(address_list=None):
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


def gen_spdk_init_fio_config(address_list, num_cpus=None):
    address_list = address_list.split(",")
    subsystems = discover_subsystems(address_list)
    subsystems = [str(x) for x in range(0, len(subsystems))]

    if num_cpus:
        threads = range(0, num_cpus)
    else:
        threads = range(0, len(subsystems))
    n = int(len(subsystems) / len(threads))

    filename_section = ""
    for t in threads:
        header = "[filename%s]" % t
        disks = "\n".join(["filename=Nvme%sn1" % x for x in subsystems[n*t:n+n*t]])
        filename_section = "\n".join([filename_section, header, disks])

    return filename_section


def run_fio(block_size, iodepth, rw, rw_mix, cpus_num, run_num):
    print("Running FIO test with args:")

    for i in range(1, run_num + 1):
        output_file = "s_" + str(block_size) + "_q_" + str(iodepth) + "_m_" + str(rw) + "_c_" + str(cpus_num) + "_run_" + str(i) + ".json"
        command = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % ("fio.conf", output_file)

        proc = subprocess.Popen(command, shell=True)
        proc.wait()

    print("Finished Test: IO Size={} QD={} Mix={} CPU Mask={}".format(block_size, iodepth, rw_mix, cpus_num))
    return


def parse_results():
    pass


# def gen_spdk_init_config(address="", block_size="16k", iodepth=8, rw="rw", rwmixread=0, run_time=10, ramp_time=10, cpus_num=None):
def gen_spdk_init_config(args):
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
        fh.write(fio_conf_template.format(rw=args.rw,
                                          rwmixread=args.rwmixread,
                                          block_size=args.block_size,
                                          iodepth=args.iodepth,
                                          ramp_time=args.ramp_time,
                                          run_time=args.run_time))

    bdevs = gen_spdk_bdev_init_config(address_list=args.address)
    with open("bdev.conf", "w") as fh:
        fh.write(bdevs)

    fio = gen_spdk_init_fio_config(address_list=args.address, num_cpus=args.cpus_num)
    with open("fio.conf", "a") as fh:
        fh.write(fio)

    run_fio(args.block_size, args.iodepth, args.rw, args.rwmixread, args.cpus_num, args.run_num)


parser = argparse.ArgumentParser()
subparsers = parser.add_subparsers()
subparser_spdk_tgt = subparsers.add_parser('spdk_tgt', help="Create SPDK config file and"
                                                            "run SPDK NVMeOF Target according"
                                                            "to provided parameters")
subparser_spdk_tgt.add_argument("-a", "--address", default="0x1", type=str,
                                help="Comma separated NIC IP addresses to use for subsystems construction.")
# TODO: Improve core mask param
# Rework to make it "num cores" and dynamically distribute between NUMA nodes if any used NICs or NVMes are not on the same node
subparser_spdk_tgt.add_argument("-c", "--core-mask", default="0x1", type=str,
                                help="Core mask to use for running SPDK NVMeOF Target.")
subparser_spdk_tgt.set_defaults(func=start_spdk_nvmf_tgt)


subparser_spdk_init = subparsers.add_parser('spdk_init', help="Create Bdev and Fio config files and"
                                                              "run SPDK NVMeOF Initiator.")

subparser_spdk_init.add_argument("-a", "--address", type=str,
                                 help="Comma separated list of IP addresses of subsystems"
                                      "to which initiator should connect to.")
# TODO: Allow block size, iodepth to be lists?
subparser_spdk_init.add_argument("-b", "--block-size", type=str, default="4k",
                                 help="Block size to use in FIO workload.")
subparser_spdk_init.add_argument("-i", "--iodepth", type=str, default="8",
                                 help="IODepth to use in FIO workload.")
subparser_spdk_init.add_argument("-r", "--rw", type=str, default="1",
                                 help="RW mode to use in FIO worload.")
subparser_spdk_init.add_argument("-m", "--rwmixread", type=str, default="1",
                                 help="Percentage of read operations to use in readwrite operations.")
subparser_spdk_init.add_argument("-t", "--run-time", type=str, default="10",
                                 help="Run time (in seconds) to run FIO workload.")
subparser_spdk_init.add_argument("-T", "--ramp-time", type=str, default="10",
                                 help="Ramp time (In seconds) to run FIO workload.")
subparser_spdk_init.add_argument("-c", "--cpus-num", type=int, default=None,
                                 help="Limit FIO workload to be run with that many CPUs."
                                      "By default each discovered remote subsystem will get it's own CPU.")
subparser_spdk_init.add_argument("-n", "--run-num", type=int, default=None,
                                 help="Number of times to run given workload. If >1 then average results will be"
                                      "calculated.")
subparser_spdk_init.set_defaults(func=gen_spdk_init_config)

args = parser.parse_args()
args.func(args)
