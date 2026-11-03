# PCIe NVMe PRP system integration tests

This SIT runner exercises near-MDTS transfers and concurrent PRP bursts using
standard SPDK APIs. It does not depend on
production-only controller registration, tracking, or queue-management APIs.

## Build and run

Build SPDK with `./configure --enable-nvme-prp-pool` (plus the options normally
used for your environment), then `make`. Build the standalone runner with:

```sh
make -C test/external_code/nvme_sit
test/external_code/nvme_sit/sit_runner --list
```

Use a dedicated PCIe NVMe test device already bound to an SPDK-compatible
driver, with hugepages available. The runner requires exactly one explicit BDF;
it never rebinds devices. Do not select a device containing mounted filesystems.
All test I/O is read-only.

```sh
sudo test/external_code/nvme_sit/run_all_sit_tests.sh --no-build --bdf=0000:01:00.0
sudo test/external_code/nvme_sit/sit_runner --bdf=0000:01:00.0 \
    --test=test_mdts_prp
```

To exercise PRPs on a controller supporting SGLs, explicitly add `--force-prp`:

```sh
sudo test/external_code/nvme_sit/run_all_sit_tests.sh --no-build \
    --bdf=0000:01:00.0 --force-prp
```

This opt-in diagnostic clears the driver's SGL capability flag for the test
attachment only. It requires the internal NVMe header; there is no public API
for this override. It does not change device Identify data, firmware, or MDTS.
The near-MDTS test still skips a controller advertising MDTS=0; the small PRP
burst can run on that controller. Normal attachments retain SGL selection.

For explicit mapping diagnostics on an MDTS=0 device, add `--test-2mib` with
`--force-prp`. This creates a test-only 2 MiB pool and driver transfer limit
without changing Identify data or production defaults. The large-transfer
test then issues separate 2 MiB READs with aligned and 512-byte-offset buffers,
covering pool-backed mapping and the PRP-list chaining boundary. This does not
validate handling of a device-advertised finite MDTS. The override requires
4 KiB controller pages and a build with PRP pools enabled.

```sh
sudo test/external_code/nvme_sit/sit_runner --bdf=0000:01:00.0 \
    --force-prp --test-2mib --test=test_mdts_prp
```

The default IOVA mode is `pa`, matching production SIT. Set `SIT_IOVA_MODE=va`
when required by the device's VFIO/IOMMU configuration.

## Coverage

- **MDTS PRP:** one raw 4,040-block READ (2,068,480 bytes) on a namespace with
  512-byte LBAs and 4 KiB controller/minimum pages. This exceeds the embedded
  503-entry PRP capacity but must fit the device's finite MDTS. Verify exactly
  one successful completion. A qualifying device with an insufficient SPDK
  transfer limit fails, including a build without PRP pool support.
- **PRP burst:** simultaneous 4/8/16 KiB READs on three independent qpairs, first with
  driver-built PRPs and then with prebuilt PRPs through the raw API. Poll a
  health-log request while data completions are pending, drain qpairs in order,
  and verify stable CAP/CC registers and ready/nonfatal controller status.
  Keep prebuilt lists alive until their commands complete.

Tests skip unsupported geometry, namespace metadata, or controllers using SGLs
instead of the PRP mapper unless `--force-prp` is specified. Missing requested
devices and register/DMA failures
are errors, not skips. A skip is reported as exit status 77, not a pass.
Exit status 1 indicates failure; 0 means all selected tests passed.

PRP burst retains the production transfer sizes, concurrent queue topology, staggered
draining, raw prebuilt-PRP alternative, and health/register checks. Production
context inspection and private PCIe configuration callbacks are excluded.
The near-MDTS regression exercises pool-backed mapping; PRP burst covers the existing
small-transfer paths. These cases do not exhaust the controller pool or force
multi-page PRP-list chaining unless `--test-2mib` is explicitly selected.
