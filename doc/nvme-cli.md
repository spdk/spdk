# nvme-cli {#nvme-cli}

# nvme-cli with SPDK Getting Started Guide

Now nvme-cli can support both kernel driver and SPDK user mode driver for most of its available
commands.

## nvme-cli

1. Clone the nvme-cli git repository from the SPDK GitHub fork. Make sure you check out the spdk branch.
git clone -b spdk https://github.com/spdk/nvme-cli.git

2. Refer to the "README.md" under nvme-cli folder to properly install nvme-cli first.

3. Run the "nvme list" command to see whether nvme-cli works and there is NVMe SSD installed.

## SPDK

1. Clone the SPDK repository from https://github.com/spdk/spdk under the nvme-cli folder.

2. Refer to the "README.md" under SPDK folder to properly install SPDK.

3. Run the "SPDK identify" example from  https://github.com/spdk/spdk/tree/master/examples/nvme/identify
to see whether SPDK works and there is NVMe SSD found.

## nvme-cli with SPDK

1. Re-build the nvme-cli after SPDK has been successfully built.

2. Execute "<spdk_folder>/scripts/setup.sh" with the "root" account.

3. Update the "spdk.conf" file under nvme-cli folder to properly configure the SPDK. Notes as following:
~~~{.sh}
spdk: default to 0 (off) and change to 1 (on) after switching to SPDK via "<spdk_folder>/scritps/setup.sh".
core_mask: default 0x100 to use the 9th core for the nvme-cli running.
mem_size: default 512 where 512MB memory allocated.
shm_id: default 1. If other running SPDK application has configured with this same 1 shm_id. This nvme-cli will access those devices from that running SPDK application.
~~~

4. Run the "nvme list" command to get the domain:bus:device.function for each found NVMe SSD.

5. Run the other nvme commands with domain:bus:device.function instead of "/dev/nvmeX" for the specified device.
Example: nvme smart-log 0000:01:00.0

6. Execute "<spdk_folder>/scripts/setup.sh reset" with the "root" account and update "spdk=0" in spdk.conf to
use the kernel driver if wanted.

## Use scenarios

### Run as the only SPDK application on the system
Use the default spdk.conf setting is ok unless the system has fewer cores or less memory. In this case,
update the spdk.conf is required.

### Run together with other running SPDK applications on shared NVMe SSDs
1. For the other running SPDK application, start with the parameter like "-i 1" to have the same "shm_id".

2. Use the default spdk.conf setting where "shm_id=1" to start the nvme-cli.

3. If other SPDK applications run with different shm_id parameter, update the "spdk.conf" accordingly.

### Run with other running SPDK applications on non-shared NVMe SSDs
1. Properly configure the other running SPDK applications.
~~~{.sh}
a. Only access the NVMe SSDs it wants.
b. Allocate a fixed number of memory instead of all available memory.
~~~

2. Properly configure the spdk.conf setting for nvme-cli.
~~~{.sh}
a. Not access the NVMe SSDs from other SPDK applications.
b. Change the mem_size to a proper size.
~~~
