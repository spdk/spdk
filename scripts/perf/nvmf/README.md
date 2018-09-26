## Running NVMeOF performace testcases

In order to reproduce test cases described in [SPDK NVMeOF performance report](https://ci.spdk.io/download/performance-reports/SPDK_nvmeof_perf_report_18.04.pdf) follow commands in subsections.

Currently RDMA NIC IP addressess assignment must be done manually before running the tests. CPU core mask for test must also be provided by user as cores are not distributed between NUMA nodes automatically.

# Test case 1 - SPDK NVMeOF Target I/O Core Scaling
On example of running SPDK Target with 4 CPU cores.
On target side issue command:
```
./scripts/perf/nvmf/run_nvmf.py spdk_tgt -c [0-1,22-23] -a 192.0.1.1,192.0.2.1
-c - Core mask to use for starting SPDK NVMeOF Target. Can be either a hexadecimal value like 0x1 or a list parameter with ranges like [0-1,22-23]
-a - List of RDMA NICs addressess in system to use for creating subsystems
```
This will generate a configuration file and run target with it.

On initiator side issue command:
```
./scripts/perf/nvmf/run_nvmf.py spdk_init -a 192.0.1.1,192.0.2.1 -b 128k -i 32 -r randread
-a - List of remote subsystems addresses to discover and connect to
-b - block size for workload
-i - io depth for workload
-r - rw mode for workload
```
This will create a list of remote bdevs to connect to, as well as fio configuration file and run FIO tests. Other parameters used by this script for running spdk initiator can be found by calling:
```
./scripts/perf/run_nvmf.py spdk_init -h
```
