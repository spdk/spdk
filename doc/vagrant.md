# Vagrant Development Environment {#vagrant}

# Introduction {#vagrant_intro}

[Vagrant](https://www.vagrantup.com/) provides a quick way to get a basic
NVMe enabled virtual machine sandbox running without the need for any
special hardware.
The Vagrant environment for SPDK has support for a variety of Linux distributions as well as FreeBSD.
Run scripts/vagrant/create_vbox.sh -h to see the complete list.
This environment requires Vagrant 1.9.4 or newer and
VirtualBox 5.1 or newer with the matching VirtualBox extension pack.

Note: If you are behind a corporate firewall, set `http_proxy` and `https_proxy` in
your environment before trying to start up the VM.  Also make sure that you
have installed the optional vagrant module `vagrant-proxyconf`:

~~~{.sh}
export http_proxy=...
export https_proxy=...
vagrant plugin install vagrant-proxyconf
~~~

In case you want use kvm/libvirt you should also install `vagrant-libvirt`

# VM Configuration {#vagrant_config}

To create a configured VM with vagrant you need to run `create_vbox.sh` script.

Basically, the script will create a new sub-directory based on distribution you choose,
copy the vagrant configuration file (a.k.a. `Vagrantfile`) to it,
and run `vagrant up` with some settings defined by the script arguments.

By default, the VM created is configured with:

- 2 vCPUs
- 4G of RAM
- 2 NICs (1 x NAT - host access, 1 x private network)

In order to modify some advanced settings like provisioning and rsyncing,
you may want to change Vagrantfile source.

For additional support, use the Vagrant help function to learn how to destroy, restart, etc.
Further below is sample output from a successful VM launch and execution of the NVMe hello
world example application.

~~~{.sh}
    vagrant --help
~~~

# Running An Example {#vagrant_example}

The following shows sample output from starting up a Ubuntu18 VM,
compiling SPDK on it and running the NVMe sample application `hello_world`.
If you don't see the NVMe device as seen below in both the `lspci` output as well as the
application output, you likely have a VirtualBox and/or Vagrant
versioning issue.

~~~{.sh}
user@dev-system:~$ cd spdk/scripts/vagrant
user@dev-system:~/spdk/scripts/vagrant$ ./create_vbox.sh ubuntu18
mkdir: created directory '/home/user/spdk/scripts/vagrant/ubuntu18'
~/spdk/scripts/vagrant/ubuntu18 ~/spdk/scripts/vagrant
vagrant-proxyconf already installed... skipping
Bringing machine 'default' up with 'virtualbox' provider...
==> default: Box 'bento/ubuntu-18.04' could not be found. Attempting to find and install...
    default: Box Provider: virtualbox
    default: Box Version: 201803.24.0
==> default: Loading metadata for box 'bento/ubuntu-18.04'
    default: URL: https://vagrantcloud.com/bento/ubuntu-18.04
==> default: Adding box 'bento/ubuntu-18.04' (v201803.24.0) for provider: virtualbox
    default: Downloading: https://vagrantcloud.com/bento/boxes/ubuntu-18.04/versions/201803.24.0/providers/virtualbox.box
==> default: Box download is resuming from prior download progress
==> default: Successfully added box 'bento/ubuntu-18.04' (v201803.24.0) for 'virtualbox'!
==> default: Importing base box 'bento/ubuntu-18.04'...
==> default: Matching MAC address for NAT networking...
==> default: Setting the name of the VM: ubuntu18_default_1237088131451_82174
==> default: Fixed port collision for 22 => 2222. Now on port 2202.
==> default: Clearing any previously set network interfaces...
==> default: Preparing network interfaces based on configuration...
    default: Adapter 1: nat
    default: Adapter 2: hostonly
==> default: Forwarding ports...
    default: 22 (guest) => 2202 (host) (adapter 1)
==> default: Running 'pre-boot' VM customizations...
==> default: Booting VM...
==> default: Waiting for machine to boot. This may take a few minutes...
    default: SSH address: 127.0.0.1:2202
    default: SSH username: vagrant
    default: SSH auth method: private key
    default: Warning: Remote connection disconnect. Retrying...
    default: Warning: Connection reset. Retrying...
<<some output trimmed>>
    default: Warning: Connection reset. Retrying...
    default: Warning: Remote connection disconnect. Retrying...
    default:
    default: Vagrant insecure key detected. Vagrant will automatically replace
    default: this with a newly generated keypair for better security.
    default:
    default: Inserting generated public key within guest...
    default: Removing insecure key from the guest if it's present...
    default: Key inserted! Disconnecting and reconnecting using new SSH key...
==> default: Machine booted and ready!
==> default: Checking for guest additions in VM...
==> default: Configuring and enabling network interfaces...
==> default: Configuring proxy for Apt...
==> default: Configuring proxy environment variables...
==> default: Rsyncing folder: /home/user/spdk/ => /home/vagrant/spdk_repo/spdk
==> default: Mounting shared folders...
    default: /vagrant => /home/user/spdk/scripts/vagrant/ubuntu18
==> default: Running provisioner: file...

  SUCCESS!

  cd to ubuntu18 and type "vagrant ssh" to use.
  Use vagrant "suspend" and vagrant "resume" to stop and start.
  Use vagrant "destroy" followed by "rm -rf ubuntu18" to destroy all trace of vm.
~~~

Check the enviroment.

~~~{.sh}
user@dev-system:~/spdk/scripts/vagrant$ cd ubuntu18
user@dev-system:~/spdk/scripts/vagrant/ubuntu18$ vagrant ssh
Welcome to Ubuntu Bionic Beaver (development branch) (GNU/Linux 4.15.0-12-generic x86_64)
<<some output trimmed>>
vagrant@vagrant:~$ lspci | grep "Non-Volatile"
00:0e.0 Non-Volatile memory controller: InnoTek Systemberatung GmbH Device 4e56
vagrant@vagrant:~$ ls
spdk_repo
~~~

Compiling SPDK and running an example.

~~~{.sh}
vagrant@vagrant:~/spdk_repo/spdk$ sudo apt update
<<output trimmed>>
vagrant@vagrant:~/spdk_repo/spdk$ sudo scripts/pkgdep.sh
<<output trimmed>>

vagrant@vagrant:~/spdk_repo/spdk$ ./configure
Creating mk/config.mk...done.
Type 'make' to build.

vagrant@vagrant:~/spdk_repo/spdk$ make
<<output trimmed>>

vagrant@vagrant:~/spdk_repo/spdk$ sudo ./scripts/setup.sh
0000:00:0e.0 (80ee 4e56): nvme -> uio_pci_generic

vagrant@vagrant:~/spdk_repo/spdk$ sudo build/examples/hello_world
Starting SPDK v18.10-pre / DPDK 18.05.0 initialization...
[ DPDK EAL parameters: hello_world -c 0x1 --legacy-mem --file-prefix=spdk0 --base-virtaddr=0x200000000000 --proc-type=auto ]
EAL: Detected 4 lcore(s)
EAL: Detected 1 NUMA nodes
EAL: Auto-detected process type: PRIMARY
EAL: Multi-process socket /var/run/dpdk/spdk0/mp_socket
EAL: Probing VFIO support...
Initializing NVMe Controllers
EAL: PCI device 0000:00:0e.0 on NUMA socket 0
EAL:   probe driver: 80ee:4e56 spdk_nvme
Attaching to 0000:00:0e.0
Attached to 0000:00:0e.0
Using controller ORCL-VBOX-NVME-VER12 (VB1234-56789        ) with 1 namespaces.
  Namespace ID: 1 size: 1GB
Initialization complete.
INFO: using host memory buffer for IO
Hello world!
~~~
