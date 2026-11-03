# Independent PCIe SQ/CQ integration test

This standalone, read-only hardware SIT follows the shared-CQ cases from the
production implementation. It uses only public SPDK APIs and does not depend on
the PRP-pool or N-1 tracker extensions.

Use an exclusively reserved, userspace-bound PCIe NVMe controller with at least
four I/O queue IDs, a maximum queue depth of at least 64, and an active namespace
without metadata. The test only reads LBA 0. `--reset` additionally resets the
entire controller and must not be used while another application uses it.

## Build and run

From the SPDK repository root:

```sh
./configure --enable-nvme-shared-cq --enable-werror
make -j4
make -C test/external_code/shared_cq
sudo test/external_code/shared_cq/shared_cq_sit \
    -r 'trtype:PCIe traddr:0000:01:00.0' --reset 2>&1 | tee /tmp/shared-cq-sit.log
```

Omit `--reset` to skip controller reset. Use `-n NSID` to select a namespace;
otherwise the first active namespace is selected. Asynchronous connection and
I/O-drain polling loops use a 30-second elapsed-time deadline; admin-command
waits use the same timeout. No fixed polling-iteration limit is used.

After changing configure options, run the root build as above. For a focused
rebuild, generate the C configuration header first, then rebuild the library
and SIT:

```sh
make include/spdk/config.h
make -C lib/nvme -j4
make -C test/external_code/shared_cq
```

Running only subdirectory builds after `./configure` can leave a stale
`include/spdk/config.h`, so the binary may still have the previous build gate.

For the disabled-build check, rebuild the libraries and SIT:

```sh
./configure --disable-nvme-shared-cq --enable-werror
make -j4
make -C test/external_code/shared_cq
sudo test/external_code/shared_cq/shared_cq_sit \
    -r 'trtype:PCIe traddr:0000:01:00.0' --expect-disabled
```

## Coverage

- Legacy defaults and coupled-qpair I/O in both builds; explicit disabled-build
  rejection and queue-management option readback.
- Independent queue IDs, identical numeric SQID/CQID, duplicate-ID rejection,
  CQ-free and mode-change busy checks, and SQ depth larger than CQ depth.
- SQID-based callback routing, polling through a sibling SQ, repeated CQ wraps,
  SQID reuse, and coexistence with a coupled qpair.
- Direct CQ polling of create-only queues, poll-group exclusion, cross-group
  rejection, asynchronous connection progress, and a create-only sibling.
- Bounded, scaled CQ completion budgets and exactly one CQ poll per group pass,
  measured with the public PCIe poll counters rather than inferred from I/O.
- Delayed SQ doorbells, poll-group migration with replacement SQs on the same
  CQ, implicit removal on SQ free,
  deletion with outstanding reads, and reuse while a sibling remains active.
- Optional reset with outstanding reads, CQ-before-SQ reconnect, old-request
  retirement, and successful post-reset I/O through the existing poll group.

Each submitted read must invoke its own callback exactly once, with the expected
SQID and successful status, except that requests deliberately interrupted by
reset/deletion may report aborts. Returned completion totals are checked against
callback totals. Cleanup failures make the test fail. DMA buffers and callback
contexts remain alive through controller detach, including failure paths.
