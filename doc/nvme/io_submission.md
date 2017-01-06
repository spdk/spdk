# NVMe I/O Submission {#nvme_io_submission}

I/O is submitted to an NVMe namespace using nvme_ns_cmd_xxx functions
defined in nvme_ns_cmd.c.  The NVMe driver submits the I/O request
as an NVMe submission queue entry on the queue pair specified in the command.
The application must poll for I/O completion on each queue pair with outstanding I/O
to receive completion callbacks.

@sa spdk_nvme_ns_cmd_read, spdk_nvme_ns_cmd_write, spdk_nvme_ns_cmd_dataset_management,
spdk_nvme_ns_cmd_flush, spdk_nvme_qpair_process_completions
