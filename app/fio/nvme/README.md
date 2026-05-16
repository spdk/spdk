# FIO plugin

## Compiling fio

First, clone the fio source repository from <https://github.com/axboe/fio>

```bash
git clone https://github.com/axboe/fio
```

Then check out the latest fio version and compile the code:

```bash
make
```

## Compiling SPDK

First, clone the SPDK source repository from <https://github.com/spdk/spdk>

```bash
git clone https://github.com/spdk/spdk
git submodule update --init
```

Then, run the SPDK configure script to enable fio (point it to the root of the
fio repository):

```bash
cd spdk
./configure --with-fio=/path/to/fio/repo <other configuration options>
```

Finally, build SPDK:

```bash
make
```

**Note to advanced users**: These steps assume you're using the DPDK submodule.
If you are using your own version of DPDK, the fio plugin requires that DPDK be
compiled with -fPIC. You can compile DPDK with -fPIC by modifying your DPDK
configuration file and adding the line:

```bash
EXTRA_CFLAGS=-fPIC
```

## Usage

To use the SPDK fio plugin with fio, specify the plugin binary using LD_PRELOAD
when running fio and set ioengine=spdk in the fio configuration file (see
example_config.fio in the same directory as this README).

```bash
LD_PRELOAD=<path to spdk repo>/build/fio/spdk_nvme fio
```

To select NVMe devices, you pass an SPDK Transport Identifier string as the
filename. These are in the form:

```bash
filename=key=value [key=value] ... ns=value
```

Specifically, for local PCIe NVMe devices it will look like this:

```bash
filename=trtype=PCIe traddr=0000.04.00.0 ns=1
```

And remote devices accessed via NVMe over Fabrics will look like this:

```bash
filename=trtype=RDMA adrfam=IPv4 traddr=192.168.100.8 trsvcid=4420 ns=1
```

**Note**: The specification of the PCIe address should not use the normal ':'
and instead only use '.'. This is a limitation in fio - it splits filenames on
':'. Also, the NVMe namespaces start at 1, not 0, and the namespace must be
specified at the end of the string.

fio by default forks a separate process for every job. It also supports just
spawning a separate thread in the same process for every job. The SPDK fio
plugin is limited to this latter thread usage model, so fio jobs must also
specify thread=1 when using the SPDK fio plugin. The SPDK fio plugin supports
multiple threads - in this case, the "1" just means "use thread mode".

fio also currently has a race condition on shutdown if dynamically loading the
ioengine by specifying the engine's full path via the ioengine parameter -
LD_PRELOAD is recommended to avoid this race condition.

When testing random workloads, it is recommended to set norandommap=1. fio's
random map processing consumes extra CPU cycles which will degrade performance
over time with the fio_plugin since all I/O are submitted and completed on a
single CPU core.

When testing FIO on multiple NVMe SSDs with SPDK plugin, it is recommended to
use multiple jobs in FIO configurion. It has been observed that there are some
performance gap between FIO(with SPDK plugin enabled) and SPDK perf
(app/spdk_nvme_perf) on testing multiple NVMe SSDs. If you use one job(i.e.,
use one CPU core) configured for FIO test, the performance is worse than SPDK
perf (also using one CPU core) against many NVMe SSDs. But if you use multiple
jobs for FIO test, the performance of FIO is similar with SPDK perf. After
analyzing this phenomenon, we think that is caused by the FIO architecture.
Mainly FIO can scale with multiple threads (i.e., using CPU cores), but it is
not good to use one thread against many I/O devices.

## SPDK NVMe fio Plugin Options

The SPDK NVMe fio plugin supports many custom options beyond standard fio
parameters. These options are specified in the fio job file in the `[global]`
section or individual job sections.

### Weighted Round Robin (WRR) Options

```bash
enable_wrr=1                # Enable weighted round robin (0/1, default: 0)
arbitration_burst=4         # Arbitration Burst (0-7, default: 0)
low_weight=16               # Low priority weight (0-255, default: 0)
medium_weight=32            # Medium priority weight (0-255, default: 0)
high_weight=64              # High priority weight (0-255, default: 0)
wrr_priority=1              # Priority class (0-3, default: 0)
```

Weighted Round Robin (WRR) is a standard NVMe feature that provides Quality of
Service (QoS) by assigning different priorities to I/O submission queues. For
details, see the NVMe specification Section 3.4.3 (Arbitration).

**Important fio Limitation**: Due to fio's `thread=1` requirement, all queues
within a single fio process inherit the same `wrr_priority` setting. To
demonstrate WRR with different priorities, run multiple separate fio processes
with different priority settings. See the WRR configuration example below.

### Memory and Environment Options

```bash
mem_size_mb=0               # Memory size for SPDK in MB (default: 0 = auto)
shm_id=-1                   # Shared Memory ID (default: -1, optional)
```

