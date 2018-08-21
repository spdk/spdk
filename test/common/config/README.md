# Virtual Test Configuration

This readme and the associated bash script, vm_setup.sh, are intended to assist developers in quickly
preparing a virtual test environment on which to run the SPDK validation tests rooted at autorun.sh.
This file contains basic information about SPDK environment requirements, an introduction to the
autorun-spdk.conf files used to moderate which tests are run by autorun.sh, and step-by-step instructions
for spinning up a VM capable of running the SPDK test suite.
There is no need for external hardware to run these tests. The linux kernel comes with the drivers necessary
to emulate an RDMA enabled NIC. NVMe controllers can also be virtualized in emulators such as QEMU.


## VM Envronment Requirements (Host):
- 8 GiB of RAM (for DPDK)
- Enable intel_kvm on the host machine from the bios.
- Enable nesting for VMs in kernel command line (for vhost tests).
  - In `/etc/default/grub` append the following to the GRUB_CMDLINE_LINUX line: intel_iommu=on kvm-intel.nested=1.

## VM Specs
When creating the user during the fedora installation, it is best to use the name sys_sgsw. Efforts are being made
to remove all references to this user, or files specific to this user from the codebase, but there are still some
trailing references to it.

## Autorun-spdk.conf
Every machine that runs the autotest scripts should include a file titled autorun-spdk.conf in the home directory
of the user that will run them. This file consists of several lines of the form 'variable_name=0/1'. autorun.sh sources
this file each time it is run, and determines which tests to attempt based on which variables are defined in the
configuration file. For a full list of the variable declarations available for autorun-spdk.conf, please see
`test/common/autotest_common.sh` starting at line 13.

## Steps for Configuring the VM
1. Download a fresh Fedora 26 image.
2. Perform the installation of Fedora 26 server.
3. Create an admin user sys_sgsw (enabling passwordless sudo for this account will make life easier during the tests).
4. Run the vm_setup.sh script which will install all proper dependencies.
5. Modify the autorun-spdk.conf file in the home directory.
6. Reboot the VM.
7. Run autorun.sh for SPDK. Any output files will be placed in `~/spdk_repo/output/`.

## Additional Steps for Preparing the Vhost Tests
The Vhost tests also require the creation of a second virtual machine nested inside of the test VM.
Please follow the directions below to complete that installation. Note that host refers to the Fedora VM
created above and guest or VM refer to the Ubuntu VM created in this section.

1. Follow instructions from spdk/scripts/vagrant/README.md
	- install all needed packages mentioned in "Mac OSX Setup" or "Windows 10 Setup" sections
	- follow steps from "Configure Vagrant" section

2. Use Vagrant scripts located in spdk/scripts/vagrant to automatically generate
	VM image to use in SPDK vhost tests.
	Example command:
	~~~{.sh}
	spdk/scripts/vagrant/create_vhost_vm.sh --move-to-def-dirs ubuntu16
	~~~
	This command will:
		- Download a Ubuntu 16.04 image file
		- upgrade the system and install needed dependencies (fio, sg3-utils, bc)
		- add entry to VM's ~/.ssh/autorized_keys
		- add appropriate options to GRUB command line and update grub
		- convert the image to .qcow2 format
		- move .qcow2 file and ssh keys to default locations used by vhost test scripts

Alternatively it is possible to create the VM image manually using following steps:
1. Create an image file for the VM. It does not have to be large, about 3.5G should suffice.
2. Create an ssh keypair for host-guest communications (performed on the host):
    - Generate an ssh keypair with the name spdk_vhost_id_rsa and save it in `/root/.ssh`.
    - Make sure that only root has read access to the private key.
3. Install the OS in the VM image (performed on guest):
    - Use the latest Ubuntu server (Currently 16.04 LTS).
    - When partitioning the disk, make one partion that consumes the whole disk mounted at /. Do not encrypt the disk or enable LVM.
    - Choose the OpenSSH server packages during install.
4. Post installation configuration (performed on guest):
    - Run the following commands to enable all necessary dependencies:
      ~~~{.sh}
      sudo apt update
      sudo apt upgrade
      sudo apt install fio sg3-utils bc
      ~~~
    - Enable the root user: "sudo passwd root -> root".
    - Enable root login over ssh: vim `/etc/ssh/sshd_config` -> PermitRootLogin=yes.
    - Disable DNS for ssh: `/etc/ssh/sshd_config` -> UseDNS=no.
    - Add the spdk_vhost key to root's known hosts: `/root/.ssh/authorized_keys` -> add spdk_vhost_id_rsa.pub key to authorized keys.
    Remember to save the private key in `~/.ssh/spdk_vhost_id_rsa` on the host.
    - Change the grub boot options for the guest as follows:
      - Add "console=ttyS0 earlyprintk=ttyS0" to the boot options in `/etc/default/grub` (for serial output redirect).
      - Add "scsi_mod.use_blk_mq=1" to boot options in `/etc/default/grub`.
      ~~~{.sh}
      sudo update-grub
      ~~~
    - Reboot the VM.
    - Remove any unnecessary packages (this is to make booting the VM faster):
      ~~~{.sh}
      apt purge snapd
      apt purge Ubuntu-core-launcher
      apt purge squashfs-tools
      apt purge unattended-upgrades
      ~~~
5. Copy the fio binary from the guest location `/usr/bin/fio` to the host location `/home/sys_sgsw/fio_ubuntu`.
6. Place the guest VM in the host at the following location: `/home/sys_sgsw/vhost_vm_image.qcow2`.
7. On the host, edit the `~/autorun-spdk.conf` file to include the following line: SPDK_TEST_VHOST=1.
