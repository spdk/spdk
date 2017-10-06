# SPDK nvmf_tgt test plan

## Objective
The purpose of these tests is to verify correct behavior of SPDK NVMe-oF
feature.
These tests are run either per-commit or as nightly tests.

## Configuration
All tests share the same basic configuration file for SPDK nvmf_tgt to run.
Static configuration from config file consists of setting number of per session
queues and enabling RPC for further configuration via RPC calls.
RPC calls used for dynamic configuration consist:
- creating Malloc backend devices
- creating Null Block backend devices
- constructing NVMe-oF subsystems
- deleting NVMe-oF subsystems

### Tests

#### Test 1: NVMe-oF namespace on a Logical Volumes device
This test configures a SPDK NVMe-oF subsystem backed by logical volume
devices and uses FIO to generate I/Os that target those subsystems.
The logical volume bdevs are backed by malloc bdevs.
Test steps:
- Step 1: Assign IP addresses to RDMA NICs.
- Step 2: Start SPDK nvmf_tgt application.
- Step 3: Create malloc bdevs.
- Step 4: Create logical volume stores on malloc bdevs.
- Step 5: Create 10 logical volume bdevs on each logical volume store.
- Step 6: Create NVMe-oF subsystems with logical volume bdev namespaces.
- Step 7: Connect to NVMe-oF susbsystems with kernel initiator.
- Step 8: Run FIO with workload parameters: blocksize=256k, iodepth=64,
workload=randwrite; varify flag is enabled so that FIO reads and verifies
the data written to the logical device. The run time is 10 seconds for a
quick test an 10 minutes for longer nightly test.
- Step 9: Disconnect kernel initiator from NVMe-oF subsystems.
- Step 10: Delete NVMe-oF subsystems from configuration.

### Compatibility testing

- Verify functionality of SPDK `nvmf_tgt` with Linux kernel NVMe-oF host
  - Exercise various kernel NVMe host parameters
    - `nr_io_queues`
    - `queue_size`
  - Test discovery subsystem with `nvme` CLI tool
    - Verify that discovery service works correctly with `nvme discover`
    - Verify that large responses work (many subsystems)

### Specification compliance

- NVMe base spec compliance
  - Verify all mandatory admin commands are implemented
    - Get Log Page
    - Identify (including all mandatory CNS values)
      - Identify Namespace
      - Identify Controller
      - Active Namespace List
      - Allocated Namespace List
      - Identify Allocated Namespace
      - Attached Controller List
      - Controller List
    - Abort
    - Set Features
    - Get Features
    - Asynchronous Event Request
    - Keep Alive
  - Verify all mandatory NVM command set I/O commands are implemented
    - Flush
    - Write
    - Read
  - Verify all mandatory log pages
    - Error Information
    - SMART / Health Information
    - Firmware Slot Information
  - Verify all mandatory Get/Set Features
    - Arbitration
    - Power Management
    - Temperature Threshold
    - Error Recovery
    - Number of Queues
    - Write Atomicity Normal
    - Asynchronous Event Configuration
  - Verify all implemented commands behave as required by the specification
- Fabric command processing
  - Verify that Connect commands with invalid parameters are failed with correct response
    - Invalid RECFMT
    - Invalid SQSIZE
    - Invalid SUBNQN, HOSTNQN (too long, incorrect format, not null terminated)
    - QID != 0 before admin queue created
    - CNTLID != 0xFFFF (static controller mode)
  - Verify that non-Fabric commands are only allowed in the correct states

### Configuration and RPC

- Verify that invalid NQNs cannot be configured via conf file or RPC
