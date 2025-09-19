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

/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:475: undefined reference to `spdk_nvme_detach_poll_async'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `register_ctrlr':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:442: undefined reference to `spdk_nvme_ctrlr_get_data'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:448: undefined reference to `spdk_nvme_ctrlr_get_first_active_ns'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:450: undefined reference to `spdk_nvme_ctrlr_get_ns'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `register_ns':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:410: undefined reference to `spdk_nvme_ns_is_active'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:425: undefined reference to `spdk_nvme_ns_get_size'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:424: undefined reference to `spdk_nvme_ns_get_id'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `register_ctrlr':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:449: undefined reference to `spdk_nvme_ctrlr_get_next_active_ns'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `io_complete':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:519: undefined reference to `spdk_get_ticks'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `main':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:811: undefined reference to `spdk_env_opts_init'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `parse_args':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:306: undefined reference to `spdk_nvme_transport_id_parse'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:312: undefined reference to `spdk_log_set_flag'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `main':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:827: undefined reference to `spdk_env_init'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:832: undefined reference to `spdk_nvme_probe'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:848: undefined reference to `spdk_nvme_ns_get_id'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `run_wrr_burst_test':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:685: undefined reference to `spdk_nvme_ns_get_sector_size'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:686: undefined reference to `spdk_nvme_ns_get_num_sectors'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:725: undefined reference to `spdk_nvme_ctrlr_get_default_io_qpair_opts'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:732: undefined reference to `spdk_zmalloc'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:749: undefined reference to `spdk_nvme_ctrlr_alloc_io_qpair'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:758: undefined reference to `spdk_nvme_qpair_get_id'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `submit_burst':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:560: undefined reference to `spdk_nvme_ns_cmd_read'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:550: undefined reference to `spdk_get_ticks'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:557: undefined reference to `spdk_nvme_ns_cmd_write'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:565: undefined reference to `spdk_log'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `flush_submissions':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:588: undefined reference to `spdk_nvme_qpair_process_completions'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `run_wrr_burst_test':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:776: undefined reference to `spdk_nvme_qpair_process_completions'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:778: undefined reference to `spdk_log'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:791: undefined reference to `spdk_nvme_ctrlr_free_io_qpair'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:795: undefined reference to `spdk_free'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `main':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:852: undefined reference to `spdk_env_fini'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `run_wrr_burst_test':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:707: undefined reference to `spdk_strerror'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `main':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:852: undefined reference to `spdk_env_fini'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:860: undefined reference to `spdk_strerror'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:834: undefined reference to `spdk_strerror'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:836: undefined reference to `spdk_env_fini'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:843: undefined reference to `spdk_env_fini'
/usr/bin/ld.bfd: wrr_burst_test.o: in function `dump_completion_log':
/root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:609: undefined reference to `spdk_get_ticks_hz'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:633: undefined reference to `spdk_nvme_cpl_get_status_string'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:609: undefined reference to `spdk_get_ticks_hz'
/usr/bin/ld.bfd: /root/spdk/examples/nvme/wrr_burst_test/wrr_burst_test.c:669: undefined reference to `spdk_strerror'
collect2: error: ld returned 1 exit status
make[1]: *** [<builtin>: wrr_burst_test] Error 1
make: *** [/root/spdk/mk/spdk.subdirs.mk:16: wrr_burst_test] Error 2
make: Leaving directory '/root/spdk/examples/nvme'
root@PAE-system:~/spdk# 
