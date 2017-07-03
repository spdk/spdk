# SPDK hotplug Test Plan

## continuous integration tests

#### Test case 1: iscsi target + Fio I/O operations
- Use qemu tool keep the system has 4 NVMe installed at virtual machine.
- At virtual machine run setup.sh.
- At virtual machine run iscsi target.
- Use rpc to create 4 target nodes base on the NVMe bdevs at virtual machine.
- Login to the target nodes.
- At virtual machine run fio.
- Remove 4 NVMe devices when fio is running, the fio should return error code and stop immediately, check the fio return code, make sure it meet the expectation.
- Logout and delete the target nodes.
- Insert 4 NVMe devices when iscsi target is running.
- At virtual machine run setup.sh.
- At virtual machine create 4 target nodes which based on the hotinsert NVMe devices.
- At virutal machine start run fio until exit.

## System/integration tests

### Test cases detail describe

#### Test case 1: iscsi target + Fio I/O operations
- Keep the system has 4 NVMe installed.
- Run setup.sh.
- Run iscsi target.
- Use rpc to create 4 target nodes base on the NVMe bdevs.
- Login to the target nodes.
- Run fio.
- Remove 4 NVMe devices when fio is running, the fio should return error code and stop immediately, check the fio return code, make sure it meet the expectation.
- Logout and delete the target nodes.
- Insert 4 NVMe devices when iscsi target is running.
- Run setup.sh.
- Create 4 target nodes which based on the hotinsert NVMe devices.
- Start run fio until exit.

#### Test case 2: nvmf target + Fio I/O operations
- Keep the system has 4 NVMe installed.
- Run setup.sh.
- Run nvmf target.
- Use rpc to create 4 subsystems base on the NVMe bdevs.
- Connect to the subsystems.
- Run fio.
- Remove 4 NVMe devices when fio is running, the fio should return error code and stop immediately, check the fio return code, make sure it meet the expectation.
- Disconnect and delete the subsystems.
- Insert 4 NVMe devices when nvmf target is running.
- Run setup.sh.
- Create 4 subsystems which based on the hotinsert NVMe devices.
- Start run fio until exit.

## Stress tests
- Stress tests (long interval time between remove NVMe devices and insert NVMe devices).
