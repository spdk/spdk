# Automated script for NVME performance test

## Configuration
Test is configured by adding parameters and options thru command line.

### Available options

#### -h, --help
Prints available commands and help.

#### --run-time
Tell fio to terminate processing after the specified period of time. Value in seconds.

#### --ramp-time
Fio will run the specified workload for this amount of time before logging any performance numbers.
Value in seconds.

#### --fio-bin
Path to fio binary.

#### --driver
Select between SPDK driver that uses fio_plugin and kernel driver. Additionally kernel driver can be set
in three configurations:
Deafault mode, Hybrid Polling and Classic Polling. When SPDK driver is chosen it can be used with
BDEV fio_plugin on NVME (PMD) fio_plugin. Before running test with spdk, it is needed to bind NVMe
devices to uio_pci_generic or vfio-pci driver first. If Kernel driver is used, devices must be binded
with kernel driver. Total of five argumets are available for this option:
'bdev', 'nvme', 'kernel-libaio', 'kernel-classic-polling' and 'kernel-hybrid-polling'.

#### --max-disk
This option will run multiple fio jobs with varying number of NVMe devices. First it will start with
max-disk number of devices then decrease number of disk by two until there will be no more devices.
If set to 'all' then max-disk number will be set to all available devices.
One of the max-disk or disk-no option can be used.

#### --disk-no
This option will run fio job on specified number of NVMe devices. If set to 'all' then max-disk number
will be set to all available devices. One of the max-disk or disk-no option can be used.

#### --cpu-allowed
Specify cores that will be used with fio tests. When spdk driver is chosen, NVMe devices will be aligned
to specific core according to its NUMA node. The script will try to align each core with devices matching
core's NUMA first and if the is no devices left with corresponding NUMA then it will use devices with other
NUMA node. It is important to choose cores that will ensure best NUMA node allocation. For example:
On System with 8 devices on NUMA node 0 and 8 devices on NUMA node 1, cores 0-27 on numa node 0 and 28-55
on numa node 1, if test is set to use 16 disk and four cores then "--cpu-allowed=1,2,28,29" can be used
resulting with 4 devices with node0 per core 1 and 2 and 4 devices with node1 per core 28 and 29. If 10 cores
are required then best option would be "--cpu-allowed=1,2,3,4,28,29,30,31,32,33" because cores 1-4 will be
aligned with 2 devices on numa0 per core and cores 28-33 will be aligned with 1 device on numa1 per core.
If kernel driver is chosen then for each job with NVME device, cpu cores with corresponding NUMA node are picked.

#### --rwmixread
Percentage of a mixed workload that should be reads.

#### --iodepth
Number of I/O units to keep in flight against each file.

#### --block-size
The block size in bytes used for I/O units.

#### --numjobs
Create the specified number of clones of a job.

#### --repeat-no
Specifies how many times run each workload. End results are averages of these workloads

#### --no-preconditioning
By default disks are preconditioned before test using fio with parameters: size=100%, loops=2, bs=1M, w=write,
iodepth=32, ioengine=spdk. It can be skiped when this option is set.

## Results
Results are stored in "results" folder. After each workload, to this folder are copied files with:
fio configuration file, json files with fio results and logs with latiencies with sampling interval 250 ms.
Number of copied files depends from number of repeats of each workload. Additionall csv file is created with averaged
results of all workloads.
