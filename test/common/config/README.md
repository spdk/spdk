# Virtual Test Configuration

This readme and the associated bash script, vm_setup.sh, are intended to assist developers in quickly
preparing a virtual test environment on which to run the SPDK validation tests rooted at autorun.sh.
This file contains basic information about SPDK environment requirements, an introduction to the
autorun-spdk.conf files used to moderate which tests are run by autorun.sh, and step-by-step instructions
for spinning up a VM capable of running the SPDK test suite.
There is no need for external hardware to run these tests. The linux kernel comes with the drivers necessary
to emulate an RDMA enabled NIC. NVMe controllers can also be virtualized in emulators such as QEMU.

## VM Environment Requirements (Host)

- 8 GiB of RAM (for DPDK)
- Enable intel_kvm on the host machine from the bios.
- Enable nesting for VMs in kernel command line (for vhost tests).
  - In `/etc/default/grub` append the following to the GRUB_CMDLINE_LINUX line: intel_iommu=on kvm-intel.nested=1.

## VM Specs

When creating the user during the fedora installation, it is best to use the name sys_sgci. Efforts are being made
to remove all references to this user, or files specific to this user from the codebase, but there are still some
trailing references to it.

## Autorun-spdk.conf

Every machine that runs the autotest scripts should include a file titled autorun-spdk.conf in the home directory
of the user that will run them. This file consists of several lines of the form 'variable_name=0/1'. autorun.sh sources
this file each time it is run, and determines which tests to attempt based on which variables are defined in the
configuration file. For a full list of the variable declarations available for autorun-spdk.conf, please see
`test/common/autotest_common.sh` starting at line 13.

## Steps for Configuring the VM

1. Download a fresh Fedora 33 image.
2. Perform the installation of Fedora 33 server.
3. Create an admin user sys_sgci (enabling passwordless sudo for this account will make life easier during the tests).
4. Run the vm_setup.sh script which will install all proper dependencies.
5. Modify the autorun-spdk.conf file in the home directory.
6. Reboot the VM.
7. Run autorun.sh for SPDK. Any output files will be placed in `~/spdk_repo/output/`.

## Additional Steps for Preparing the Vhost Tests

The Vhost tests require the creation of a virtual guest machine to be run in the host system.
Please follow the directions below to complete that installation. Note that host refers to the Fedora VM
created above and guest or VM refer to the Fedora VM created in this section, which are meant to be used in Vhost tests.

To create the VM image manually use following steps:

1. Create an image file for the VM. It does not have to be large, about 3.5G should suffice.
2. Create an ssh keypair for host-guest communications (performed on the host):
  - Generate an ssh keypair with the name spdk_vhost_id_rsa and save it in `/root/.ssh`.
  - Make sure that only root has read access to the private key.
3. Install the OS in the VM image (performed on guest):
  - Use the latest Fedora Cloud (Currently Fedora 32).
  - When partitioning the disk, make one partition that consumes the whole disk mounted at /. Do not encrypt the disk or enable LVM.
  - Choose the OpenSSH server packages during install.
4. Post installation configuration (performed on guest):
  - Run the following commands to enable all necessary dependencies:
    ~~~{.sh}
    sudo dnf update
    sudo dnf upgrade
    sudo dnf -y install git sg3_utils bc wget libubsan libasan xfsprogs btrfs-progs ntfsprogs ntfs-3g
    git clone https://github.com/spdk/spdk.git
    ./spdk/scripts/pkgdep.sh -p -f -r -u
    ~~~
  - Enable the root user: "sudo passwd root -> root".
  - Enable root login over ssh: vim `/etc/ssh/sshd_config` -> PermitRootLogin=yes.
  - Change the grub boot options for the guest as follows:
        - Add "console=ttyS0 earlyprintk=ttyS0" to the boot options in `/etc/default/grub` (for serial output redirect).
        - Add "scsi_mod.use_blk_mq=1" to boot options in `/etc/default/grub`.
          ~~~{.sh}
          sudo grub2-mkconfig -o /boot/grub2/grub.cfg
          ~~~
  - Reboot the VM.
  - Remove any unnecessary packages (this is to make booting the VM faster):
    ~~~{.sh}
    sudo dnf clean all
    ~~~
5. Install fio:
   ~~~
   ./spdk/test/common/config/vm_setup.sh -t 'fio'
   ~~~
6. Place the guest VM in the host at the following location: `$DEPENDENCY_DIR/spdk_test_image.qcow2`.
7. On the host, edit the `~/autorun-spdk.conf` file to include the following line: SPDK_TEST_VHOST=1.
