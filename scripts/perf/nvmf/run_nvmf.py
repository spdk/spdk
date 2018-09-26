#!/usr/bin/env python3

from subprocess import check_call, call, check_output, Popen, PIPE
import subprocess
import os
import sys
import re
import argparse
import json
import itertools


global script_dir
global spdk_dir


def get_used_numa_nodes():
    used_numa_nodes = set()
    for bdf in get_nvme_devices_bdf():
        output = check_output("lspci -vv -s %s | grep -i NUMA" % bdf, shell=True).decode(encoding="utf-8")
        output = "".join(filter(lambda x: x.isdigit(), output))  # Join because filter returns an iterable
        used_numa_nodes.add(int(output))
    return used_numa_nodes


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


def gen_core_mask(num_cpus, numa_list):
    cpu_per_socket = check_output('lscpu | grep -oP "per socket:\s+\K\d+"', shell=True).decode("utf-8")
    cpu_per_socket = int(cpu_per_socket)
    cpu_socket_ranges = [list(range(x * cpu_per_socket, cpu_per_socket + x * cpu_per_socket)) for x in numa_list]

    # Mix CPUs in Round robin fashion
    mixed_cpus = list(itertools.chain.from_iterable(zip(*cpu_socket_ranges)))
    cpu_list = mixed_cpus[0:num_cpus]
    cpu_list = [str(x) for x in cpu_list]
    cpu_list = "[" + ",".join(cpu_list) + "]"
    return cpu_list


def gen_spdk_tgt_config(address, num_cores=1):
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

    nvme_section = add_nvme_to_spdk_tgt_conf()
    subsystems_section = add_subsystems_to_spdk_tgt_conf(address)
    with open(os.path.join(spdk_dir, "spdk_tgt.conf"), "w") as config_fh:
        config_fh.write(global_section)
        config_fh.write(nvme_section)
        config_fh.write(subsystems_section)
    print("Done generating SPDK Target config file")


def start_spdk_nvmf_tgt(args):
    global spdk_dir
    nvmf_app_path = os.path.join(spdk_dir, "app/nvmf_tgt/nvmf_tgt")
    nvmf_cfg_file = os.path.join(spdk_dir, "spdk_tgt.conf")
    command = [nvmf_app_path, "-c", nvmf_cfg_file]
    command = " ".join(command)
    gen_spdk_tgt_config(args.address, num_cores=args.num_cores)
    proc = subprocess.Popen(command, shell=True)
    with open(os.path.join(spdk_dir, "nvmf.pid"), "w") as fh:
        fh.write(str(proc.pid))
    # proc.wait()


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


def run_fio(block_size, iodepth, rw, rw_mix, cpus_num, run_num, output_dir=""):
    print("Running FIO test with args:")

    if output_dir:
        try:
            os.mkdir(output_dir)
        except FileExistsError:
            pass

    for i in range(1, run_num + 1):
        output_file = "s_" + str(block_size) + "_q_" + str(iodepth) + "_m_" + str(rw) + "_c_" + str(cpus_num) + "_run_" + str(i) + ".json"
        output_file = os.path.join(output_dir, output_file)  # Add output dir if it's available
        command = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % ("fio.conf", output_file)
        print(command)

        proc = subprocess.Popen(command, shell=True)
        proc.wait()

    print("Finished Test: IO Size={} QD={} Mix={} CPU Mask={}".format(block_size, iodepth, rw_mix, cpus_num))
    return


