import os
import sys
import argparse
import multiprocessing
import subprocess
from subprocess import check_call, call, check_output, Popen, PIPE


def gen_cpu_mask_config(output_dir, spdk_cpu_list, vm_count, vm_cpu_num):
    spdk = gen_spdk_cpu_mask_config(spdk_cpu_list)
    qemu = gen_qemu_cpu_mask_config(spdk_cpu_list, vm_count, vm_cpu_num)
    file = os.path.join(output_dir, "mask_config")
    with open(file, "w") as fh:
        fh.write("".join([spdk, qemu]))


def gen_spdk_cpu_mask_config(spdk_cpu_list):
    cpus = "vhost_0_reactor_mask=[%s]" % (spdk_cpu_list)
    pr_core = "vhost_0_master_core=%s" % (spdk_cpu_list.split("-")[0])
    return "\n".join([cpus, pr_core, "\n"])


def get_host_cpus():
    cpu_num = multiprocessing.cpu_count()
    cpu_list = list(range(0, cpu_num))
    output = check_output("lscpu | grep 'per core'", shell=True)

    # Assuming 2-socket server
    if "2" in str(output):
        ht_enabled = True
        cpu_chunk = int(cpu_num/4)
        numa0_cpus = cpu_list[0:cpu_chunk]
        numa0_cpus.extend(cpu_list[2*cpu_chunk:3*cpu_chunk])
        numa1_cpus = cpu_list[cpu_chunk:2*cpu_chunk]
        numa1_cpus.extend(cpu_list[3*cpu_chunk:4*cpu_chunk])
    else:
        ht_enabled = False
        cpu_chunk = int(cpu_num/2)
        numa0_cpus = cpu_list[:cpu_chunk]
        numa1_cpus = cpu_list[cpu_chunk:]
    return [numa0_cpus, numa1_cpus]


def gen_qemu_cpu_mask_config(spdk_cpu_list, vm_count, vm_cpu_num):
    print("Creating masks for QEMU")
    ret = ""

    # Exclude SPDK cores from available CPU list
    numa0_cpus, numa1_cpus = get_host_cpus()
    spdk_cpu_min, spdk_cpu_max = spdk_cpu_list.split("-")
    spdk_cpus = range(int(spdk_cpu_min), int(spdk_cpu_max) + 1)

    numa0_cpus = sorted(list(set(numa0_cpus) - set(spdk_cpus)))
    numa1_cpus = sorted(list(set(numa1_cpus) - set(spdk_cpus)))

    used_numa = 0
    available = numa0_cpus
    for i in range(0, vm_count):
        cpus = [str(x) for x in available[0:vm_cpu_num]]

        # If there is not enough cores on first numa node for a VM
        # then switch to next numa node
        if len(cpus) < vm_cpu_num and used_numa == 0:
            available = numa1_cpus
            used_numa = 1
            cpus = [str(x) for x in available[0:vm_cpu_num]]

        # If not enough cores on second numa node - break and exit
        if len(cpus) < vm_cpu_num and used_numa == 1:
            print("There is not enough CPU Cores available on \
            Numa node1 to create VM %s" % i)
            break

        cpus = ",".join(cpus)
        cpus = "VM_%s_qemu_mask=%s" % (i, cpus)
        numa = "VM_%s_qemu_numa_node=%s\n" % (i, 0)

        # Remove used CPU cores from available list
        available = available[vm_cpu_num:]
        ret = "\n".join([ret, cpus, numa])

    return ret


def create_fio_cfg(template_dir, output_dir, **kwargs):
    fio_tempalte = os.path.join(template_dir, "fio_test.conf")
    with open("scripts/perf/vhost/fio_test.conf", "r") as fh:
        cfg = fh.read()
    cfg = cfg.format(**kwargs)

    file = os.path.join(output_dir, "fio_job.cfg")
    with open(file, "w") as fh:
        fh.write(cfg)


script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
parser = argparse.ArgumentParser()

parser.add_argument('blksize', default="4k", type=str,
                    help="Block size param for FIO. Default: 4k")
parser.add_argument('iodepth', default="128", type=str,
                    help="Iodepth param for FIO. Default: 128")
parser.add_argument('rw', default="randread", type=str,
                    help="RW param for FIO. Default: randread")
parser.add_argument('-m', '--rwmixread', default="70", type=str,
                    help="Percentage of reads in read-write mode. Default: 70")
parser.add_argument('-r', '--runtime', default="10", type=str,
                    help="Run time param for FIO (in seconds). Default: 10")
parser.add_argument('-R', '--ramptime', default="10", type=str,
                    help="Ramp time param for FIO (in seconds). Default: 10")
parser.add_argument('-c', '--ctrl-type', default="spdk_vhost_scsi", type=str,
                    help="Type of vhost controller to use in test.\
                    Possible options: spdk_vhost_scsi, spdk_vhost_blk.\
                    Default: spdk_vhost_scsi")
parser.add_argument('-s', '--split', default=False, type=bool,
                    help="Use split vbdevs instead of logical volumes. Default: false")
parser.add_argument('-d', '--max-disks', default=0, type=int,
                    help="How many physical disks to use in test. Default: all disks.\
                    Depending on the number of --vm-count disks may be split into\
                    smaller logical bdevs (splits or logical volumes) so that\
                    each virtual machine gets it's own bdev to work on.")
parser.add_argument('-v', '--vm-count', default=1, type=int,
                    help="How many VMs to run in test. Default: 1")

subparsers = parser.add_subparsers()
cpu_cfg_create = subparsers.add_parser('create_cpu_cfg',
                                       help="Generate a CPU config file for test.\
                                       This option will attempt to automatically\
                                       generate config file with SPDK/QEMU cpu lists.\
                                       CPU cores on NUMA Node 0 will be used first\
                                       (including logical cores when HT is enabled)\
                                       and NUMA Node 1 will be used last.")
cpu_cfg_create.add_argument('spdk_cpu_list', default=None)
cpu_cfg_create.add_argument('vm_cpu_num', default=None, type=int)

cpu_cfg_load = subparsers.add_parser('load_cpu_cfg',
                                     help="Load and use a CPU config file for test\
                                     Example configuration files can be found in:\
                                     test/vhost/common/autotest.config")
cpu_cfg_load.add_argument('custom_mask_file', default=None)

args = parser.parse_args()
create_fio_cfg(script_dir, script_dir, **vars(args))

cpu_cfg_arg = ""
disk_arg = ""
split_arg = ""
if "spdk_cpu_list" in args:
    gen_cpu_mask_config(script_dir, args.spdk_cpu_list, args.vm_count, args.vm_cpu_num)
    cpu_cfg_arg = "--custom-cpu-cfg=mask.config"
if "custom_mask_file" in args:
    cpu_cfg_arg = "--custom-cpu-cfg=%s" % args.custom_mask_file
if args.split is True:
    split_arg = "--use-split"
if args.max_disks > 0:
    disk_arg = "--max-disks=%s" % args.max_disks


command = " ".join(["test/vhost/perf_bench/vhost_perf.sh",
                    "--vm-count=%s" % args.vm_count,
                    "--ctrl-type=%s" % args.ctrl_type,
                    "%s" % split_arg,
                    "%s" % disk_arg,
                    "--fio-job=%s" % "/home/klateck/work/spdk/fio_job.cfg"])
pr = check_output(command, shell=True)
