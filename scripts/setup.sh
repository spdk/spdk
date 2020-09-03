#!/usr/bin/env bash

set -e

os=$(uname -s)

if [[ $os != Linux && $os != FreeBSD ]]; then
	echo "Not supported platform ($os), aborting"
	exit 1
fi

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

function usage() {
	if [[ $os == Linux ]]; then
		options="[config|reset|status|cleanup|help]"
	else
		options="[config|reset|help]"
	fi

	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Helper script for allocating hugepages and binding NVMe, I/OAT, VMD and Virtio devices"
	echo "to a generic VFIO kernel driver. If VFIO is not available on the system, this script"
	echo "will fall back to UIO. NVMe and Virtio devices with active mountpoints will be ignored."
	echo "All hugepage operations use default hugepage size on the system (hugepagesz)."
	echo "Usage: $(basename $1) $options"
	echo
	echo "$options - as following:"
	echo "config            Default mode. Allocate hugepages and bind PCI devices."
	if [[ $os == Linux ]]; then
		echo "cleanup           Remove any orphaned files that can be left in the system after SPDK application exit"
	fi
	echo "reset             Rebind PCI devices back to their original drivers."
	echo "                  Also cleanup any leftover spdk files/resources."
	echo "                  Hugepage memory size will remain unchanged."
	if [[ $os == Linux ]]; then
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
	echo "PCI_WHITELIST"
	echo "PCI_BLACKLIST     Whitespace separated list of PCI devices (NVMe, I/OAT, VMD, Virtio)."
	echo "                  Each device must be specified as a full PCI address."
	echo "                  E.g. PCI_WHITELIST=\"0000:01:00.0 0000:02:00.0\""
	echo "                  To blacklist all PCI devices use a non-valid address."
	echo "                  E.g. PCI_WHITELIST=\"none\""
	echo "                  If PCI_WHITELIST and PCI_BLACKLIST are empty or unset, all PCI devices"
	echo "                  will be bound."
	echo "                  Each device in PCI_BLACKLIST will be ignored (driver won't be changed)."
	echo "                  PCI_BLACKLIST has precedence over PCI_WHITELIST."
	echo "TARGET_USER       User that will own hugepage mountpoint directory and vfio groups."
	echo "                  By default the current user will be used."
	echo "DRIVER_OVERRIDE   Disable automatic vfio-pci/uio_pci_generic selection and forcefully"
	echo "                  bind devices to the given driver."
	echo "                  E.g. DRIVER_OVERRIDE=uio_pci_generic or DRIVER_OVERRIDE=/home/public/dpdk/build/kmod/igb_uio.ko"
	echo "PCI_BLOCK_SYNC_ON_RESET"
	echo "                  If set in the environment, the attempt to wait for block devices associated"
	echo "                  with given PCI device will be made upon reset"
	exit 0
}

