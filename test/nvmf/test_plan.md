# SPDK NVMe-oF target test plan

## Compatibility testing

- Verify functionality of SPDK `nvmf_tgt` with Linux kernel NVMe-oF host
  - Exercise various kernel NVMe host parameters
    - `nr_io_queues`
    - `queue_size`
  - Test discovery subsystem with `nvme` CLI tool
    - Verify that discovery service works correctly with `nvme discover`
    - Verify that large responses work (many subsystems)

## Specification compliance

- Virtual mode
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

## Configuration and RPC

- Verify that invalid NQNs cannot be configured via conf file or RPC
