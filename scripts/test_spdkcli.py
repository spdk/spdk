#!/usr/bin/env python
import pexpect
import subprocess
import json
import os


def create_aio_file():
    ret_val = os.system("dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000")


def delete_aio_file():
    ret_val = os.system("rm /tmp/sample_aio")


def get_nvme_disks():
    out = subprocess.check_output(["./gen_nvme.sh", "--json"])
    disks = json.loads(out)

    return disks['config']


def match_output(output, filename):
    file = open("spdkcli_test_output","w") 
    file.write(output)
    file.close()
    try:
        out = subprocess.check_output(["../test/app/match/match", filename])
    except:
        print "Output does not match pattern"
    #os.system("rm spdkcli_test_output")


child = pexpect.spawn('python ./spdkcli.py')
child.expect(">")
child.sendline("cd /")
child.expect("/>")
child.sendline("ls")
child.expect("/>")
print child.before
child.sendline("/bdevs/Malloc create 32 512 Malloc0")
child.expect("/>")
child.sendline("/bdevs/Malloc create 32 512 Malloc1")
child.expect("/>")
child.sendline("/bdevs/Malloc create 32 512 Malloc2")
child.expect("/>")
child.sendline("/lvol_stores create lvs Malloc0")
child.expect("/>")
child.sendline("/bdevs/Error create Malloc1")
child.expect("/>")
child.sendline("/bdevs/Split_Disk split_bdev Malloc2 2")
child.expect("/>")
child.sendline("/bdevs/Logical_Volume create lvol 16 lvs")
child.expect("/>")
child.sendline("/bdevs/Null create null_bdev 32 512")
child.expect("/>")
nvme_disks = get_nvme_disks()
for nvme in nvme_disks:
    nvme_params = nvme['params']
    cmd = "%s %s %s" % (nvme_params['name'], nvme_params['trtype'],
                        nvme_params['traddr'])
    child.sendline("/bdevs/NVMe create %s" % cmd)
    child.expect("/>")
create_aio_file()
child.sendline("/bdevs/AIO create aio /tmp/sample_aio 512")
child.expect("/>")
child.sendline("ls")
child.expect("/>")
match_output(child.before, "spdkcli_test_output.match")
print child.before
child.sendline("bdevs/AIO delete aio")
child.expect("/>")
delete_aio_file()
for nvme in nvme_disks:
    child.sendline("/bdevs/NVMe delete %sn1" % nvme['params']['name'])
    child.expect("/>")
child.sendline("/bdevs/Null delete null_bdev")
child.expect("/>")
child.sendline("/bdevs/Logical_Volume delete lvs/lvol")
child.expect("/>")
child.sendline("/bdevs/Split_Disk destruct_split_bdev Malloc2")
child.expect("/>")
child.sendline("/lvol_stores delete lvs")
child.expect("/>")
child.sendline("/bdevs/Malloc delete Malloc0")
child.expect("/>")
child.sendline("/bdevs/Malloc delete Malloc1")
child.expect("/>")
child.sendline("/bdevs/Malloc delete Malloc2")
child.expect("/>")
child.sendline("ls")
child.expect("/>")
print child.before
child.close()
