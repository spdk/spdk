# SPDK vhost Test Plan

## Current Tests

### Integrity tests

#### vhost self test
- compiles SPDK and Qemu
- launches SPDK Vhost
- starts VM with 1 NVMe device attached to it
- issues controller "reset" command using sg3_utils on guest system
- performs data integrity check using dd to write and read data from the device
- runs on 3 host systems (Ubuntu 16.04, Centos 7.3 and Fedora 25)
  and 1 guest system (Ubuntu 16.04)
- runs against vhost scsi and vhost blk

#### FIO Integrity tests
- NVMe device is split into 4 LUNs, each is attached to separate vhost controller
- FIO uses job configuration with randwrite mode to verify if random pattern was
  written to and read from correctly on each LUN
- runs on Fedora 25 and Ubuntu 16.04 guest systems
- runs against vhost scsi and vhost blk

#### Lvol tests
- starts vhost with at least 1 NVMe device
- starts 1 VM or multiple VMs
- lvol store is constructed on each NVMe device
- on each lvol store 1 lvol bdev will be constructed for each running VM
- Logical volume block device is used as backend instead of using
  NVMe device backed directly
- after set up, data integrity check will be performed by FIO randwrite
  operation with verify flag enabled
- optionally nested lvols can be tested with use of appropriate flag;
  On each base lvol store additional lvol bdev will be created which will
  serve as a base for nested lvol stores.
  On each of the nested lvol stores there will be 1 lvol bdev created for each
  VM running. Nested lvol bdevs will be used along with base lvol bdevs for
  data integrity check.
- runs against vhost scsi and vhost blk

#### Filesystem integrity
- runs SPDK with 1 VM with 1 NVMe device attached.
- creates a partition table and filesystem on passed device, and mounts it
- runs Linux kernel source compilation
- Tested file systems: ext2, ext3, ext4, brtfs, ntfs, fat
- runs against vhost scsi and vhost blk

#### Windows HCK SCSI Compliance Test 2.0.
- Runs SPDK with 1 VM with Windows Server 2012 R2 operating system
- 4 devices are passed into the VM: NVMe, Split NVMe, Malloc and Split Malloc
- On each device Windows HCK SCSI Compliance Test 2.0 is run

#### MultiOS test
- start 3 VMs with guest systems: Ubuntu 16.04, Fedora 25 and Windows Server 2012 R2
- 3 physical NVMe devices are split into 9 LUNs
- each guest uses 3 LUNs from 3 different physical NVMe devices
- Linux guests run FIO integrity jobs to verify read/write operations,
    while Windows HCK SCSI Compliance Test 2.0 is running on Windows guest

#### vhost hot-remove tests
- removing NVMe device (unbind from driver) which is already claimed
    by controller in vhost
- hotremove tests performed with and without I/O traffic to device
- I/O traffic, if present in test, has verification enabled
- checks that vhost and/or VMs do not crash
- checks that other devices are unaffected by hot-remove of a NVMe device
- performed against vhost blk and vhost scsi

#### vhost scsi hot-attach and hot-detach tests
- adding and removing devices via RPC to a controller which is already in use by a VM
- I/O traffic generated with FIO read/write operations, verification enabled
- checks that vhost and/or VMs do not crash
- checks that other devices in the same controller are unaffected by hot-attach
    and hot-detach operations

### Vhost initiator test
Testing vhost initiator with fio sequential read/write and random read/write with verificiation enabled.
Tests 1-8 should be run on vhost and guest.

#### Test Case 1
1. Run vhost with one scsi controller and with one malloc bdev with 512 block size.
2. Prepare config for bdevio with virtio section.
3. Run bdevio with config.
4. Generate the fio config file given the list of all bdevs.
5. Run fio tests: iodepth=128, block_size=4k, rw, randread, randwrite, read, write, randrw with verify
6. Check if fio tests are successful

#### Test Case 2
1. Run vhost with one scsi controller and with one nvme bdev with 512 block size.
2. Repeat steps 2-6 from test case 1.

#### Test Case 3
1. Run vhost with one scsi controller and with one lvol bdev with 512 block size.
2. Repeat steps 2-6 from test case 1

#### Test Case 4
1. Run vhost with one scsi controller and with one malloc bdev with 4096 block size.
2. Repeat steps 2-6 from test case 1.

#### Test Case 5
1. Run vhost with one scsi controller and with one nvme bdev with 4096 block size.
2. Repeat steps 2-6 from test case 1.

#### Test Case 6
1. Run vhost with one scsi controller and with one lvol bdev with 4096 block size.
2. Repeat steps 2-6 from test case 1

#### Test Case 7
1. Run vhost with one scsi controller and with one nvme bdev with 512 block size and disk size larger than 4G
   to test if we can read, write to device with fio offset set to 4G.
2. Repeat steps 2-6 from test case 1.

#### Test Case 8
1. Run vhost with one scsi controller and with one nvme bdev with 4096 block size and disk size larger than 4G
   to test if we can read, write to device with fio offset set to 4G.
2. Repeat steps 2-6 from test case 1.

#### Test Case 9
1. Run vhost with one scsi controller and with one malloc bdev.
2. Run vhost with virtio initiator and pass two cores.
   Split virtio dev into 2 devices and add each split device to one controller. Use one CPU core for every controller.
3. Run guest with multiqueue enabled with core numbers equal to 2 and with num_queues equal to 2.
4. Check if two additional disk showed up in vm.
5. Run fio test on vm for two disks.
6. Check if all cores are used by vm.

### Test Case 10
1. Run vhost with two scsi controllers, one with nvme bdev and one with malloc bdev
2. Run vhost with virtio initiator and pass two sockects from first vhost.
3. Run guest with two devices from second vhost.
4. Run fio tests on vm using two devices.
5. Check if fio tests pass.

### Test Case 11
1. Run vhost with one controller and one malloc bdev.
2. Run fio test with given configuration:
   [job_wr1]
   stonewall
   rw=write
   [job_rd1]
   stonewall
   rw=read
   [job_tr1]
   stonewall
   rw=trim
   [job_rd2]
   stonewall
   rw=read
3. Check if fio test ends with failure.

#### Annotation
- More test cases will come after resolving
  [current vhost initiator limitations](spdk/tree/master/lib/bdev/virtio/README.md#Todo).

### Performance tests
Tests verifying the performance and efficiency of the module.

#### FIO Performance 6 NVMes
- SPDK is run on 2 CPU cores
- Run with vhost scsi
- 6 VMs are run with 2 cores, 1 controller (2 queues), 1 Split NVMe LUN each
- FIO configurations runs are 15 minute job combinations of:
    - IO depth: 1, 8, 128
    - Blocksize: 4k
    - RW modes: read, randread, write, randwrite, rw, randrw

    Write modes are additionally run with 10 minute ramp-up time to allow better
    measurements. Randwrite mode uses longer ramp-up preconditioning of 90 minutes per run.


#### Full Performance Suite
On-demand performance tests allowing to run test jobs which can be combinations of:
- SPDK cores: 1-3 CPU cores,
- VM cores: 1-5 CPU cores per VM,
- VM count: 1-12,
- vhost controller queues: single, multi
- FIO IO depth: 1, 2, 4, 8, 32, 64, 128
- FIO Blocksize: 4k
- FIO RW modes: read, randread, write, randwrite, rw, randrw
- each test job takes from 30 to 120 minutes


## Future tests and improvements

### Performance tests
- Establish a baseline for acceptance level of FIO Performance 6 NVMe test results

### Stress tests
- Add stability and stress tests (long duration tests, long looped start/stop tests, etc.)
to test pool
