# Running NVMe-OF Performance Test Cases

Scripts contained in this directory are used to run TCP and RDMA benchmark tests,
that are later published at [spdk.io performance reports section](https://spdk.io/doc/performance_reports.html).
To run the scripts in your environment please follow steps below.

## Test Systems Requirements

- The OS installed on test systems must be a Linux OS.
  Scripts were primarily used on systems with Fedora and
  Ubuntu 18.04/20.04 distributions.
- Each test system must have at least one RDMA-capable NIC installed for RDMA tests.
  For TCP tests any TCP-capable NIC will do. However, high-bandwidth,
  high-performance NICs like Intel E810 CQDA2 or Mellanox ConnectX-5 are
  suggested because the NVMe-oF workload is network bound.
  So, if you use a NIC capable of less than 100Gbps on NVMe-oF target
  system, you will quickly saturate your NICs.
- Python3 interpreter must be available on all test systems.
  Paramiko and Pandas modules must be installed.
- nvmecli package must be installed on all test systems.
- fio must be downloaded from [Github](https://github.com/axboe/fio) and built.
  This must be done on Initiator test systems to later build SPDK with
  "--with-fio" option.
- All test systems must have a user account with a common name,
  password and passwordless sudo enabled.
- [mlnx-tools](https://github.com/Mellanox/mlnx-tools) package must be downloaded
  to /usr/src/local directory in order to configure NIC ports IRQ affinity.
  If custom directory is to be used, then it must be set using irq_scripts_dir
  option in Target and Initiator configuration sections.
- `sysstat` package must be installed for SAR CPU utilization measurements.
- `bwm-ng` package must be installed for NIC bandwidth utilization measurements.
- `pcm` package must be installed for pcm, pcm-power and pcm-memory measurements.

### Optional

- For test using the Kernel Target, nvmet-cli must be downloaded and build on Target system.
  nvmet-cli is available [here](http://git.infradead.org/users/hch/nvmetcli.git).

## Manual configuration

Before running the scripts some manual test systems configuration is required:

- Configure IP address assignment on the NIC ports that will be used for test.
  Make sure to make these assignments persistent, as in some cases NIC drivers may be reloaded.
- Adjust firewall service to allow traffic on IP - port pairs used in test
  (or disable firewall service completely if possible).
- Adjust or completely disable local security engines like AppArmor or SELinux.

## JSON configuration for test run automation

An example json configuration file with the minimum configuration required
to automate NVMe-oF testing is provided in this repository.
The following sub-chapters describe each configuration section in more detail.

### General settings section

``` ~sh
"general": {
    "username": "user",
    "password": "password",
    "transport": "transport_type",
    "skip_spdk_install": bool
}
```

Required:

- username - username for the SSH session
- password - password for the SSH session
- transport - transport layer to be used throughout the test ("tcp" or "rdma")

Optional:

- skip_spdk_install - by default SPDK sources will be copied from Target
  to the Initiator systems each time run_nvmf.py script is run. If the SPDK
  is already in place on Initiator systems and there's no need to re-build it,
  then set this option to true.
  Default: false.

### Target System Configuration

``` ~sh
"target": {
  "mode": "spdk",
  "nic_ips": ["192.0.1.1", "192.0.2.1"],
  "core_mask": "[1-10]",
  "null_block_devices": 8,
  "nvmet_bin": "/path/to/nvmetcli",
  "sar_settings": true,
  "pcm_settings": false,
  "enable_bandwidth": [true, 60],
  "enable_dpdk_memory": true
  "num_shared_buffers": 4096,
  "scheduler_settings": "static",
  "zcopy_settings": false,
  "dif_insert_strip": true,
  "null_block_dif_type": 3,
  "pm_settings": [true, 30, 1, 60],
  "irq_settings": {
    "mode": "cpulist",
    "cpulist": "[0-10]",
    "exclude_cpulist": false
  }
}
```

Required:

- mode - Target application mode, "spdk" or "kernel".
- nic_ips - IP addresses of NIC ports to be used by the target to export
  NVMe-oF subsystems.
- core_mask - Used by SPDK target only.
  CPU core mask either in form of actual mask (i.e. 0xAAAA) or core list
  (i.e. [0,1,2-5,6).
  At this moment the scripts cannot restrict the Kernel target to only
  use certain CPU cores. Important: upper bound of the range is inclusive!

Optional, common:

- null_block_devices - int, number of null block devices to create.
  Detected NVMe devices are not used if option is present. Default: 0.
- sar_settings - bool
  Enable SAR CPU utilization measurement on Target side. SAR thread will
  wait until fio finishes it's "ramp_time" and then start measurement for
  fio "run_time" duration. Default: enabled.
- pcm_settings - bool
  Enable [PCM](https://github.com/opcm/pcm.git) measurements on Target side.
  Measurements include CPU, memory and power consumption. Default: enabled.
- enable_bandwidth - bool. Measure bandwidth utilization on network
  interfaces. Default: enabled.
- tuned_profile - tunedadm profile to apply on the system before starting
  the test.
- irq_scripts_dir - path to scripts directory of Mellanox mlnx-tools package;
  Used to run set_irq_affinity.sh script.
  Default: /usr/src/local/mlnx-tools/ofed_scripts
- enable_pm - bool;
  if bool is set to true, power measurement is enabled via collect-bmc-pm on
  the target side. Default: true.
- irq_settings - dict;
  Choose how to adjust network interface IRQ settings.
  mode: default - run IRQ alignment script with no additional options.
  mode: bynode - align IRQs to be processed only on CPU cores matching NIC
    NUMA node.
  mode: cpulist - align IRQs to be processed only on CPU cores provided
    in the cpulist parameter.
  cpulist: list of CPU cores to use for cpulist mode. Can be provided as
    list of individual cores ("[0,1,10]"), core ranges ("[0-10]"), or mix
    of both ("[0-1,10,20-22]")
  exclude_cpulist: reverse the effect of cpulist mode. Allow IRQ processing
    only on CPU cores which are not provided in cpulist parameter.

Optional, Kernel Target only:

- nvmet_bin - path to nvmetcli binary, if not available in $PATH.
  Only for Kernel Target. Default: "nvmetcli".

Optional, SPDK Target only:

- zcopy_settings - bool. Disable or enable target-size zero-copy option.
  Default: false.
- scheduler_settings - str. Select SPDK Target thread scheduler (static/dynamic).
  Default: static.
- num_shared_buffers - int, number of shared buffers to allocate when
  creating transport layer. Default: 4096.
- max_queue_depth - int, max number of outstanding I/O per queue. Default: 128.
- dif_insert_strip - bool. Only for TCP transport. Enable DIF option when
  creating transport layer. Default: false.
- num_cqe - int, number of completion queue entries. See doc/json_rpc.md
  "nvmf_create_transport" section. Default: 4096.
- null_block_dif_type - int, 0-3. Level of DIF type to use when creating
  null block bdev. Default: 0.
- enable_dpdk_memory - bool. Wait for a fio ramp_time to finish and
  call env_dpdk_get_mem_stats RPC call to dump DPDK memory stats.
  Default: enabled.
- adq_enable - bool; only for TCP transport.
  Configure system modules, NIC settings and create priority traffic classes
  for ADQ testing. You need and ADQ-capable NIC like the Intel E810.
- bpf_scripts - list of bpftrace scripts that will be attached during the
  test run. Available scripts can be found in the spdk/scripts/bpf directory.
- dsa_settings - bool. Only for TCP transport. Enable offloading CRC32C
  calculation to DSA. You need a CPU with the Intel(R) Data Streaming
  Accelerator (DSA) engine.
- scheduler_core_limit - int, 0-100. Dynamic scheduler option to load limit on
  the core to be considered full.
- irq_settings - dict;
  Choose how to adjust network interface IRQ settings.
  Same as in common options section, but SPDK Target allows more modes:
  mode: shared - align IRQs to be processed only on the same CPU cores which
    are already used by SPDK Target process.
  mode: split - align IRQs to be processed only on CPU cores which are not
    used by SPDK Target process.
  mode: split-bynode - same as "split", but reduce the number of CPU cores
    to use for IRQ processing to only these matching NIC NUMA node.

### Initiator system settings section

There can be one or more `initiatorX` setting sections, depending on the test setup.

``` ~sh
"initiator1": {
  "ip": "10.0.0.1",
  "nic_ips": ["192.0.1.2"],
  "target_nic_ips": ["192.0.1.1"],
  "mode": "spdk",
  "fio_bin": "/path/to/fio/bin",
  "nvmecli_bin": "/path/to/nvmecli/bin",
  "cpus_allowed": "0,1,10-15",
  "cpus_allowed_policy": "shared",
  "num_cores": 4,
  "cpu_frequency": 2100000,
  "adq_enable": false,
  "kernel_engine": "io_uring",
  "irq_settings": { "mode": "bynode" }
}
```

Required:

- ip - management IP address of initiator system to set up SSH connection.
- nic_ips - list of IP addresses of NIC ports to be used in test,
  local to given initiator system.
- target_nic_ips - list of IP addresses of Target NIC ports to which initiator
  will attempt to connect to.
- mode - initiator mode, "spdk" or "kernel". For SPDK, the bdev fio plugin
  will be used to connect to NVMe-oF subsystems and submit I/O. For "kernel",
  nvmecli will be used to connect to NVMe-oF subsystems and fio will use the
  libaio ioengine to submit I/Os.

Optional, common:

- nvmecli_bin - path to nvmecli binary; Will be used for "discovery" command
  (for both SPDK and Kernel modes) and for "connect" (in case of Kernel mode).
  Default: system-wide "nvme".
- fio_bin - path to custom fio binary, which will be used to run IO.
  Additionally, the directory where the binary is located should also contain
  fio sources needed to build SPDK fio_plugin for spdk initiator mode.
  Default: /usr/src/fio/fio.
- cpus_allowed - str, list of CPU cores to run fio threads on. Takes precedence
  before `num_cores` setting. Default: None (CPU cores randomly allocated).
  For more information see `man fio`.
- cpus_allowed_policy - str, "shared" or "split". CPU sharing policy for fio
  threads. Default: shared. For more information see `man fio`.
- num_cores - By default fio threads on initiator side will use as many CPUs
  as there are connected subsystems. This option limits the number of CPU cores
  used for fio threads to this number; cores are allocated randomly and fio
  `filename` parameters are grouped if needed. `cpus_allowed` option takes
  precedence and `num_cores` is ignored if both are present in config.
- cpu_frequency - int, custom CPU frequency to set. By default test setups are
  configured to run in performance mode at max frequencies. This option allows
  user to select CPU frequency instead of running at max frequency. Before
  using this option `intel_pstate=disable` must be set in boot options and
  cpupower governor be set to `userspace`.
- tuned_profile - tunedadm profile to apply on the system before starting
  the test.
- irq_scripts_dir - path to scripts directory of Mellanox mlnx-tools package;
  Used to run set_irq_affinity.sh script.
  Default: /usr/src/local/mlnx-tools/ofed_scripts
- kernel_engine - Select fio ioengine mode to run tests. io_uring libraries and
  io_uring capable fio binaries must be present on Initiator systems!
  Available options:
  - libaio (default)
  - io_uring
- irq_settings - dict;
  Same as "irq_settings" in Target common options section.

Optional, SPDK Initiator only:

- adq_enable - bool; only for TCP transport. Configure system modules, NIC
  settings and create priority traffic classes for ADQ testing.
  You need an ADQ-capable NIC like Intel E810.
- enable_data_digest - bool; only for TCP transport. Enable the data
  digest for the bdev controller. The target can use IDXD to calculate the
  data digest or fallback to a software optimized implementation on system
  that don't have the Intel(R) Data Streaming Accelerator (DSA) engine.

### Fio settings section

``` ~sh
"fio": {
  "bs": ["4k", "128k"],
  "qd": [32, 128],
  "rw": ["randwrite", "write"],
  "rwmixread": 100,
  "rate_iops": 10000,
  "num_jobs": 2,
  "offset": true,
  "offset_inc": 10,
  "run_time": 30,
  "ramp_time": 30,
  "run_num": 3
}
```

Required:

- bs - fio IO block size
- qd -  fio iodepth
- rw - fio rw mode
- rwmixread - read operations percentage in case of mixed workloads
- num_jobs - fio numjobs parameter
  Note: may affect total number of CPU cores used by initiator systems
- run_time - fio run time
- ramp_time - fio ramp time, does not do measurements
- run_num - number of times each workload combination is run.
  If more than 1 then final result is the average of all runs.

Optional:

- rate_iops - limit IOPS to this number
- offset - bool; enable offseting of the IO to the file. When this option is
  enabled the file is "split" into a number of chunks equal to "num_jobs"
  parameter value, and each "num_jobs" fio thread gets it's own chunk to
  work with.
  For more detail see "offset", "offset_increment" and "size" in fio man
  pages. Default: false.
- offset_inc - int; Percentage value determining the offset, size and
  offset_increment when "offset" option is enabled. By default if "offset"
  is enabled fio file will get split evenly between fio threads doing the
  IO. Offset_inc can be used to specify a custom value.

#### Test Combinations

It is possible to specify more than one value for bs, qd and rw parameters.
In such case script creates a list of their combinations and runs IO tests
for all of these combinations. For example, the following configuration:

``` ~sh
  "bs": ["4k"],
  "qd": [32, 128],
  "rw": ["write", "read"]
```

results in following workloads being tested:

- 4k-write-32
- 4k-write-128
- 4k-read-32
- 4k-read-128

#### Important note about queue depth parameter

qd in fio settings section refers to iodepth generated per single fio target
device ("filename" in resulting fio configuration file). It is re-calculated
while the script is running, so generated fio configuration file might contain
a different value than what user has specified at input, especially when also
using "numjobs" or initiator "num_cores" parameters. For example:

Target system exposes 4 NVMe-oF subsystems. One initiator system connects to
all of these systems.

Initiator configuration (relevant settings only):

``` ~sh
"initiator1": {
  "num_cores": 1
}
```

Fio configuration:

``` ~sh
"fio": {
  "bs": ["4k"],
  "qd": [128],
  "rw": ["randread"],
  "rwmixread": 100,
  "num_jobs": 1,
  "run_time": 30,
  "ramp_time": 30,
  "run_num": 1
}
```

In this case generated fio configuration will look like this
(relevant settings only):

``` ~sh
[global]
numjobs=1

[job_section0]
filename=Nvme0n1
filename=Nvme1n1
filename=Nvme2n1
filename=Nvme3n1
iodepth=512
```

`num_cores` option results in 4 connected subsystems to be grouped under a
single fio thread (job_section0). Because `iodepth` is local to `job_section0`,
it is distributed between each `filename` local to job section in round-robin
(by default) fashion. In case of fio targets with the same characteristics
(IOPS & Bandwidth capabilities) it means that iodepth is distributed **roughly**
equally. Ultimately above fio configuration results in iodepth=128 per filename.

`numjobs` higher than 1 is also taken into account, so that desired qd per
filename is retained:

``` ~sh
[global]
numjobs=2

[job_section0]
filename=Nvme0n1
filename=Nvme1n1
filename=Nvme2n1
filename=Nvme3n1
iodepth=256
```

Besides `run_num`, more information on these options can be found in `man fio`.

## Running the test

Before running the test script run the spdk/scripts/setup.sh script on Target
system. This binds the devices to VFIO/UIO userspace driver and allocates
hugepages for SPDK process.

Run the script on the NVMe-oF target system:

``` ~sh
cd spdk
sudo PYTHONPATH=$PYTHONPATH:$PWD/python scripts/perf/nvmf/run_nvmf.py
```

By default script uses config.json configuration file in the scripts/perf/nvmf
directory. You can specify a different configuration file at runtime as below:

``` ~sh
sudo PYTHONPATH=$PYTHONPATH:$PWD/python scripts/perf/nvmf/run_nvmf.py -c /path/to/config.json
```

PYTHONPATH environment variable is needed because script uses SPDK-local Python
modules. If you'd like to get rid of `PYTHONPATH=$PYTHONPATH:$PWD/python`
you need to modify your environment so that Python interpreter is aware of
`spdk/scripts` directory.

## Test Results

Test results for all workload combinations are printed to screen once the tests
are finished. Additionally all aggregate results are saved to /tmp/results/nvmf_results.conf
Results directory path can be changed by -r script parameter.
