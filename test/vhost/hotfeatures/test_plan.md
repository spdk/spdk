## Tests Plan for vhost blk feature: Hot remove.

##Test case1:   BLK-HR-1
Configuration: 	1 NVMe disk
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: Start vhost with one blk controller.
	Step2: Add one NVMe bdev to blk controller.
	Step3: Check NVMe pci address.
	Step4: Run the command to hot remove NVMe disk.
	Step5: Check vhost messages. 

Expected result:
The Vhost don't crash and still working properly.


##Test case2:   BLK-HR-2
Configuration: 	1 NVMe disk
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: 	Start vhost with one blk controller.
	Step2: 	Add one NVMe bdev to blk controller.
	Step3: 	Run one VM, attach to blk controller.
	Step4: 	Run FIO integrity on NVMe disk.
	Step5: 	Check NVMe pci address.
	Step6: 	Run the command to hot remove NVMe disk.
	Step7: 	Check vhost messages. 
	Step8: 	Check FIO returns error. 
	Step9: 	Reboot VM.
	Step10: Check vhost messages. 

Expected result:
The Vhost don't crash and still working properly.
Fio should be automatic stopped before hot remove NVMe disk.

##Test case3:   BLK-HR-3
Configuration: 	2 NVMe disks
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: 	Start vhost with two blk controller.
	Step2: 	Add one NVMe bdev to blk controller.
	Step3: 	Run one VM, attach to blk controller.
	Step4: 	Run FIO integrity on both NVMe disks.
	Step5: 	Check NVMe pci address one NVMe disk.
	Step6: 	Run the command to hot remove NVMe disk.
	Step7: 	Check vhost messages. 
	Step8: 	Check FIO returns error on removed NVMe disk. 
	Step9:	Check FIO finished properly on remaining NVMe disk.
	Step9: 	Reboot both VM's
	Step10: Check vhost messages. 
	Step11: Run FIO integrity on not-removed NVMe disk.
	Step12: Check FIO finished properly.

Expected result:
The Vhost don't crash and still working properly.
Fio should be automatic stopped on VM before hot remove NVMe disk.
Fio should be worked properly on VM with not-removed NVMe disk.


##Test case4:   BLK-HR-4
Configuration: 	2 NVMe disks
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: 	Start vhost with two blk controller.
	Step2: 	Add one NVMe bdev to blk controller.
	Step3: 	Run two VM, attach to blk controller.
	Step4: 	Run FIO integrity on both NVMe disks.
	Step5: 	Check NVMe pci address one NVMe disk.
	Step6: 	Run the command to hot remove NVMe disk.
	Step7: 	Check vhost messages. 
	Step8: 	Check FIO returns error on removed NVMe disk. 
	Step9:	Check FIO finished properly on remaining NVMe disk
	Step10: Reboot all VM's.
	Step11: Check vhost messages. 
	Step12: Run FIO integrity on not-removed NVMe disk.
	Step13: Check FIO finished properly

Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic stopped on both VM's before hot remove NVMe disk.
Fio should be worked properly on both VM's with not-removed NVMe disk.

##Test case5:   BLK-HR-5
Configuration: 	1 NVMe disks
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: 	Start vhost with two blk controller.
	Step2: 	Add one split NVMe bdev to blk controller.
	Step3: 	Run one VM, attach to blk controller.
	Step4: 	Run FIO integrity on both NVMe disks in VM's.
	Step5: 	Check NVMe pci address NVMe disk.
	Step6: 	Run the command to hot remove NVMe disk.
	Step7: 	Check vhost messages. 
	Step8: 	Check FIO returns error on both VM's . 
	Step9: 	Reboot both VM's
	Step10: Check vhost messages. 
	Step11: Run FIO integrity on both VMs.
	Step12: Check FIO returns error.

Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic stopped on both VM's before hot remove NVMe disk.



## Tests Plan for vhost scsi feature: Hot remove.

##Test case1: 	SCSI-HR-1
Configuration: 	1 NVMe disk
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: Start vhost with one scsi controller.
	Step2: Add one NVMe bdev to scis controller.
	Step3: Check NVMe pci address.
	Step4: Run the command to hot remove NVMe disk.
	Step5: Check vhost messages. 

Expected result:
The Vhost don't crash and still working properly.


##Test case2:   SCSI-HR-2
Configuration: 	1 NVMe disk
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: 	Start vhost with two scsi controller.
	Step2: 	Add one split NVMe bdev to scsi controller.
	Step3: 	Run one VM, attach to scsi controller.
	Step4: 	Run FIO integrity on both NVMe disks in VM's.
	Step5: 	Check NVMe pci address NVMe disk.
	Step6: 	Run the command to hot remove NVMe disk.
	Step7: 	Check vhost messages. 
	Step8: 	Check FIO returns error on both VM's . 
	Step9: 	Check if removed devices are gone from lsblk
	Step10: Reboot both VM's
	Step11: Check vhost messages. 
	Step12: Check if removed devices are gone from lsblk.
	

Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic stopped on both VM's before hot remove NVMe disk.

##Test case3:   SCSI-HR-3
Configuration: 	2 NVMe disks
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: 	Start vhost with two scsi controller.
	Step2: 	Add both NVMe bdevs to scsi controller.
	Step3: 	Run one VM, attach to scsi controller.
	Step4: 	Run FIO integrity on both NVMe disks in VM's.
	Step5: 	Check NVMe pci address NVMe disk.
	Step6: 	Run the command to hot remove NVMe disk.
	Step7: 	Check vhost messages. 
	Step8: 	Check FIO returns error on both VM's . 
	Step9: 	Check if removed devices are gone from lsblk
	Step10: Reboot both VM's
	Step11: Check vhost messages. 
	Step12: Check if removed devices are gone from lsblk.
	

Expected result:
The Vhost and VM's don't crash and still working properly.
Fio should be automatic stopped on both VM's before hot remove NVMe disk.


##Test case4:   SCSI-HR-4
Configuration: 	2 NVMe disks
		UIO_PCI_GENERIC driver 

Test Plan:
	Step1: 	Start vhost with two scsi controller.
	Step2: 	Add one NVMe bdev to scsi controller.
	Step3: 	Run one VM, attach to scsi controller.
	Step4: 	Run FIO integrity on both NVMe disks.
	Step5: 	Check NVMe pci address one NVMe disk.
	Step6: 	Run the command to hot remove NVMe disk.
	Step7: 	Check vhost messages. 
	Step8: 	Check FIO returns error on removed NVMe disk. 
	Step9:	Check FIO finished properly on remaining NVMe disk.
	Step9: 	Reboot both VM's
	Step10: Check vhost messages. 
	Step11: Run FIO integrity on not-removed NVMe disk.
	Step12: Check FIO finished properly.

Expected result:
The Vhost don't crash and still working properly.
Fio should be automatic stopped on VM before hot remove NVMe disk.
Fio should be worked properly on VM with not-removed NVMe disk.

