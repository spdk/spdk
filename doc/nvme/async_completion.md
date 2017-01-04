# NVMe Asynchronous Completion {#nvme_async_completion}

The userspace NVMe driver follows an asynchronous polled model for
I/O completion.

# I/O commands {#nvme_async_io}

The application may submit I/O from one or more threads on one or more queue pairs
and must call spdk_nvme_qpair_process_completions()
for each queue pair that submitted I/O.

When the application calls spdk_nvme_qpair_process_completions(),
if the NVMe driver detects completed I/Os that were submitted on that queue,
it will invoke the registered callback function
for each I/O within the context of spdk_nvme_qpair_process_completions().

# Admin commands {#nvme_async_admin}

The application may submit admin commands from one or more threads
and must call spdk_nvme_ctrlr_process_admin_completions()
from at least one thread to receive admin command completions.
The thread that processes admin completions need not be the same thread that submitted the
admin commands.

When the application calls spdk_nvme_ctrlr_process_admin_completions(),
if the NVMe driver detects completed admin commands submitted from any thread,
it will invote the registered callback function
for each command within the context of spdk_nvme_ctrlr_process_admin_completions().

It is the application's responsibility to manage the order of submitted admin commands.
If certain admin commands must be submitted while no other commands are outstanding,
it is the application's responsibility to enforce this rule
using its own synchronization method.
