#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#
set -e
shopt -s nullglob extglob

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
	echo "                  For NUMA systems, the hugepages will be distributed on node0 by"
	echo "                  default."
	echo "HUGE_EVEN_ALLOC   If set to 'yes', hugepages will be evenly distributed across all"
	echo "                  system's NUMA nodes (effectively ignoring anything set in HUGENODE)."
	echo "                  Uses kernel's default for hugepages size."
	echo "NRHUGE            Number of hugepages to allocate. This variable overwrites HUGEMEM."
	echo "HUGENODE          Specific NUMA node to allocate hugepages on. Multiple nodes can be"
	echo "                  separated with comas. By default, NRHUGE will be applied on each node."
	echo "                  Hugepages can be defined per node with e.g.:"
	echo "                  HUGENODE='nodes_hp[0]=2048,nodes_hp[1]=512,2' - this will allocate"
	echo "                  2048 pages for node0, 512 for node1 and default NRHUGE for node2."
	echo "HUGEPGSZ          Size of the hugepages to use in kB. If not set, kernel's default"
	echo "                  setting is used."
	echo "SHRINK_HUGE       If set to 'yes', hugepages allocation won't be skipped in case"
	echo "                  number of requested hugepages is lower from what's already"
	echo "                  allocated. Doesn't apply when HUGE_EVEN_ALLOC is in use."
	echo "CLEAR_HUGE        If set to 'yes', the attempt to remove hugepages from all nodes will"
	echo "                  be made prior to allocation".
	echo "PCI_ALLOWED"
	echo "PCI_BLOCKED       Whitespace separated list of PCI devices (NVMe, I/OAT, VMD, Virtio)."
	echo "                  Each device must be specified as a full PCI address."
	echo "                  E.g. PCI_ALLOWED=\"0000:01:00.0 0000:02:00.0\""
	echo "                  To block all PCI devices: PCI_ALLOWED=\"none\""
	echo "                  To allow all PCI devices except 0000:01:00.0: PCI_BLOCKED=\"0000:01:00.0\""
	echo "                  To allow only PCI device 0000:01:00.0: PCI_ALLOWED=\"0000:01:00.0\""
	echo "                  If PCI_ALLOWED and PCI_BLOCKED are empty or unset, all PCI devices"
	echo "                  will be bound."
	echo "                  Each device in PCI_BLOCKED will be ignored (driver won't be changed)."
	echo "                  PCI_BLOCKED has precedence over PCI_ALLOWED."
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
	if [[ -z $1 ]]; then
		return 0
	fi

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

function check_for_driver_freebsd() {
	# Check if dpdk drivers (nic_uio, contigmem) are in the kernel's module path.
	local search_paths path driver
	IFS=";" read -ra search_paths < <(kldconfig -rU)

	for driver in contigmem.ko nic_uio.ko; do
		for path in "${search_paths[@]}"; do
			[[ -f $path/$driver ]] && continue 2
		done
		return 1
	done
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

	if [[ $driver_name == "$old_driver_name" ]]; then
		pci_dev_echo "$bdf" "Already using the $old_driver_name driver"
		return 0
	fi

	if [[ $old_driver_name != "no driver" ]]; then
		echo "$bdf" > "/sys/bus/pci/devices/$bdf/driver/unbind"
	fi

	pci_dev_echo "$bdf" "$old_driver_name -> $driver_name"

	if [[ $driver_name == "none" ]]; then
		return 0
	fi

	local probe_attempts=0
	echo "$driver_name" > "/sys/bus/pci/devices/$bdf/driver_override"
	while ! echo "$bdf" > "/sys/bus/pci/drivers_probe" && ((probe_attempts++ < 10)); do
		pci_dev_echo "$bdf" "failed to bind to $driver_name, retrying ($probe_attempts)"
		sleep 0.5
	done 2> /dev/null

	echo "" > "/sys/bus/pci/devices/$bdf/driver_override"

	if [[ ! -e /sys/bus/pci/drivers/$driver_name/$bdf ]]; then
		pci_dev_echo "$bdf" "failed to bind to $driver_name, aborting"
		return 1
	fi

	iommu_group=$(basename $(readlink -f /sys/bus/pci/devices/$bdf/iommu_group))
	if [ -e "/dev/vfio/$iommu_group" ]; then
		if [ -n "$TARGET_USER" ]; then
			chown "$TARGET_USER" "/dev/vfio/$iommu_group"
		fi
	fi
}

