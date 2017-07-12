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

#### FIO Integrity tests
- NVMe device is split into 4 LUNs, each is attached to separate vhost controller
- FIO uses job configuration with randwrite mode to verify if random pattern was
  written to and read from correctly on each LUN
- runs on Fedora 25 and Ubuntu 16.04 guest systems

#### Filesystem integrity
- runs SPDK with 1 VM with 1 NVMe device attached.
- creates a partition table and filesystem on passed device, and mounts it
- runs Linux kernel source compilation
- Tested file systems: ext2, ext3, ext4, brtfs, ntfs, fat

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

#### RPC tests
- start SPDK vhost with fresh configuration, just RPC enabled
- modify vhost running configuration using related RPC calls; this includes:
    - creating and removing of scsi and block controllers,
    - adding and removing scsi luns,
    - adding and removing controllers to/from VMs during I/O operations
        (hotplug and hotremove tests),
    - verification of actual configuration vs user input based on "get" RPC calls

#### Hotplug & Hotremove tests
- run as part of RPC tests
- hotplug and hotremove operations performed both with and without
    background I/O traffic
- I/O traffic generated with FIO read/write operations,
    with enabled I/O verification
- vhost device hotplug and hotremove tests - adding and removing device to
    a controller which is already in use
- LUN hotremove from VM (simulated by unbinding the disk from driver)


### Performance tests
Tests verifying the performance and efficiency of the module.

#### FIO Performance 6 NVMes
- SPDK is run on 2 CPU cores
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
