#!/usr/bin/env python3.5
import argparse
import pexpect
import subprocess
import json
import os
import sys


testdir = os.path.dirname(os.path.realpath(sys.argv[0]))
pci_scsi = None
pci_blk = None
child = None


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


def load_spdk_tgt_pmem():
    execute_command("/bdevs/pmemblk create_pmem_pool /tmp/sample_pmem 32 512")
    execute_command("/bdevs/pmemblk create/tmp/sample_pmem pmem_bdev", "pmem_bdev")


def clear_spdk_tgt_pmem():
    global in_ls
    in_ls = False
    execute_command("/bdevs/pmemblk delete pmem_bdev","pmem_bdev")
    execute_command("/bdevs/pmemblk delete_pmem_pool /tmp/sample_pmem")

def get_scsi_pci():
    pci_scsi = subprocess.check_output("lspci -nn -D | grep '1af4:1004'"
                                       " | head -1 | awk '{print $1;}'", shell=True)

    return pci_scsi

def get_blk_pci():
    pci_blk = subprocess.check_output("lspci -nn -D | grep '1af4:1001'"
                                      " | head -1 | awk '{print $1;}'", shell=True)

    return pci_blk

def load_spdk_tgt_virtio():
    global in_ls, pci_blk, pci_scsi, virtio_child, child
    in_ls = True
    execute_command("/bdevs/malloc create 32 512 Malloc0", "Malloc0")
    execute_command("/bdevs/malloc create 32 512 Malloc1", "Malloc1")
    pci_blk = get_blk_pci()
    if pci_blk:
        execute_command("/bdevs/virtioblk_disk create virtioblk_pci pci 0000:00:07.0")
    pci_scsi = get_scsi_pci()
    if pci_scsi:
        execute_command("/bdevs/virtioscsi_disk create virtioscsi_pci pci 0000:00:06.0")
    execute_command("/vhost/scsi create sample_scsi", "sample_scsi")
    execute_command("/vhost/scsi/sample_scsi add_lun 0 Malloc0")
    execute_command("/vhost/block create sample_block Malloc1", "Malloc1")
    base_child = child
    child = virtio_child
    execute_command("/bdevs/virtioblk_disk create virtioblk_user user  %s" %
                    os.path.join(testdir, "sample_block"), "virtioblk_user")
    execute_command("/bdevs/virtioscsi_disk create virtioscsi_user user  %s" %
                    os.path.join(testdir, "sample_scsi"), "virtioscsi_user")
    child = base_child

def clear_spdk_tgt_virtio():
    global in_ls, virtio_child, child
    in_ls = False
    base_child = child
    child = virtio_child
    execute_command("/bdevs/virtioscsi_disk delete virtioscsi_user")
    execute_command("/bdevs/virtioblk_disk delete virtioblk_user")
    child = base_child
    execute_command("/vhost/block delete sample_block", "sample_block")
    execute_command("/vhost/scsi/sample_scsi remove_target 0", "Malloc0")
    execute_command("/vhost/scsi delete sample_scsi", " sample_scsi")
    if pci_blk:
        execute_command("/bdevs/virtioblk_disk delete virtioblk_pci", "virtioblk_pci")
    if pci_scsi:
        execute_command("/bdevs/virtioscsi_disk delete virtioscsi_pci", "virtioscsi_pci")
    execute_command("/bdevs/malloc delete Malloc0", "Malloc0")
    execute_command("/bdevs/malloc delete Malloc1", "Malloc1")

if __name__ == "__main__":
    global child, virtio_child
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
    if args.job == "load_spdk_tgt_pmem":
        load_spdk_tgt_pmem()
    if args.job == "clear_spdk_tgt_pmem":
        clear_spdk_tgt_pmem()
    if args.job in ["load_spdk_tgt_virtio", "clear_spdk_tgt_virtio"]:
        cmd = os.path.join(testdir, "../../scripts/spdkcli.py") + " -s /var/tmp/virtio.sock"
        virtio_child = pexpect.spawn(cmd)
        virtio_child.expect(">")
        virtio_child.sendline("cd /")
        virtio_child.expect(">")
    if args.job == "load_spdk_tgt_virtio":
        load_spdk_tgt_virtio()
    if args.job == "clear_spdk_tgt_virtio":
        clear_spdk_tgt_virtio()
