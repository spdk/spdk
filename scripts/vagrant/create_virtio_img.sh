#!/usr/bin/env bash
SYSTEM=`uname -s`

if [ ! "${SYSTEM}" = "FreeBSD" ]; then
	WHICH_OS=`lsb_release -i | awk '{print $3}'`
	virtio_scsi_disk='/var/lib/libvirt/images/virtio_scsi.img'
	virtio_blk_disk='/var/lib/libvirt/images/virtio_blk.img'

	qemu-img create -f raw $virtio_scsi_disk 128M
	qemu-img create -f raw $virtio_blk_disk 128M
	#Change SE Policy on Fedora
	if [ $WHICH_OS == "Fedora" ]; then
		sudo chcon -t svirt_image_t $virtio_scsi_disk
		sudo chcon -t svirt_image_t $virtio_blk_disk
	fi

	chmod 777 $virtio_scsi_disk
	chmod 777 $virtio_blk_disk
	chown qemu:qemu $virtio_scsi_disk
	chown qemu:qemu $virtio_blk_disk
fi
