#!/usr/bin/env python
import argparse
import pexpect
import subprocess
import json
import os

child = None


def get_nvme_disks():
    out = subprocess.check_output(["../../scripts/gen_nvme.sh", "--json"])
    disks = json.loads(out)

    return disks['config']


def execute_command(cmd):
    global child
    child.sendline(cmd)
    child.expect("/>")
    print("before: %s" % child.before)
    if "error response" in child.before:
        print("Error in cmd: %s" % cmd)
        exit(1)

def load_spdk_tgt():
    global child
    child = pexpect.spawn('python ../../scripts/spdkcli.py')
    child.expect(">")
    execute_command("cd /")
    execute_command("/bdevs/Malloc create 32 512 Malloc0")
    execute_command("/bdevs/Malloc create 32 512 Malloc1")
    execute_command("/bdevs/Malloc create 32 512 Malloc2")
    execute_command("/bdevs/Malloc create 32 4096 Malloc3")
    execute_command("/lvol_stores create lvs Malloc0")
    execute_command("/bdevs/Error create Malloc1")
    execute_command("/bdevs/Logical_Volume create lvol 16 lvs")
    execute_command("/bdevs/Null create null_bdev 32 512")
    nvme_disks = get_nvme_disks()
    for nvme in nvme_disks:
        nvme_params = nvme['params']
        cmd = "%s %s %s" % (nvme_params['name'], nvme_params['trtype'],
                            nvme_params['traddr'])
        execute_command("/bdevs/NVMe create %s" % cmd)
    execute_command("/bdevs/AIO create aio /tmp/sample_aio 512")
    execute_command("bdevs/Split_Disk split_bdev Nvme0n1 4")
    execute_command("vhost/block create vhost_blk1 Nvme0n1p0")
    execute_command("vhost/block create vhost_blk2 Nvme0n1p0 0x1 readonly")
    execute_command("vhost/scsi create vhost_scsi1")
    execute_command("vhost/scsi create vhost_scsi2")
    execute_command("vhost/scsi/vhost_scsi1 add_lun 0 Malloc2")
    execute_command("vhost/scsi/vhost_scsi2 add_lun 0 Malloc3")
    execute_command("vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p1")
    execute_command("vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p2")


def clear_spdk_tgt():
    global child
    child = pexpect.spawn('python ../../scripts/spdkcli.py')
    child.expect(">")
    execute_command("cd /")
    execute_command("vhost/scsi/vhost_scsi2 remove_target 2")
    execute_command("vhost/scsi/vhost_scsi2 remove_target 1")
    execute_command("vhost/scsi/vhost_scsi2 remove_target 0")
    execute_command("vhost/scsi/vhost_scsi1 remove_target 0")
    execute_command("vhost/scsi delete vhost_scsi2")
    execute_command("vhost/scsi delete vhost_scsi1")
    execute_command("vhost/block delete vhost_blk2")
    execute_command("vhost/block delete vhost_blk1")
    execute_command("/bdevs/Split_Disk destruct_split_bdev Nvme0n1")
    execute_command("bdevs/AIO delete aio")
    nvme_disks = get_nvme_disks()
    for nvme in nvme_disks:
        execute_command("/bdevs/NVMe delete %sn1" % nvme['params']['name'])
    execute_command("/bdevs/Null delete null_bdev")
    execute_command("/bdevs/Logical_Volume delete lvs/lvol")
    execute_command("/lvol_stores delete lvs")
    execute_command("/bdevs/Malloc delete Malloc0")
    execute_command("/bdevs/Malloc delete Malloc1")
    execute_command("/bdevs/Malloc delete Malloc2")
    execute_command("/bdevs/Malloc delete Malloc3")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-job', dest='job')
    parser.add_argument('-filename', dest='filename')

    args = parser.parse_args()
    if args.job == "load_spdk_tgt":
        load_spdk_tgt()
    if args.job == "clear_spdk_tgt":
        clear_spdk_tgt()
