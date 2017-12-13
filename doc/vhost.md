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

Next, start the SPDK vhost target application.  The following command will start vhost
on CPU cores 0 and 1 (cpumask 0x3) with all future socket files placed in /var/tmp.
Vhost will fully occupy given CPU cores for I/O polling. Particular vhost devices can
be yet restricted to run on a subset of these CPU cores. See @ref vhost_dev_create for
details.

~~~{.sh}
app/vhost/vhost -S /var/tmp -m 0x3
~~~

# SPDK Configuration {#vhost_config}

## Create bdev (block device) {#vhost_bdev_create}

SPDK bdevs are block devices which will be exposed to the guest OS.
For vhost-scsi, bdevs are exposed as as SCSI LUNs on SCSI devices attached to the
vhost-scsi controller in the guest OS.
For vhost-blk, bdevs are exposed directly as block devices in the guest OS and are
not associated at all with SCSI.

SPDK supports several different types of storage backends, including NVMe,
Linux AIO, malloc ramdisk and Ceph RBD.  Refer to @ref bdev_getting_started for
additional information on configuring SPDK storage backends.

This guide will base on a malloc (ramdisk) bdev named Malloc0. The following RPC
will create a 64MB malloc bdev with 512-byte block size.

~~~{.sh}
scripts/rpc.py construct_malloc_bdev 64 512 Malloc0
~~~

## Create a virtio device {#vhost_dev_create}

### Vhost-SCSI

The following RPC will create a vhost-scsi controller which can be accessed
by QEMU via /var/tmp/vhost.0. All the I/O polling will be pinned to the least
occupied CPU core within given cpumask - in this case always CPU 0. For NUMA
systems, the cpumask should specify cores on the same CPU socket as its
associated VM.

~~~{.sh}
scripts/rpc.py construct_vhost_scsi_controller --cpumask 0x1 vhost.0
~~~

The following RPC will attach the Malloc0 bdev to the vhost.0 vhost-scsi
controller.  Malloc0 will appear as a single LUN on a SCSI device with
target ID 0. SPDK Vhost-SCSI device currently supports only one LUN per SCSI target.

~~~{.sh}
scripts/rpc.py add_vhost_scsi_lun vhost.0 0 Malloc0
~~~

To remove a bdev from a vhost-scsi controller use the following RPC:

~~~{.sh}
scripts/rpc.py remove_vhost_scsi_dev vhost.0 0
~~~

### Vhost-BLK

The following RPC will create a vhost-blk device exposing Malloc0 bdev.
The device will be accessible to QEMU via /var/tmp/vhost.1. All the I/O polling
will be pinned to the least occupied CPU core within given cpumask - in this case
always CPU 0. For NUMA systems, the cpumask should specify cores on the same CPU
socket as its associated VM.

~~~{.sh}
scripts/rpc.py construct_vhost_blk_controller --cpumask 0x1 vhost.1 Malloc0
~~~

It is also possible to construct a read-only vhost-blk device by specifying an
extra `-r` or `--readonly` parameter.

~~~{.sh}
scripts/rpc.py construct_vhost_blk_controller --cpumask 0x1 -r vhost.1 Malloc0
~~~

## QEMU {#vhost_qemu_config}

Now the virtual machine can be started with QEMU.  The following command-line
parameters must be added to connect the virtual machine to its vhost-scsi
controller.

First, specify the memory backend for the virtual machine.  Since QEMU must
share the virtual machine's memory with the SPDK vhost target, the memory
must be specified in this format with share=on. Additional memory can be
hot-added on VM runtime, but at least one memory-backend-file must be available
before a VM can be started.

~~~{.sh}
-object memory-backend-file,id=mem,size=1G,mem-path=/dev/hugepages,share=on
~~~

Second, ensure QEMU boots from the virtual machine image and not the
SPDK malloc block device by specifying bootindex=0 for the boot image.

~~~{.sh}
-drive file=guest_os_image.qcow2,if=none,id=disk
-device ide-hd,drive=disk,bootindex=0
~~~

Finally, specify the SPDK vhost devices:

### Vhost-SCSI

~~~{.sh}
-chardev socket,id=char0,path=/var/tmp/vhost.0
-device vhost-user-scsi-pci,id=scsi0,chardev=char0
~~~

### Vhost-BLK

~~~{.sh}
-chardev socket,id=char1,path=/var/tmp/vhost.1
-device vhost-user-blk-pci,id=blk0,chardev=char1,logical_block_size=512,size=64M
~~~

## Example output {#vhost_example}

TODO add actual outputs

