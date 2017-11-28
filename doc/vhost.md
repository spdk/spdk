# vhost {#vhost}

# vhost Users Guide {#vhost_users_guide}

The Storage Performance Development Kit vhost application is named `vhost`.
This application extends SPDK to present virtio storage controllers to QEMU-based
VMs and process I/O submitted to devices attached to those controllers.

# Prerequisites {#vhost_prereqs}

This guide assumes the SPDK has been built according to the instructions in @ref
getting_started.  The SPDK vhost target is built with the default configure options.

## Supported Guest Operating Systems

The guest OS must contain virtio-scsi or virtio-blk drivers.  Most Linux and FreeBSD
distributions include virtio drivers.
[Windows virtio drivers](https://fedoraproject.org/wiki/Windows_Virtio_Drivers) must be
installed separately.  The SPDK vhost target has been tested with Ubuntu 16.04, Fedora
25, and Windows 2012 R2.

## QEMU

Userspace vhost-scsi target support was added to upstream QEMU in v2.10.0.  Run
the following command to confirm your QEMU supports userspace vhost-scsi.

~~~{.sh}
qemu-system-x86_64 -device vhost-user-scsi-pci,help
~~~

Userspace vhost-blk target support is not yet upstream in QEMU, but patches
are available in SPDK's QEMU repository:

~~~{.sh}
git clone -b spdk https://github.com/spdk/qemu
cd qemu
mkdir build
cd build
../configure
make
~~~

# Starting SPDK vhost target {#vhost_start}

First, run the SPDK setup.sh script to setup some hugepages for the SPDK vhost target
application.  This will allocate 4096MiB (4GiB) of hugepages, enough for the SPDK
vhost target and the virtual machine.

~~~{.sh}
HUGEMEM=4096 scripts/setup.sh
~~~

Next, start the SPDK vhost target application.  This will put the vhost socket files
in /var/tmp.

~~~{.sh}
app/vhost/vhost -S /var/tmp
~~~

# SPDK Configuration {#vhost_config}

## Create bdev (block device)

SPDK bdevs are block devices which will be exposed to the guest OS.
For vhost-scsi, bdevs are exposed as as SCSI LUNs on SCSI devices attached to the
vhost-scsi controller in the guest OS.
For vhost-blk, bdevs are exposed directly as block devices in the guest OS and are
not associated at all with SCSI.

SPDK supports several different types of storage backends, including NVMe,
Linux AIO, malloc ramdisk and Ceph RBD.  Refer to @ref bdev_getting_started for
additional information on configuring SPDK storage backends.

The following RPC will create a malloc (ramdisk) bdev named Malloc0.  It will have
size 64MB and present a 512-byte block size.

~~~{.sh}
scripts/rpc.py construct_malloc_bdev 64 512 Malloc0
~~~

## Map bdev to vhost-scsi controller

The following RPC will create a vhost-scsi controller which can be accessed
by QEMU via /var/tmp/vhost.0.

~~~{.sh}
scripts/rpc.py construct_vhost_scsi_controller vhost.0
~~~

The following RPC will attach the Malloc0 bdev to the vhost.0 vhost-scsi
controller.  Malloc0 will appear as a single LUN on a SCSI device with
target ID 0.

~~~{.sh}
scripts/rpc.py add_vhost_scsi_lun vhost.0 0 Malloc0
~~~

## QEMU

Now the virtual machine can be started with QEMU.  The following command-line
parameters must be added to connect the virtual machine to its vhost-scsi
controller.

First, specify the memory backend for the virtual machine.  Since QEMU must
share the virtual machine's memory with the SPDK vhost target, the memory
must be specified in this format with share=on.

~~~{.sh}
-object memory-backend-file,id=mem,size=1G,mem-path=/dev/hugepages,share=on
~~~

Second, ensure QEMU boots from the virtual machine image and not the
SPDK malloc block device by specifying bootindex=0 for the boot image.

~~~{.sh}
-drive file=guest_os_image.qcow2,if=none,id=disk
-device ide-hd,drive=disk,bootindex=0
~~~

Next, specify the vhost socket name:

~~~{.sh}
-chardev socket,id=char0,path=/var/tmp/vhost.0
~~~

Finally, map the vhost socket name to a vhost-user-scsi-pci device.

~~~{.sh}
-device vhost-user-scsi-pci,id=scsi0,chardev=char0
~~~

## Example output {#vhost_example}

(needs to be updated)

host: $ sudo ./app/vhost/vhost -c vhost.conf -s 1024 -m 1 &
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

host: $ cd ..
host: $ sudo ./qemu/build/x86_64-softmmu/qemu-system-x86_64 --enable-kvm -m 1024 \
  -cpu host -smp 4 -nographic \
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
  sda1 sda1                                80G          block:scsi:pci
sdb    sdb   Malloc disk      2:0:1:0     128M INTEL    block:scsi:virtio:pci
sdc    sdc   Malloc disk      2:0:5:0     128M INTEL    block:scsi:virtio:pci
vda    vda                                128M 0x1af4   block:virtio:pci

guest: $ sudo poweroff
host: $ fg
<< CTRL + C >>
vhost.c:1006:session_shutdown: *NOTICE*: Exiting
~~~

We can see that `sdb` and `sdc` are SPDK vhost-scsi LUNs, and `vda` is SPDK vhost-blk disk.


# Advanced Topics

## vhost-blk

(add info here on configuring SPDK and QEMU for vhost-blk)

## Core Affinity Configuration

Vhost target can be restricted to run on certain cores by specifying a `ReactorMask`.
Default is to allow vhost target work on core 0. For NUMA systems, it is essential
to run vhost with cores on each socket to achieve optimal performance.

Each controller may be assigned a set of cores using the optional
`Cpumask` parameter in configuration file.  For NUMA systems, the Cpumask should
specify cores on the same CPU socket as its associated VM. The `vhost` application will
pick one core from `ReactorMask` masked by `Cpumask`. `Cpumask` must be a subset of
`ReactorMask`.

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
assigned to the VM and add `num_queues` parameter to the QEMU `device`. It should be enough
to set `num_queues=4` to saturate physical device. Adding too many queues might lead to SPDK
vhost performance degradation if many vhost devices are used because each device will require
additional `num_queues` to be polled.

# Known bugs and limitations {#vhost_bugs}

## Windows virtio-blk driver before version 0.1.130-1 only works with 512-byte sectors

The Windows `viostor` driver before version 0.1.130-1 is buggy and does not
correctly support vhost-blk devices with non-512-byte block size.
See the [bug report](https://bugzilla.redhat.com/show_bug.cgi?id=1411092) for
more information.
