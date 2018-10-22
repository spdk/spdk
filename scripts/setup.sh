#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

function usage()
{
	if [ `uname` = Linux ]; then
		options="[config|reset|status|cleanup|help]"
	else
		options="[config|reset|help]"
	fi

	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Helper script for allocating hugepages and binding NVMe, I/OAT and Virtio devices to"
	echo "a generic VFIO kernel driver. If VFIO is not available on the system, this script will"
	echo "fall back to UIO. NVMe and Virtio devices with active mountpoints will be ignored."
	echo "All hugepage operations use default hugepage size on the system (hugepagesz)."
	echo "Usage: $(basename $1) $options"
	echo
	echo "$options - as following:"
	echo "config            Default mode. Allocate hugepages and bind PCI devices."
	if [ `uname` = Linux ]; then
		echo "cleanup            Remove any orphaned files that can be left in the system after SPDK application exit"
	fi
	echo "reset             Rebind PCI devices back to their original drivers."
	echo "                  Also cleanup any leftover spdk files/resources."
	echo "                  Hugepage memory size will remain unchanged."
	if [ `uname` = Linux ]; then
		echo "status            Print status of all SPDK-compatible devices on the system."
	fi
	echo "help              Print this help message."
	echo
	echo "The following environment variables can be specified."
	echo "HUGEMEM           Size of hugepage memory to allocate (in MB). 2048 by default."
	echo "                  For NUMA systems, the hugepages will be evenly distributed"
	echo "                  between CPU nodes"
	echo "NRHUGE            Number of hugepages to allocate. This variable overwrites HUGEMEM."
	echo "HUGENODE          Specific NUMA node to allocate hugepages on. To allocate"
	echo "                  hugepages on multiple nodes run this script multiple times -"
	echo "                  once for each node."
	echo "PCI_WHITELIST     Whitespace separated list of PCI devices (NVMe, I/OAT, Virtio) to bind."
	echo "                  Each device must be specified as a full PCI address."
	echo "                  E.g. PCI_WHITELIST=\"0000:01:00.0 0000:02:00.0\""
	echo "                  To blacklist all PCI devices use a non-valid address."
	echo "                  E.g. PCI_WHITELIST=\"none\""
	echo "                  If empty or unset, all PCI devices will be bound."
	echo "TARGET_USER       User that will own hugepage mountpoint directory and vfio groups."
	echo "                  By default the current user will be used."
	exit 0
}

# In monolithic kernels the lsmod won't work. So
# back that with a /sys/modules check. Return a different code for
# built-in vs module just in case we want that down the road.
function check_for_driver {
	$(lsmod | grep $1 > /dev/null)
	if [ $? -eq 0 ]; then
		return 1
	else
		if [[ -d /sys/module/$1 ]]; then
			return 2
		else
			return 0
		fi
	fi
	return 0
}

