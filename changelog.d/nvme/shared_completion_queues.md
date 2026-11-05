Added experimental independent PCIe I/O submission and completion queue
management, gated by `--enable-nvme-shared-cq` (disabled by default) and a
per-controller `SPDK_NVME_QUEUE_MGMT_MODE_SHARED_CQ` opt-in.
Applications can allocate standalone CQs, bind multiple SQs, and poll a CQ
directly or through a single owning poll group. Legacy coupled qpairs remain
the default, and admin queues are unchanged.

Added size-aware transport registration and optional managed-queue and
poll-group operations while preserving the original registration prefix.
