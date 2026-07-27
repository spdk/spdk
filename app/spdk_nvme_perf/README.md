# spdk_nvme_perf Usage Guide

## Quick Start Examples

### Local NVMe Device Testing

```bash
# Basic 4K random read test
./spdk_nvme_perf -q 64 -o 4096 -w randread -t 60 -c 0x1

# Multi-device performance test
./spdk_nvme_perf -q 128 -o 4096 -w randread -t 300 -c 0xFF \
  -r "trtype:PCIe traddr:0000:01:00.0" \
  -r "trtype:PCIe traddr:0000:02:00.0"
```

### NVMe-oF Testing (Common Use Case)

```bash
# TCP transport - All subsystems from discovery log
# Connects to discovery subsystem and uses all subsystems found
./spdk_nvme_perf -q 128 -o 4096 -w randread -t 300 \
  -r "trtype:TCP adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420"

# RDMA transport - Specific subsystem only
# Connects only to the specified subsystem NQN
./spdk_nvme_perf -q 128 -o 4096 -w randread -t 300 \
  -r "trtype:RDMA adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1"
```

### Interrupt Mode

Interrupt mode can be enabled with `-E` for initiator-side testing when the selected
transport supports interrupt-capable queue pairs. This includes local PCIe devices
and NVMe-oF RDMA initiators.

```bash
./spdk_nvme_perf -q 128 -o 4096 -w randread -t 300 -E \
  -r "trtype:RDMA adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1"
```

### Advanced Examples

```bash
# 70/30 read/write split at a single I/O size, with latency tracking
./spdk_nvme_perf -q 64 -o 4096 -w randrw -M 70 -t 300 -L \
  -r "trtype:PCIe traddr:0000:01:00.0"

# Blended I/O workloads in a single run (see Mixed Workloads below)
./spdk_nvme_perf -q 64 -t 300 --mixed-workload 4k:randread:70,128k:randwrite:30 \
  -r "trtype:PCIe traddr:0000:01:00.0"

# Multi-queue pairs per namespace usage
./spdk_nvme_perf -q 256 -o 4096 -w randread -t 300 -c 0xFF \
  -r "trtype:PCIe traddr:0000:01:00.0"
```

## Essential Options Reference

| Option              | Description                        | Example                              |
|---------------------|------------------------------------|--------------------------------------|
| `-q, --io-depth`    | Queue depth                        | `-q 64`                              |
| `-o, --io-size`     | I/O size in bytes                  | `-o 4k`                              |
| `-w, --io-pattern`  | Workload type                      | `-w randread`                        |
| `-t, --time`        | Test duration (seconds)            | `-t 300`                             |
| `-r, --transport`   | Target specification               | See examples above                   |
| `-c, --core-mask`   | CPU cores to use                   | `-c 0xFF`                            |
| `--mixed-workload`  | Blend of sizes/workloads/ratios    | `--mixed-workload 4k:randread:100`   |

For complete options list, use: `./spdk_nvme_perf --help`

## Examples

### 1. Basic Sequential Read Test

```bash
./build/bin/spdk_nvme_perf -q 32 -o 4096 -w read -t 60 -r 'trtype:PCIe traddr:0000.01.00.0'
```

### 2. Random Write Test

```bash
./build/bin/spdk_nvme_perf -q 128 -o 4k -w randwrite -t 60 -r 'trtype:PCIe traddr:0000.01.00.0'
```

### 3. Mixed Read/Write (70% Reads, 30% Writes)

```bash
./build/bin/spdk_nvme_perf -q 64 -o 4K -w randrw -M 70 -t 60 -r 'trtype:PCIe traddr:0000.01.00.0'
```

### 4. Random Read Test

```bash
./build/bin/spdk_nvme_perf -q 128 -o 4096 -w randread -t 300 -r 'trtype:PCIe traddr:0000.01.00.0'
```

### 5. Latency Test with Warmup

```bash
./build/bin/spdk_nvme_perf -q 1 -o 4096 -w randread -t 60 -a 10 -r 'trtype:PCIe traddr:0000.01.00.0'
```

### 6. Test with Multiple Namespaces

