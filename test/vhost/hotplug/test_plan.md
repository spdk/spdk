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