function pci_can_bind() {
	if [[ ${#PCI_WHITELIST[@]} == 0 ]]; then
		#no whitelist specified, bind all devices
		return 1
	fi

	for i in ${PCI_WHITELIST[@]}
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
		if [ -n "$TARGET_USER" ]; then
			chown "$TARGET_USER" "/dev/vfio/$iommu_group"
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

function linux_hugetlbfs_mounts() {
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
		link_bdf=$(basename "$link_name")
		if [ "$link_bdf" = "$1" ]; then
			eval "$2=$dev"
			return
		fi
	done
}

function get_virtio_names_from_bdf {
	blk_devs=`lsblk --nodeps --output NAME`
	virtio_names=''

	for dev in $blk_devs; do
		if readlink "/sys/block/$dev" | grep -q "$1"; then
			virtio_names="$virtio_names $dev"
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
		if pci_can_bind $bdf == "0" ; then
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
			if pci_can_bind $bdf == "0" ; then
				echo "Skipping un-whitelisted I/OAT device at $bdf"
				continue
			fi
			linux_bind_driver "$bdf" "$driver_name"
		done
	done
	rm $TMP

	# virtio
	TMP=`mktemp`
	#collect all the device_id info of virtio devices.
	grep "PCI_DEVICE_ID_VIRTIO" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	for dev_id in `cat $TMP`; do
		for bdf in $(iter_pci_dev_id 1af4 $dev_id); do
			if pci_can_bind $bdf == "0" ; then
				echo "Skipping un-whitelisted Virtio device at $bdf"
				continue
			fi
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

function cleanup_linux {
	shopt -s extglob nullglob
	dirs_to_clean=""
	dirs_to_clean="$(echo {/var/run,/tmp}/dpdk/spdk{,_pid}+([0-9])) "
	if [[ -d $XDG_RUNTIME_DIR && $XDG_RUNTIME_DIR != *" "* ]]; then
		dirs_to_clean+="$(readlink -e assert_not_empty $XDG_RUNTIME_DIR/dpdk/spdk{,_pid}+([0-9]) || true) "
	fi

	files_to_clean=""
	for dir in $dirs_to_clean; do
		files_to_clean+="$(echo $dir/*) "
	done
	shopt -u extglob nullglob

	files_to_clean+="$(echo /dev/shm/* | egrep '(spdk_tgt|iscsi|vhost|nvmf|rocksdb|bdevtest|bdevperf)_trace|spdk_iscsi_conns' || true) "
	files_to_clean="$(readlink -e assert_not_empty $files_to_clean || true)"
	if [[ -z "$files_to_clean" ]]; then
		echo "Clean"
		return 0;
	fi

	shopt -s extglob
	for fd_dir in $(echo /proc/+([0-9])); do
		opened_files+="$(readlink -e assert_not_empty $fd_dir/fd/* || true)"
	done
	shopt -u extglob

	if [[ -z "$opened_files" ]]; then
		echo "Can't get list of opened files!"
		exit 1
	fi

	echo 'Cleaning'
	for f in $files_to_clean; do
		if ! echo "$opened_files" | egrep -q "^$f\$"; then
			echo "Removing:    $f"
			rm $f
		else
			echo "Still open: $f"
		fi
	done

	for dir in $dirs_to_clean; do
	if ! echo "$opened_files" | egrep -q "^$dir\$"; then
		echo "Removing:    $dir"
		rmdir $dir
	else
		echo "Still open: $dir"
	fi
	done
	echo "Clean"

	unset dirs_to_clean files_to_clean opened_files
}

function configure_linux {
	configure_linux_pci
	hugetlbfs_mounts=$(linux_hugetlbfs_mounts)

	if [ -z "$hugetlbfs_mounts" ]; then
		hugetlbfs_mounts=/mnt/huge
		echo "Mounting hugetlbfs at $hugetlbfs_mounts"
		mkdir -p "$hugetlbfs_mounts"
		mount -t hugetlbfs nodev "$hugetlbfs_mounts"
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
		if [ -n "$TARGET_USER" ]; then
			for mount in $hugetlbfs_mounts; do
				chown "$TARGET_USER" "$mount"
				chmod g+w "$mount"
			done
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
	check_for_driver nvme
	driver_loaded=$?
	set -e
	for bdf in $(iter_pci_class_code 01 08 02); do
		if pci_can_bind $bdf == "0" ; then
			echo "Skipping un-whitelisted NVMe controller $blkname ($bdf)"
			continue
		fi
		if [ $driver_loaded -ne 0 ]; then
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
	check_for_driver ioatdma
	driver_loaded=$?
	set -e
	for dev_id in `cat $TMP`; do
		for bdf in $(iter_pci_dev_id 8086 $dev_id); do
			if pci_can_bind $bdf == "0" ; then
				echo "Skipping un-whitelisted I/OAT device at $bdf"
				continue
			fi
			if [ $driver_loaded -ne 0 ]; then
				linux_bind_driver "$bdf" ioatdma
			else
				linux_unbind_driver "$bdf"
			fi
		done
	done
	rm $TMP

	# virtio
	TMP=`mktemp`
	#collect all the device_id info of virtio devices.
	grep "PCI_DEVICE_ID_VIRTIO" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}' > $TMP

	# TODO: check if virtio-pci is loaded first and just unbind if it is not loaded
	# Requires some more investigation - for example, some kernels do not seem to have
	#  virtio-pci but just virtio_scsi instead.  Also need to make sure we get the
	#  underscore vs. dash right in the virtio_scsi name.
	modprobe virtio-pci || true
	for dev_id in `cat $TMP`; do
		for bdf in $(iter_pci_dev_id 1af4 $dev_id); do
			if pci_can_bind $bdf == "0" ; then
				echo "Skipping un-whitelisted Virtio device at $bdf"
				continue
			fi
			linux_bind_driver "$bdf" virtio-pci
		done
	done
	rm $TMP

	echo "1" > "/sys/bus/pci/rescan"
}

function reset_linux {
	reset_linux_pci
	for mount in $(linux_hugetlbfs_mounts); do
		rm -f "$mount"/spdk*map_*
	done
	rm -f /run/.spdk*
}

function status_linux {
	echo "Hugepages"
	printf "%-6s %10s %8s / %6s\n" "node" "hugesize"  "free" "total"

	numa_nodes=0
	shopt -s nullglob
	for path in /sys/devices/system/node/node?/hugepages/hugepages-*/; do
		numa_nodes=$((numa_nodes + 1))
		free_pages=`cat $path/free_hugepages`
		all_pages=`cat $path/nr_hugepages`

		[[ $path =~ (node[0-9]+)/hugepages/hugepages-([0-9]+kB) ]]

		node=${BASH_REMATCH[1]}
		huge_size=${BASH_REMATCH[2]}

		printf "%-6s %10s %8s / %6s\n" $node $huge_size $free_pages $all_pages
	done
	shopt -u nullglob

	# fall back to system-wide hugepages
	if [ "$numa_nodes" = "0" ]; then
		free_pages=`grep HugePages_Free /proc/meminfo | awk '{ print $2 }'`
		all_pages=`grep HugePages_Total /proc/meminfo | awk '{ print $2 }'`
		node="-"
		huge_size="$HUGEPGSZ"

		printf "%-6s %10s %8s / %6s\n" $node $huge_size $free_pages $all_pages
	fi

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

	#collect all the device_id info of virtio devices.
	TMP=`grep "PCI_DEVICE_ID_VIRTIO" $rootdir/include/spdk/pci_ids.h \
	| awk -F"x" '{print $2}'`
	echo -e "BDF\t\tNuma Node\tDriver Name\t\tDevice Name"
	for dev_id in $TMP; do
		for bdf in $(iter_pci_dev_id 1af4 $dev_id); do
			driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}'`
			node=`cat /sys/bus/pci/devices/$bdf/numa_node`;
			blknames=''
			get_virtio_names_from_bdf "$bdf" blknames
			echo -e "$bdf\t$node\t\t$driver\t\t$blknames"
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
	configure_freebsd_pci
	# If contigmem is already loaded but the HUGEMEM specified doesn't match the
	#  previous value, unload contigmem so that we can reload with the new value.
	if kldstat -q -m contigmem; then
		if [ `kenv hw.contigmem.num_buffers` -ne "$((HUGEMEM / 256))" ]; then
			kldunload contigmem.ko
		fi
	fi
	if ! kldstat -q -m contigmem; then
		kenv hw.contigmem.num_buffers=$((HUGEMEM / 256))
		kenv hw.contigmem.buffer_size=$((256 * 1024 * 1024))
		kldload contigmem.ko
	fi
}

function reset_freebsd {
	kldunload contigmem.ko || true
	kldunload nic_uio.ko || true
}

mode=$1

if [ -z "$mode" ]; then
	mode="config"
fi

: ${HUGEMEM:=2048}
: ${PCI_WHITELIST:=""}

if [ -n "$NVME_WHITELIST" ]; then
	PCI_WHITELIST="$PCI_WHITELIST $NVME_WHITELIST"
fi

if [ -n "$SKIP_PCI" ]; then
	PCI_WHITELIST="none"
fi

declare -a PCI_WHITELIST=(${PCI_WHITELIST})

if [ -z "$TARGET_USER" ]; then
	TARGET_USER="$SUDO_USER"
	if [ -z "$TARGET_USER" ]; then
		TARGET_USER=`logname 2>/dev/null` || true
	fi
fi

if [ `uname` = Linux ]; then
	HUGEPGSZ=$(( `grep Hugepagesize /proc/meminfo | cut -d : -f 2 | tr -dc '0-9'` ))
	HUGEPGSZ_MB=$(( $HUGEPGSZ / 1024 ))
	: ${NRHUGE=$(( (HUGEMEM + HUGEPGSZ_MB - 1) / HUGEPGSZ_MB ))}

	if [ "$mode" == "config" ]; then
		configure_linux
	elif [ "$mode" == "cleanup" ]; then
		cleanup_linux
	elif [ "$mode" == "reset" ]; then
		reset_linux
	elif [ "$mode" == "status" ]; then
		status_linux
	elif [ "$mode" == "help" ]; then
		usage $0
	else
		usage $0 "Invalid argument '$mode'"
	fi
else
	if [ "$mode" == "config" ]; then
		configure_freebsd
	elif [ "$mode" == "reset" ]; then
		reset_freebsd
	elif [ "$mode" == "cleanup" ]; then
		echo "setup.sh cleanup function not yet supported on $(uname)"
	elif [ "$mode" == "status" ]; then
		echo "setup.sh status function not yet supported on $(uname)"
	elif [ "$mode" == "help" ]; then
		usage $0
	else
		usage $0 "Invalid argument '$mode'"
	fi
fi