```bash
./build/bin/spdk_nvme_perf -q 32 -o 4096 -w randwrite -t 60 -r 'trtype:PCIe traddr:0000.01.00.0 ns:1' -r 'trtype:PCIe traddr:0000.01.00.0 ns:2'
```

### Multi-Process Usage

**Note:** For multi-process configurations, it is recommended to use the
[stub application](../../test/app/stub) as the primary process instead of
`spdk_nvme_perf`. The stub app ensures proper resource management when
multiple secondary processes are running.

Example:

```bash
# Start stub as primary (keeps SPDK resources alive)
./test/app/stub/stub -i 100 &

# Run secondary perf processes
./spdk_nvme_perf -i 100 -c 0x2 -q 64 -o 4096 -w randread -t 60
```

See NVMe Multi-Process documentation for detailed configuration.

## Mixed Workloads

`-o`/`-w`/`-M` describe a single I/O shape: one size, one access pattern, and
optionally a read/write split at that one size. Real applications rarely look
like that - a database issues small random reads while its log writer issues
large sequential writes.

`--mixed-workload` drives several weighted workloads from one set of queues:

```text
--mixed-workload <size>:<pattern>:<pct>[,<size>:<pattern>:<pct>,...]
```

| Field       | Meaning                                                            |
|-------------|--------------------------------------------------------------------|
| `<size>`    | I/O size; decimal bytes with an optional `k`/`K`/`m`/`M`/`g`/`G`   |
| `<pattern>` | One of `read`, `write`, `randread`, `randwrite`                     |
| `<pct>`     | Integer 1-100 share of the total I/O count; all must sum to 100    |

`--mixed-workload` is mutually exclusive with `-o`, `-w` and `-M`; combining
it with any of them is an error.

```bash
# 70% 4K random reads blended with 30% 128K random writes
./build/bin/spdk_nvme_perf -q 128 -t 60 \
  --mixed-workload 4k:randread:70,128k:randwrite:30 \
  -r 'trtype:PCIe traddr:0000:01:00.0'

# Small random reads competing with a large sequential write stream
./build/bin/spdk_nvme_perf -q 128 -t 60 -c 0xF \
  --mixed-workload 4k:randread:60,8k:randread:20,1m:write:20 \
  -r 'trtype:TCP adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420'
```

### Semantics

- **Percentages are I/O counts, not bandwidth.** With
  `4k:randread:70,128k:randwrite:30`, 30% of the *operations* are writes but
  those writes are roughly 93% of the *bytes*. The summary reports both shares
  so the distinction is visible.
- **Random workloads** draw offsets from their own range, so each workload is
  naturally aligned to its own I/O size. `-F/--zipf` is only supported when
  every random workload in the mix shares the same I/O size, since the Zipf
  generator is shared across them; a mix of random workloads with different
  sizes rejects `-F/--zipf` at startup.
- **Sequential workloads share a single cursor per queue.** A mix of
  sequential sizes therefore forms one contiguous, non-overlapping stream
  instead of several streams colliding with each other. The corollary is that
  no individual sequential workload is sequential in isolation - only the
  combined stream is.
- **Buffers and geometry are sized from the largest workload**, so queue
  depth and memory footprint are those of the biggest I/O in the mix.
- A device is dropped from the run (with a warning) when any workload size is
  not a multiple of its block size, or is larger than the device.

### Output

The once-per-second progress line gains a per-workload I/O rate, and the
summary gains a per-workload breakdown:

```text
Per-workload breakdown:
Workload                               IOPS        MiB/s    IO%  Bytes% AvgLat(us) MinLat(us) MaxLat(us)
4096B randread (70% cfg)          254881.44       995.63  70.0%    6.8%      15.32       4.11     512.40
131072B randwrite (30% cfg)       109234.90     13654.36  30.0%   93.2%     102.77      41.02    1893.60
```

Per-workload latency is reported separately because the aggregate
`Average/min/max` row blends every transfer size, which is not meaningful once
the sizes differ. Single-workload runs, including `-w rw`/`-w randrw`, keep
their original output unchanged.

## Compiling perf on FreeBSD

To use spdk_nvme_perf on FreeBSD over NVMe-oF, explicitly link userspace library of HBA. For example, on a setup with Mellanox HBA,
```make
	LIBS += -lmlx5
```
