# nvme-cli {#nvme-cli}

# nvme-cli with SPDK Getting Started Guide

Now nvme-cli can support both kernel driver and SPDK user mode driver for most of its available commands.

1. Clone the nvme-cli repository from the SPDK GitHub fork. Make sure you check out the spdk branch.
~~~{.sh}
git clone -b spdk https://github.com/spdk/nvme-cli.git
~~~

2. Clone the SPDK repository from https://github.com/spdk/spdk under the nvme-cli folder.

3. Refer to the "README.md" under SPDK folder to properly build SPDK.

4. Refer to the "README.md" under nvme-cli folder to properly build nvme-cli.

5. Execute "<spdk_folder>/scripts/setup.sh" with the "root" account.

6. Update the "spdk.conf" file under nvme-cli folder to properly configure the SPDK. Notes as following:
~~~{.sh}
spdk=0
Default to 0 (off) and change to 1 (on) after switching to SPDK via "<spdk_folder>/scripts/setup.sh".

core_mask=0x100
Default to use the 9th core for the nvme-cli running.

mem_size=512
Default to use 512MB memory allocated.

shm_id=1
Default to 1. If other running SPDK application has configured with this same 1 shm_id.
This nvme-cli will access those devices from that running SPDK application.
~~~

7. Run the "./nvme list" command to get the domain:bus:device.function for each found NVMe SSD.

8. Run the other nvme commands with domain:bus:device.function instead of "/dev/nvmeX" for the specified device.
~~~{.sh}
Example: ./nvme smart-log 0000:01:00.0
~~~

9. Execute "<spdk_folder>/scripts/setup.sh reset" with the "root" account and update "spdk=0" in spdk.conf to
use the kernel driver if wanted.

## Use scenarios

### Run as the only SPDK application on the system
1. Modify the spdk to 1 in spdk.conf. If the system has fewer cores or less memory, update the spdk.conf accordingly.

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

## Note
To run the newly built nvme-cli, either explicitly run as "./nvme" or added it into the $PATH to avoid invoke
other already installed version.
