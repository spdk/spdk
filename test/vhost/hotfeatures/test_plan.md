## Tests Plan for vhost feature: Hot remove.

### The vhost blk feature.

#### Test Case1 - The hot remove on single disk.
Positive test in which to make a hot remove of NVMe disk.
Steps:
- Start vhost with one blk controller.
- Add one NVMe bdev to blk controller.
- Check pci address bus device function of NVMe.
- Run the command to hot remove NVMe disk.

Expected result:
The Vhost doesn't crash and still working properly.

#### Test Case2 -  The hot remove on single disk and single vm.
Positive test in which to make a hot remove of NVMe disk.
Steps:
- Start vhost with one blk controller.
- Add one NVMe bdev to blk controller.
- Run one VM, attach to blk controller.
- Run FIO integrity on NVMe disk.
- Check pci address bus device function.
- Run the command to hot remove NVMe disk.
- Check FIO returns an interrupt operation error.
- Reboot VM.
- Run FIO integrity on NVMe disk.
- Check FIO returns a run error.

Expected result:
The Vhost doesn't crash and still working properly.
Fio should be automatic crashed immediately after hot remove NVMe disk.

#### Test Case3: The hot remove on multiple disk and single vm.
Positive test in which to make a hot remove of first NVMe disk.
Steps:
- Start vhost with two blk controller.
- Add one NVMe bdev to blk controller.
- Run one VM, attach to first blk controller.
- Run FIO integrity on first NVMe disks.
- Check pci address bus device function of first NVMe disk.
- Run the command to hot remove of first NVMe disk.
- Check FIO returns an interrupt operation error on removed NVMe disk.
- Reboot VM
- Run FIO integrity on removed NVMe disk.
- Check FIO returns a run error.

Expected result:
The Vhost doesn't crash and still working properly.
Fio should be automatic crashed on VM after hot removed NVMe disk.

#### Test Case4 - The hot remove on multiple disk and multiple vm.
Positive test in which to make a hot remove of first NVMe disk.
Steps::
- Start vhost with two blk controller.
- Add one NVMe bdev to blk controller.
- Run two VM, attach to blk controller.
- Run FIO integrity on both NVMe disks.
- Check pci address bus device function of first NVMe disk.
- Run the command to hot remove of first NVMe disk.
- Check FIO returns an interrupt operation error on removed NVMe disk.
- Check finished status FIO. Write and read in the not-removed
  NVMe disk should be successful.
- Reboot all VM's.
- Run FIO integrity on not-removed NVMe disk.
- Check finished status FIO. Write and read in the not-removed
  NVMe disk should be successful.
- Run FIO integrity on removed NVMe disk.
- Check FIO returns a run error.

Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic crashed on both VM's immediately after hot remove NVMe disk.
Fio should be worked properly on both VM's with not-removed NVMe disk.

#### Test Case5 - The hot remove on single disk and multiple vm.
Positive test in which to make a hot remove of NVMe disk.
Steps:
- Start vhost with two blk controller.
- Add one split NVMe bdev to blk controller.
- Run one VM, attach to blk controller.
- Run FIO integrity on both NVMe disks in VM's.
- Check pci address bus device function of NVMe disk.
- Run the command to hot remove NVMe disk.
- Check FIO returns an interrupt operation error on both VM's.
- Reboot both VM's.
- Run FIO integrity on both VMs.
- Check FIO returns a run error.

Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic crashed on both VM's immediately after hot remove NVMe disk.

### The vhost scsi feature.

#### Test Case1 - The hot remove on single disk.
Positive test in which to make a hot remove of one NVMe disk.
Steps:
- Start vhost with one scsi controller.
- Add one NVMe bdev to scsi controller.
- Check pci address bus device function of NVMe.
- Run the command to hot remove NVMe disk.

Expected result:
The Vhost doesn't crash and still working properly.

#### Test Case2 -  The hot remove on single disk and multiple vm.
Positive test in which to make a hot remove of NVMe disk.
Steps:
- Start vhost with two scsi controller.
- Add one split NVMe bdev to scsi controller.
- Run one VM, attach to scsi controller.
- Run FIO integrity on both NVMe disks in VM's.
- Check pci address bus device function of NVMe disk.
- Run the command to hot remove NVMe disk.
- Check FIO returns an interrupt operation error on both VM's.
- Check if removed devices are gone from lsblk.
- Reboot both VM's.
- Check if removed devices are gone from lsblk.
- Run FIO integrity on both VMs.
- Check FIO returns a run error.


Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic crashed on both VM's immediately after hot remove NVMe disk.

#### Test Case3: The hot remove on multiple disk and single vm.
Positive test in which to make a hot remove of first NVMe disk.
Steps:
- Start vhost with two scsi controller.
- Add both NVMe bdevs to scsi controller.
- Run one VM, attach to scsi controller.
- Run FIO integrity on both NVMe disks in VM's.
- Check NVMe pci address NVMe disk.
- Run the command to hot remove NVMe disk.
- Check FIO returns an interrupt operation error on both VM's.
- Check if removed devices are gone from lsblk.
- Reboot both VM's.
- Check if removed devices are gone from lsblk.
- Run FIO integrity on both VMs.
- Check FIO returns a run error.


Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic crashed on both VM's immediately after hot remove NVMe disk.


#### Test Case4 - The hot remove on multiple disk and multiple vm.
Positive test in which to make a hot remove of first NVMe disk.
Test Plan:
- Start vhost with two scsi controller.
-  Add one NVMe bdev to scsi controller.
- Run one VM, attach to scsi controller.
- Run FIO integrity on both NVMe disks.
- Check pci address bus device function of first NVMe disk.
- Run the command to hot remove of first NVMe disk.
- Check FIO returns an interrupt operation error on removed NVMe disk.
- Check if removed devices are gone from lsblk.
- Check finished status FIO. Write and read in the not-removed.
  NVMe disk should be successful.
- Reboot both VM's.
- Check if removed disk are gone from lsblk.
- Run FIO integrity on not-removed NVMe disk.
- Check finished status FIO. Write and read in the not-removed.
  NVMe disk should be successful.
- Run FIO integrity on removed NVMe disk.
- Check FIO returns a run error.
- Check if removed devices are gone from lsblk.

Expected result:
The Vhost doesn't crash and still working properly.
Fio should be automatic crashed on VM immediately after hot remove NVMe disk.
Fio should be worked properly on VM with not-removed NVMe disk.
