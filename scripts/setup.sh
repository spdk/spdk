#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

function nvme_whitelist_contains() {
	for i in ${NVME_WHITELIST[@]}
	do
		if [ "$i" == "$1" ] ; then
			 return 1
		fi
	done
	return 0
}

function linux_bind_driver() {
	bdf="$1"
	driver_name="$2"
	old_driver_name="no driver"
	ven_dev_id=$(lspci -n -s $bdf | cut -d' ' -f3 | sed 's/:/ /')

	if [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
		old_driver_name=$(basename $(readlink /sys/bus/pci/devices/$bdf/driver))

		if [ "$driver_name" = "$old_driver_name" ]; then
			return 0
		fi

		echo "$ven_dev_id" > "/sys/bus/pci/devices/$bdf/driver/remove_id" 2> /dev/null || true
		echo "$bdf" > "/sys/bus/pci/devices/$bdf/driver/unbind"
	fi

	echo "$bdf ($ven_dev_id): $old_driver_name -> $driver_name"

	echo "$ven_dev_id" > "/sys/bus/pci/drivers/$driver_name/new_id" 2> /dev/null || true
	echo "$bdf" > "/sys/bus/pci/drivers/$driver_name/bind" 2> /dev/null || true

	iommu_group=$(basename $(readlink -f /sys/bus/pci/devices/$bdf/iommu_group))
	if [ -e "/dev/vfio/$iommu_group" ]; then
		if [ "$username" != "" ]; then
			chown "$username" "/dev/vfio/$iommu_group"
		fi
	fi
}

function linux_unbind_driver() {
	bdf="$1"
	ven_dev_id=$(lspci -n -s $bdf | cut -d' ' -f3 | sed 's/:/ /')

	if ! [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
		return 0
	fi

	old_driver_name=$(basename $(readlink /sys/bus/pci/devices/$bdf/driver))

	echo "$ven_dev_id" > "/sys/bus/pci/devices/$bdf/driver/remove_id" 2> /dev/null || true
	echo "$bdf" > "/sys/bus/pci/devices/$bdf/driver/unbind"
	echo "$bdf ($ven_dev_id): $old_driver_name -> no driver"
}

function linux_hugetlbfs_mount() {
	mount | grep ' type hugetlbfs ' | awk '{ print $3 }'
}

function get_nvme_name_from_bdf {
	set +e
	nvme_devs=`lsblk -d --output NAME | grep "^nvme"`
	set -e
	for dev in $nvme_devs; do
		link_name=$(readlink /sys/block/$dev/device/device) || true
		if [ -z "$link_name" ]; then
			link_name=$(readlink /sys/block/$dev/device)
		fi
		bdf=$(basename "$link_name")
		if [ "$bdf" = "$1" ]; then
			eval "$2=$dev"
			return
		fi
	done
}

function get_virtio_names_from_bdf {
	set +e
	virtio_ctrlrs=`lsblk --nodeps --output "NAME,SUBSYSTEMS" | grep virtio | awk '{print $1}'`
	set -e
	virtio_names=''

	for ctrlr in $virtio_ctrlrs; do
		if readlink "/sys/block/$ctrlr" | grep -q "$1"; then
			virtio_names="$virtio_names $ctrlr"
		fi
	done

	eval "$2='$virtio_names'"
}

function configure_linux_pci {
	driver_name=vfio-pci
	if [ -z "$(ls /sys/kernel/iommu_groups)" ]; then
		# No IOMMU. Use uio.
		driver_name=uio_pci_generic
	fi

	# NVMe
	modprobe $driver_name || true
	for bdf in $(iter_pci_class_code 01 08 02); do
		blkname=''
		get_nvme_name_from_bdf "$bdf" blkname
		if [[ ${#NVME_WHITELIST[@]} != 0 ]] && nvme_whitelist_contains $bdf == "0" ; then
			echo "Skipping un-whitelisted NVMe controller $blkname ($bdf)"
			continue
		fi
		if [ "$blkname" != "" ]; then
			mountpoints=$(lsblk /dev/$blkname --output MOUNTPOINT -n | wc -w)
		else
			mountpoints="0"
		fi
		if [ "$mountpoints" = "0" ]; then
			linux_bind_driver "$bdf" "$driver_name"
		else
			echo Active mountpoints on /dev/$blkname, so not binding PCI dev $bdf
		fi
	done

	# IOAT
	TMP=`mktemp`
	#collect all the device_id info of ioat devices.
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	for dev_id in `cat $TMP`; do
		for bdf in $(iter_pci_dev_id 8086 $dev_id); do
			linux_bind_driver "$bdf" "$driver_name"
		done
	done
	rm $TMP

	# virtio-scsi
	TMP=`mktemp`
	#collect all the device_id info of virtio-scsi devices.
	grep "PCI_DEVICE_ID_VIRTIO_SCSI" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	for dev_id in `cat $TMP`; do
		for bdf in $(iter_pci_dev_id 1af4 $dev_id); do
			blknames=''
			get_virtio_names_from_bdf "$bdf" blknames
			for blkname in $blknames; do
				if mount | grep -q "/dev/$blkname"; then
					echo Active mountpoints on /dev/$blkname, so not binding PCI dev $bdf
					continue 2
				fi
			done

			linux_bind_driver "$bdf" "$driver_name"
		done
	done
	rm $TMP

	echo "1" > "/sys/bus/pci/rescan"
}

function configure_linux {
	if [ "$SKIP_PCI" == 0 ]; then
		configure_linux_pci
	fi

	hugetlbfs_mount=$(linux_hugetlbfs_mount)

	if [ -z "$hugetlbfs_mount" ]; then
		hugetlbfs_mount=/mnt/huge
		echo "Mounting hugetlbfs at $hugetlbfs_mount"
		mkdir -p "$hugetlbfs_mount"
		mount -t hugetlbfs nodev "$hugetlbfs_mount"
	fi

	if [ -z "$HUGENODE" ]; then
		hugepages_target="/proc/sys/vm/nr_hugepages"
	else
		hugepages_target="/sys/devices/system/node/node${HUGENODE}/hugepages/hugepages-${HUGEPGSZ}kB/nr_hugepages"
	fi

	echo "$NRHUGE" > "$hugepages_target"
	allocated_hugepages=`cat $hugepages_target`
	if [ "$allocated_hugepages" -lt "$NRHUGE" ]; then
		echo ""
		echo "## ERROR: requested $NRHUGE hugepages but only $allocated_hugepages could be allocated."
		echo "## Memory might be heavily fragmented. Please try flushing the system cache, or reboot the machine."
		exit 1
	fi

	if [ "$driver_name" = "vfio-pci" ]; then
		if [ "$username" != "" ]; then
			chown "$username" "$hugetlbfs_mount"
			chmod g+w "$hugetlbfs_mount"
		fi

		MEMLOCK_AMNT=`ulimit -l`
		if [ "$MEMLOCK_AMNT" != "unlimited" ] ; then
			MEMLOCK_MB=$(( $MEMLOCK_AMNT / 1024 ))
			echo ""
			echo "Current user memlock limit: ${MEMLOCK_MB} MB"
			echo ""
			echo "This is the maximum amount of memory you will be"
			echo "able to use with DPDK and VFIO if run as current user."
			echo -n "To change this, please adjust limits.conf memlock "
			echo "limit for current user."

			if [ $MEMLOCK_AMNT -lt 65536 ] ; then
				echo ""
				echo "## WARNING: memlock limit is less than 64MB"
				echo -n "## DPDK with VFIO may not be able to initialize "
				echo "if run as current user."
			fi
		fi
	fi
}

function reset_linux_pci {
	# NVMe
	set +e
	lsmod | grep nvme > /dev/null
	driver_loaded=$?
	set -e
	for bdf in $(iter_pci_class_code 01 08 02); do
		if [ $driver_loaded -eq 0 ]; then
			linux_bind_driver "$bdf" nvme
		else
			linux_unbind_driver "$bdf"
		fi
	done

	# IOAT
	TMP=`mktemp`
	#collect all the device_id info of ioat devices.
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	set +e
	lsmod | grep ioatdma > /dev/null
	driver_loaded=$?
	set -e
	for dev_id in `cat $TMP`; do
		for bdf in $(iter_pci_dev_id 8086 $dev_id); do
			if [ $driver_loaded -eq 0 ]; then
				linux_bind_driver "$bdf" ioatdma
			else
				linux_unbind_driver "$bdf"
			fi
		done
	done
	rm $TMP

	# virtio-scsi
	TMP=`mktemp`
	#collect all the device_id info of virtio-scsi devices.
	grep "PCI_DEVICE_ID_VIRTIO_SCSI" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	# TODO: check if virtio-pci is loaded first and just unbind if it is not loaded
	# Requires some more investigation - for example, some kernels do not seem to have
	#  virtio-pci but just virtio_scsi instead.  Also need to make sure we get the
	#  underscore vs. dash right in the virtio_scsi name.
	modprobe virtio-pci || true
	for dev_id in `cat $TMP`; do
		for bdf in $(iter_pci_dev_id 1af4 $dev_id); do
			linux_bind_driver "$bdf" virtio-pci
		done
	done
	rm $TMP

	echo "1" > "/sys/bus/pci/rescan"
}

function reset_linux {
	if [ "$SKIP_PCI" == 0 ]; then
		reset_linux_pci
	fi

	hugetlbfs_mount=$(linux_hugetlbfs_mount)
	rm -f "$hugetlbfs_mount"/spdk*map_*
	rm -f /run/.spdk*
}

function status_linux {
	echo "NVMe devices"

	echo -e "BDF\t\tNuma Node\tDriver name\t\tDevice name"
	for bdf in $(iter_pci_class_code 01 08 02); do
		driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
		node=`cat /sys/bus/pci/devices/$bdf/numa_node`;
		if [ "$driver" = "nvme" -a -d /sys/bus/pci/devices/$bdf/nvme ]; then
			name="\t"`ls /sys/bus/pci/devices/$bdf/nvme`;
		else
			name="-";
		fi
		echo -e "$bdf\t$node\t\t$driver\t\t$name";
	done

	echo "I/OAT DMA"

	#collect all the device_id info of ioat devices.
	TMP=`grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}'`
	echo -e "BDF\t\tNuma Node\tDriver Name"
	for dev_id in $TMP; do
		for bdf in $(iter_pci_dev_id 8086 $dev_id); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			node=`cat /sys/bus/pci/devices/$bdf/numa_node`;
			echo -e "$bdf\t$node\t\t$driver"
		done
	done

	echo "virtio"

	#collect all the device_id info of virtio-scsi devices.
	TMP=`grep "PCI_DEVICE_ID_VIRTIO_SCSI" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}'`
	echo -e "BDF\t\tNuma Node\tDriver Name"
	for dev_id in $TMP; do
		for bdf in $(iter_pci_dev_id 1af4 $dev_id); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			node=`cat /sys/bus/pci/devices/$bdf/numa_node`;
			echo -e "$bdf\t$node\t\t$driver"
		done
	done
}

function configure_freebsd_pci {
	TMP=`mktemp`

	# NVMe
	GREP_STR="class=0x010802"

	# IOAT
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP
	for dev_id in `cat $TMP`; do
		GREP_STR="${GREP_STR}\|chip=0x${dev_id}8086"
	done

	AWK_PROG="{if (count > 0) printf \",\"; printf \"%s:%s:%s\",\$2,\$3,\$4; count++}"
	echo $AWK_PROG > $TMP

	BDFS=`pciconf -l | grep "${GREP_STR}" | awk -F: -f $TMP`

	kldunload nic_uio.ko || true
	kenv hw.nic_uio.bdfs=$BDFS
	kldload nic_uio.ko
	rm $TMP
}

function configure_freebsd {
	if [ "$SKIP_PCI" == 0 ]; then
		configure_freebsd_pci
	fi

	kldunload contigmem.ko || true
	kenv hw.contigmem.num_buffers=$((HUGEMEM / 256))
	kenv hw.contigmem.buffer_size=$((256 * 1024 * 1024))
	kldload contigmem.ko
}

function reset_freebsd {
	kldunload contigmem.ko || true

	if [ "$SKIP_PCI" == 0 ]; then
		kldunload nic_uio.ko || true
	fi
}

username=$1
mode=$2

if [ "$username" = "reset" -o "$username" = "config" -o "$username" = "status" ]; then
	mode="$username"
	username=""
fi

if [ "$mode" == "" ]; then
	mode="config"
fi

if [ "$username" = "" ]; then
	username="$SUDO_USER"
	if [ "$username" = "" ]; then
		username=`logname 2>/dev/null` || true
	fi
fi

: ${HUGEMEM:=2048}
: ${SKIP_PCI:=0}
: ${NVME_WHITELIST:=""}
declare -a NVME_WHITELIST=(${NVME_WHITELIST})

if [ `uname` = Linux ]; then
	HUGEPGSZ=$(( `grep Hugepagesize /proc/meminfo | cut -d : -f 2 | tr -dc '0-9'` ))
	HUGEPGSZ_MB=$(( $HUGEPGSZ / 1024 ))
	: ${NRHUGE=$(( (HUGEMEM + HUGEPGSZ_MB - 1) / HUGEPGSZ_MB ))}

	if [ "$mode" == "config" ]; then
		configure_linux
	elif [ "$mode" == "reset" ]; then
		reset_linux
	elif [ "$mode" == "status" ]; then
		status_linux
	fi
else
	if [ "$mode" == "config" ]; then
		configure_freebsd
	elif [ "$mode" == "reset" ]; then
		reset_freebsd
	fi
fi
