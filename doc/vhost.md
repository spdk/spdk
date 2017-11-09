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
-device vhost-user-scsi-pci,id=scsi0,chardev=char0[,num_queues=N]
~~~

Userspace vhost-blk adds the following command line option for QEMU:
~~~
-device vhost-user-blk-pci,logical_block_size=4096,size=512M,chardev=char0[,num_queues=N]
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

## Multi-Queue Block Layer (blk-mq)

For best performance use the Linux kernel block multi-queue feature with vhost.
To enable it on Linux, it is required to modify kernel options inside the
virtual machine.

Instructions below for Ubuntu OS:
1. `vi /etc/default/grub`
2. Make sure mq is enabled:
`GRUB_CMDLINE_LINUX="scsi_mod.use_blk_mq=1"`
3. `sudo update-grub`
4. Reboot virtual machine

To achieve better performance, make sure to increase number of cores
assigned to the VM and add `num_queues` parameter to `device`. It should be enought
to set `num_queues=4` to saturate phisical device. More queues might not bring
better performance and can lead to SPDK vhost performance degradation if many
controllers are used becauce each device will require additional `num_queues` to
be polled.

## Example

Run SPDK vhost to present two controllers: Virtio SCSI and Virtio block.

Virtio SCSI controller with two LUNs:

- SCSI target 1, LUN 0 backed be Malloc0 bdev
- SCSI target 5, LUN 0 backed be Malloc1 bdev

Virtio block device backed by Malloc2 bdev

For better performance use 4 VCPU (`-smp 4`) and 4 queues for each controller
(`num_queues=4`). Assume that qemu and spdk are in respectively `qemu` and `spdk`
directories and SPDK is build with `CONFIG_DEBUG=y` for increased log verbosity.

~~~
host: ~$ cd spdk
host: ~$ cat vhost.conf
[Global]
  LogFacility "local7"

[Rpc]
  Enable Yes
  Listen 127.0.0.1:5261

[Malloc]
  NumberOfLuns 3
  LunSizeInMb 128
  BlockSize 512

[VhostScsi0]
  Name vhost_scsi0_socket
  Dev 1 Malloc0
  Dev 5 Malloc1

[VhostBlk0]
  Name vhost_blk0_socket
  Dev Malloc2

host: ~$ sudo ./app/vhost/vhost -c vhost.conf -m -s 1024 -m 1 &
[ DPDK EAL parameters: vhost -c 1 -m 1024 --file-prefix=spdk_pid191213 ]
EAL: Detected 48 lcore(s)
EAL: Probing VFIO support...
EAL: VFIO support initialized
<< REMOVED CONSOLE LOG >>
VHOST_CONFIG: bind to vhost_scsi0_socket
vhost.c: 592:spdk_vhost_dev_construct: *NOTICE*: Controller vhost_scsi0_socket: new controller added
vhost_scsi.c: 840:spdk_vhost_scsi_dev_add_dev: *NOTICE*: Controller vhost_scsi0_socket: defined device 'Dev 1' using lun 'Malloc0'
vhost_scsi.c: 840:spdk_vhost_scsi_dev_add_dev: *NOTICE*: Controller vhost_scsi0_socket: defined device 'Dev 5' using lun 'Malloc1'
VHOST_CONFIG: vhost-user server: socket created, fd: 65
VHOST_CONFIG: bind to vhost_blk0_socket
vhost.c: 592:spdk_vhost_dev_construct: *NOTICE*: Controller vhost_blk0_socket: new controller added
vhost_blk.c: 720:spdk_vhost_blk_construct: *NOTICE*: Controller vhost_blk0_socket: using bdev 'Malloc2'

host: ~$ cd ..
host: ~$ sudo ./qemu/build/x86_64-softmmu/qemu-system-x86_64 --enable-kvm -m 1024 -cpu host -smp 4 -nographic \
        -object memory-backend-file,id=mem,size=1G,mem-path=/dev/hugepages,share=on -numa node,memdev=mem \
        -drive file=guest_os_image.qcow2,if=none,id=disk \
        -device ide-hd,drive=disk,bootindex=0 \
        -chardev socket,id=spdk_vhost_scsi0,path=./spdk/vhost_scsi0_socket \
        -device vhost-user-scsi-pci,id=scsi0,chardev=spdk_vhost_scsi0,num_queues=4 \
        -chardev socket,id=spdk_vhost_blk0,path=./spdk/vhost_blk0_socket \
        -device vhost-user-blk-pci,logical_block_size=512,size=128M,chardev=spdk_vhost_blk0,num_queues=4

<< LOGIN TO GUEST OS >>
guest: ~$ lsblk --output "NAME,KNAME,MODEL,HCTL,SIZE,VENDOR,SUBSYSTEMS"
NAME   KNAME MODEL            HCTL        SIZE VENDOR   SUBSYSTEMS
fd0    fd0                                  4K          block:platform
sda    sda   QEMU HARDDISK    1:0:0:0      80G ATA      block:scsi:pci
└─sda1 sda1                                80G          block:scsi:pci
sdb    sdb   Malloc disk      2:0:1:0     128M INTEL    block:scsi:virtio:pci
sdc    sdc   Malloc disk      2:0:5:0     128M INTEL    block:scsi:virtio:pci
vda    vda                                128M 0x1af4   block:virtio:pci

guest: ~$ sudo poweroff
host: $ fg
^Cvhost.c:1006:session_shutdown: *NOTICE*: Exiting
~~~

We can see that `sdb` and `sdc` are SPDK vhost-scsi LUNs, and `vda` is SPDK vhost-blk disk.

# Known bugs and limitations {#vhost_bugs}

## Windows virtio-blk driver before version 0.1.130-1 only works with 512-byte sectors

The Windows `viostor` driver before version 0.1.130-1 is buggy and does not
correctly support vhost-blk devices with non-512-byte block size.
See the [bug report](https://bugzilla.redhat.com/show_bug.cgi?id=1411092) for
more information.
