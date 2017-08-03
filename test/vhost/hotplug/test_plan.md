#Vhost hotattach and hotdetach test plan

## Objective
The purpose of these tests is to verify that SPDK vhost remains stable during
hot-attach and hot-detach operations performed on SCSI controllers devices.
Hot-attach is a scenario where a device is added to controller already in use by
guest VM, while in hot-detach device is removed from controller when already in use.

## Test Cases Description
1. FIO I/O traffic is run during hot-attach and detach operations.
By default FIO uses default_integrity*.job config files located in
test/vhost/hotfeatures/fio_jobs directory.
2. FIO mode of operation in random write (randwrite) with verification enabled
which results in also performing read operations.
3. Test case descriptions below contain manual steps for testing.
Automated tests are located in test/vhost/hotfeatures.

### Hotattach, Hotdetach Test Cases prerequisites
1. Run vhost with 8 empty controllers. Prepare 16 nvme disks.
If you don't have 16 disks use split.
2. In test cases fio status is checked after every run if there are any errors.

### Hotattach Test Cases prerequisites
1. Run vms, first with ctrlr-1 and ctrlr-2 and second one with ctrlr-3 and ctrlr-4.

## Test Case 1
1. Attach NVMe to Ctrlr 1
2. Run fio integrity on attached device

## Test Case 2
1. Run fio integrity on attached device from test case 1
2. During fio attach another NVMe to Ctrlr 1
3. Run fio integrity on both devices

## Test Case 3
1. Run fio integrity on attached devices from previous test cases
2. During fio attach NVMe to Ctrl2
3. Run fio integrity on all devices

## Test Case 4
2. Run fio integrity on attached device from previous test cases
3. During fio attach NVMe to Ctrl3/VM2
4. Run fio integrity on all devices
5. Reboot VMs
6. Run fio integrity again on all devices


### Hotdetach Test Cases prerequisites
1. Run vms, first with ctrlr-5 and ctrlr-6 and second with ctrlr-7 and ctrlr-8.

## Test Case 1
1. Run fio on all devices
2. Detatch NVMe from Ctrl5 during fio
3. Check vhost or VMs did not crash
4. Check that detatched device is gone from VM
5. Check that fio job run on detached device stopped and failed

## Test Case 2
1. Attach NVMe to Ctrlr 5
2. Run fio on 1 device from Ctrl 5
3. Detatch NVMe from Ctrl5 during fio traffic
4. Check vhost or VMs did not crash
5. Check that fio job run on detached device stopped and failed
6. Check that detatched device is gone from VM

## Test Case 3
1. Attach NVMe to Ctrlr 5
2. Run fio with integrity on all devices, except one
3. Detatch NVMe without traffic during fio running on other devices
4. Check vhost or VMs did not crash
5. Check that fio jobs did not fail
6. Check that detatched device is gone from VM

## Test Case 4
1. Attach NVMe to Ctrlr 5
2. Run fio on 1 device from Ctrl 5
3. Run separate fio with integrity on all other devices (all VMs)
4. Detatch NVMe from Ctrl1 during fio traffic
5. Check vhost or VMs did not crash
6. Check that fio job run on detached device stopped and failed
7. Check that other fio jobs did not fail
8. Check that detatched device is gone from VM
9. Reboot VMs
10. Check that detatched device is gone from VM
11. Check that all other devices are in place
12. Run fio integrity on all remianing devices

# Vhost blk and scsi hot remove test plan

## Objective
The purpose of these tests is to verify that SPDK vhost remains stable during
hot-remove operations performed on SCSI and BLK controllers devices.
Hot-remove is a scenario where a NVMe device is removed when already in use.

## Test cases description
1. FIO I/O traffic is run during hot-remove operations.
   By default FIO uses default_integrity*.job config files located in
   test/vhost/hotplug/fio_jobs directory.
2. FIO mode of operation is random write (randwrite) with verification enabled
   which results in also performing read operations.
3. Test case descriptions below contain manual steps for testing.
   Automated tests are located in test/vhost/hotplug.

### Hotremove test cases prerequisites
1. Run vhost with prepare 2 NVMe disks and 2 splits per disk.
2. In test cases fio status is checked after every run if any errors occurred.

### Vhost BLK hot-remove test case.

## Test Case 1
1. Run the command to hot remove NVMe disk.
2. Check vhost did not crash.

## Test Case 2
1. Use rpc command to create blk controller on one NVMe disk.
2. Attach one NVMe bdev to blk controller.
3. Run one VM, attach to blk controller.
4. Run FIO I/O traffic with verification enabled on NVMe disk.
5. Run the command to hot remove NVMe disk.
6. Check that fio job run on hot-removed device stopped.
   Expect: Fio should return error message and return code != 0.
7. Reboot VM.
8. Run FIO I/O traffic with verification enabled on NVMe disk.
9. Check that fio job run on hot-removed device stopped.
   Expect: Fio should return error message and return code != 0.

