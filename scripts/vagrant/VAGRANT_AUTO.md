# Vagrant prerequisites
## Vagrant scripts install on different systems
    There're documens about vagrant setup enviroments:
        https://www.virtualbox.org/wiki/Linux_Downloads
        https://spdk.io/doc/vagrant.html
        spdk/scripts/vagrant/README.md
    We tried these on Fedora and Ubuntu for auto running.

## Examples on Fedora28/Fedora27/Fedora26
    1. yum check-update
    2. yum update -y
    3. yum install qt*
    4. yum install libsdl*
    5. rpm -ivh VirtualBox-5.2-5.2.16_123759_fedora26-1.x86_64.rpm (select the right version in https://www.virtualbox.org/wiki/Linux_Downloads)
    6. VBoxManage extpack install Oracle_VM_VirtualBox_Extension_Pack-5.2.16.vbox-extpack(install the same pack as your installed version of VirtualBox)
    7. rpm -ivh vagrant_2.1.2_x86_64.rpm

## EXamples on Fedora29/Fedora30
    1. dnf clean all
    2. dnf update
    3. yum check-update
    4. yum update -y
    5. yum install libsdl*
    6. yum install libcrypto.so*
    7. yum install libvpx.so*
    8. rpm -ivh VirtualBox-6.0-6.0.8_130520_fedora29-1.x86_64.rpm
    9. VBoxManage extpack install Oracle_VM_VirtualBox_Extension_Pack-6.0.8.vbox-extpack
    10. rpm -ivh vagrant_2.2.4_x86_64.rpm

    Above: User need install virtualbox, virutalbox-extension-pack,vagrant.
    ***there's another provider about libvirt, for vagrant will no longer supports libvirt well.
        export SPDK_VAGRANT_PROVIDER=libvirt;
        yum install libvirt*
        vagrant plugin install vagrant-libvirt

## Examples on Ubuntu18/Ubuntu16
    1. apt-get update
    2. apt-get upgrade
    3. apt-get install -y
    4. dpkg -i virtualbox-6.0_6.0.8-130520_Ubuntu_bionic_amd64.deb
    5. VBoxManage extpack install Oracle_VM_VirtualBox_Extension_Pack-6.0.8.vbox-extpack
    6. dpkg -i vagrant_2.2.4_x86_64.deb
    there may be still some installations, please keep installing successfully without conflicts, packages' versions should be matched.

## Other systems
    There are some other packages for other systems list in https://www.virtualbox.org/wiki/Linux_Downloads.

# Vagrant dependencies
## Prxoy setting.
    Our scirpt will download packages from websites (such as https://app.vagrantup.com/boxes, librxe,spdk,libiscsi,ocf...),If these
    blocked you from downloading, you should consider about setting up proxy.

    Proxy is the key setting for some network which was restricted for security .If you don't have such restrictions,please unset it.
        export http_proxy=
        unset http_proxy

    Code will export http_proxy=,https_proxy=$http_proxy,SPDK_VAGRANT_HTTP_PROXY=$http_proxy.

# Vagrant auto destinations
## Vagrant_auto.sh aims to integrate spdk automation for users.
    1. Virtualbox provides mthods to manage Hardisk,NVMe,Virtual NAT in a VM.
    2. There're some basic boxes which are tiny systems for ubuntu,fedora,centos,freebsd.
    3. Vagrant can manage these VMs, to vagrant up/vagrant halt/vagrant ssh/vagrant destroy.
    4. For example: If use this script to create a VM like fedora28, virtulbox creates a VM with HW as following:
        00:00.0 Host bridge: Intel Corporation 440FX - 82441FX PMC [Natoma] (rev 02)
        00:01.0 ISA bridge: Intel Corporation 82371SB PIIX3 ISA [Natoma/Triton II]
        00:01.1 IDE interface: Intel Corporation 82371AB/EB/MB PIIX4 IDE (rev 01)
        00:02.0 VGA compatible controller: InnoTek Systemberatung GmbH VirtualBox Graphics Adapter
        00:03.0 Ethernet controller: Intel Corporation 82540EM Gigabit Ethernet Controller (rev 02)
        00:04.0 System peripheral: InnoTek Systemberatung GmbH VirtualBox Guest Service
        00:05.0 Multimedia audio controller: Intel Corporation 82801AA AC'97 Audio Controller (rev 01)
        00:07.0 Bridge: Intel Corporation 82371AB/EB/MB PIIX4 ACPI (rev 08)
        00:08.0 Ethernet controller: Intel Corporation 82540EM Gigabit Ethernet Controller (rev 02)
        00:0d.0 SATA controller: Intel Corporation 82801HM/HEM (ICH8M/ICH8M-E) SATA Controller [AHCI mode] (rev 02)
        00:0e.0 Non-Volatile memory controller: InnoTek Systemberatung GmbH Device 4e56
    5. There're several key settings about CPUs and RAMs, which are decied by our physical machines that are running this script.
        you should make sure :
        a. How Many cores do you have?  a computer /notebook ,consider about the HW limitation, maybe exprort NCPU=2/4.
        b. What's the memory size of your physical machine? generally 8G/8192, at least 4096 is needed, export MEMORY_SIZE=4096/8192/..
           Of cource the bigger the better.
    In all:
        export NCPU=2
        exort VM_OS=fedora28/ubuntu18
        export MEMORY_SIZE=4096
        Please use "./vagrant_auto.sh -h" to look up details.
    or
        if you want to keep this setting for person preferences:
        you can also change setting in script:
        VM_OS=${VM_OS:-fedora28}--->{VM_OS:-ubuntu18}
        MEMORY_SIZE=${MEMORY_SIZE:-16384}--->${MEMORY_SIZE:-4096}
        NCPU=${NCPU:-16}--->${NCPU:-4}
        PROVIDER=${PROVIDER:-virtualbox}--->${PROVIDER:-libvirt}
        http_proxy=${http_proxy:-http://proxy-prc.intel.com:911}--->http_proxy=${http_proxy:-}
## Vagrant steps about auto test for SPDK.
    Once a VM has been created successfully. VM will install the environments that needed by SPDK.
    1. Download SPDK
    2. Update systems
    3. Install tools on this VM. Including librxe/softroce,libiscsi,qat,ocf,rocksdb... These tools are optional.
    4. Compile SPDK, and run demo cases.
    5. Choose to edit autorun-spdk.conf and run whole tests including nvmf/iscsi.
## Vagrant login VM
    User can use "./vagrant_auto.sh -s" to login VM that created by ourself, if there exists other VM, please make sure the variables
    for the VM.
## Vagrant check
    Use "./vagrant_auto.sh -c" to check registered VM .
