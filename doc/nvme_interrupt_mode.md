# NVMe Interrupt Mode {#nvme_interrupt_mode}

The SPDK NVMe initiator supports interrupt mode for transports that expose
interrupt-capable queue pairs. This allows initiator applications to sleep when
idle and wake on completion events instead of continuously polling, reducing CPU
usage when the initiator is not under load.

## Using spdk_nvme_perf

The `spdk_nvme_perf` application enables interrupt mode with the `-E` option.
Interrupt mode is supported for local PCIe devices and for NVMe-oF RDMA
initiators.

Example: Using `spdk_nvme_perf` in interrupt mode against an NVMe-oF RDMA target
~~~{.sh}
perf -q 128 -o 4096 -w randread -E \
	-r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' \
	-t 300
~~~

Applications using the NVMe library directly may enable interrupt mode through
`spdk_nvme_ctrlr_opts.enable_interrupts`. When enabled, interrupt-capable queue
pairs may expose file descriptors through `spdk_nvme_qpair_get_fd()`.

## Operational notes

* Interrupt mode is supported only in a primary SPDK process.
* Secondary processes are not supported when interrupts are enabled.
* Not all transports support interrupt-capable queue pairs. For the initiator
  side, PCIe and RDMA support interrupt mode.
* The application must continue to drive completions correctly after wakeup
  (for example, through its poll-group or qpair completion path).

## Verification

* `spdk_nvme_perf -E` can be used to verify interrupt-mode behavior for both
  PCIe and RDMA initiators.
* The regression test `test/nvmf/host/interrupt.sh` demonstrates expected
  initiator-side idle and busy transitions for NVMe-oF RDMA.
