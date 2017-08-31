# Vagrant Development Environment {#vagrant}

# Introduction {#vagrant_intro}

[Vagrant](https://www.vagrantup.com/) provides a quick way to get a basic
NVMe enabled virtual machine sandbox running without the need for any
special hardware.
The Vagrant environment for SPDK has support for Ubuntu 16.04 and
CentOS 7.2. This environment requires Vagrant 1.9.4 or newer and
VirtualBox 5.1 or newer with the matching VirtualBox extension pack.

The VM builds SPDK and DPDK from source which are located at `/spdk`.

Note: If you are behind a corporate firewall, set `http_proxy` and `https_proxy` in
your environment before trying to start up the VM.  Also make sure that you
have installed the optional vagrant module 'vagrant-proxyconf'.

# VM Configuration {#vagrant_config}

This vagrant environment creates a VM based on environment variables found in `env.sh`.
To use, edit `env.sh`, then:

~~~{.sh}
    cd scripts/vagrant
    source ./env.sh
    vagrant up
~~~

At this point you can use `vagrant ssh` to ssh into the VM. The `/spdk` directory is
sync'd from the host system and the build is automatically done.  Other notable files:

- `build.sh`: executed on the VM automatically when provisioned
- `Vagrantfile`: startup parameters/commands for the VM

The few commands we mention here are enough to get you up and running; for additional
support, use the Vagrant help function to learn how to destroy, restart, etc.  Further
below is sample output from a successful VM launch and execution of the NVMe hello
world example application.

~~~{.sh}
    vagrant --help
~~~

By default, the VM created is configured with:
- Ubuntu 16.04
- 2 vCPUs
- 4G of RAM
- 2 NICs (1 x NAT - host access, 1 x private network)

# Providers {#vagrant_providers}

Currently only the VirtualBox provider is supported.

# Running An Example {#vagrant_example}

The following shows sample output from starting up a VM and running
the NVMe sample application `hello_world`. If you don't see the
NVMe device as seen below in both the `lspci` output as well as the
application output, you likely have a VirtualBox and/or Vagrant
versioning issue.

~~~{.sh}
user@dev-system:~$ cd spdk
user@dev-system:~/spdk$ cd scripts/
user@dev-system:~/spdk/scripts$ cd vagrant/
user@dev-system:~/spdk/scripts/vagrant$ vagrant up
Bringing machine 'default' up with 'virtualbox' provider...
==> default: Clearing any previously set forwarded ports...
==> default: Clearing any previously set network interfaces...
==> default: Preparing network interfaces based on configuration...
    default: Adapter 1: nat
    default: Adapter 2: hostonly
==> default: Forwarding ports...
    default: 22 (guest) => 2222 (host) (adapter 1)
==> default: Running 'pre-boot' VM customizations...
==> default: Booting VM...
==> default: Waiting for machine to boot. This may take a few minutes...
    default: SSH address: 127.0.0.1:2222
    default: SSH username: vagrant
    default: SSH auth method: private key
    default:
    default: Vagrant insecure key detected. Vagrant will automatically replace
    default: this with a newly generated keypair for better security.
    default:
    default: Inserting generated public key within guest...
    default: Removing insecure key from the guest if it's present...
    default: Key inserted! Disconnecting and reconnecting using new SSH key...
==> default: Machine booted and ready!
==> default: Checking for guest additions in VM...
    default: Guest Additions Version: 5.1
    default: VirtualBox Version: 5.1
==> default: Configuring and enabling network interfaces...
==> default: Rsyncing folder: /home/peluse/spdk/ => /spdk
==> default: Mounting shared folders...
    default: /vagrant => /home/peluse/spdk/scripts/vagrant
==> default: Running provisioner: shell...
    default: Running: /tmp/vagrant-shell20170524-2405-3cam94.sh
==> default: vm.nr_hugepages = 1024
==> default: Hit:1 http://us.archive.ubuntu.com/ubuntu xenial InRelease
<< some output trimmed >>
==> default: Fetched 2,329 kB in 3s (588 kB/s)
==> default: Reading package lists...
==> default: Building dependency tree...
==> default: Reading state information...
==> default: Calculating upgrade...
==> default: The following packages have been kept back:
==> default:   linux-generic linux-headers-generic linux-image-generic
==> default: The following packages will be upgraded:
==> default:   accountsservice apparmor apt apt-transport-https apt-utils base-files bash
<< some output trimmed >>
==> default: 167 upgraded, 0 newly installed, 0 to remove and 3 not upgraded.
==> default: Need to get 124 MB of archives.
==> default: After this operation, 39.5 MB of additional disk space will be used.
==> default: Get:1 http://us.archive.ubuntu.com/ubuntu xenial-updates/main amd64 base-files amd64 9.4ubuntu4.4 [60.                 2 kB]
<< some output trimmed >>
==> default: Preconfiguring packages ...
==> default: Fetched 11.8 MB in 17s (669 kB/s)
==> default: Setting up libc6:amd64 (2.23-0ubuntu7) ...
==> default: Processing triggers for libc-bin (2.23-0ubuntu3) ...
<< some output trimmed >>
==> default: Running provisioner: shell...
    default: Running: /tmp/vagrant-shell20170524-2405-1wt8p3c.sh
==> default: 0:/tmp/vagrant-shell
==> default: SUDOCMD: sudo -H -u vagrant
==> default: KERNEL_OS: GNU/Linux
==> default: KERNEL_MACHINE: x86_64
==> default: KERNEL_RELEASE: 4.4.0-21-generic
==> default: KERNEL_VERSION: #37-Ubuntu SMP Mon Apr 18 18:33:37 UTC 2016
==> default: DISTRIB_ID: Ubuntu
==> default: DISTRIB_RELEASE: 16.04
==> default: DISTRIB_CODENAME: xenial
==> default: DISTRIB_DESCRIPTION: Ubuntu 16.04 LTS
==> default: Reading package lists...
==> default: Building dependency tree...
<< some output trimmed >>
==> default: Processing triggers for libc-bin (2.23-0ubuntu3) ...
==> default: Creating CONFIG.local...
==> default: done.
==> default: Type 'make' to build.
==> default: Configuration done
==> default: make[3]: Entering directory '/spdk/dpdk'
==> default: == Build lib
==> default: == Build lib/librte_compat
==> default: == Build lib/librte_eal
==> default: == Build lib/librte_eal/common
==> default:   SYMLINK-FILE include/rte_compat.h
<< some output trimmed >>
==> default: Build complete [x86_64-native-linuxapp-gcc]
==> default: make[3]: Leaving directory '/spdk/dpdk'
==> default:   CC lib/blob/blobstore.o
==> default:   CC lib/bdev/bdev.o
<< some output trimmed >>
==> default:   LINK test/lib/nvme/e2edp/nvme_dp
==> default: Running provisioner: shell...
    default: Running: inline script
==> default: 0000:00:0e.0 (80ee 4e56): nvme -> uio_pci_generic

user@dev-system:~/spdk/scripts/vagrant$ vagrant ssh
Welcome to Ubuntu 16.04 LTS (GNU/Linux 4.4.0-21-generic x86_64)

 * Documentation:  https://help.ubuntu.com/
vagrant@localhost:~$ lspci | grep "Non-Volatile"
00:0e.0 Non-Volatile memory controller: InnoTek Systemberatung GmbH Device 4e56

vagrant@localhost:~$ sudo /spdk/examples/nvme/hello_world/hello_world
Starting DPDK 17.02.0 initialization...
[ DPDK EAL parameters: hello_world -c 0x1 --file-prefix=spdk_pid17681 ]
EAL: Detected 2 lcore(s)
EAL: Probing VFIO support...
Initializing NVMe Controllers
EAL: PCI device 0000:00:0e.0 on NUMA socket 0
EAL:   probe driver: 80ee:4e56 spdk_nvme
Attaching to 0000:00:0e.0
Attached to 0000:00:0e.0
Using controller ORCL-VBOX-NVME-VER12 (VB1234-56789        ) with 1 namespaces.
  Namespace ID: 1 size: 1GB
Initialization complete.
Hello world!
vagrant@localhost:~$
~~~