~~~{.sh}
host:~# HUGENODE=0 HUGEMEM=2048 ./scripts/setup.sh
0000:01:00.0 (8086 0953): nvme -> vfio-pci
~~~

~~~{.sh}
host:~# ./app/vhost/vhost -s 1024 -m 0x3 &
<TODO log>
~~~

~~~{.sh}
host:~# ./scripts/rpc.py construct_vhost_scsi_controller vhost.0
~~~

~~~{.sh}
host:~# ./scripts/rpc.py construct_nvme_bdev -b Nvme0 -t pcie -a 0000:01:00.0
~~~

~~~{.sh}
host:~# ./scripts/rpc.py add_vhost_scsi_lun vhost.0 0 Nvme0
~~~

~~~{.sh}
host:~# taskset -c 3,4 qemu-system-x86_64 \
  --enable-kvm \
  -cpu host -smp 2 \
  -m 1G -object memory-backend-file,id=mem0,size=1G,mem-path=/dev/hugepages,share=on -numa node,memdev=mem0 \
  -drive file=guest_os_image.qcow2,if=none,id=disk \
  -device ide-hd,drive=disk,bootindex=0 \
  -chardev socket,id=spdk_vhost_scsi0,path=/tmp/vhost_scsi0 \
  -device vhost-user-scsi-pci,id=scsi0,chardev=spdk_vhost_scsi0,num_queues=4 \
  -chardev socket,id=spdk_vhost_blk0,path=./spdk/vhost_blk0_socket \
  -device vhost-user-blk-pci,logical_block_size=512,size=64M,chardev=spdk_vhost_blk0,num_queues=4
~~~

~~~{.sh}
guest:~# lsblk --output "NAME,KNAME,MODEL,HCTL,SIZE,VENDOR,SUBSYSTEMS"
NAME   KNAME MODEL            HCTL        SIZE VENDOR   SUBSYSTEMS
fd0    fd0                                  4K          block:platform
sda    sda   QEMU HARDDISK    1:0:0:0      80G ATA      block:scsi:pci
  sda1 sda1                                80G          block:scsi:pci
sdb    sdb   Malloc disk      2:0:1:0     128M INTEL    block:scsi:virtio:pci
sdc    sdc   Malloc disk      2:0:5:0     128M INTEL    block:scsi:virtio:pci
vda    vda                                128M 0x1af4   block:virtio:pci
~~~

~~~{.sh}
guest:~# poweroff
~~~

~~~{.sh}
host:~# fg
<< CTRL + C >>
vhost.c:1006:session_shutdown: *NOTICE*: Exiting
~~~

We can see that `sdb` and `sdc` are SPDK vhost-scsi LUNs, and `vda` is SPDK a
vhost-blk disk.


# Advanced Topics

## Multi-Queue Block Layer (blk-mq) {#vhost_multiqueue}

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

## Hot-attach/hot-detach {#vhost_hotattach}

Hotplug/hotremove within a vhost controller is called hot-attach/detach. This is to
distinguish it from SPDK bdev hotplug/hotremove. E.g. if an NVMe bdev is attached
to a vhost-scsi controller, physically hotremoving the NVMe will trigger a vhost-scsi
hot-detach. It is also possible to hot-detach a bdev manually via RPC - for example
when the bdev is about to be attached to another controller. See the details below.

Please also note that hot-attach/detach is Vhost-SCSI-specific. There are no RPCs
to hot-attach/detach the bdev from a Vhost-BLK device. If a Vhost-BLK device exposes
an NVMe bdev that is hotremoved, all the I/O traffic on that Vhost-BLK device will
be aborted - possibly flooding a VM with syslog warnings and errors.

### Hot-attach

Hot-attach is is done by simply attaching a bdev to a vhost controller with a QEMU VM
already started. No other extra action is necessary.

~~~{.sh}
scripts/rpc.py add_vhost_scsi_lun vhost.0 0 Malloc0
~~~

### Hot-detach

Just like hot-attach, the hot-detach is done by simply removing a bdev from a controller
when a QEMU VM is already started.

~~~{.sh}
scripts/rpc.py remove_vhost_scsi_dev vhost.0 0
~~~

Removing an entire bdev will hot-detach it from a controller as well.

~~~{.sh}
scripts/rpc.py delete_bdev Malloc0
~~~

# Known bugs and limitations {#vhost_bugs}

## Windows virtio-blk driver before version 0.1.130-1 only works with 512-byte sectors

The Windows `viostor` driver before version 0.1.130-1 is buggy and does not
correctly support vhost-blk devices with non-512-byte block size.
See the [bug report](https://bugzilla.redhat.com/show_bug.cgi?id=1411092) for
more information.