# In monolithic kernels the lsmod won't work. So
# back that with a /sys/modules. We also check
# /sys/bus/pci/drivers/ as neither lsmod nor /sys/modules might
# contain needed info (like in Fedora-like OS).
function check_for_driver() {
	if lsmod | grep -q ${1//-/_}; then
		return 1
	fi

	if [[ -d /sys/module/${1} || -d \
		/sys/module/${1//-/_} || -d \
		/sys/bus/pci/drivers/${1} || -d \
		/sys/bus/pci/drivers/${1//-/_} ]]; then
		return 2
	fi
	return 0
}

function pci_dev_echo() {
	local bdf="$1"
	shift
	echo "$bdf (${pci_ids_vendor["$bdf"]#0x} ${pci_ids_device["$bdf"]#0x}): $*"
}

function linux_bind_driver() {
	bdf="$1"
	driver_name="$2"
	old_driver_name=${drivers_d["$bdf"]:-no driver}
	ven_dev_id="${pci_ids_vendor["$bdf"]#0x} ${pci_ids_device["$bdf"]#0x}"

	if [[ $driver_name == "$old_driver_name" ]]; then
		pci_dev_echo "$bdf" "Already using the $old_driver_name driver"
		return 0
	fi

	echo "$ven_dev_id" > "/sys/bus/pci/devices/$bdf/driver/remove_id" 2> /dev/null || true
	echo "$bdf" > "/sys/bus/pci/devices/$bdf/driver/unbind"

	pci_dev_echo "$bdf" "$old_driver_name -> $driver_name"

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
	local bdf="$1"
	local ven_dev_id
	ven_dev_id="${pci_ids_vendor["$bdf"]#0x} ${pci_ids_device["$bdf"]#0x}"
	local old_driver_name=${drivers_d["$bdf"]:-no driver}

	if [[ -e /sys/bus/pci/drivers/$old_driver_name ]]; then
		echo "$ven_dev_id" > "/sys/bus/pci/drivers/$old_driver_name/remove_id" 2> /dev/null || true
		echo "$bdf" > "/sys/bus/pci/drivers/$old_driver_name/unbind"
	fi

	pci_dev_echo "$bdf" "$old_driver_name -> no driver"
}

function linux_hugetlbfs_mounts() {
	mount | grep ' type hugetlbfs ' | awk '{ print $3 }'
}

function get_block_dev_from_bdf() {
	local bdf=$1
	local block

	for block in /sys/block/*; do
		if [[ $(readlink -f "$block/device") == *"/$bdf/"* ]]; then
			echo "${block##*/}"
			return 0
		fi
	done
}

function get_mounted_part_dev_from_bdf_block() {
	local bdf=$1
	local blocks block part

	blocks=($(get_block_dev_from_bdf "$bdf"))

	for block in "${blocks[@]}"; do
		for part in "/sys/block/$block/$block"*; do
			[[ -b /dev/${part##*/} ]] || continue
			if [[ $(< /proc/self/mountinfo) == *" $(< "$part/dev") "* ]]; then
				echo "${part##*/}"
			fi
		done
	done
}

function collect_devices() {
	# NVMe, IOAT, IDXD, VIRTIO, VMD

	local ids dev_type dev_id bdf bdfs in_use driver

	ids+="PCI_DEVICE_ID_INTEL_IOAT"
	ids+="|PCI_DEVICE_ID_INTEL_IDXD"
	ids+="|PCI_DEVICE_ID_VIRTIO"
	ids+="|PCI_DEVICE_ID_INTEL_VMD"
	ids+="|SPDK_PCI_CLASS_NVME"

	local -gA nvme_d ioat_d idxd_d virtio_d vmd_d all_devices_d drivers_d

	while read -r _ dev_type dev_id; do
		bdfs=(${pci_bus_cache["0x8086:$dev_id"]})
		[[ $dev_type == *NVME* ]] && bdfs=(${pci_bus_cache["$dev_id"]})
		[[ $dev_type == *VIRT* ]] && bdfs=(${pci_bus_cache["0x1af4:$dev_id"]})
		[[ $dev_type =~ (NVME|IOAT|IDXD|VIRTIO|VMD) ]] && dev_type=${BASH_REMATCH[1],,}
		for bdf in "${bdfs[@]}"; do
			in_use=0
			if [[ $1 != status ]] && ! pci_can_use "$bdf"; then
				pci_dev_echo "$bdf" "Skipping un-whitelisted controller at $bdf"
				in_use=1
			fi
			if [[ $1 != status ]] && [[ $dev_type == nvme || $dev_type == virtio ]]; then
				if ! verify_bdf_mounts "$bdf"; then
					in_use=1
				fi
			fi
			eval "${dev_type}_d[$bdf]=$in_use"
			all_devices_d["$bdf"]=$in_use
			if [[ -e /sys/bus/pci/devices/$bdf/driver ]]; then
				driver=$(readlink -f "/sys/bus/pci/devices/$bdf/driver")
				drivers_d["$bdf"]=${driver##*/}
			fi
		done
	done < <(grep -E "$ids" "$rootdir/include/spdk/pci_ids.h")
}

function collect_driver() {
	local bdf=$1
	local override_driver=$2
	local drivers driver

	[[ -e /sys/bus/pci/devices/$bdf/modalias ]] || return 1
	if drivers=($(modprobe -R "$(< "/sys/bus/pci/devices/$bdf/modalias")")); then
		# Pick first entry in case multiple aliases are bound to a driver.
		driver=$(readlink -f "/sys/module/${drivers[0]}/drivers/pci:"*)
		driver=${driver##*/}
	else
		driver=$override_driver
	fi 2> /dev/null
	echo "$driver"
}

function verify_bdf_mounts() {
	local bdf=$1
	local blknames=($(get_mounted_part_dev_from_bdf_block "$bdf"))

	if ((${#blknames[@]} > 0)); then
		for name in "${blknames[@]}"; do
			pci_dev_echo "$bdf" "Active mountpoints on /dev/$name, so not binding PCI dev"
		done
		return 1
	fi
}

function configure_linux_pci() {
	local driver_path=""
	driver_name=""
	if [[ -n "${DRIVER_OVERRIDE}" ]]; then
		driver_path="$DRIVER_OVERRIDE"
		driver_name="${DRIVER_OVERRIDE##*/}"
		# modprobe and the sysfs don't use the .ko suffix.
		driver_name=${driver_name%.ko}
		# path = name -> there is no path
		if [[ "$driver_path" = "$driver_name" ]]; then
			driver_path=""
		fi
		# igb_uio is a common driver to override with and it depends on uio.
		if [[ "$driver_name" = "igb_uio" ]]; then
			modprobe uio
		fi
	elif [[ -n "$(ls /sys/kernel/iommu_groups)" || (-e \
	/sys/module/vfio/parameters/enable_unsafe_noiommu_mode && \
	"$(cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode)" == "Y") ]]; then
		driver_name=vfio-pci
	elif modinfo uio_pci_generic > /dev/null 2>&1; then
		driver_name=uio_pci_generic
	elif [[ -r "$rootdir/dpdk/build/kmod/igb_uio.ko" ]]; then
		driver_path="$rootdir/dpdk/build/kmod/igb_uio.ko"
		driver_name="igb_uio"
		modprobe uio
		echo "WARNING: uio_pci_generic not detected - using $driver_name"
	else
		echo "No valid drivers found [vfio-pci, uio_pci_generic, igb_uio]. Please either enable the vfio-pci or uio_pci_generic"
		echo "kernel modules, or have SPDK build the igb_uio driver by running ./configure --with-igb-uio-driver and recompiling."
		return 1
	fi

	# modprobe assumes the directory of the module. If the user passes in a path, we should use insmod
	if [[ -n "$driver_path" ]]; then
		insmod $driver_path || true
	else
		modprobe $driver_name
	fi

	for bdf in "${!all_devices_d[@]}"; do
		if ((all_devices_d["$bdf"] == 0)); then
			if [[ -n ${nvme_d["$bdf"]} ]]; then
				# Some nvme controllers may take significant amount of time while being
				# unbound from the driver. Put that task into background to speed up the
				# whole process. Currently this is done only for the devices bound to the
				# nvme driver as other, i.e., ioatdma's, trigger a kernel BUG when being
				# unbound in parallel. See https://bugzilla.kernel.org/show_bug.cgi?id=209041.
				linux_bind_driver "$bdf" "$driver_name" &
			else
				linux_bind_driver "$bdf" "$driver_name"
			fi
		fi
	done
	wait

	echo "1" > "/sys/bus/pci/rescan"
}

function cleanup_linux() {
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

	files_to_clean+="$(ls -1 /dev/shm/* \
		| grep -E '(spdk_tgt|iscsi|vhost|nvmf|rocksdb|bdevio|bdevperf|vhost_fuzz|nvme_fuzz)_trace|spdk_iscsi_conns' || true) "
	files_to_clean="$(readlink -e assert_not_empty $files_to_clean || true)"
	if [[ -z "$files_to_clean" ]]; then
		echo "Clean"
		return 0
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
		if ! echo "$opened_files" | grep -E -q "^$f\$"; then
			echo "Removing:    $f"
			rm $f
		else
			echo "Still open: $f"
		fi
	done

	for dir in $dirs_to_clean; do
		if ! echo "$opened_files" | grep -E -q "^$dir\$"; then
			echo "Removing:    $dir"
			rmdir $dir
		else
			echo "Still open: $dir"
		fi
	done
	echo "Clean"

	unset dirs_to_clean files_to_clean opened_files
}

function configure_linux() {
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
	allocated_hugepages=$(cat $hugepages_target)
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

			MEMLOCK_AMNT=$(su "$TARGET_USER" -c "ulimit -l")
			if [[ $MEMLOCK_AMNT != "unlimited" ]]; then
				MEMLOCK_MB=$((MEMLOCK_AMNT / 1024))
				cat <<- MEMLOCK
					"$TARGET_USER" user memlock limit: $MEMLOCK_MB MB

					This is the maximum amount of memory you will be
					able to use with DPDK and VFIO if run as user "$TARGET_USER".
					To change this, please adjust limits.conf memlock limit for user "$TARGET_USER".
				MEMLOCK
				if ((MEMLOCK_AMNT < 65536)); then
					echo ""
					echo "## WARNING: memlock limit is less than 64MB"
					echo -n "## DPDK with VFIO may not be able to initialize "
					echo "if run as user \"$TARGET_USER\"."
				fi
			fi
		fi
	fi

	if [ ! -f /dev/cpu/0/msr ]; then
		# Some distros build msr as a module.  Make sure it's loaded to ensure
		#  DPDK can easily figure out the TSC rate rather than relying on 100ms
		#  sleeps.
		modprobe msr || true
	fi
}

function reset_linux_pci() {
	# virtio
	# TODO: check if virtio-pci is loaded first and just unbind if it is not loaded
	# Requires some more investigation - for example, some kernels do not seem to have
	#  virtio-pci but just virtio_scsi instead.  Also need to make sure we get the
	#  underscore vs. dash right in the virtio_scsi name.
	modprobe virtio-pci || true
	for bdf in "${!all_devices_d[@]}"; do
		((all_devices_d["$bdf"] == 0)) || continue

		[[ -n ${nvme_d["$bdf"]} ]] && fallback_driver=nvme
		[[ -n ${ioat_d["$bdf"]} ]] && fallback_driver=ioatdma
		[[ -n ${idxd_d["$bdf"]} ]] && fallback_driver=idxd
		[[ -n ${virtio_d["$bdf"]} ]] && fallback_driver=virtio-pci
		[[ -n ${vmd_d["$bdf"]} ]] && fallback_driver=vmd
		driver=$(collect_driver "$bdf" "$fallback_driver")

		if ! check_for_driver "$driver"; then
			linux_bind_driver "$bdf" "$driver"
		else
			linux_unbind_driver "$bdf"
		fi
	done

	echo "1" > "/sys/bus/pci/rescan"
}

function reset_linux() {
	reset_linux_pci
	for mount in $(linux_hugetlbfs_mounts); do
		rm -f "$mount"/spdk*map_*
	done
	rm -f /run/.spdk*
}

function status_linux() {
	echo "Hugepages"
	printf "%-6s %10s %8s / %6s\n" "node" "hugesize" "free" "total"

	numa_nodes=0
	shopt -s nullglob
	for path in /sys/devices/system/node/node*/hugepages/hugepages-*/; do
		numa_nodes=$((numa_nodes + 1))
		free_pages=$(cat $path/free_hugepages)
		all_pages=$(cat $path/nr_hugepages)

		[[ $path =~ (node[0-9]+)/hugepages/hugepages-([0-9]+kB) ]]

		node=${BASH_REMATCH[1]}
		huge_size=${BASH_REMATCH[2]}

		printf "%-6s %10s %8s / %6s\n" $node $huge_size $free_pages $all_pages
	done
	shopt -u nullglob

	# fall back to system-wide hugepages
	if [ "$numa_nodes" = "0" ]; then
		free_pages=$(grep HugePages_Free /proc/meminfo | awk '{ print $2 }')
		all_pages=$(grep HugePages_Total /proc/meminfo | awk '{ print $2 }')
		node="-"
		huge_size="$HUGEPGSZ"

		printf "%-6s %10s %8s / %6s\n" $node $huge_size $free_pages $all_pages
	fi

	echo -e "\nBDF\t\tVendor\tDevice\tNUMA\tDriver\t\tDevice name\n"
	echo "NVMe devices"

	for bdf in "${!nvme_d[@]}"; do
		driver=${drivers_d["$bdf"]}
		if [ "$numa_nodes" = "0" ]; then
			node="-"
		else
			node=$(cat /sys/bus/pci/devices/$bdf/numa_node)
			if ((node == -1)); then
				node=unknown
			fi
		fi
		if [ "$driver" = "nvme" ] && [ -d /sys/bus/pci/devices/$bdf/nvme ]; then
			name="\t"$(ls /sys/bus/pci/devices/$bdf/nvme)
		else
			name="-"
		fi
		echo -e "$bdf\t${pci_ids_vendor["$bdf"]#0x}\t${pci_ids_device["$bdf"]#0x}\t$node\t${driver:--}\t\t$name"
	done

	echo ""
	echo "I/OAT Engine"

	for bdf in "${!ioat_d[@]}"; do
		driver=${drivers_d["$bdf"]}
		if [ "$numa_nodes" = "0" ]; then
			node="-"
		else
			node=$(cat /sys/bus/pci/devices/$bdf/numa_node)
			if ((node == -1)); then
				node=unknown
			fi
		fi
		echo -e "$bdf\t${pci_ids_vendor["$bdf"]#0x}\t${pci_ids_device["$bdf"]#0x}\t$node\t${driver:--}"
	done

	echo ""
	echo "IDXD Engine"

	for bdf in "${!idxd_d[@]}"; do
		driver=${drivers_d["$bdf"]}
		if [ "$numa_nodes" = "0" ]; then
			node="-"
		else
			node=$(cat /sys/bus/pci/devices/$bdf/numa_node)
		fi
		echo -e "$bdf\t${pci_ids_vendor["$bdf"]#0x}\t${pci_ids_device["$bdf"]#0x}\t$node\t${driver:--}"
	done

	echo ""
	echo "virtio"

	for bdf in "${!virtio_d[@]}"; do
		driver=${drivers_d["$bdf"]}
		if [ "$numa_nodes" = "0" ]; then
			node="-"
		else
			node=$(cat /sys/bus/pci/devices/$bdf/numa_node)
			if ((node == -1)); then
				node=unknown
			fi
		fi
		blknames=($(get_mounted_part_dev_from_bdf_block "$bdf"))
		echo -e "$bdf\t${pci_ids_vendor["$bdf"]#0x}\t${pci_ids_device["$bdf"]#0x}\t$node\t\t${driver:--}\t\t" "${blknames[@]}"
	done

	echo ""
	echo "VMD"

	for bdf in "${!vmd_d[@]}"; do
		driver=${drivers_d["$bdf"]}
		node=$(cat /sys/bus/pci/devices/$bdf/numa_node)
		if ((node == -1)); then
			node=unknown
		fi
		echo -e "$bdf\t$node\t\t$driver"
	done
}

function status_freebsd() {
	local pci

	status_print() (
		local dev driver

		echo -e "BDF\t\tVendor\tDevice\tDriver"

		for pci; do
			driver=$(pciconf -l "pci$pci")
			driver=${driver%@*}
			printf '%s\t%s\t%s\t%s\n' \
				"$pci" \
				"${pci_ids_vendor["$pci"]}" \
				"${pci_ids_device["$pci"]}" \
				"$driver"
		done
	)

	local contigmem=present
	if ! kldstat -q -m contigmem; then
		contigmem="not present"
	fi

	cat <<- BSD_INFO
		Contigmem ($contigmem)
		Buffer Size: $(kenv hw.contigmem.buffer_size)
		Num Buffers: $(kenv hw.contigmem.num_buffers)

		NVMe devices
		$(status_print "${!nvme_d[@]}")

		I/IOAT DMA
		$(status_print "${!ioat_d[@]}")

		IDXD DMA
		$(status_print "${!idxd_d[@]}")

		VMD
		$(status_print "${!vmd_d[@]}")
	BSD_INFO
}

function configure_freebsd_pci() {
	local BDFS

	BDFS+=("${!nvme_d[@]}")
	BDFS+=("${!ioat_d[@]}")
	BDFS+=("${!idxd_d[@]}")
	BDFS+=("${!vmd_d[@]}")

	# Drop the domain part from all the addresses
	BDFS=("${BDFS[@]#*:}")

	local IFS=","
	kldunload nic_uio.ko || true
	kenv hw.nic_uio.bdfs="${BDFS[*]}"
	kldload nic_uio.ko
}

function configure_freebsd() {
	configure_freebsd_pci
	# If contigmem is already loaded but the HUGEMEM specified doesn't match the
	#  previous value, unload contigmem so that we can reload with the new value.
	if kldstat -q -m contigmem; then
		if [ $(kenv hw.contigmem.num_buffers) -ne "$((HUGEMEM / 256))" ]; then
			kldunload contigmem.ko
		fi
	fi
	if ! kldstat -q -m contigmem; then
		kenv hw.contigmem.num_buffers=$((HUGEMEM / 256))
		kenv hw.contigmem.buffer_size=$((256 * 1024 * 1024))
		kldload contigmem.ko
	fi
}

function reset_freebsd() {
	kldunload contigmem.ko || true
	kldunload nic_uio.ko || true
}

CMD=reset cache_pci_bus

mode=$1

if [ -z "$mode" ]; then
	mode="config"
fi

: ${HUGEMEM:=2048}
: ${PCI_WHITELIST:=""}
: ${PCI_BLACKLIST:=""}

if [ -n "$NVME_WHITELIST" ]; then
	PCI_WHITELIST="$PCI_WHITELIST $NVME_WHITELIST"
fi

if [ -n "$SKIP_PCI" ]; then
	PCI_WHITELIST="none"
fi

if [ -z "$TARGET_USER" ]; then
	TARGET_USER="$SUDO_USER"
	if [ -z "$TARGET_USER" ]; then
		TARGET_USER=$(logname 2> /dev/null) || true
	fi
fi

collect_devices "$mode"

if [[ $mode == reset && $PCI_BLOCK_SYNC_ON_RESET == yes ]]; then
	# Note that this will wait only for the first block device attached to
	# a given storage controller. For nvme this may miss some of the devs
	# in case multiple namespaces are being in place.
	# FIXME: Wait for nvme controller(s) to be in live state and determine
	# number of configured namespaces, build list of potential block devs
	# and pass them to sync_dev_uevents. Is it worth the effort?
	bdfs_to_wait_for=()
	for bdf in "${!all_devices_d[@]}"; do
		((all_devices_d["$bdf"] == 0)) || continue
		if [[ -n ${nvme_d["$bdf"]} || -n ${virtio_d["$bdf"]} ]]; then
			[[ $(collect_driver "$bdf") != "${drivers_d["$bdf"]}" ]] || continue
			bdfs_to_wait_for+=("$bdf")
		fi
	done
	if ((${#bdfs_to_wait_for[@]} > 0)); then
		echo "Waiting for block devices as requested"
		export UEVENT_TIMEOUT=5 DEVPATH_LOOKUP=yes DEVPATH_SUBSYSTEM=pci
		"$rootdir/scripts/sync_dev_uevents.sh" \
			block/disk \
			"${bdfs_to_wait_for[@]}" &
		sync_pid=$!
	fi
fi

if [[ $os == Linux ]]; then
	HUGEPGSZ=$(($(grep Hugepagesize /proc/meminfo | cut -d : -f 2 | tr -dc '0-9')))
	HUGEPGSZ_MB=$((HUGEPGSZ / 1024))
	: ${NRHUGE=$(((HUGEMEM + HUGEPGSZ_MB - 1) / HUGEPGSZ_MB))}

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
		echo "setup.sh cleanup function not yet supported on $os"
	elif [ "$mode" == "status" ]; then
		status_freebsd
	elif [ "$mode" == "help" ]; then
		usage $0
	else
		usage $0 "Invalid argument '$mode'"
	fi
fi

if [[ -e /proc/$sync_pid/status ]]; then
	wait "$sync_pid"
fi
