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

### Advanced Examples

```bash
# Mixed workload with latency tracking
./spdk_nvme_perf -q 64 -o 4096 -w randrw -M 70 -t 300 -L \
  -r "trtype:PCIe traddr:0000:01:00.0"

# Multi-queue pairs per namespace usage
./spdk_nvme_perf -q 256 -o 4096 -w randread -t 300 -c 0xFF \
  -r "trtype:PCIe traddr:0000:01:00.0"
```

## Essential Options Reference

| Option            | Description            | Example           |
|-------------------|------------------------|-------------------|
| `-q, --io-depth`  | Queue depth            | `-q 64`           |
| `-o, --io-size`   | I/O size in bytes      | `-o 4k`           |
| `-w, --io-pattern`| Workload type          | `-w randread`     |
| `-t, --time`      | Test duration (seconds)| `-t 300`          |
| `-r, --transport` | Target specification   | See examples above|
| `-c, --core-mask` | CPU cores to use       | `-c 0xFF`         |

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

## Compiling perf on FreeBSD

To use spdk_nvme_perf on FreeBSD over NVMe-oF, explicitly link userspace library of HBA. For example, on a setup with Mellanox HBA,
```make
	LIBS += -lmlx5
```
