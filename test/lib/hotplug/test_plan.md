# SPDK hotplug Test Plan

## Functional tests

### Test cases detail describe

#### Test case 1: iscsi target + Fio I/O operations
- Run iscsi target and use rpc create 4 NVMe nodes.
- Iscsiadm discovery and login, find this 4 NVMe nodes. 
- Run fio.
- Remove 4 NVMe devices when fio is running, the result is an fio abnormal exit.
- Iscsiadm logout and delete.
- Insert 4 NVMe devices when iscsi target is running.
- Use rpc create 4 NVMe nodes and iscsiadm discovery and login. 
- Start run fio until normal exit.

#### Test case 2: nvmf target + Fio I/O operations
- Run nvmf target and use rpc create 4 NVMe nodes.
- Modprobe nvme_rdma and nvme_fabrics, use nvme command discover and connect, find this 4 NVMe nodes. 
- Run fio.
- Remove 4 NVMe devices when fio is running.
- Nvme disconnect.
- Insert 4 NVMe devices when nvmf target is running.
- Use rpc create 4 NVMe nodes and nvme discover and connect. 
- Start run fio until normal exit.

## Stress tests
- Stress tests (long interval time between remove NVMe devices and insert NVMe devices).