#### mem_size_mb (Memory Size)

**Use Auto (0) unless you have a specific need.** SPDK will automatically
calculate the required memory based on detected devices and system
configuration. This is the recommended setting for the vast majority of users.

Manual memory sizing is complex and should only be specified if you encounter
allocation failures or have specific memory constraints.

#### shm_id (Shared Memory ID)

**This parameter is optional.** If not specified (or set to -1), each fio
process gets isolated memory.

Use cases:

- **Multiple processes sharing devices**: Set all processes to the same shm_id
- **Process isolation**: Omit shm_id or use -1 (default behavior)

For most use cases, do not specify this parameter.

### Scatter-Gather List (SGL) Options

**Note**: The `enable_sgl` parameter name is misleading. SPDK automatically
uses SGLs when the controller supports them. This option controls which SPDK
NVMe API is used internally (PRP-based vs SGL-based submission path), not
whether SGLs are used for data transfer on the wire.

```bash
enable_sgl=1                # Use SGL submission API (0/1, default: 0)
sge_size=4096               # SGL element size in bytes (default: 4096)
disable_pcie_sgl_merge=1    # Disable SGL element merging (0/1, default: 0)
bit_bucket_data_len=512     # Bit Bucket data length for reads (default: 0)
```

Use `enable_sgl=1` when comparing PRP vs SGL submission performance, or when
working with workloads that benefit from the SGL submission path.

### NVMe/TCP Digest Options

```bash
digest_enable=HEADER        # NVMe/TCP digest (NONE|HEADER|DATA|BOTH,
                            # default: NONE)
```

### Interrupt Mode

```bash
enable_interrupts=1         # Enable interrupt mode (0/1, default: 0)
```

### Debugging and Logging

```bash
print_qid_mappings=1        # Print job-to-qid mappings (0/1, default: 0)
spdk_tracing=1              # Enable SPDK tracing (0/1, default: 0)
log_flags=nvme,bdev         # Enable log flags (comma-separated)
```

#### print_qid_mappings

Prints how fio jobs map to NVMe queue pairs: `Job 0 -> Queue Pair 1`. Each fio
job gets a dedicated I/O queue pair for parallel processing.

#### spdk_tracing

Enables SPDK's tracing framework for I/O flow analysis. Creates binary trace
files that can be analyzed with `spdk_trace` tool.

#### log_flags

Controls modular logging for specific SPDK components (e.g., `nvme`, `bdev`,
`thread`). In DEBUG builds, automatically sets log level to DEBUG when any
flags are enabled.

## End-to-end Data Protection (Optional)

Running with PI setting, following settings steps are required. First, format
device namespace with proper PI setting. For example:

```bash
nvme format /dev/nvme0n1 -l 1 -i 1 -p 0 -m 1
```

In fio configure file, add PRACT and set PRCHK by flags(GUARD|REFTAG|APPTAG)
properly. For example:

```bash
pi_act=0
pi_chk=GUARD
```

Blocksize should be set as the sum of data and metadata. For example, if data
blocksize is 512 Byte, host generated PI metadata is 8 Byte, then blocksize in
fio configure file should be 520 Byte:

```bash
bs=520
```

The storage device may use a block format that requires separate metadata
(DIX). In this scenario, the fio_plugin will automatically allocate an extra
4KiB buffer per I/O to hold this metadata. For some cases, such as 512 byte
blocks with 32 metadata bytes per block and a 128KiB I/O size, 4KiB isn't large
enough. In this case, the `md_per_io_size` option may be specified to increase
the size of the metadata buffer.

Expose two options 'apptag' and 'apptag_mask', users can change them in the
configuration file when using application tag and application tag mask in
end-to-end data protection. Application tag and application tag mask are set to
0x1234 and 0xFFFF by default.

## VMD (Optional)

To enable VMD enumeration add enable_vmd flag in fio configuration file:

```bash
enable_vmd=1
```

## ZNS

To use Zoned Namespaces then build the io-engine against, and run using, a fio
version >= 3.23 and add:

```bash
zonemode=zbd
```

To your fio-script, also have a look at script-examples provided with fio:

```bash
fio/examples/zbd-seq-read.fio
fio/examples/zbd-rand-write.fio
```

### Maximum Open Zones

Zoned Namespaces has a resource constraint on the amount of zones which can be
in an opened state at any point in time. You can control how many zones fio
will keep in an open state by using the ``--max_open_zones`` option.

If you use a fio version newer than 3.26, fio will automatically detect and set
the proper value. If you use an old version of fio, make sure to provide the
proper --max_open_zones value yourself.

### Maximum Active Zones

Zoned Namespaces has a resource constraint on the number of zones that can be
active at any point in time. Unlike ``max_open_zones``, then fio currently do
not manage this constraint, and there is thus no option to limit it either.