function linux_unbind_driver() {
	local bdf="$1"
	local old_driver_name=${drivers_d["$bdf"]:-no driver}

	if [[ $old_driver_name == "no driver" ]]; then
		pci_dev_echo "$bdf" "Not bound to any driver"
		return 0
	fi

	if [[ -e /sys/bus/pci/drivers/$old_driver_name ]]; then
		echo "$bdf" > "/sys/bus/pci/drivers/$old_driver_name/unbind"
		echo "" > "/sys/bus/pci/devices/$bdf/driver_override"
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
		fi
	done
}

function get_used_bdf_block_devs() {
	local bdf=$1
	local blocks block blockp dev mount holder
	local used

	hash lsblk &> /dev/null || return 1
	blocks=($(get_block_dev_from_bdf "$bdf"))

	for block in "${blocks[@]}"; do
		# Check if the device is hold by some other, regardless if it's mounted
		# or not.
		for holder in "/sys/class/block/$block"*/holders/*; do
			[[ -e $holder ]] || continue
			blockp=${holder%/holders*} blockp=${blockp##*/}
			if [[ -e $holder/slaves/$blockp ]]; then
				used+=("holder@$blockp:${holder##*/}")
			fi
		done
		while read -r dev mount; do
			if [[ -e $mount ]]; then
				used+=("mount@$block:$dev")
			fi
		done < <(lsblk -l -n -o NAME,MOUNTPOINT "/dev/$block")
		if ((${#used[@]} == 0)); then
			# Make sure we check if there's any valid data present on the target device
			# regardless if it's being actively used or not. This is mainly done to make
			# sure we don't miss more complex setups like ZFS pools, etc.
			if block_in_use "$block" > /dev/null; then
				used+=("data@$block")
			fi
		fi
	done

	if ((${#used[@]} > 0)); then
		printf '%s\n' "${used[@]}"
	fi
}

function collect_devices() {
	# NVMe, IOAT, DSA, IAA, VIRTIO, VMD

	local ids dev_type dev_id bdf bdfs in_use driver

	ids+="PCI_DEVICE_ID_INTEL_IOAT"
	ids+="|PCI_DEVICE_ID_INTEL_DSA"
	ids+="|PCI_DEVICE_ID_INTEL_IAA"
	ids+="|PCI_DEVICE_ID_VIRTIO"
	ids+="|PCI_DEVICE_ID_INTEL_VMD"
	ids+="|SPDK_PCI_CLASS_NVME"

	local -gA nvme_d ioat_d dsa_d iaa_d virtio_d vmd_d all_devices_d drivers_d

	while read -r _ dev_type dev_id; do
		bdfs=(${pci_bus_cache["0x8086:$dev_id"]})
		[[ $dev_type == *NVME* ]] && bdfs=(${pci_bus_cache["$dev_id"]})
		[[ $dev_type == *VIRT* ]] && bdfs=(${pci_bus_cache["0x1af4:$dev_id"]})
		[[ $dev_type =~ (NVME|IOAT|DSA|IAA|VIRTIO|VMD) ]] && dev_type=${BASH_REMATCH[1],,}
		for bdf in "${bdfs[@]}"; do
			in_use=0
			if [[ $1 != status ]]; then
				if ! pci_can_use "$bdf"; then
					pci_dev_echo "$bdf" "Skipping denied controller at $bdf"
					in_use=1
				fi
				if [[ $dev_type == nvme || $dev_type == virtio ]]; then
					if ! verify_bdf_block_devs "$bdf"; then
						in_use=1
					fi
				fi
				if [[ $dev_type == vmd ]]; then
					if [[ $PCI_ALLOWED != *"$bdf"* ]]; then
						pci_dev_echo "$bdf" "Skipping not allowed VMD controller at $bdf"
						in_use=1
					elif [[ " ${drivers_d[*]} " =~ "nvme" ]]; then
						if [[ "${DRIVER_OVERRIDE}" != "none" ]]; then
							if [ "$mode" == "config" ]; then
								cat <<- MESSAGE
									Binding new driver to VMD device. If there are NVMe SSDs behind the VMD endpoint
									which are attached to the kernel NVMe driver,the binding process may go faster
									if you first run this script with DRIVER_OVERRIDE="none" to unbind only the
									NVMe SSDs, and then run again to unbind the VMD devices."
								MESSAGE
							fi
						fi
					fi
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
	local drivers driver

	if [[ -e /sys/bus/pci/devices/$bdf/modalias ]] \
		&& drivers=($(modprobe -R "$(< "/sys/bus/pci/devices/$bdf/modalias")")); then
		# Pick first entry in case multiple aliases are bound to a driver.
		driver=$(readlink -f "/sys/module/${drivers[0]}/drivers/pci:"*)
		driver=${driver##*/}
	else
		[[ -n ${nvme_d["$bdf"]} ]] && driver=nvme
		[[ -n ${ioat_d["$bdf"]} ]] && driver=ioatdma
		[[ -n ${dsa_d["$bdf"]} ]] && driver=dsa
		[[ -n ${iaa_d["$bdf"]} ]] && driver=iaa
		[[ -n ${virtio_d["$bdf"]} ]] && driver=virtio-pci
		[[ -n ${vmd_d["$bdf"]} ]] && driver=vmd
	fi 2> /dev/null
	echo "$driver"
}

function verify_bdf_block_devs() {
	local bdf=$1
	local blknames
	blknames=($(get_used_bdf_block_devs "$bdf")) || return 1

	if ((${#blknames[@]} > 0)); then
		local IFS=","
		pci_dev_echo "$bdf" "Active devices: ${blknames[*]}, so not binding PCI dev"
		return 1
	fi
}

function configure_linux_pci() {
	local driver_path=""
	driver_name=""
	igb_uio_fallback=""

	if [[ -r "$rootdir/dpdk/build-tmp/kernel/linux/igb_uio/igb_uio.ko" ]]; then
		# igb_uio is a common driver to override with and it depends on uio.
		modprobe uio || true
		if ! check_for_driver igb_uio || insmod "$rootdir/dpdk/build-tmp/kernel/linux/igb_uio/igb_uio.ko"; then
			igb_uio_fallback="$rootdir/dpdk/build-tmp/kernel/linux/igb_uio/igb_uio.ko"
		fi
	fi

	if [[ "${DRIVER_OVERRIDE}" == "none" ]]; then
		driver_name=none
	elif [[ -n "${DRIVER_OVERRIDE}" ]]; then
		driver_path="$DRIVER_OVERRIDE"
		driver_name="${DRIVER_OVERRIDE##*/}"
		# modprobe and the sysfs don't use the .ko suffix.
		driver_name=${driver_name%.ko}
		# path = name -> there is no path
		if [[ "$driver_path" = "$driver_name" ]]; then
			driver_path=""
		fi
	elif [[ -n "$(ls /sys/kernel/iommu_groups)" || (-e \
	/sys/module/vfio/parameters/enable_unsafe_noiommu_mode && \
	"$(cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode)" == "Y") ]]; then
		driver_name=vfio-pci
		# Just in case, attempt to load VFIO_IOMMU_TYPE1 module into the kernel - this
		# should be done automatically by modprobe since this particular module should
		# be a part of vfio-pci dependencies, however, on some distros, it seems that
		# it's not the case. See #1689.
		if modinfo vfio_iommu_type1 > /dev/null; then
			modprobe vfio_iommu_type1
		fi
	elif ! check_for_driver uio_pci_generic || modinfo uio_pci_generic > /dev/null 2>&1; then
		driver_name=uio_pci_generic
	elif [[ -e $igb_uio_fallback ]]; then
		driver_path="$igb_uio_fallback"
		driver_name="igb_uio"
		echo "WARNING: uio_pci_generic not detected - using $driver_name"
	else
		echo "No valid drivers found [vfio-pci, uio_pci_generic, igb_uio]. Please enable one of the kernel modules."
		return 1
	fi

	# modprobe assumes the directory of the module. If the user passes in a path, we should use insmod
	if [[ $driver_name != "none" ]]; then
		if [[ -n "$driver_path" ]]; then
			insmod $driver_path || true
		else
			modprobe $driver_name
		fi
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
	local dirs_to_clean=() files_to_clean=() opened_files=() file_locks=()
	local match_spdk="spdk_tgt|iscsi|vhost|nvmf|rocksdb|bdevio|bdevperf|vhost_fuzz|nvme_fuzz|accel_perf|bdev_svc"

	dirs_to_clean=({/var/run,/tmp}/dpdk/spdk{,_pid}+([0-9]))
	if [[ -d $XDG_RUNTIME_DIR ]]; then
		dirs_to_clean+=("$XDG_RUNTIME_DIR/dpdk/spdk"{,_pid}+([0-9]))
	fi

	for dir in "${dirs_to_clean[@]}"; do
		files_to_clean+=("$dir/"*)
	done
	file_locks+=(/var/tmp/spdk_pci_lock*)
	file_locks+=(/var/tmp/spdk_cpu_lock*)

	files_to_clean+=(/dev/shm/@(@($match_spdk)_trace|spdk_iscsi_conns)*)
	files_to_clean+=("${file_locks[@]}")

	# This may fail in case path that readlink attempts to resolve suddenly
	# disappears (as it may happen with terminating processes).
	opened_files+=($(readlink -f /proc/+([0-9])/fd/+([0-9]))) || true

	if ((${#opened_files[@]} == 0)); then
		echo "Can't get list of opened files!"
		exit 1
	fi

	echo 'Cleaning'
	for f in "${files_to_clean[@]}"; do
		[[ -e $f ]] || continue
		if [[ ${opened_files[*]} != *"$f"* ]]; then
			echo "Removing:    $f"
			rm $f
		else
			echo "Still open: $f"
		fi
	done

	for dir in "${dirs_to_clean[@]}"; do
		[[ -d $dir ]] || continue
		if [[ ${opened_files[*]} != *"$dir"* ]]; then
			echo "Removing:    $dir"
			rmdir $dir
		else
			echo "Still open: $dir"
		fi
	done
	echo "Clean"
}

check_hugepages_alloc() {
	local hp_int=$1
	local allocated_hugepages

	allocated_hugepages=$(< "$hp_int")

	if ((NRHUGE <= allocated_hugepages)) && [[ $SHRINK_HUGE != yes ]]; then
		echo "INFO: Requested $NRHUGE hugepages but $allocated_hugepages already allocated ${2:+on node$2}"
		return 0
	fi

	echo $((NRHUGE < 0 ? 0 : NRHUGE)) > "$hp_int"

	allocated_hugepages=$(< "$hp_int")
	if ((allocated_hugepages < NRHUGE)); then
		cat <<- ERROR

			## ERROR: requested $NRHUGE hugepages but $allocated_hugepages could be allocated ${2:+on node$2}.
			## Memory might be heavily fragmented. Please try flushing the system cache, or reboot the machine.
		ERROR
		return 1
	fi
}

clear_hugepages() { echo 0 > /proc/sys/vm/nr_hugepages; }

configure_linux_hugepages() {
	local node system_nodes
	local nodes_to_use nodes_hp

	if [[ $CLEAR_HUGE == yes ]]; then
		clear_hugepages
	fi

	if [[ $HUGE_EVEN_ALLOC == yes ]]; then
		clear_hugepages
		check_hugepages_alloc /proc/sys/vm/nr_hugepages
		return 0
	fi

	for node in /sys/devices/system/node/node*; do
		[[ -e $node ]] || continue
		nodes[${node##*node}]=$node/hugepages/hugepages-${HUGEPGSZ}kB/nr_hugepages
	done

	if ((${#nodes[@]} == 0)); then
		# No NUMA support? Fallback to common interface
		check_hugepages_alloc /proc/sys/vm/nr_hugepages
		return 0
	fi

	IFS="," read -ra nodes_to_use <<< "$HUGENODE"
	if ((${#nodes_to_use[@]} == 0)); then
		nodes_to_use[0]=0
	fi

	# Align indexes with node ids
	for node in "${!nodes_to_use[@]}"; do
		if [[ ${nodes_to_use[node]} =~ ^nodes_hp\[[0-9]+\]= ]]; then
			eval "${nodes_to_use[node]}"
		elif [[ ${nodes_to_use[node]} =~ ^[0-9]+$ ]]; then
			nodes_hp[nodes_to_use[node]]=$NRHUGE
		fi
	done

	for node in "${!nodes_hp[@]}"; do
		if [[ -z ${nodes[node]} ]]; then
			echo "Node $node doesn't exist, ignoring" >&2
			continue
		fi
		NRHUGE=${nodes_hp[node]:-$NRHUGE} check_hugepages_alloc "${nodes[node]}" "$node"
	done
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

	configure_linux_hugepages

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

	if [ $(uname -i) == "x86_64" ] && [ ! -e /dev/cpu/0/msr ]; then
		# Some distros build msr as a module.  Make sure it's loaded to ensure
		#  DPDK can easily figure out the TSC rate rather than relying on 100ms
		#  sleeps.
		modprobe msr &> /dev/null || true
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

		driver=$(collect_driver "$bdf")
		if [[ -n $driver ]] && ! check_for_driver "$driver"; then
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
		for hp in "$mount"/spdk*map_*; do
			flock -n "$hp" true && rm -f "$hp"
		done
	done
	rm -f /run/.spdk*
}

function status_linux() {
	echo "Hugepages" >&2
	printf "%-6s %10s %8s / %6s\n" "node" "hugesize" "free" "total" >&2

	numa_nodes=0
	for path in /sys/devices/system/node/node*/hugepages/hugepages-*/; do
		numa_nodes=$((numa_nodes + 1))
		free_pages=$(cat $path/free_hugepages)
		all_pages=$(cat $path/nr_hugepages)

		[[ $path =~ (node[0-9]+)/hugepages/hugepages-([0-9]+kB) ]]

		node=${BASH_REMATCH[1]}
		huge_size=${BASH_REMATCH[2]}

		printf "%-6s %10s %8s / %6s\n" $node $huge_size $free_pages $all_pages
	done

	# fall back to system-wide hugepages
	if [ "$numa_nodes" = "0" ]; then
		free_pages=$(grep HugePages_Free /proc/meminfo | awk '{ print $2 }')
		all_pages=$(grep HugePages_Total /proc/meminfo | awk '{ print $2 }')
		node="-"
		huge_size="$HUGEPGSZ"

		printf "%-6s %10s %8s / %6s\n" $node $huge_size $free_pages $all_pages
	fi

	printf '\n%-8s %-15s %-6s %-6s %-7s %-16s %-10s %s\n' \
		"Type" "BDF" "Vendor" "Device" "NUMA" "Driver" "Device" "Block devices" >&2

	sorted_bdfs=($(printf '%s\n' "${!all_devices_d[@]}" | sort))

	for bdf in "${sorted_bdfs[@]}"; do
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
			name=$(ls /sys/bus/pci/devices/$bdf/nvme)
		else
			name="-"
		fi

		if [[ -n ${nvme_d["$bdf"]} || -n ${virtio_d["$bdf"]} ]]; then
			blknames=($(get_block_dev_from_bdf "$bdf"))
		else
			blknames=("-")
		fi

		desc=""
		desc=${desc:-${nvme_d["$bdf"]:+NVMe}}
		desc=${desc:-${ioat_d["$bdf"]:+I/OAT}}
		desc=${desc:-${dsa_d["$bdf"]:+DSA}}
		desc=${desc:-${iaa_d["$bdf"]:+IAA}}
		desc=${desc:-${virtio_d["$bdf"]:+virtio}}
		desc=${desc:-${vmd_d["$bdf"]:+VMD}}

		printf '%-8s %-15s %-6s %-6s %-7s %-16s %-10s %s\n' \
			"$desc" "$bdf" "${pci_ids_vendor["$bdf"]#0x}" "${pci_ids_device["$bdf"]#0x}" \
			"$node" "${driver:--}" "${name:-}" "${blknames[*]:--}"
	done
}

function status_freebsd() {
	local pci

	status_print() (
		local type=$1
		local dev driver

		shift

		for pci; do
			printf '%-8s %-15s %-6s %-6s %-16s\n' \
				"$type" \
				"$pci" \
				"${pci_ids_vendor["$pci"]}" \
				"${pci_ids_device["$pci"]}" \
				"${pci_bus_driver["$pci"]}"
		done | sort -k2,2
	)

	local contigmem=present
	local contigmem_buffer_size
	local contigmem_num_buffers

	if ! kldstat -q -m contigmem; then
		contigmem="not present"
	fi
	if ! contigmem_buffer_size=$(kenv hw.contigmem.buffer_size 2> /dev/null); then
		contigmem_buffer_size="not set"
	fi
	if ! contigmem_num_buffers=$(kenv hw.contigmem.num_buffers 2> /dev/null); then
		contigmem_num_buffers="not set"
	fi

	cat <<- BSD_INFO
		Contigmem ($contigmem)
		Buffer Size: $contigmem_buffer_size
		Num Buffers: $contigmem_num_buffers

	BSD_INFO

	printf '\n%-8s %-15s %-6s %-6s %-16s\n' \
		"Type" "BDF" "Vendor" "Device" "Driver" >&2

	status_print "NVMe" "${!nvme_d[@]}"
	status_print "I/OAT" "${!ioat_d[@]}"
	status_print "DSA" "${!dsa_d[@]}"
	status_print "IAA" "${!iaa_d[@]}"
	status_print "VMD" "${!vmd_d[@]}"
}

function configure_freebsd_pci() {
	local BDFS

	BDFS+=("${!nvme_d[@]}")
	BDFS+=("${!ioat_d[@]}")
	BDFS+=("${!dsa_d[@]}")
	BDFS+=("${!iaa_d[@]}")
	BDFS+=("${!vmd_d[@]}")

	# Drop the domain part from all the addresses
	BDFS=("${BDFS[@]#*:}")

	local IFS=","
	kldunload nic_uio.ko || true
	kenv hw.nic_uio.bdfs="${BDFS[*]}"
	kldload nic_uio.ko
}

function configure_freebsd() {
	if ! check_for_driver_freebsd; then
		echo "DPDK drivers (contigmem and/or nic_uio) are missing, aborting" >&2
		return 1
	fi
	configure_freebsd_pci
	# If contigmem is already loaded but the HUGEMEM specified doesn't match the
	#  previous value, unload contigmem so that we can reload with the new value.
	if kldstat -q -m contigmem; then
		# contigmem may be loaded, but the kernel environment doesn't have to
		# be necessarily set at this point. If it isn't, kenv will fail to
		# pick up the hw. options. Handle it.
		if ! contigmem_num_buffers=$(kenv hw.contigmem.num_buffers); then
			contigmem_num_buffers=-1
		fi 2> /dev/null
		if ((contigmem_num_buffers != HUGEMEM / 256)); then
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
: ${PCI_ALLOWED:=""}
: ${PCI_BLOCKED:=""}

if [ -n "$NVME_ALLOWED" ]; then
	PCI_ALLOWED="$PCI_ALLOWED $NVME_ALLOWED"
fi

if [ -n "$SKIP_PCI" ]; then
	PCI_ALLOWED="none"
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
	if [[ -n $HUGEPGSZ && ! -e /sys/kernel/mm/hugepages/hugepages-${HUGEPGSZ}kB ]]; then
		echo "${HUGEPGSZ}kB is not supported by the running kernel, ignoring" >&2
		unset -v HUGEPGSZ
	fi

	HUGEPGSZ=${HUGEPGSZ:-$(grep Hugepagesize /proc/meminfo | cut -d : -f 2 | tr -dc '0-9')}
	HUGEPGSZ_MB=$((HUGEPGSZ / 1024))
	: ${NRHUGE=$(((HUGEMEM + HUGEPGSZ_MB - 1) / HUGEPGSZ_MB))}

	if [ "$mode" == "config" ]; then
		configure_linux
	elif [ "$mode" == "cleanup" ]; then
		cleanup_linux
		clear_hugepages
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
