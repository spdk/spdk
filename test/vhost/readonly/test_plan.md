# vhost-block readonly feature test plan

## Objective
Vhost block controllers can be created with readonly feature which prevents any write operations on this device.
The purpose of this test is to verify proper operation of this feature.

## Test cases desription
...

## Test cases

### blk_ro_tc1
1. Start vhost
2. Create vhost-blk with NVMe device and readonly feature enabled, using RPC
3. Run VM with attached vhost-blk controller
4. Check visibility of readonly flag using lsblk, fdisk
5. ...

### blk_ro_tc2
1. Start vhost
2. Create two vhost-blks with NVMe device, one controller with readonly feature enabled, using RPC
3. Run two VMs with each attached vhost-blk controller
4. 1st VM: Check visibility of readonly flag using lsblk, fdisk
5. 1st VM: ...
6. 2nd VM: Ensure that readonly flag is disabled
7. 2nd VM: ...
