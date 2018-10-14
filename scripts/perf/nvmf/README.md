## Running NVMeOF performace testcases

In order to reproduce test cases described in [SPDK NVMeOF performance report](https://ci.spdk.io/download/performance-reports/SPDK_nvmeof_perf_report_18.04.pdf) follow instructions in subsections.

Currently RDMA NIC IP addressess assignment must be done manually before running the tests.

# Prepare configuration file for running the tests
Providing variables for setting up target and initiators, and also for running the FIO jobs is done via .json config file.
Short explanation of its structure and fields in following subsections.

## General
Options which apply to both target and all initiator servers. Only "password" and "username" fields for now,
all servers are required to have the same user credentials for running the test.

## Target
Information about target server.
### rdma_ips
List of IP addresses on this server to use for creating a test configuration.
Subsystems with NVMe namespaces will be split between provided IP addresses.
So for example providing 2 IP's with 16 NVMe drives present will result each IP managing
8 NVMe subystems.
### mode
"spdk" or "kernel" values allowed.
### use_null_block
Use null block device instead of present NVMe drives. Used for latency measurements as described
in Test Case 3 of performance report.
### num_cores
Number of cores to use for polling operation. Applies only to target in SPDK mode.
If any of the present NVMe drives is located on separate NUMA node then number of cores
will be split between NUMA nodes.
### nvmet_dir
Path to directory with nvmetcli application. If not provided then system-wide package will be used
by default.

## Initiator
Describes initiator arguments. There can be more than one initiator section in the configuration file.
For the sake of easier results parsing from multiple initiators please use only digits and letters
in initiator section name.
### ip
Management IP address used for SSH communication with initiator server.
### rdma_ips
List of target IP addresses to which the initiator should try to connect to.
### mode
"spdk" or "kernel" values allowed.
### num_cores
Applies only to SPDK initiator. Number of CPUs core to use for running FIO job.
If not specified then by default each connected subsystem gets its own CPU core.

## fio
Fio job parameters.
- bs: block size
- qd: io depth
- rw: workload mode
- rwmixread: percentage of reads in readwrite workloads
- run_time: time (in seconds) to run workload
- ramp_time: time (in seconds) to run workload before statistics are gathered
- run_num: how many times to run given workload in loop
