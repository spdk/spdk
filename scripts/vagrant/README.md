# SPDK Vagrant and VirtualBox

The following guide explains how to use the scripts in the `spdk/scripts/vagrant`.

## Git

Install and configure Git on your platform. Both Mac and Windows platforms are supported

### Mac OS X

OSX platforms already have git installed, however, installing the [Apple xCode](https://developer.apple.com/xcode/) developer kit and [xCode Command Line tools](https://developer.apple.com/xcode/features/) will provide UNIX command line tools such as make, awk, sed, ssh, tar, and zip.

### Windows

Windows platforms should install [Git](https://git-scm.com/download/win) from git-scm.com.  This provides everything needed to use git on Windows, including a `git-bash` command line environment.

## VirtualBox

Install VirtualBox and its dependencies

1. Install [VirtualBox 5.1](https://www.virtualbox.org/wiki/Downloads) or newer
2. Install [VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads)

### Mac OSX

Quick start instructions with OSX:

1. Install Homebrew

```
  /usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
```

2. Once installed:

```
   brew doctor
   brew update
```

3. Install Virtual Box Cask

```
   brew cask install virtualbox
```

4. Install Virtual Box Extentions

```
   brew cask install virtualbox-extension-pack
```

5. Install Vagrant Cask

```
   brew cask install vagrant
```

### Windows

1. Install [VirtualBox 5.1](https://www.virtualbox.org/wiki/Downloads) or newer
2. Install [VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads)

Note: VirtualBox requires virtualization to be enabled in the BIOS.
Note: You should disable Hyper-V in Windows RS 3 laptop. Search `windows features` uncheck Hyper-V, restart laptop

## Vagrant

Install and configure Vagrant

### Mac OSX

Install Vagrant Cask

```
   brew cask install vagrant
```

### Windows

Install and configure [Vagrant 1.9.4](https://www.vagrantup.com) or newer

## Configure Vagrant

If you are behind a corporate firewall, configure the following proxy settings.

1. Set the http_proxy and https_proxy
```
  $ export http_proxy=....
```
2. Install the proxyconf plugin
```
  $ vagrant plugin install vagrant-proxyconf
```

## Download SPDK from GitHub

Use git to clone a new spdk repository. GerritHub can also be used. See the instructions at [spdk.io](http://www.spdk.io/development/#gerrithub) to setup your GerritHub account. Note that the spdk repository will be `rsync'd` into your VM, so you can use this repository to continue development within the VM.

```
  $ mkdir github
  $ cd github
  $ git clone https://github.com/spdk/spdk
  $ cd spdk
  $ git submodule update --init
  $ cd ..
```

## Build and launch a VM

Use the `spdk/scripts/vagrant/create_vbox.sh` script to create a VM of your choice.  Supported VM platforms are:

- centos7
- ubuntu16
- ubuntu18
- fedora26
- fedora27
- freebsd11

```
 Usage: spdk/scripts/vagrant/create_vbox.sh [-d <distro>] | [-n <num-cpu>] | [-s <ram-size>] | [-h] | [-v] | [-r]
  -d | --distro <centos7 ubuntu16 ubuntu18 fedora26 fedora27 freebsd11>
  -s | --vram-size <bytes>                                     default: 4096
  -n | --vmcpu-num <int>                                       default: 4
  -v | --verbose
  -r | --dry-run
  -h | --help
```

Note: is it recommended that you do not call the `create_vbox.sh` script from within the spdk repository. This script will:

1. create a subdirectory named `--distro` in your $PWD
2. copy the needed files from `spdk/scripts/vagrant/` into the `--distro` directory
3. create a working VM in the `--distro` directory

This arrangement allows the provisioning of multiple, different VMs within that same directory hiearchy with a single spdk repository.  Following the creation of your VM:

```
  $ cd <distro>
  $ vagrant ssh
```

## Finish VM Initializtion

After using `vagrant ssh` to enter your VM you must complete the initialization of the VM with the following steps. This only needs to be done once.  Note: that a copy of your original SPDK repository will exist in `spdk_repo` directory of the `/home/vagrant` user account.  To complete initialization of the VM complete the following steps.

### ubuntu16, ubuntu18, centos7, fedora26, fedora27

1. Run the pdkdep.sh script (must be root)

```
  vagrant@localhost:~$ sudo spdk_repo/spdk/scripts/pkgdep.sh
```

2. Verify you have an emulated NVMe device

```
  vagrant@localhost:~$ lspci | grep "Non-Volatile"
  00:0e.0 Non-Volatile memory controller: InnoTek Systemberatung GmbH Device 4e56
```

3. Build SPDK

```
  vagrant@localhost:/~$ cd spdk_repo/spdk
  vagrant@localhost:/spdk$ git submodule update --init
  vagrant@localhost:/spdk$ ./configure --enable-debug --enable-werror
  vagrant@localhost:/spdk$ make
```

4. Run the hello_world example to validate the environment is set up correctly.

```
  vagrant@localhost:/spdk$ sudo scripts/setup.sh
  vagrant@localhost:/spdk$ sudo examples/bdev/hello_world/hello_bdev
```
### freebsd11

1. Run the pdkdep.sh script (must be root)

```
   [vagrant@bazinga ~]$ sudo spdk_repo/spdk/scripts/pkgdep.sh
```

2. Install the FreeBSD source in /usr/src

```
   [vagrant@bazinga ~]$ sudo git clone https://github.com/freebsd/freebsd.git -b release/11.1.0 /usr/src
```

3. Build SPDK

```
  [vagrant@bazinga ~]$ cd spdk_repo/spdk
  [vagrant@bazinga ~/spdk_repo/spdk]$ git submodule update --init
  [vagrant@bazinga ~/spdk_repo/spdk]$ ./configure --enable-debug --enable-werror
  [vagrant@bazinga ~/spdk_repo/spdk]$ gmake MAKE=gmake
```
