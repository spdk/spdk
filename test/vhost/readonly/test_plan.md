# vhost-block readonly feature test plan

## Objective
Vhost block controllers can be created with readonly feature which prevents any write operations on this device.
The purpose of this test is to verify proper operation of this feature.

## Test cases description
To test readonly feature, this test will create normal vhost-blk controller with NVMe device and on a VM it will
create and mount a partition to which it will copy a file. Next it will poweroff a VM, remove vhost controller and
create new readonly vhost-blk controller with the same device.

## Test cases

### blk_ro_tc1
1. Start vhost
2. Create vhost-blk controller with NVMe device and readonly feature disabled using RPC
3. Run VM with attached vhost-blk controller
4. Check visibility of readonly flag using lsblk, fdisk
5. Create new partition
6. Create new file on new partition
7. Shutdown VM, remove vhost controller
8. Create vhost-blk with previously used NVMe device and readonly feature now enabled using RPC
9. Run VM with attached vhost-blk controller
10. Check visibility of readonly flag using lsblk, fdisk
11. Try to delete previous file
12. Try to create new file
13. Try to remove partition
14. Repeat steps 2 to 4
15. Remove file from disk, delete partition
16. Shutdown VM, exit vhost
