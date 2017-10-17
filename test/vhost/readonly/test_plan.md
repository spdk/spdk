# vhost-block readonly feature test plan

## Objective
Vhost block controllers can be created with readonly feature which prevents any write operations on this device.
The purpose of this test is to verify proper operation of this feature.

## Test cases desription
To test readonly feature, this test will create normal vhost-blk controller with nvme device and on a VM it will
create and mount a partition to which it will copy a file. Next it will poweroff a vm, remove vhost controller and 
create new readonly vhost-blk controller with the same device. On VM after mounting previous partition, previously file
should be present. To verify readonly feature this test will try to remove file, create new file and delete partition
from write protected block.

## Test cases

### blk_ro_tc1
1. Start vhost
2. Create vhost-blk with NVMe device and readonly feature disabled, using RPC
3. Run VM with attached vhost-blk controller
4. Check visibility of readonly flag using lsblk, fdisk
5. Create new partition
6. Create new file on new partition
7. Kill Vm, remove vhost controller
8. Create vhost-blk with previously used NVMe device and readonly feature now enabled, using RPC
9. Run VM with attached vhost-blk controller
10.Check visibility of readonly flag using lsblk, fdisk
11.Try to delete previous file
12.Try to create new file
13.Try to remove partition
14.End test case

### blk_ro_tc2
1. Start vhost
2. Create two vhost-blks with NVMe device, one controller with readonly feature enabled, using RPC
3. Run two VMs with each attached vhost-blk controller
4. 1st VM: Check visibility of readonly flag using lsblk, fdisk
5. 1st VM: ...
6. 2nd VM: Ensure that readonly flag is disabled
7. 2nd VM: ...
