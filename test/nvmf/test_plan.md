# SPDK nvmf_tgt test plan

## Objective
The purpose of these tests is to verify correct behavior of SPDK NVMeOF
feature.
These tests are run either per-commit or as nightly tests.

## Configuration
All tests share the same basic configuration file for SPDK nvmf_tgt to run.
Static configuration from config file consists of setting number of per session
queues and enabling RPC for further configuration via RPC calls.
RPC calls used for dynamic configuration consist:
- creating Malloc backend devices
- creating Null Block backend devices
- constructing NVMe-OF subsystems
- deleting NVMe-OF subsystems

### Tests

#### Test 1: NVMe-OF namespace on a Logival Volumes device
This test configures a SPDK NVMe-OF subsystem backed by logival volume
devices and uses FIO to generate I/Os that target those subsystems.
The logical volume bdevs are backed by malloc bdevs.
Test steps:
- Step 1: Assign IP addresses to RDMA NICs.
- Step 2: Start SPDK nvmf_tgt application.
- Step 3: Create malloc bdevs.
- Step 4: Create logical volume stores on malloc bdevs.
- Step 5: Create 10 logical volume bdevs on each logical volume store.
- Step 6: Create NVMe-OF subsystems with logical volume bdev namespaces.
- Step 7: Connect to NVMe-OF susbsystems with kernel initiator.
- Step 8: Run FIO with workload parameters: blocksize=256k, iodepth=64,
workload=randwrite; varify flag is enabled so that FIO reads and verifies
the data written to the logical device. The run time is 10 seconds for a
quick test an 10 minutes for longer nightly test.
- Step 9: Disconnect kernel initiator from NVMe-OF subsystems.
- Step 10: Delete NVMe-OF subsystems from configuration.

#### NVMe-OF namespace on a Pmem device
This test configures a SPDK NVMe-OF subsystem backed by pmem
devices and uses FIO to generate I/Os that target those subsystems.
The logical volume bdevs are backed by malloc bdevs.
Test steps:
- assign IP addresses to RDMA NICs.
- start SPDK nvmf_tgt application.
- create 10 pmem pools.
- create pmem bdevs on pmem pools.
- create NVMe-OF subsystems with 10 pmem bdevs namespaces.
- connect to NVMe-OF susbsystems with kernel initiator.
- run FIO with workload parameters: blocksize=128kB, iodepth=16,
    workload=randwrite; varify flag is enabled so that FIO reads and verifies
    the data written to the pmem device. The run time is 10 seconds for a
    quick test an 10 minutes for longer nightly test.
- disconnect kernel initiator from NVMe-OF subsystems.
- delete NVMe-OF subsystems from configuration.