def parse_results(output_dir="."):
    json_files = os.listdir(output_dir)
    json_files = sorted(json_files)
    json_files = list(filter(lambda x: ".json" in x, json_files))
    print(json_files)

    # Create empty results file
    csv_file = "nvmf_results.csv"
    with open(os.path.join(output_dir, csv_file), "w") as fh:
        header_line = ",".join(["Name",
                                "read_iops", "read_bw", "read_avg_lat", "read_min_lat", "read_max_lat",
                                "write_iops", "write_bw", "write_avg_lat", "write_min_lat", "write_max_lat"])
        fh.write(header_line + "\n")

    for file in json_files:
        row_name, _ = os.path.splitext(file)

        # Regardless of number of threads used in fio config all results will
        # be aggregated into single entry due to use of "group_reporting" option,
        # so there will be only one entry with stats in json file
        job_pos = 0

        with open(os.path.join(output_dir, file), "r") as json_data:
            data = json.load(json_data)

            if "lat_ns" in data["jobs"][job_pos]["read"]:
                lat = "lat_ns"
                lat_units = "ns"
            else:
                lat = "lat"
                lat_units = "us"

            read_iops = float(data["jobs"][job_pos]["read"]["iops"])
            read_bw = float(data["jobs"][job_pos]["read"]["bw"])
            read_avg_lat = float(data["jobs"][job_pos]["read"][lat]["mean"])
            read_min_lat = float(data["jobs"][job_pos]["read"][lat]["min"])
            read_max_lat = float(data["jobs"][job_pos]["read"][lat]["max"])
            write_iops = float(data["jobs"][job_pos]["write"]["iops"])
            write_bw = float(data["jobs"][job_pos]["write"]["bw"])
            write_avg_lat = float(data["jobs"][job_pos]["write"][lat]["mean"])
            write_min_lat = float(data["jobs"][job_pos]["write"][lat]["min"])
            write_max_lat = float(data["jobs"][job_pos]["write"][lat]["max"])

        row_fields = [row_name,
                      read_iops, read_bw, read_avg_lat, read_min_lat, read_max_lat,
                      write_iops, write_bw, write_avg_lat, write_min_lat, write_max_lat]
        row_fields = [str(x) for x in row_fields]  # Make sure these are str for "join" operation
        row = ",".join(row_fields)

        with open(os.path.join(output_dir, csv_file), "a") as fh:
            fh.write(row + "\n")


def gen_spdk_init_config(rw, rwmixread, block_size, iodepth, ramp_time, run_time, address, cpus_num):
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

    bdevs = gen_spdk_bdev_init_config(address_list=address)
    with open("bdev.conf", "w") as fh:
        fh.write(bdevs)

    fio = gen_spdk_init_fio_config(address_list=address, num_cpus=cpus_num)
    with open("fio.conf", "a") as fh:
        fh.write(fio)


def run_spdk_init(args):
    block_sizes = args.block_size.split(",")
    iodepths = args.iodepth.split(",")
    rws = args.rw.split(",")
    if args.cpus_num:
        cpus_num = args.cpus_num.split(",")
    else:
        cpus_num = None

    for block_size, iodepth, rw, cpu_num in itertools.product(block_sizes, iodepths, rws, cpus_num):
        print("Running config:", block_size, iodepth, rw, cpu_num, args.rwmixread, args.ramp_time, args.run_time)
        gen_spdk_init_config(rw=rw,
                             rwmixread=args.rwmixread,
                             block_size=block_size,
                             iodepth=iodepth,
                             ramp_time=args.ramp_time,
                             run_time=args.run_time,
                             address=args.address,
                             cpus_num=cpu_num)
        run_fio(block_size, iodepth, rw, args.rwmixread, cpu_num, args.run_num, args.output_dir)
    parse_results(args.output_dir)


script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
spdk_dir = os.path.abspath(os.path.join(script_dir, "../../../"))

parser = argparse.ArgumentParser()
subparsers = parser.add_subparsers()
subparser_spdk_tgt = subparsers.add_parser('spdk_tgt', help="Create SPDK config file and"
                                                            "run SPDK NVMeOF Target according"
                                                            "to provided parameters")
subparser_spdk_tgt.add_argument("-a", "--address", default="0x1", type=str,
                                help="Comma separated NIC IP addresses to use for subsystems construction.")
# TODO: Improve core mask param
# Rework to make it "num cores" and dynamically distribute between NUMA nodes if any used NICs or NVMes are not on the same node
subparser_spdk_tgt.add_argument("-n", "--num-cores", default="1", type=int,
                                help="Number of cores to use for running SPDK NVMeOF Target."
                                     "If there is more than 1 NUMA node in system and if any of NVMe disk is"
                                     "operating on different NUMA, then script will try to distribute requested"
                                     "number of cores evenly.")
subparser_spdk_tgt.set_defaults(func=start_spdk_nvmf_tgt)


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
subparser_spdk_init.set_defaults(func=run_spdk_init)

args = parser.parse_args()
args.func(args)
