# Automated script for NVMe performance test

## Compile SPDK with LTO

The link time optimization (lto) gcc flag allows the linker to run a post-link optimization pass on the code. During
that pass the linker inlines thin wrappers such as those around DPDK calls which results in a shallow call stack and
significantly improves performance. Therefore, we recommend compiling SPDK with the lto flag prior to running this
benchmark script to achieve optimal performance.
Link time optimization can be enabled in SPDK by doing the following:

~{.sh}
./configure --enable-lto
~

## Configuration

Test is configured by using command-line options.

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

Select between SPDK driver and kernel driver. The Linux Kernel driver has three configurations:
Default mode, Hybrid Polling and Classic Polling. The SPDK driver supports 2 fio_plugin modes: bdev and NVMe PMD.
Before running test with spdk, you will need to bind NVMe devics to the Linux uio_pci_generic or vfio-pci driver.
When running test with the Kernel driver, NVMe devices use the Kernel driver. The 5 valid values for this option are:
'bdev', 'nvme', 'kernel-libaio', 'kernel-classic-polling' and 'kernel-hybrid-polling'.

#### --max-disk

This option will run multiple fio jobs with varying number of NVMe devices. First it will start with
max-disk number of devices then decrease number of disk by two until there are no more devices.
If set to 'all' then max-disk number will be set to all available devices.
Only one of the max-disk or disk-no option can be used.

#### --disk-no

This option will run fio job on specified number of NVMe devices. If set to 'all' then max-disk number
will be set to all available devices. Only one of the max-disk or disk-no option can be used.

#### --cpu-allowed

Specifies the CPU cores that will be used by fio to execute the performance test cases. When spdk driver is chosen,
the script attempts to assign NVMe devices to CPU cores on the same NUMA node. The script will try to align each
core with devices matching core's NUMA first but if the is no devices left within the CPU core NUMA then it will use
devices from the other NUMA node. It is important to choose cores that will ensure best NUMA node alignment. For example:
On System with 8 devices on NUMA node 0 and 8 devices on NUMA node 1, cores 0-27 on numa node 0 and 28-55
on numa node 1, if test is set to use 16 disk and four cores then "--cpu-allowed=1,2,28,29" can be used
resulting with 4 devices with node0 per core 1 and 2 and 4 devices with node1 per core 28 and 29. If 10 cores
are required then best option would be "--cpu-allowed=1,2,3,4,28,29,30,31,32,33" because cores 1-4 will be
aligned with 2 devices on numa0 per core and cores 28-33 will be aligned with 1 device on numa1 per core.
If kernel driver is chosen then for each job with NVME device, all cpu cores with corresponding NUMA node are picked.

#### --rw

Type of I/O pattern.  Accepted values are: randrw, rw

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
iodepth=32, ioengine=spdk. It can be skipped when this option is set.

#### "--no-io-scaling"

For SPDK fio plugin iodepth is multiplied by number of devices. When this option is set this multiplication will be disabled.

## Results

Results are stored in "results" folder. After each workload, to this folder are copied files with:
fio configuration file, json files with fio results and logs with latencies with sampling interval 250 ms.
Number of copied files depends from number of repeats of each workload. Additionally csv file is created with averaged
results of all workloads.
