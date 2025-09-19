# WRR Burst Test Example

This example exercises NVMe Weighted Round Robin (WRR) arbitration by submitting large command bursts on multiple I/O queue pairs with different priorities. Each queue prepares its commands while the `delay_cmd_submit` feature is enabled; after all queues are primed the application rings every submission queue doorbell, allowing the controller to arbitrate a synchronized burst.

## Build

From the repository root run either build system (after a standard `./configure`):

```bash
ninja -C build examples/nvme/wrr_burst_test
```

or simply build everything:

```bash
make
```

> **Note**: Do not call `make -C examples/nvme wrr_burst_test`; that shortcut only compiles the object file in this directory and fails at link time because the SPDK libraries are not linked in. Let the top-level build drive the link step.

The resulting binary lives at `build/examples/nvme/wrr_burst_test/wrr_burst_test` (path may differ if you use a different build directory).

## Run

Before running, ensure the SPDK environment has access to hugepages. Allocate them via the provided script:

```bash
sudo HUGEMEM=8192 scripts/setup.sh
```

or manually reserve 2 MB pages and mount hugetlbfs. Without hugepages DPDK initialization fails with:

```
EAL: No free 2048 kB hugepages reported on node 0
[...]
Failed to initialize DPDK
```

Minimal command (local PCIe controller, defaults):

```bash
sudo build/examples/nvme/wrr_burst_test/wrr_burst_test
```

Useful options:

- `-r <trid>` NVMe transport ID (e.g. `trtype:PCIe` or `trtype:RDMA adrfam:IPv4 traddr:...`).
- `-C <num>` Commands per qpair (default 255).
- `-N <num>` Logical blocks per command (default 8).
- `-S <lba>` Starting LBA offset (default 0).
- `-Q <entries>` Queue depth / request pool size (default 511, capped at 511).
- `-O <path>` CSV output path (default `wrr_burst_log.csv`; use `-` for stdout).
- `--hpw`, `--mpw`, `--lpw` High/medium/low WRR weights (defaults 32/16/4).
- `--burst <0-7>` Arbitration burst value (default 7, 0 is valid and results in a burst of 1 command).
- `-W` Issue writes instead of reads.

To pin the test to a specific PCIe device, supply the BDF in the transport ID. For example:

```bash
sudo build/examples/nvme/wrr_burst_test/wrr_burst_test \
  -r "trtype:PCIe traddr:0000:81:00.0"
```

SPDK parses the `traddr` string and matches it against the controller's bus:device.function address during `spdk_nvme_probe()`, so only the selected device is attached.

At runtime the tool allocates nine qpairs (3 high, 3 medium, 3 low), primes each with 255 commands, rings all doorbells, then polls completions while recording timing metadata. Queue depth is limited to 511 entries per qpair and each qpair operates on its own 0x2000-LBA region to avoid overlap between queues.

## Trace Support

Enable SPDK tracepoints to capture doorbell activity:

```bash
sudo build/examples/nvme/wrr_burst_test/wrr_burst_test -e nvme_pcie
```

After the run inspect the log with `build/bin/spdk_trace -s wrr_burst_test -p <pid>`.

## Output

The CSV contains one row per command with:

- submission sequence index
- qpair ID and priority class
- command ID, opcode, SLBA, NLB
- submit / complete timestamps (µs) and latency
- completion status string

## Error
root@PAE-system:~/spdk# ./build/examples/wrr_burst_test -r "trtype:PCIe traddr:0000:1:00.0" nvme_pcie --hpw 5 --mpw 3 --lpw 1 --burst 1
EAL: '-c <coremask>' option is deprecated, and will be removed in a future release
EAL:    Use '-l <corelist>' or '--lcores=<corelist>' option instead
Probing 0000:01:00.0
Attached to 0000:01:00.0
Controller: MO001600KWZQP        (S50KNE0MB00502      )
  Namespace 1: size 1490 GiB

Running WRR burst test on namespace 1

Config:
  Commands/qpair      : 255
  LBAs/command        : 8
  Arbitration weights : HPW=5 MPW=3 LPW=1
  Arbitration burst   : 1
  Queue depth         : 511
  LBA stride/queue    : 0x2000
  Mode                : read
  Qpair 0 mapped to priority high (QID 1, base LBA 0)
malloc(): corrupted top size
Aborted (core dumped)
root@PAE-system:~/spdk# 