## Test Case 3
1. Use rpc command to create one blk controller second of two NVMe disk.
2. Attach one NVMe bdev to blk controller.
3. Run one VM, attach to first blk controller.
4. Run FIO I/O traffic with verification enabled on first NVMe disk.
5. Run the command to hot remove of first NVMe disk.
6. Check that fio job run on hot-removed device stopped.
   Expect: Fio should return error message and return code != 0.
7. Reboot VM
8. Run FIO I/O traffic with verification enabled on on removed NVMe disk.
9. Check that fio job run on hot-removed device stopped.
   Expect: Fio should return error message and return code != 0.

## Test Case 4
1. Use rpc command to create two blk controller on two NVMe disks.
2. Attach one NVMe bdev to blk controller.
3. Run two VM, attach to blk controllers.
4. Run FIO I/O traffic with verification enabled on on both NVMe disks.
5. Run the command to hot remove of first NVMe disk.
6. Check that fio job run on hot-removed device stopped.
   Expect: Fio should return error message and return code != 0.
7. Check finished status FIO. Write and read in the not-removed
   NVMe disk should be successful.
8. Reboot all VMs.
9. Run FIO I/O traffic with verification enabled on on not-removed NVMe disk.
10. Check finished status FIO. Write and read in the not-removed
    NVMe disk should be successful.
    Expect: Fio should return return code == 0.
11. Run FIO I/O traffic with verification enabled on on removed NVMe disk.
12. Check that fio job run on hot-removed device stopped.
    Expect: Fio should return error message and return code != 0.

## Test Case 5
1. Use rpc command to create two blk controller, each with separate split NVMe bdev.
2. Run one VM, attach to blk controller.
3. Run FIO I/O traffic with verification enabled on both NVMe disks in VMs.
4. Run the command to hot remove NVMe disk.
5. Check that fio job run on hot-remove device stopped on both VMs.
   Expect: Fio should return error message and return code != 0.
6. Reboot both VMs.
7. Run FIO I/O traffic with verification enabled on on both VMs.
8. Check that fio job run on hot-remove device stopped on both VMs.
   Expect: Fio should return error message and return code != 0.

### Restart Vhost app.
### Vhost SCSI hot-remove test cases.

## Test Case 1
1. Run the command to hot remove NVMe disk.
2. Check vhost did not crash.

## Test Case 2
1. Use rpc command to create two scsi controller, each on separate split NVMe bdev.
2. Attach one split NVMe bdev to scsi controller.
3. Run one VM, attach to scsi controller.
4. Run FIO I/O traffic with verification enabled on on both NVMe disks in VMs.
5. Run the command to hot remove NVMe disk.
6. Check that fio job run on hot-remove device stopped on both VMs.
   Expect: Fio should return error message and return code != 0.
7. Check if removed devices are gone from lsblk.
8. Reboot both VMs.
9. Check if removed devices are gone from lsblk.
10. Run FIO I/O traffic with verification enabled on on both VMs.
11. Check that fio job run on hot-remove device stopped on both VMs.
    Expect: Fio should return error message and return code != 0.

## Test Case 3
1. Use rpc command to create two scsi controller on two NVMe disks.
2. Attach both NVMe bdevs to scsi controller.
3. Run one VM, attach to scsi controller.
4. Run FIO I/O traffic with verification enabled on on both NVMe disks in VMs.
5. Run the command to hot remove NVMe disk.
6. Check that fio job run on hot-remove device stopped on both VMs.
   Expect: Fio should return error message and return code != 0.
7. Check if removed devices are gone from lsblk.
8. Reboot both VMs.
9. Check if removed devices are gone from lsblk.
10. Run FIO I/O traffic with verification enabled on on both VMs.
11. Check that fio job run on hot-remove device stopped on both VMs.
    Expect: Fio should return error message and return code != 0.

## Test Case 4
1. Attachd one NVMe bdev to scsi controller.
2. Run one VM, attach to scsi controller.
3. Run FIO I/O traffic with verification enabled on on both NVMe disks.
4. Run the command to hot remove of first NVMe disk.
5. Check that fio job run on hot-removed device stopped.
   Expect: Fio should return error message and return code != 0.
6. Check if removed devices are gone from lsblk.
7. Check finished status FIO. Write and read in the not-removed.
   NVMe disk should be successful.
8. Reboot both VMs.
9. Check if removed disk are gone from lsblk.
10. Run FIO I/O traffic with verification enabled on on not-removed NVMe disk.
11. Check finished status FIO. Write and read in the not-removed.
    NVMe disk should be successful.
	Expect: Fio should return return code == 0.
12. Run FIO I/O traffic with verification enabled on on removed NVMe disk.
13. Check that fio job run on hot-removed device stopped.
	Expect: Fio should return error message and return code != 0.
14. Check if removed devices are gone from lsblk.
