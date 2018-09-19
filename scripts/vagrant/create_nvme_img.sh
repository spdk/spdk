#!/usr/bin/env bash
SYSTEM=`uname -s`
size="1024M"

# NVMe img size example format: 2048M
if [ -n $1 ]; then
	size=$1
fi

if [ ! "${SYSTEM}" = "FreeBSD" ]; then
	WHICH_OS=`lsb_release -i | awk '{print $3}'`
	nvme_disk='/var/lib/libvirt/images/nvme_disk.img'

	qemu-img create -f raw $nvme_disk ${size}
	#Change SE Policy on Fedora
	if [ $WHICH_OS == "Fedora" ]; then
		sudo chcon -t svirt_image_t $nvme_disk
	fi

	chmod 777 $nvme_disk
	chown qemu:qemu $nvme_disk
fi
