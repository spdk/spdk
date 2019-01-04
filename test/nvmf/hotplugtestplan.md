# NVMF Hotplug test plan

## Objectives

The purpose of these tests is to verify the correct behavior of SPDK NVMe-oF hotplug support.

## Configurations

Hotplug feature is mainly used for NVMe bdev. So we will test nvme bdev with following different configurations:
static configuration file and rpc method;
Intel U.2 NVMe SSD;
single core and multiple core config;
heavy fio workload(refer to SPDK github issue 498, https://github.com/spdk/spdk/issues/498);
different nvmf, transport(PCIe, RDMA and TCP) and nvme configuration;
virtual bdev on raw nvme bdev:
lvol bdev on nvme bdev,
raid bdev on nvme bdev;
multiple nvmf host.

## tests

### test 1: run nvmf target with one core and normal fio workload
The SSD can be P3700, P4500/4600 and P4800X.
The nvmf, transport and nvme configuration can differ,
like MaxQueueDepth, MaxQueuesPerSession, InCapsuleDataSize, etc.
The bdev can be: nvme bdev, lvol bdev, crypto bdev and raid bdev.
two host connect to one target using switch.
Steps are as below:

1. run spdk nvmf target in target machine with configuration file;
2. two host connect to the target;
3. run fio job in two host;
4. while running fio, unplug all nvme ssd;
5. check if nvmf target not crash, and fio reported error;
6. plug into the  nvme ssd again;
7. run scripts/setup.sh to bind all nvme ssd;
8. use rpc method to create bdev and subsystem;
9. in host machine, reconnect to the nvmf target and fio job;
10. wait for the fio finishs successfully;
11. use ctrlr+c to kill nvmf target, check if nvmf target not crash;

Negative case:
after step 5 above, use ctrlr+c to kill nvmf target, check if nvmf target not crash.

### test 2: run nvmf target with one core and heavy fio workload
same config with test 1.
same steps with test 1 except heavy fio workload.
### test 3: run nvmf target with multiple core(0xffff) and normal fio workload
same config with test 1 except multiple cores.
same steps with test 1 except running nvmf with "-m 0xffff".

### test 4: run nvmf target with multiple core(0xffff) and heavy fio workload
same config with test 3.
same steps with test 3 except heavy fio workload.
