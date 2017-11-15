This readme is aimed at helping developers spin up a machine on which they can run the
spdk test suite rooted at autorun.sh. The goal is that developers will be able to
quickly spin up a local validation system comparable to that run in the offical spdk
build pool.
Below are the requirements for instantiating an spdk ready fedora 26 vm on your system.
There is a small section titled Virtual Machine requirements that details some of th
preliminary steps required to prepare a suitable vm environment on a host. There is also
a section detailing the autorun-spdk.conf system. Lastly I include a list of instructions
for how to actually prepare the virtual machine.

Virtual Machine Envronment Requirements (for Host):
8 GiB of RAM (for DPDK)
enable intel_kvm on your host machine machine
enable nesting for vms in kernel command line (for vhost tests)
    in /etc/default/grub
    append the following to the GRUB_CMDLINE_LINUX line
    intel_iommu=on kvm-intel.nested=1

Virtual Machine Specs:
    It is best to create a user with the name sys_sgsw for the time being and give them passwordless sudo access.
    We have been working to remove all direct references to this user in the autotest scripts, but there are still
    a couple of trailing references to it.


    Steps:
        1. Download a fresh fedora 26 image.
        2. perform the installation of fedora 26 server.
        3. create an admin user sys_sgsw (enabling passwordless sudo for this account will make life easier during the tests).
        4. clone the spdk repository into your home directory.
        5. run the test_vm_setup.sh script which will install all proper dependencies.
        6. modify the autorun-spdk.conf file in your home directory to fit your needs.
        7. run autorun.sh for spdk. any output files will be placed in ~/spdk_repo/output/


    Additional Steps for preparing Vhost tests:
        The Vhost tests also require the creation of a second virtual machine nested inside of the test vm.
        Please follow the directions below to complete that installation. Note that host refers to the fedora vm
        created above and guest refers to the ubuntu vm created in this section.
        1. Create image file for VM; does not have to be large in size, ~3.5G should suffice
        2. create ssh keypair for host-guest communications (performed on the host)
        - generate an ssh keypair with the name spdk_vhost_id_rsa and save it in /root/.ssh
        - make sure that only root has read access to the private key
        3. Install OS in VM image (performed on guest)
        - use latest Ubuntu Server 16.04 LTS (last time it was 16.04.02)
        - user is sys_sgci / sys_sgci
        - 1 partition, size = 100% size of image file, mount point = /; NO LVMs, NO ENCRYPTION - plain simple install on /
        - Choose OpenSSH server during install
        4. After install (performed on guest)
        - sudo apt update
        - sudo apt upgrade
        - enable root user; sudo passwd root -> "root"
        - sudo apt install fio
        - sudo apt install sg3-utils
        - sudo apt install bc
        - vim /etc/ssh/sshd_config -> PermitRootLogin=yes
        - vim /etc/ssh/sshd_config -> UseDNS=no
        - /root/.ssh/authorized_keys -> add spdk_vhost_id_rsa.pub key to authorized keys
        (remember to have private key in ~/.ssh/spdk_vhost_id_rsa​ file on your host server !)
        - add "console=ttyS0 earlyprintk=ttyS0" to boot options in /etc/default/grub (for serial output redirect)
        - add "scsi_mod.use_blk_mq=1" to boot options in /etc/default/grub​
        - sudo update-grub
        - reboot
        - Remove unnecessary packages: (this is for quicker boot)
        - apt purge snapd
        - apt purge ubuntu-core-launcher
        - apt purge squashfs-tools
        - apt purge unattended-upgrades
        5. copy the fio binary from the guest location /usr/bin/fio to the host location /home/sys_sgsw/fio_ubuntu.
        6. place your guest vm in the host at the following location /home/sys_sgsw/vhost_vm_image.qcow2
