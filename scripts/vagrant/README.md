# SPDK Vagrant and VirtualBox

The following guide explains how to use the scripts in the `spdk/scripts/vagrant`. Mac, Windows, and Linux platforms are supported.

1. Install and configure [Git](https://git-scm.com/) on your platform.
2. Install [VirtualBox 5.1](https://www.virtualbox.org/wiki/Downloads) or newer
3. Install [VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads)
4. Install and configure [Vagrant 1.9.4](https://www.vagrantup.com) or newer

## Mac OSX Setup (High Sierra)

OSX platforms already have Git installed, however, installing the [Apple xCode](https://developer.apple.com/xcode/) developer kit and [xCode Command Line tools](https://developer.apple.com/xcode/features/) will provide UNIX command line tools such as make, awk, sed, ssh, tar, and zip. xCode can be installed through the App Store on you Mac.

Quick start instructions for OSX:

1. Install Homebrew
2. Install Virtual Box Cask
3. Install Virtual Box Extentions
4. Install Vagrant Cask

```
   /usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
   brew doctor
   brew update
   brew cask install virtualbox
   brew cask install virtualbox-extension-pack
   brew cask install vagrant
```

## Windows 10 Setup

1. Windows platforms should install [Git](https://git-scm.com/download/win) from git-scm.com.
   - This provides everything needed to use git on Windows, including a `git-bash` command line environment.
2. Install [VirtualBox 5.1](https://www.virtualbox.org/wiki/Downloads) or newer
3. Install [VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads)
4. Install and configure [Vagrant 1.9.4](https://www.vagrantup.com) or newer

- Note: VirtualBox requires virtualization to be enabled in the BIOS.
- Note: You should disable Hyper-V in Windows RS 3 laptop. Search `windows features` uncheck Hyper-V, restart laptop

## Linux Setup

Following the generic instructions should be sufficient for most Linux distributions. For more thorough instructions on installing VirtualBox on your distribution of choice, please see the following [guide](https://www.virtualbox.org/wiki/Linux_Downloads).

## Configure Vagrant

If you are behind a corporate firewall, configure the following proxy settings.

1. Set the http_proxy and https_proxy
2. Install the proxyconf plugin

```
  $ export http_proxy=....
  $ export https_proxy=....
  $ vagrant plugin install vagrant-proxyconf
```

## Download SPDK from GitHub

Use git to clone a new spdk repository. GerritHub can also be used. See the instructions at [spdk.io](http://www.spdk.io/development/#gerrithub) to setup your GerritHub account. Note that this spdk repository will be rsync'd into your VM, so you can use this repository to continue development within the VM.

## Create a Virtual Box

Use the `spdk/scripts/vagrant/create_vbox.sh` script to create a VM of your choice.  Supported VM platforms are:

- centos7
- ubuntu16
- ubuntu18
- fedora26
- fedora27
- fedora28
- freebsd11

```
$ spdk/scripts/vagrant/create_vbox.sh -h
 Usage: create_vbox.sh [-n <num-cpus>] [-s <ram-size>] [-x <http-proxy>] [-hvrld] <distro>

  distro = <centos7 | ubuntu16 | ubuntu18 | fedora26 | fedora27 | fedora28 | freebsd11>

  -s <ram-size> in kb       default: 4096
  -n <num-cpus> 1 to 4      default: 4
  -x <http-proxy>           default: ""
  -p <provider>             libvirt or virtualbox
  --vhost-host-dir=<path>   directory path with vhost test dependencies
                            (test VM qcow image, fio binary, ssh keys)
  --vhost-vm-dir=<path>     directory where to put vhost dependencies in VM
  -r dry-run
  -l use a local copy of spdk, don't try to rsync from the host.
  -d deploy a test vm by provisioning all prerequisites for spdk autotest
  -h help
  -v verbose

 Examples:

  ./scripts/vagrant/create_vbox.sh -x http://user:password@host:port fedora27
  ./scripts/vagrant/create_vbox.sh -s 2048 -n 2 ubuntu16
  ./scripts/vagrant/create_vbox.sh -rv freebsd
  ./scripts/vagrant/create_vbox.sh fedora26
```

It is recommended that you call the `create_vbox.sh` script from outside of the spdk repository. Call this script from a parent directory. This will allow the creation of multiple VMs in separate <distro> directories, all using the same spdk repository.  For example:

```
   $ spdk/scripts/vagrant/create_vbox.sh -s 2048 -n 2 fedora26
```

This script will:

1. create a subdirectory named <distro> in your $PWD
2. copy the needed files from `spdk/scripts/vagrant/` into the <distro> directory
3. create a working virtual box in the <distro> directory
4. rsync the `~/.gitconfig` file to `/home/vagrant/` in the newly provisioned virtual box
5. rsync a copy of the source `spdk` repository to `/home/vagrant/spdk_repo/spdk` (optional)
6. rsync a copy of the `~/vagrant_tools` directory to `/home/vagrant/tools` (optional)
7. execute vm_setup.sh on the guest to install all spdk dependencies (optional)

This arrangement allows the provisioning of multiple, different VMs within that same directory hierarchy using the same spdk repository. Following the creation of the vm you'll need to ssh into your virtual box and finish the VM initializaton.

```
  $ cd <distro>
  $ vagrant ssh
```

## Finish VM Initializtion

A copy of the `spdk` repository you cloned will exist in the `spdk_repo` directory of the `/home/vagrant` user account. After using `vagrant ssh` to enter your VM you must complete the initialization of your VM by running the `scripts/vagrant/update.sh` script. For example:

```
   $ script -c 'sudo spdk_repo/spdk/scripts/vagrant/update.sh' update.log
```

The `update.sh` script completes initialization of the VM by automating the following steps.

1. Runs yum/apt-get update (Linux)
2. Runs the scripts/pdkdep.sh script
3. Installs the FreeBSD source in /usr/sys (FreeBSD only)

This only needs to be done once. This is also not necessary for Fedora VMs provisioned with the -d flag. The `vm_setup` script performs these operations instead.

## Post VM Initializtion

Following VM initializtion you must:

1. Verify you have an emulated NVMe device
2. Compile your spdk source tree
3. Run the hello_world example to validate the environment is set up correctly

### Verify you have an emulated NVMe device

```
  $ lspci | grep "Non-Volatile"
  00:0e.0 Non-Volatile memory controller: InnoTek Systemberatung GmbH Device 4e56
```

### Compile SPDK

```
  $ cd spdk_repo/spdk
  $ git submodule update --init
  $ ./configure --enable-debug
  $ make
```

### Run the hello_world example script

```
  $ sudo scripts/setup.sh
  $ cd examples/bdev/hello_world
  $ sudo ./hello_bdev
```

### Running autorun.sh with vagrant

After running vm_setup.sh the `run-autorun.sh` can be used to run `spdk/autorun.sh` on a Fedora vagrant machine. Note that the `spdk/scripts/vagrant/autorun-spdk.conf` should be copied to `~/autorun-spdk.conf` before starting your tests.

```
   $ cp spdk/scripts/vagrant/autorun-spdk.conf ~/
   $ spdk/scripts/vagrant/run-autorun.sh -h
     Usage: scripts/vagrant/run-autorun.sh -d <path_to_spdk_tree> [-h] | [-q] | [-n]
       -d : Specify a path to an SPDK source tree
       -q : No output to screen
       -n : Noop - dry-run
       -h : This help

     Examples:
         run-spdk-autotest.sh -d . -q
         run-spdk-autotest.sh -d /home/vagrant/spdk_repo/spdk
```

## FreeBSD Appendix

---
**NOTE:** As of this writing the FreeBSD Virtualbox instance does not correctly support the vagrant-proxyconf feature.
---

The following steps are done by the `update.sh` script. It is recommened that you capture the output of `update.sh` with a typescript. E.g.:

```
  $ script update.log sudo spdk_repo/spdk/scripts/vagrant/update.sh
```

1. Updates the pkg catalog
1. Installs the needed FreeBSD packages on the system by calling pkgdep.sh
2. Installs the FreeBSD source in /usr/src

```
   $ sudo pkg upgrade -f
   $ sudo spdk_repo/spdk/scripts/pkgdep.sh
   $ sudo git clone --depth 10 -b releases/11.1.0 https://github.com/freebsd/freebsd.git /usr/src
```

To build spdk on FreeBSD use `gmake MAKE=gmake`.  E.g.:

```
    $ cd spdk_repo/spdk
    $ git submodule update --init
    $ ./configure --enable-debug
    $ gmake MAKE=gmake
```
