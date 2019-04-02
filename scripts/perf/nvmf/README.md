## Running NVMe-OF Performace Testcases

In order to reproduce test cases described in [SPDK NVMe-OF Performance Test Cases](https://dqtibwqq6s6ux.cloudfront.net/download/performance-reports/SPDK_nvmeof_perf_report_18.04.pdf) follow the following instructions.

Currently RDMA NIC IP address assignment must be done manually before running the tests.

# Prepare the configuration file
Configure the target, initiators, and FIO workload in the json configuration file.

## General
Options which apply to both target and all initiator servers such as "password" and "username" fields.
All servers are required to have the same user credentials for running the test.
Test results can be found in /tmp/results directory.
### transport
Transport layer to use between Target and Initiator servers - rdma or tcp.

## Target
Configure the target server information.
### nic_ips
List of IP addresses othat will be used in this test..
NVMe namespaces will be split between provided IP addresses.
So for example providing 2 IP's with 16 NVMe drives present will result in each IP managing
8 NVMe subystems.
### mode
"spdk" or "kernel" values allowed.
### use_null_block
Use null block device instead of present NVMe drives. Used for latency measurements as described
in Test Case 3 of performance report.
### num_cores
List of CPU cores to assign for running SPDK NVMe-OF Target process. Can specify exact core numbers or ranges, eg:
[0, 1, 10-15].
### nvmet_dir
Path to directory with nvmetcli application. If not provided then system-wide package will be used
by default. Not used if "mode" is set to "spdk".
### num_shared_buffers
Number of shared buffers to use when creating transport layer.

## Initiator
Describes initiator arguments. There can be more than one initiator section in the configuration file.
For the sake of easier results parsing from multiple initiators please use only digits and letters
in initiator section name.
### ip
Management IP address used for SSH communication with initiator server.
### nic_ips
List of target IP addresses to which the initiator should try to connect.
### mode
"spdk" or "kernel" values allowed.
### num_cores
Applies only to SPDK initiator. Number of CPUs core to use for running FIO job.
If not specified then by default each connected subsystem gets its own CPU core.
### nvmecli_dir
Path to directory with nvme-cli application. If not provided then system-wide package will be used
by default. Not used if "mode" is set to "spdk".

## fio
Fio job parameters.
- bs: block size
- qd: io depth
- rw: workload mode
- rwmixread: percentage of reads in readwrite workloads
- run_time: time (in seconds) to run workload
- ramp_time: time (in seconds) to run workload before statistics are gathered
- run_num: how many times to run given workload in loop
