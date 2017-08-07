# vhost {#vhost}

# vhost Getting Started Guide {#vhost_getting_started}

The Storage Performance Development Kit vhost application is named `vhost`.
This application extends SPDK to present virtio storage controllers to QEMU-based
VMs and process I/O submitted to devices attached to those controllers.

# Prerequisites {#vhost_prereqs}

The base SPDK build instructions are located in README.md in the SPDK root directory.
This guide assumes familiarity with building SPDK using the default options.

If you want to kill the application using signal, make sure use the SIGTERM, then the application
will release all the share memory resource before exit, the SIGKILL will make the share memory
resource have no chance to be released by application, you may need to release the resource manually.

## Supported Guest Operating Systems

The guest OS must contain virtio drivers. The SPDK vhost target has been tested
with Ubuntu 16.04, Fedora 25, Windows 2012 R2.

# Building

## SPDK

The vhost target is built by default.
Once built, the binary will be at `app/vhost/vhost`.

## QEMU

Vhost functionality is dependent on QEMU patches to enable virtio-scsi and
virtio-blk in userspace - those patches are currently working their way
through the QEMU mailing list, but temporary patches to enable this
functionality are available in the spdk branch at https://github.com/spdk/qemu.

# Configuration {#vhost_config}

## SPDK

A vhost-specific configuration file is used to configure the SPDK vhost
target.  A fully documented example configuration file is located at
`etc/spdk/vhost.conf.in`.  This file defines the following:

### Storage Backends

Storage backends are devices which will be exposed to the guest OS.
Vhost-blk backends are exposed as block devices in the guest OS, and vhost-scsi backends are
exposed as as SCSI LUNs on devices attached to the vhost-scsi controller in the guest OS.
SPDK supports several different types of storage backends, including NVMe,
Linux AIO, malloc ramdisk and Ceph RBD.  Refer to @ref bdev_getting_started for
additional information on specifying storage backends in the configuration file.

### Mappings Between Block Controllers and Storage Backends

The vhost target exposes block devices to the virtual machines.
The device in the vhost controller is associated with an SPDK block device, and the
configuration file defines those associations.  The block device to Dev mapping
is specified in the configuration file as:

~~~
[VhostBlkX]
  Name vhost.X          # Name of vhost socket
  Dev BackendX          # "BackendX" is block device name from previous
                        # sections in config file
  #Cpumask 0x1          # Optional parameter defining which core controller uses
~~~

### Mappings Between SCSI Controllers and Storage Backends

The vhost target exposes SCSI controllers to the virtual machines.
Each device in the vhost controller is associated with an SPDK block device and
configuration file defines those associations.  The block device to Dev mappings
are specified in the configuration file as:

~~~
[VhostScsiX]
  Name vhost.X          # Name of vhost socket
  Dev 0 BackendX        # "BackendX" is block device name from previous
                        # sections in config file
  Dev 1 BackendY
  ...
  Dev n BackendN
  #Cpumask 0x1          # Optional parameter defining which core controller uses
~~~

### Vhost Sockets

Userspace vhost uses UNIX domain sockets for communication between QEMU
and the vhost target.  Each vhost controller is associated with a UNIX domain
socket file with filename equal to the Name argument in configuration file.
Sockets are created at current working directory when starting the SPDK vhost
target.

### Core Affinity Configuration

Vhost target can be restricted to run on certain cores by specifying a `ReactorMask`.
Default is to allow vhost target work on core 0. For NUMA systems, it is essential
to run vhost with cores on each socket to achieve optimal performance.

Each controller may be assigned a set of cores using the optional
`Cpumask` parameter in configuration file.  For NUMA systems, the Cpumask should
specify cores on the same CPU socket as its associated VM. The `vhost` application will
pick one core from `ReactorMask` masked by `Cpumask`. `Cpumask` must be a subset of
`ReactorMask`.

## QEMU

Userspace vhost-scsi adds the following command line option for QEMU:
~~~
-device vhost-user-scsi-pci,id=scsi0,chardev=char0
~~~

Userspace vhost-blk adds the following command line option for QEMU:
~~~
-device vhost-user-blk-pci,logical_block_size=4096,size=512M,chardev=char0
~~~

In order to start qemu with vhost you need to specify following options:

 - Socket, which QEMU will use for vhost communication with SPDK:
~~~
-chardev socket,id=char0,path=/path/to/vhost/socket
~~~

 - Hugepages to share memory between vm and vhost target
~~~
-object memory-backend-file,id=mem,size=1G,mem-path=/dev/hugepages,share=on
~~~

# Running Vhost Target

To get started, the following example is usually sufficient:
~~~
app/vhost/vhost -c /path/to/vhost.conf
~~~

A full list of command line arguments to vhost can be obtained by:
~~~
app/vhost/vhost -h
~~~


## Example

Assume that qemu and spdk are in respectively `qemu` and `spdk` directories.
~~~
./qemu/build/x86_64-softmmu/qemu-system-x86_64 \
        -m 1024 \
        -object memory-backend-file,id=mem,size=1G,mem-path=/dev/hugepages,share=on \
        -numa node,memdev=mem \
        -drive file=$PROJECTS/os.qcow2,if=none,id=disk \
        -device ide-hd,drive=disk,bootindex=0 \
        -chardev socket,id=char0,path=./spdk/vhost.0 \
        -device vhost-user-scsi-pci,id=scsi0,chardev=char0 \
        -chardev socket,id=char1,path=./spdk/vhost.1 \
        -device vhost-user-blk-pci,logical_block_size=4096,size=512M,chardev=char1 \
        --enable-kvm
~~~

# Experimental Features {#vhost_experimental}

## Multi-Queue Block Layer (blk-mq)

It is possible to use the Linux kernel block multi-queue feature with vhost.
To enable it on Linux, it is required to modify kernel options inside the
virtual machine.

Instructions below for Ubuntu OS:
1. `vi /etc/default/grub`
2. Make sure mq is enabled:
`GRUB_CMDLINE_LINUX="scsi_mod.use_blk_mq=1"`
3. `sudo update-grub`
4. Reboot virtual machine

To achieve better performance, make sure to increase number of cores
assigned to the VM.

# Known bugs and limitations {#vhost_bugs}

## Windows virtio-blk driver before version 0.1.130-1 only works with 512-byte sectors

The Windows `viostor` driver before version 0.1.130-1 is buggy and does not
correctly support vhost-blk devices with non-512-byte block size.
See the [bug report](https://bugzilla.redhat.com/show_bug.cgi?id=1411092) for
more information.
