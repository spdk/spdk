#!/usr/bin/env bash
WHICH_OS=`lsb_release -i | awk '{print $3}'`
nvme_disk='/var/lib/libvirt/images/nvme_disk.img'

sudo qemu-img create -f raw $nvme_disk 1024M
#Change SE Policy on Fedora
if [ $WHICH_OS == "Fedora" ]; then
  sudo chcon -t svirt_image_t $nvme_disk
fi

sudo chmod 777 $nvme_disk
sudo chown qemu:qemu $nvme_disk
