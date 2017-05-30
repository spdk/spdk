#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function linux_iter_pci_class_code {
	# Argument is the class code
	lspci -mm -n -D | awk -v cc="\"$1\"" -F " " '{if (cc ~ $2) print $1}' | tr -d '"'
}

function linux_iter_pci_dev_id {
	# Argument 1 is the vendor id
	# Argument 2 is the device id
	lspci -mm -n -D | awk -v ven="\"$1\"" -v dev="\"$2\"" -F " " '{if (ven ~ $3 && dev ~ $4) print $1}' | tr -d '"'
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

function linux_hugetlbfs_mount() {
	mount | grep ' type hugetlbfs ' | awk '{ print $3 }'
}

function configure_linux {
	driver_name=vfio-pci
	if [ -z "$(ls /sys/kernel/iommu_groups)" ]; then
		# No IOMMU. Use uio.
		driver_name=uio_pci_generic
	fi

	# NVMe
	modprobe $driver_name || true
	for bdf in $(linux_iter_pci_class_code 0108); do
		linux_bind_driver "$bdf" "$driver_name"
	done


	# IOAT
	TMP=`mktemp`
	#collect all the device_id info of ioat devices.
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	for dev_id in `cat $TMP`; do
		for bdf in $(linux_iter_pci_dev_id 8086 $dev_id); do
			linux_bind_driver "$bdf" "$driver_name"
		done
	done
	rm $TMP

	# virtio-scsi
	TMP=`mktemp`
	#collect all the device_id info of virtio-scsi devices.
	grep "VIRTIO_PCI_DEVICEID_SCSI" $rootdir/lib/bdev/virtio/rte_virtio/virtio_pci.h \
	| awk -F"x" '{print $2}' > $TMP

	for dev_id in `cat $TMP`; do
		for bdf in $(linux_iter_pci_dev_id 1af4 $dev_id); do
			linux_bind_driver "$bdf" "$driver_name"
		done
	done
	rm $TMP

	echo "1" > "/sys/bus/pci/rescan"

	hugetlbfs_mount=$(linux_hugetlbfs_mount)

	if [ -z "$hugetlbfs_mount" ]; then
		hugetlbfs_mount=/mnt/huge
		echo "Mounting hugetlbfs at $hugetlbfs_mount"
		mkdir -p "$hugetlbfs_mount"
		mount -t hugetlbfs nodev "$hugetlbfs_mount"
	fi
	echo "$NRHUGE" > /proc/sys/vm/nr_hugepages

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

function reset_linux {
	# NVMe
	modprobe nvme || true
	for bdf in $(linux_iter_pci_class_code 0108); do
		linux_bind_driver "$bdf" nvme
	done


	# IOAT
	TMP=`mktemp`
	#collect all the device_id info of ioat devices.
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	modprobe ioatdma || true
	for dev_id in `cat $TMP`; do
		for bdf in $(linux_iter_pci_dev_id 8086 $dev_id); do
			linux_bind_driver "$bdf" ioatdma
		done
	done
	rm $TMP

	# virtio-scsi
	TMP=`mktemp`
	#collect all the device_id info of virtio-scsi devices.
	grep "VIRTIO_PCI_DEVICEID_SCSI" $rootdir/lib/bdev/virtio/rte_virtio/virtio_pci.h \
	| awk -F"x" '{print $2}' > $TMP

	modprobe virtio-pci || true
	for dev_id in `cat $TMP`; do
		for bdf in $(linux_iter_pci_dev_id 1af4 $dev_id); do
			linux_bind_driver "$bdf" virtio-pci
		done
	done
	rm $TMP

	echo "1" > "/sys/bus/pci/rescan"

	hugetlbfs_mount=$(linux_hugetlbfs_mount)
	rm -f "$hugetlbfs_mount"/spdk*map_*
}

function status_linux {
	echo "NVMe devices"

	echo -e "BDF\t\tNuma Node\tDriver name\t\tDevice name"
	for bdf in $(linux_iter_pci_class_code 0108); do
		driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
		node=`cat /sys/bus/pci/devices/$bdf/numa_node`;
		if [ "$driver" = "nvme" ]; then
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
		for bdf in $(linux_iter_pci_dev_id 8086 $dev_id); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			node=`cat /sys/bus/pci/devices/$bdf/numa_node`;
			echo -e "$bdf\t$node\t\t$driver"
		done
	done

	echo "virtio"

	#collect all the device_id info of virtio-scsi devices.
	TMP=`grep "VIRTIO_PCI_DEVICEID_SCSI" $rootdir/lib/bdev/virtio/rte_virtio/virtio_pci.h \
	| awk -F"x" '{print $2}'`
	echo -e "BDF\t\tNuma Node\tDriver Name"
	for dev_id in $TMP; do
		for bdf in $(linux_iter_pci_dev_id 1af4 $dev_id); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			node=`cat /sys/bus/pci/devices/$bdf/numa_node`;
			echo -e "$bdf\t$node\t\t$driver"
		done
	done
}

function configure_freebsd {
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

	kldunload contigmem.ko || true
	kenv hw.contigmem.num_buffers=$((NRHUGE * 2 / 256))
	kenv hw.contigmem.buffer_size=$((256 * 1024 * 1024))
	kldload contigmem.ko
}

function reset_freebsd {
	kldunload contigmem.ko || true
	kldunload nic_uio.ko || true
}

: ${NRHUGE:=1024}

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

if [ `uname` = Linux ]; then
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
