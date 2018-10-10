#!/usr/bin/env python3

import subprocess
import os
import sys
import argparse
import json
import uuid
from common import *


global script_dir
global spdk_dir


# SPDK Target functions
def spdk_tgt_add_nullblock():
    null_section = """[Null]
  Dev Nvme0n1 102400 4096"""

    return null_section


def spdk_tgt_add_nvme_conf(req_num_disks=None):
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


def spdk_tgt_add_subsystem_conf(address=None, req_num_disks=None):
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


def spdk_tgt_gen_config(address, num_cores=1, use_null_block=False):
    print("Generating SPDK Target config file")
    global spdk_dir
    global script_dir

    numa_list = get_used_numa_nodes()
    core_mask = gen_core_mask(num_cores, numa_list)

    # TODO: Improve core mask param
    # Rework to make it "num cores" and dynamically distribute between NUMA nodes if any used NICs or NVMes are not on the same node
    with open(os.path.join(script_dir, "spdk_tgt_template.conf"), "r") as fh:
        global_section = fh.read()
        global_section = global_section.format(core_mask=core_mask)

    if use_null_block:
        nvme_section = spdk_tgt_add_nullblock()
        subsystems_section = spdk_tgt_add_subsystem_conf(address, req_num_disks=1)
    else:
        nvme_section = spdk_tgt_add_nvme_conf()
        subsystems_section = spdk_tgt_add_subsystem_conf(address)

    with open(os.path.join(spdk_dir, "spdk_tgt.conf"), "w") as config_fh:
        config_fh.write(global_section)
        config_fh.write(nvme_section)
        config_fh.write(subsystems_section)
    print("Done generating SPDK Target config file")


def spdk_tgt_start(args):
    global spdk_dir
    nvmf_app_path = os.path.join(spdk_dir, "app/nvmf_tgt/nvmf_tgt")
    nvmf_cfg_file = os.path.join(spdk_dir, "spdk_tgt.conf")
    command = [nvmf_app_path, "-c", nvmf_cfg_file]
    command = " ".join(command)
    spdk_tgt_gen_config(args.address, num_cores=args.num_cores, use_null_block=args.use_null_block)
    proc = subprocess.Popen(command, shell=True)
    with open(os.path.join(spdk_dir, "nvmf.pid"), "w") as fh:
        fh.write(str(proc.pid))
    # proc.wait()


# Kernel Target functions
def kernel_tgt_gen_nullblock_conf(address):
    nvmet_cfg = {
        "ports": [],
        "hosts": [],
        "subsystems": [],
    }

    nvmet_cfg["subsystems"].append({
        "allowed_hosts": [],
        "attr": {
            "allow_any_host": "1",
            "version": "1.3"
        },
        "namespaces": [
            {
                "device": {
                    "path": "/dev/nullb0",
                    "uuid": "%s" % uuid.uuid4()
                },
                "enable": 1,
                "nsid": 1
            }
        ],
        "nqn": "nqn.2018-09.io.spdk:cnode1"
    })

    nvmet_cfg["ports"].append({
        "addr": {
            "adrfam": "ipv4",
            "traddr": address,
            "trsvcid": "4420",
            "trtype": "rdma"
        },
        "portid": 1,
        "referrals": [],
        "subsystems": ["nqn.2018-09.io.spdk:cnode1"]
    })

    with open("kernel.conf", 'w') as fh:
        fh.write(json.dumps(nvmet_cfg, indent=2))


def kernel_tgt_gen_subsystem_conf(nvme_list, address_list):

    nvmet_cfg = {
        "ports": [],
        "hosts": [],
        "subsystems": [],
    }

    # Split disks between NIC IP's
    disks_per_ip = int(len(nvme_list) / len(address_list))
    disk_chunks = [nvme_list[i*disks_per_ip:disks_per_ip + disks_per_ip*i] for i in range(0, len(address_list))]

    subsys_no = 1
    for ip, chunk in zip(address_list, disk_chunks):
        for disk in chunk:
            nvmet_cfg["subsystems"].append({
                "allowed_hosts": [],
                "attr": {
                    "allow_any_host": "1",
                    "version": "1.3"
                },
                "namespaces": [
                    {
                        "device": {
                            "path": disk,
                            "uuid": "%s" % uuid.uuid4()
                        },
                        "enable": 1,
                        "nsid": subsys_no
                    }
                ],
                "nqn": "nqn.2018-09.io.spdk:cnode%s" % subsys_no
            })

            nvmet_cfg["ports"].append({
                "addr": {
                    "adrfam": "ipv4",
                    "traddr": ip,
                    "trsvcid": "%s" % (4420 + subsys_no),
                    "trtype": "rdma"
                },
                "portid": subsys_no,
                "referrals": [],
                "subsystems": ["nqn.2018-09.io.spdk:cnode%s" % subsys_no]
            })
            subsys_no += 1

    with open("kernel.conf", "w") as fh:
        fh.write(json.dumps(nvmet_cfg, indent=2))
    pass


def kernel_tgt_run(args):
    print("Configuring kernel NVMeOF Target")
    address = args.address.split(",")

    if args.use_null_block:
        print("Configuring with null block device.")
        if len(address) > 1:
            print("Testing with null block limited to single RDMA NIC.")
            print("Please specify only 1 IP address.")
            exit(1)
            kernel_tgt_gen_nullblock_conf(address[0])
    else:
        print("Configuring with NVMe drives.")
        nvme_list = get_nvme_devices()
        kernel_tgt_gen_subsystem_conf(nvme_list, address)

    nvmet_command("clear", args.path)
    nvmet_command("restore kernel.conf", args.path)
    print("Done configuring kernel NVMeOF Target")


script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
spdk_dir = os.path.abspath(os.path.join(script_dir, "../../../"))

parser = argparse.ArgumentParser()
subparsers = parser.add_subparsers()
subparser_spdk_tgt = subparsers.add_parser('spdk_tgt', help="Create SPDK config file and"
                                                            "run SPDK NVMeOF Target according"
                                                            "to provided parameters")
subparser_spdk_tgt.add_argument("-a", "--address", default="0x1", type=str,
                                help="Comma separated NIC IP addresses to use for subsystems construction.")
subparser_spdk_tgt.add_argument("-n", "--num-cores", default="1", type=int,
                                help="Number of cores to use for running SPDK NVMeOF Target."
                                     "If there is more than 1 NUMA node in system and if any of NVMe disk is"
                                     "operating on different NUMA, then script will try to distribute requested"
                                     "number of cores evenly.")
subparser_spdk_tgt.add_argument("--use-null-block", default=False, type=bool,
                                help="Use single null block bdev instead of NVMe drives for test."
                                     "Used for latency tests.")
subparser_spdk_tgt.set_defaults(func=spdk_tgt_start)

subparser_kernel_tgt = subparsers.add_parser('kernel_tgt', help="Run kernel NVMeOF target configuration")
subparser_kernel_tgt.add_argument("-a", "--address", default="0x1", type=str,
                                  help="Comma separated NIC IP addresses to use for subsystems construction.")
subparser_kernel_tgt.add_argument("--use-null-block", default=False, type=bool,
                                  help="Use single null block bdev instead of NVMe drives for test."
                                       "Used for latency tests.")
subparser_kernel_tgt.add_argument("--path", default=None, type=str,
                                  help="Path to directory with nvmetcli script. If not provided"
                                       "then default system package will be used.")
subparser_kernel_tgt.set_defaults(func=kernel_tgt_run)


args = parser.parse_args()
args.func(args)
