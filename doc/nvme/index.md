# NVMe Driver {#nvme}

# Public Interface {#nvme_interface}

- spdk/nvme.h

# Key Functions {#nvme_key_functions}

Function                                | Description
--------------------------------------- | -----------
spdk_nvme_probe()                       | @copybrief spdk_nvme_probe()
spdk_nvme_ns_cmd_read()                 | @copybrief spdk_nvme_ns_cmd_read()
spdk_nvme_ns_cmd_write()                | @copybrief spdk_nvme_ns_cmd_write()
spdk_nvme_ns_cmd_dataset_management()   | @copybrief spdk_nvme_ns_cmd_dataset_management()
spdk_nvme_ns_cmd_flush()                | @copybrief spdk_nvme_ns_cmd_flush()
spdk_nvme_qpair_process_completions()   | @copybrief spdk_nvme_qpair_process_completions()

# Key Concepts {#nvme_key_concepts}

- @ref nvme_initialization
- @ref nvme_io_submission
- @ref nvme_async_completion
