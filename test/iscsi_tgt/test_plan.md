# SPDK iscsi_tgt test plan

## Objective
The purpose of these tests is to verify correct behavior of SPDK iSCSI target
feature.
These tests are run either per-commit or as nightly tests.

## Configuration
All tests share the same basic configuration file for SPDK iscsi_tgt to run.
Static configuration from config file consists of setting number of per session
queues and enabling RPC for further configuration via RPC calls.
RPC calls used for dynamic configuration consist:
- creating Malloc backend devices
- creating Null Block backend devices
- creating Pmem backend devices
- constructing iSCSI subsystems
- deleting iSCSI subsystems

### Tests

#### Test 1: iSCSI  namespace on a Pmem device
This test configures a SPDK iSCSI subsystem backed by pmem
devices and uses FIO to generate I/Os that target those subsystems.
Test steps:
- Step 1: Start SPDK iscsi_tgt application.
- Step 2: Create 10 pmem pools.
- Step 3: Create pmem bdevs on pmem pools.
- Step 4: Create iSCSI subsystems with 10 pmem bdevs namespaces.
- Step 5: Connect to iSCSI susbsystems with kernel initiator.
- Step 6: Run FIO with workload parameters: blocksize=4096, iodepth=64,
	workload=randwrite; varify flag is enabled so that
	FIO reads and verifies the data written to the pmem device.
	The run time is 10 seconds for a quick test an 10 minutes
	for longer nightly test.
- Step 7: Run FIO with workload parameters: blocksize=128kB, iodepth=4,
	workload=randwrite; varify flag is enabled so that
	FIO reads and verifies the data written to the pmem device.
	The run time is 10 seconds for a quick test an 10 minutes
	for longer nightly test.
- Step 8: Disconnect kernel initiator from iSCSI subsystems.
- Step 9: Delete iSCSI subsystems from configuration.