When running with the SPDK/NVMe fio io-engine you can be exposed to error
messages, in the form of completion errors, with the NVMe status code of 0xbd
("Too Many Active Zones"). To work around this, then you can reset all zones
before fio start running its jobs by using the engine option:

```bash
--initial_zone_reset=1
```

### Zone Append

When running FIO against a Zoned Namespace you need to specify --iodepth=1 to
avoid "Zone Invalid Write: The write to a zone was not at the write pointer."
I/O errors. However, if your controller supports Zone Append, you can use the
engine option:

```bash
--zone_append=1
```

To send zone append commands instead of write commands to the controller. When
using zone append, you will be able to specify a --iodepth greater than 1.

### Shared Memory Increase

If your device has a lot of zones, fio can give you errors such as:

```bash
smalloc: OOM. Consider using --alloc-size to increase the shared memory
available.
```

This is because fio needs to allocate memory for the zone-report, that is,
retrieve the state of zones on the device including auxiliary accounting
information. To solve this, then you can follow fio's advice and increase
``--alloc-size``.

## FDP

To use FDP enabled device build and run the io-engine against fio version >=
3.34 and add:

```bash
fdp=1
```

to your fio-script, also have a look at script-example provided with fio:

```bash
fio/examples/uring-cmd-fdp.fio
```

## Example Configuration Files

### Basic Random Read Test

```ini
[global]
ioengine=spdk
thread=1
direct=1
norandommap=1
bs=4k
iodepth=32
runtime=60
time_based=1

[nvme-randread]
filename=trtype=PCIe traddr=0000.01.00.0 ns=1
rw=randread
```

### Mixed Read/Write Workload

```ini
[global]
ioengine=spdk
thread=1
direct=1
norandommap=1
bs=4k
runtime=300
time_based=1

[mixed-workload]
filename=trtype=PCIe traddr=0000.01.00.0 ns=1
rw=randrw
rwmixread=70
iodepth=64
```

### NVMe over Fabrics with Protection Information

```ini
[global]
ioengine=spdk
thread=1
direct=1
bs=520
pi_act=0
pi_chk=GUARD
apptag=0x1234

[nvmf-read]
filename=trtype=RDMA adrfam=IPv4 traddr=192.168.1.100 trsvcid=4420 ns=1
rw=read
iodepth=64
runtime=30
```

### High Performance Sequential Write

```ini
[global]
ioengine=spdk
thread=1
direct=1
bs=128k
runtime=120
time_based=1

[seq-write]
filename=trtype=PCIe traddr=0000.02.00.0 ns=1
rw=write
iodepth=128
```

### ZNS Device with Zone Append

```ini
[global]
ioengine=spdk
thread=1
direct=1
zonemode=zbd
initial_zone_reset=1
zone_append=1

[zns-write]
filename=trtype=PCIe traddr=0000.02.00.0 ns=1
rw=write
bs=4k
iodepth=16
runtime=60
```

### Weighted Round Robin Configuration

```ini
# Run these in separate terminals to demonstrate WRR with different priorities
# Terminal 1: High priority workload
[global]
ioengine=spdk
thread=1
direct=1
enable_wrr=1
arbitration_burst=4
high_weight=128
medium_weight=64
low_weight=32
wrr_priority=2

[high-priority-job]
filename=trtype=PCIe traddr=0000.01.00.0 ns=1
rw=randwrite
bs=4k
iodepth=32
runtime=60

# Terminal 2: Low priority workload (in separate fio process)
[global]
ioengine=spdk
thread=1
direct=1
enable_wrr=1
arbitration_burst=4
high_weight=128
medium_weight=64
low_weight=32
wrr_priority=0

[low-priority-job]
filename=trtype=PCIe traddr=0000.01.00.0 ns=1
rw=randread
bs=4k
iodepth=32
runtime=60
```

### Multi-Job Performance Test

```ini
[global]
ioengine=spdk
thread=1
direct=1
norandommap=1
bs=4k
runtime=60
time_based=1

[job1]
filename=trtype=PCIe traddr=0000.01.00.0 ns=1
rw=randread
iodepth=32

[job2]
filename=trtype=PCIe traddr=0000.01.00.0 ns=1
rw=randwrite
iodepth=32

[job3]
filename=trtype=PCIe traddr=0000.01.00.0 ns=1
rw=read
iodepth=16
```

### NVMe/TCP with Digest Verification

```ini
[global]
ioengine=spdk
thread=1
direct=1
digest_enable=BOTH
hostnqn=nqn.2014-08.org.nvmexpress:uuid:12345678-1234-1234-1234-123456789abc

[tcp-digest-test]
filename=trtype=TCP adrfam=IPv4 traddr=192.168.1.50 trsvcid=4420 ns=1
rw=randrw
rwmixread=50
bs=8k
iodepth=64
runtime=180
```
