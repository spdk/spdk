#!/usr/bin/env python3.5
import argparse
import pexpect
import subprocess
import json
import os
import sys


def get_nvme_disks():
    out = subprocess.check_output([os.path.join(testdir, "../../scripts/gen_nvme.sh"), "--json"])
    disks = json.loads(out.decode())

    return disks['config']


def execute_command(cmd, element=None):
    child.sendline(cmd)
    child.expect("/>")
    print("before: %s" % child.before.decode())
    if "error response" in child.before.decode():
        print("Error in cmd: %s" % cmd)
        exit(1)
    ls_tree = cmd.split(" ")[0]
    if ls_tree and element:
        child.sendline("ls %s" % ls_tree)
        child.expect("/>")
        print("child: %s" % child.before.decode())
        if in_ls:
            if element not in child.before.decode():
                print("Element %s not in list" % element)
                exit(1)
        else:
            if element in child.before.decode():
                print("Element %s is in list" % element)
                exit(1)


def load_spdk_tgt():
    execute_command("/bdevs/malloc create 32 512 Malloc0", "Malloc0")
    execute_command("/bdevs/malloc create 32 512 Malloc1", "Malloc1")
    execute_command("/bdevs/malloc create 32 512 Malloc2", "Malloc2")
    execute_command("/bdevs/malloc create 32 4096 Malloc3", "Malloc3")
    execute_command("/lvol_stores create lvs Malloc0", "lvs")
    execute_command("/bdevs/error create Malloc1", "EE_Malloc1")
    execute_command("/bdevs/logical_volume create lvol 16 lvs", "lvs/lvol")
    execute_command("/bdevs/null create null_bdev 32 512", "null_bdev")
    nvme_disks = get_nvme_disks()
    for nvme in nvme_disks:
        nvme_params = nvme['params']
        cmd = "%s %s %s" % (nvme_params['name'], nvme_params['trtype'],
                            nvme_params['traddr'])
        execute_command("/bdevs/nvme create %s" % cmd, "%sn1" % nvme_params['name'])
    execute_command("/bdevs/aio create sample /tmp/sample_aio 512", "sample")
    execute_command("bdevs/split_disk split_bdev Nvme0n1 4", "Nvme0n1p0")
    execute_command("vhost/block create vhost_blk1 Nvme0n1p0", "Nvme0n1p0")
    execute_command("vhost/block create vhost_blk2 Nvme0n1p1 0x2 readonly", "Nvme0n1p1")
    execute_command("vhost/scsi create vhost_scsi1", "vhost_scsi1")
    execute_command("vhost/scsi create vhost_scsi2", "vhost_scsi2")
    execute_command("vhost/scsi/vhost_scsi1 add_lun 0 Malloc2", "Malloc2")
    execute_command("vhost/scsi/vhost_scsi2 add_lun 0 Malloc3", "Malloc3")
    execute_command("vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p2", "Nvme0n1p2")
    execute_command("vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p3", "Nvme0n1p3")


def clear_spdk_tgt():
    global in_ls
    in_ls = False
    execute_command("vhost/scsi/vhost_scsi2 remove_target 2", "Nvme0n1p3")
    execute_command("vhost/scsi/vhost_scsi2 remove_target 1", "Nvme0n1p2")
    execute_command("vhost/scsi/vhost_scsi2 remove_target 0", "Malloc3")
    execute_command("vhost/scsi/vhost_scsi1 remove_target 0", "Malloc2")
    execute_command("vhost/scsi delete vhost_scsi2", "vhost_scsi2")
    execute_command("vhost/scsi delete vhost_scsi1", "vhost_scsi1")
    execute_command("vhost/block delete vhost_blk2", "vhost_blk2")
    execute_command("vhost/block delete vhost_blk1", "vhost_blk1")
    execute_command("/bdevs/split_disk destruct_split_bdev Nvme0n1", "Nvme0n1p0")
    execute_command("bdevs/aio delete sample", "sample")
    nvme_disks = get_nvme_disks()
    for nvme in nvme_disks:
        execute_command("/bdevs/nvme delete %sn1" % nvme['params']['name'],
                        "%sn1" % nvme['params']['name'])
    execute_command("/bdevs/null delete null_bdev", "null_bdev")
    execute_command("/bdevs/logical_volume delete lvs/lvol", "lvs/lvol")
    execute_command("/lvol_stores delete lvs", "lvs")
    execute_command("/bdevs/malloc delete Malloc0", "Malloc0")
    execute_command("/bdevs/malloc delete Malloc1", "Malloc1")
    execute_command("/bdevs/malloc delete Malloc2", "Malloc2")
    execute_command("/bdevs/malloc delete Malloc3", "Malloc3")


if __name__ == "__main__":
    testdir = os.path.dirname(os.path.realpath(sys.argv[0]))
    child = pexpect.spawn(os.path.join(testdir, "../../scripts/spdkcli.py"))
    child.expect(">")
    child.sendline("cd /")
    child.expect("/>")
    in_ls = True

    parser = argparse.ArgumentParser()
    parser.add_argument('-job', dest='job')
    parser.add_argument('-filename', dest='filename')

    args = parser.parse_args()
    if args.job == "load_spdk_tgt":
        load_spdk_tgt()
    if args.job == "clear_spdk_tgt":
        clear_spdk_tgt()
