## Running NVMe-OF Performace Testcases

In order to reproduce test cases described in [SPDK NVMe-OF Performance Test Cases](https://ci.spdk.io/download/performance-reports/SPDK_nvmeof_perf_report_18.04.pdf) follow the following instructions.

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

### null_block_devices

Integer. Use null block devices instead of present NVMe drives.
If set to 1, can be used for latency measurements as described in Test Case 3 of performance report.

### null_block_dif_type

Integer. Enable data protection on created null block device. Defaults to 0 if option
not present in JSON configuration file. See doc/jsonrpc.md "bdev_null_create" for details.

### num_cores

List of CPU cores to assign for running SPDK NVMe-OF Target process. Can specify exact core numbers or ranges, eg:
[0, 1, 10-15].

### nvmet_bin

Path to nvmetcli application executable. If not provided then system-wide package will be used
by default. Not used if "mode" is set to "spdk".

### num_shared_buffers

Number of shared buffers to use when creating transport layer.

### dif_insert_strip

Boolean. If set to true - enable "dif_insert_or_strip" option for TCP transport layer.

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

### cpus_allowed

List of CPU cores to assign for running SPDK NVMe-OF initiator process.
Can specify exact core numbers: 0,5
or ranges: 10-15
or binding to CPUs 0, 5, and 8 to 15: `cpus_allowed=0,5,8-15`.
If not specified then will use num_cores option.
If specified with num_cores then cpu_allowed parameter has higher priority than num_cores.

### num_cores

Applies only to SPDK initiator. Number of CPUs core to use for running FIO job.
If not specified then by default each connected subsystem gets its own CPU core.

### nvmecli_dir

Path to directory with nvme-cli application. If not provided then system-wide package will be used
by default. Not used if "mode" is set to "spdk".

### fio_bin

Path to the fio binary that will be used to compile SPDK and run the test.
If not specified, then the script will use /usr/src/fio/fio as the default.

### extra_params

Space separated string with additional settings for "nvme connect" command
other than -t, -s, -n and -a.

## fio

Fio job parameters.

- bs: block size
- qd: io depth - Per connected fio filename target
- rw: workload mode
- rwmixread: percentage of reads in readwrite workloads
- run_time: time (in seconds) to run workload
- ramp_time: time (in seconds) to run workload before statistics are gathered
- run_num: how many times to run given workload in loop

# Running Test

Before running the test script use the setup.sh script to bind the devices you want to
use in the test to the VFIO/UIO driver.
Run the script on the NVMe-oF target system:

    cd spdk
    sudo PYTHONPATH=$PYTHONPATH:$PWD/scripts scripts/perf/nvmf/run_nvmf.py
The script uses the config.json configuration file in the scripts/perf/nvmf directory by default. You can
specify a different configuration file at runtime as shown below:
sudo PYTHONPATH=$PYTHONPATH:$PWD/scripts scripts/perf/nvmf/run_nvmf.py /path/to/config file/json config file

The script uses another spdk script (scripts/rpc.py) so we pass the path to rpc.py by setting the Python path
as a runtime environment parameter.

# Test Results

When the test completes, you will find a csv file (nvmf_results.csv) containing the results in the target node
directory /tmp/results.

# Processor Counter Monitor (PCM)
PCM Tools provides a number of command-line utilities for real-time monitoring.
Before using PCM Tools in nvmf perf scripts it needs to be installed on Target machine.
PCM source and instructions are available on https://github.com/opcm/pcm.
To enable PCM in perf test you need to add Target setting in config.json file:
```
"pcm_settings": ["pcm_directory", "measure_cpu", "measure_memory", delay_time, measure_interval, sample_count]
```
example:
```
"pcm_settings": ["/tmp/pcm", true, true, 10, 1, 30]
```
Example above will run PCM measure for cpu and memory, with start delay 10s, sample every 1 second,
and 30 samples for cpu measure. PCM memory do not support sample count.

# Bandwidth monitor (bwm-ng)
PCM Tools provides a number of command-line utilities for real-time monitoring.
Before using bwm-ng in nvmf perf scripts it needs to be installed on Target machine.
To enable bandwidth monitor in perf test you need to add Target setting in config.json file:
```
"bandwidth_settings": [bool, sample_count]
```
example:
```
"bandwidth_settings": [true, 30]
```
