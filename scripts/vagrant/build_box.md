Use default vagrant box for example fedora30 from our scripts
Got to vm
run pkgdep.sh and vm_setup.sh
Create new box:
    vagrant package --output f30_spdk.box
Add the Box into Your Vagrant Install
    vagrant box add f30_spdk f30_spdk.box

If You do this on Ubuntu You will need libguestfs-tools package
and add your user to kvm group.
Then start new vm:
    ./spdk/scripts/vagrant/create_vbox.sh -b /var/lib/libvirt/images/nvme_disk.img-nvme-0 -n 20 -s 16384 -x $http_proxy -p libvirt -v -l f30_spdk

Problem that I have now is no ssh connection via keys.
Solved by adding to Vagrantfile user and password from hand.
