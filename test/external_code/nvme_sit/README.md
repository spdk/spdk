# PCIe NVMe tracker capacity system integration test

This standalone runner ports the production N-1 capacity workload to ordinary
upstream PCIe I/O qpairs. It does not depend on production-only registration,
tracking, or shared-CQ APIs.

## Build and run

Build SPDK with `./configure --enable-nvme-n-minus-1-trackers` (plus your usual
options), then `make`. Build the standalone runner with:

```sh
make -C test/external_code/nvme_sit
test/external_code/nvme_sit/sit_runner --list
```

Use a dedicated PCIe NVMe test device already bound to an SPDK-compatible
driver, with hugepages available. The runner requires exactly one explicit BDF;
it never rebinds devices. Do not select a device containing mounted filesystems.
All test I/O consists of raw, single-block READs at LBA 0.

```sh
sudo test/external_code/nvme_sit/run_all_sit_tests.sh --no-build \
    --bdf=0000:01:00.0
sudo test/external_code/nvme_sit/sit_runner --bdf=0000:01:00.0 \
    --test=test_tracker_capacity
```

The default IOVA mode is `pa`, matching production SIT. Set `SIT_IOVA_MODE=va`
when required by the device's VFIO/IOMMU configuration.

## Coverage

For each supported depth among 64, 512, and 1024:

- Set both `io_queue_size` and `io_queue_requests` to the requested depth and
  verify the effective queue depth before running.
- Submit exactly N-1 READs without polling. Inspect the internal outstanding
  tracker list and software request queue to prove they reached the hardware
  submission path, rather than accepting submission success as proof.
- Drain and require exactly one successful callback per READ.
- Repeat the burst four times to exercise tracker reuse and ring wraparound.
- Resubmit from completion callbacks for 64 completions per initial command,
  exercising sustained progress at N-1 capacity and the unchanged CQ doorbell
  batching. Require the exact completion count for every command stream.

Each drain has a 30-second wall-clock deadline, independent of the number of
polls needed for callback resubmissions. A timeout reports outstanding requests,
polls, completions, time since the last completion, and the SQ/CQ positions.

Commands use the public raw API and driver-built payload mappings. Only the
occupancy assertions use internal PCIe structures. There are no capability or
transfer-limit overrides.

A disabled N-1 build, unsupported queue depth, or unsupported namespace geometry
is reported explicitly as SKIP. If no depth can run, the test exits 77, not 0.
Missing requested devices, unexpected depth clamping, tracker deferral, timeout,
or completion errors are failures. Exit status 1 indicates failure; 0 means all
supported depths passed. Unsupported depths are printed individually.

This is functional regression coverage, not a proof of CQ-headroom safety for
every controller, nor a performance benchmark. Compare the default build and
experimental build under representative workloads before deployment.
