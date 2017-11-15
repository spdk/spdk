This readme is aimed at helping developers spin up a machine on which they can run the SPDK test suite rooted at autorun.sh.
The goal is that developers will be able to quickly create a local validation system comparable to that run in the offical
SPDK build pool.
Below are the requirements for instantiating an SPDK ready fedora 26 VM on your system. There is a small section titled VM
environment requirements that details some of the preliminary steps required to prepare a suitable VM environment on a host.
There is also a section detailing the autorun-spdk.conf system. Lastly I include a list of instructions for how to actually
prepare the virtual machine.

VM envronment requirements (for host):
    1. 8 GiB of RAM (for DPDK)
    2. Enable intel_kvm on your host machine from the bios.
    3. Enable nesting for VMs in kernel command line (for vhost tests).
        - In /etc/default/grub append the following to the GRUB_CMDLINE_LINUX line: intel_iommu=on kvm-intel.nested=1.

VM specs:
    It is best to create a user with the name sys_sgsw for the time being and give them passwordless sudo access.
    We have been working to remove all direct references to this user in the autotest scripts, but there are still
    a couple of trailing references to it in the codebase.

Autorun-spdk.conf:
    Every machine that runs the autotest scripts should include a file titled autorun-spdk.conf in the home directory
    of the user that will run them. This file consists of several lines of the form 'variable_name=0/1'. autorun.sh sources
    this file each time it is run, and determines which tests to attempt based on which variables are defined in the
    configuration file. For a full list of the variable declarations available for autorun-spdk.conf, please see
    scripts/autotest_common.sh starting at line 13.

Steps for configuring the VM:
    1. Download a fresh fedora 26 image.
    2. Perform the installation of fedora 26 server.
    3. Create an admin user sys_sgsw (enabling passwordless sudo for this account will make life easier during the tests).
    4. Run the test_vm_setup.sh script which will install all proper dependencies.
    5. At the end of the test_vm_setup.sh script, you will be asked to validate as sys_sgsw to enable rpcbind.
    6. Modify the autorun-spdk.conf file in your home directory to fit your testing needs.
    7. Reboot the VM.
    8. Run autorun.sh for SPDK. Any output files will be placed in ~/spdk_repo/output/.

Additional steps for preparing the Vhost tests:
    The Vhost tests also require the creation of a second virtual machine nested inside of the test VM.
    Please follow the directions below to complete that installation. Note that host refers to the fedora VM
    created above and guest or VM refer to the ubuntu VM created in this section.
    1. Create an image file for the VM. It does not have to be large, about 3.5G should suffice.
    2. Create an ssh keypair for host-guest communications (performed on the host):
        - Generate an ssh keypair with the name spdk_vhost_id_rsa and save it in /root/.ssh.
        - Make sure that only root has read access to the private key.
    3. Install the OS in the VM image (performed on guest):
        - Use the latest Ubuntu Server (Currently 16.04 LTS).
        - When partitioning the disk, make one partion that consumes the whole disk mounted at /. Do not encrypt or enable LVM.
        - Choose the OpenSSH server packages during install.
    4. Post installation configuration (performed on guest):
        - Run the following commands to enable all necessary dependencies:
            i. sudo apt update
            ii. sudo apt upgrade
            iii. sudo apt install fio sg3-utils bc
        - Enable the root user: "sudo passwd root -> root".
        - Enable root login over ssh: vim /etc/ssh/sshd_config -> PermitRootLogin=yes.
        - Disable DNS for ssh: vim /etc/ssh/sshd_config -> UseDNS=no.
        - Add your spdk_vhost key to root's known hosts: /root/.ssh/authorized_keys -> add spdk_vhost_id_rsa.pub key to authorized keys.
        Remember to have the private key in ~/.ssh/spdk_vhost_id_rsa​ file on your host server.
        -Change the grub boot options for the guest as follows:
            i. Add "console=ttyS0 earlyprintk=ttyS0" to the boot options in /etc/default/grub (for serial output redirect).
            ii. Add "scsi_mod.use_blk_mq=1" to boot options in /etc/default/grub​.
            iii. sudo update-grub
        - Reboot the VM.
        - Remove any unnecessary packages (this is to make booting the VM faster):
            i. apt purge snapd
            ii. apt purge ubuntu-core-launcher
            iii. apt purge squashfs-tools
            iv. apt purge unattended-upgrades
    5. Copy the fio binary from the guest location /usr/bin/fio to the host location /home/sys_sgsw/fio_ubuntu.
    6. Place your guest VM in the host at the following location: /home/sys_sgsw/vhost_vm_image.qcow2.
    7. On the host, edit the ~/autorun-spdk.conf file to include the following line: SPDK_TEST_VHOST=1.
