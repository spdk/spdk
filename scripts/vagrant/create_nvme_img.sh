#!/usr/bin/env bash
WHICH_OS=`lsb_release -i | awk '{print $3}'`
nvme_disk='/var/lib/libvirt/images/nvme_disk.img'

qemu-img create -f raw $nvme_disk 1024M
#Change SE Policy on Fedora
if [ $WHICH_OS == "Fedora" ]; then
  sudo chcon -t svirt_image_t $nvme_disk
fi

chmod 777 $nvme_disk
chown qemu:qemu $nvme_disk
