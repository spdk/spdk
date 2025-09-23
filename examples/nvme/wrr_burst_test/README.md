# WRR Burst Test Example

This example exercises NVMe Weighted Round Robin (WRR) arbitration by submitting large command bursts on multiple I/O queue pairs with different priorities. Each queue prepares its commands while the `delay_cmd_submit` feature is enabled; after all queues are primed the application rings every submission queue doorbell, allowing the controller to arbitrate a synchronized burst.

## Build

From the repository root run either build system:

```bash
ninja -C build examples/nvme/wrr_burst_test
```

or

```bash
make -C examples/nvme wrr_burst_test
```

The resulting binary lives at `build/examples/nvme/wrr_burst_test/wrr_burst_test` (path may differ if you use a different build directory).

## Run

Minimal command (local PCIe controller, defaults):

```bash
sudo build/examples/nvme/wrr_burst_test/wrr_burst_test
```

Useful options:

- `-r <trid>` NVMe transport ID (e.g. `trtype:PCIe` or `trtype:RDMA adrfam:IPv4 traddr:...`).
- `-C <num>` Commands per qpair (default 255).
- `-N <num>` Logical blocks per command (default 8).
- `-S <lba>` Starting LBA offset (default 0).
- `-Q <entries>` Queue depth / request pool size (default 512).
- `-O <path>` CSV output path (default `wrr_burst_log.csv`; use `-` for stdout).
- `--hpw`, `--mpw`, `--lpw` High/medium/low WRR weights (defaults 32/16/4).
- `--burst <0-7>` Arbitration burst value (default 7).
- `-W` Issue writes instead of reads.

To pin the test to a specific PCIe device, supply the BDF in the transport ID. For example:

```bash
sudo build/examples/nvme/wrr_burst_test/wrr_burst_test \
  -r "trtype:PCIe traddr:0000:81:00.0"
```

SPDK parses the `traddr` string and matches it against the controller's bus:device.function address during `spdk_nvme_probe()`, so only the selected device is attached.

At runtime the tool allocates nine qpairs (3 high, 3 medium, 3 low), primes each with 255 commands, rings all doorbells, then polls completions while recording timing metadata.

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

Use this data to verify that completion shares track the configured WRR weights.

##################
root@PAE-system:~/spdk# make
ninja: Entering directory `/root/spdk/dpdk/build-tmp'
ninja: no work to do.
  CC examples/nvme/wrr_burst_test/wrr_burst_test.o
wrr_burst_test.c: In function ‘probe_cb’:
wrr_burst_test.c:484:9: warning: implicit declaration of function ‘SPDK_UNUSED’ [-Wimplicit-function-declaration]
  484 |         SPDK_UNUSED(cb_ctx);
      |         ^~~~~~~~~~~
wrr_burst_test.c: In function ‘dump_completion_log’:
wrr_burst_test.c:633:53: error: ‘struct spdk_nvme_status’ has no member named ‘raw’
  633 |                 struct spdk_nvme_status status = { .raw = entry->status_raw };
      |                                                     ^~~
make[3]: *** [/root/spdk/mk/spdk.common.mk:540: wrr_burst_test.o] Error 1
make[2]: *** [/root/spdk/mk/spdk.subdirs.mk:16: wrr_burst_test] Error 2
make[1]: *** [/root/spdk/mk/spdk.subdirs.mk:16: nvme] Error 2
make: *** [/root/spdk/mk/spdk.subdirs.mk:16: examples] Error 2