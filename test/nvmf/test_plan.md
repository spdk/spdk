# SPDK nvmf_tgt feature test plan

## Objective
The purpose of these tests is to verify correct behavior of SPDK NVMeOF
feature.
These tests are run either per-commit or as nightly tests.

## Methodology
All tests share the same basic configuration file for SPDK nvmf_tgt to run.
Other configuration steps are done via RPC calls, as needed in each test script.

### Tests

#### Lvol integrity
This tests checks that Logical Volumes devices can be correctly used in
SPDK NVMeOF configuration.
This includes using logical volumes as devices in NVMeOF subsystems and
correct I/O operations by usin FIO generated traffic with
"verify" flag enabled.
Logical volume stores and logical volume bdevs are backed by dynamically
created malloc bdevs.
Test steps:
- assign RDMA NICs with IP addresses
- run SPDK nvmf_tgt
- create malloc bdev
- create logical volume store on malloc bdev
- create 10 logical volume bdevs on logical volume store
- create NVMf subsystem with logical volume bdevs as namespaces
- connect from kernel to created SPDK NVMf subsystem
- run FIO traffic: blocksize=256k, iodepth=64, 10 seconds runtime;
mode is randwrite with 'verify' enabled so that way read operations
are covered too
- disconnect from NVMf subsystems
