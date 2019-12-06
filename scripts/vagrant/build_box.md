Build default vagrant box for example fedora30 from our scripts
```
./spdk/scripts/vagrant/create_vbox.sh -b /var/lib/libvirt/images/nvme_disk.img-nvme-0 -d -n 20 -s 16384 -x $http_proxy -p libvirt -v fedora30
```
Important is use -d option to provision all dependencies.
Create new box:
    vagrant package --output spdk_fedora30.box
Add the Box into Your Vagrant Install
    vagrant box add spdk_fedora30 spdk_fedora30.box

If You do this on Ubuntu You will need libguestfs-tools package
and add your user to kvm group.
Then start new vm:
    ./spdk/scripts/vagrant/create_vbox.sh -b /var/lib/libvirt/images/nvme_disk.img-nvme-0 -n 20 -s 16384 -x $http_proxy -p libvirt -v -l spdk_fedora30
