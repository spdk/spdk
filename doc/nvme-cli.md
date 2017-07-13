# nvme-cli with SPDK Getting Started Guide {#nvme-cli}

Now nvme-cli can support both kernel driver and SPDK user mode driver for most of its available
commands.

## nvme-cli

1. Refer to the "README.md" under this folder to properly install nvme-cli first.

2. Run the "nvme list" command to see whether nvme-cli works and there is NVMe SSD installed.

## SPDK

3. Clone the SPDK repository from "https://github.com/spdk/spdk" under the nvme-cli folder.

4. Refer to the "README.md" under SPDK folder to properly install SPDK.

5. Run the "SPDK identify" example from  "https://github.com/spdk/spdk/tree/master/examples/nvme/identify"
to see whether SPDK works and there is NVMe SSD found.

## nvme-cli with SPDK

6. Re-build the nvme-cli after SPDK has been successfully built.

7. Execute "<spdk_folder>/scripts/setup.sh" with the "root" account.

8. Update the "spdk.conf" file under nvme-cli folder to properly configure the SPDK. Notes as following:
* spdk: default to 1 (on) and change to 0 (off) after switching back to kernel driver via
"<spdk_folder>/scritps/setup.sh reset".
* core_mask: default 0x100 to use the 9th core for the nvme-cli running.
* mem_size: default 512 where 512MB memory allocated.
* shm_id: default 1. If other running SPDK application has configured with this same 1 shm_id. This nvme-cli
will access those devices from that running SPDK application.

9. Run the "nvme list" command to get the domain:bus:device.function for each found NVMe SSD.

10. Run the other nvme commands with domain:bus:device.function instead of "/dev/nvmeX" for the specified device.

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
* Only access the NVMe SSDs it wants.
* Allocate a fixed number of memory instead of all available memory.

2. Properly configure the spdk.conf setting for nvme-cli.
* Not access the NVMe SSDs from other SPDK applications.
* Change the mem_size to a proper size.
