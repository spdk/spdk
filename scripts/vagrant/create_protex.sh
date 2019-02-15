sudo rm /var/lib/libvirt/images/nvme_disk.img -f 
sudo ./create_nvme_img.sh

./create_vbox.sh -p libvirt -x `echo $http_proxy` ubuntu18
cd ./ubuntu18-libvirt
vagrant ssh
