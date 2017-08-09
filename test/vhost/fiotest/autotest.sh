#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

dry_run=false
no_shutdown=false
fio_bin="fio"
fio_jobs="$BASE_DIR/fio_jobs/"
test_type=spdk_vhost_scsi
reuse_vms=false
force_build=false
vms=()
used_vms=""
disk_split=""
x=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                print help and exit"
	echo "    --test-type=TYPE      Perform specified test:"
	echo "                          virtio - test host virtio-scsi-pci using file as disk image"
	echo "                          kernel_vhost - use kernel driver vhost-scsi"
	echo "                          spdk_vhost_scsi - use spdk vhost scsi"
	echo "                          spdk_vhost_blk - use spdk vhost block"
	echo "-x                        set -x for script debug"
	echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
	echo "    --qemu-src=QEMU_DIR   Location of the QEMU sources"
	echo "    --dpdk-src=DPDK_DIR   Location of the DPDK sources"
	echo "    --fio-jobs=           Fio configs to use for tests. Can point to a directory or"
	echo "                          can point to a directory with regex mask, example: ./dir/*.job"
	echo "                          All VMs will run the same fio job when FIO executes."
	echo "                          (no unique jobs for specific VMs)"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	echo "    --dry-run             Don't perform any tests, run only and wait for enter to terminate"
	echo "    --no-shutdown         Don't shutdown at the end but leave envirionment working"
	echo "    --force-build         Force SPDK rebuild with the specified DPDK path."
	echo "    --vm=NUM[,OS][,DISKS] VM configuration. This parameter might be used more than once:"
	echo "                          NUM - VM number (mandatory)"
	echo "                          OS - VM os disk path (optional)"
	echo "                          DISKS - VM os test disks/devices path (virtio - optional, kernel_vhost - mandatory)"
	echo "                          If test-type=spdk_vhost_blk then each disk can have additional size parameter, e.g."
	echo "                          --vm=X,os.qcow,DISK_size_35G; unit can be M or G; default - 20G"
	echo "    --disk-split          By default all test types execute fio jobs on all disks which are available on guest"
	echo "                          system. Use this option if only some of the disks should be used for testing."
	echo "                          Example: --disk-split=4,1-3 will result in VM 1 using it's first disk (ex. /dev/sda)"
	echo "                          and VM 2 using it's disks 1-3 (ex. /dev/sdb, /dev/sdc, /dev/sdd)"
	exit 0
}

#default raw file is NVMe drive

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			fio-bin=*) fio_bin="--fio-bin=${OPTARG#*=}" ;;
			qemu-src=*) QEMU_SRC_DIR="${OPTARG#*=}" ;;
			dpdk-src=*) DPDK_SRC_DIR="${OPTARG#*=}" ;;
			fio-jobs=*) fio_jobs="${OPTARG#*=}" ;;
			dry-run) dry_run=true ;;
			no-shutdown) no_shutdown=true ;;
			test-type=*) test_type="${OPTARG#*=}" ;;
			force-build) force_build=true ;;
			vm=*) vms+=("${OPTARG#*=}") ;;
			disk-split=*) disk_split="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done
shift $(( OPTIND - 1 ))

if [[ -d "$fio_jobs" ]]; then
	fio_jobs="$fio_jobs/*.job"
fi

. $BASE_DIR/common.sh

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

echo "==============="
echo "INFO: checking qemu"

if [[ ! -x $INSTALL_DIR/bin/qemu-system-x86_64 ]]; then
	echo "INFO: can't find $INSTALL_DIR/bin/qemu-system-x86_64 - building and installing"

	if [[ ! -d $QEMU_SRC_DIR ]]; then
		echo "ERROR: Cannot find qemu source in $QEMU_SRC_DIR"
		exit 1
	else
		echo "INFO: qemu source exists $QEMU_SRC_DIR - building"
		qemu_build_and_install
	fi
fi

echo "==============="
echo ""
echo "INFO: checking spdk"
echo ""

if [[ ! -x $SPDK_BUILD_DIR/app/vhost/vhost ]] || $force_build ; then
	echo "INFO: $SPDK_BUILD_DIR/app/vhost/vhost - building and installing"
	spdk_build_and_install
fi

vm_kill_all

if [[ $test_type =~ "spdk_vhost" ]]; then
	echo "==============="
	echo ""
	echo "INFO: running SPDK"
	echo ""
	$BASE_DIR/run_vhost.sh $x --work-dir=$TEST_DIR
	echo
fi

echo "==============="
echo ""
echo "Setting up VM"
echo ""

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py "
rpc_py+="-s 127.0.0.1 "

for vm_conf in ${vms[@]}; do
	IFS=',' read -ra conf <<< "$vm_conf"
	setup_cmd="$BASE_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
	if [[ x"${conf[0]}" == x"" ]] || ! assert_number ${conf[0]}; then
		echo "ERROR: invalid VM configuration syntax $vm_conf"
		exit 1;
	fi

	# Sanity check if VM is not defined twice
	for vm_num in $used_vms; do
		if [[ $vm_num -eq ${conf[0]} ]]; then
			echo "ERROR: VM$vm_num defined more than twice ( $(printf "'%s' " "${vms[@]}"))!"
			exit 1
		fi
	done

	setup_cmd+=" -f ${conf[0]}"
	used_vms+=" ${conf[0]}"
	[[ x"${conf[1]}" != x"" ]] && setup_cmd+=" --os=${conf[1]}"
	[[ x"${conf[2]}" != x"" ]] && setup_cmd+=" --disk=${conf[2]}"

	if [[ $test_type =~ "spdk_vhost" ]]; then

		echo "INFO: Adding device via RPC ..."
		echo ""

		while IFS=':' read -ra disks; do
			for disk in "${disks[@]}"; do
				if [[ "$test_type" == "spdk_vhost_blk" ]]; then
					disk=${disk%%_*}
					echo "INFO: Creating vhost block controller naa.$disk.${conf[0]} with device $disk"
					$rpc_py construct_vhost_blk_controller naa.$disk.${conf[0]} $disk
				else
					echo "INFO: Trying to remove nonexistent controller"
					if $rpc_py remove_vhost_scsi_controller unk0 > /dev/null; then
						echo "ERROR: Removing nonexistent controller succeeded, but it shouldn't"
						false
					fi
					echo "INFO: Creating controller naa.$disk.${conf[0]}"
					$rpc_py construct_vhost_scsi_controller naa.$disk.${conf[0]}

					echo "INFO: Adding initial device (0) to naa.$disk.${conf[0]}"
					$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $disk

					echo "INFO: Trying to remove nonexistent device on existing controller"
					if $rpc_py remove_vhost_scsi_dev naa.$disk.${conf[0]} 1 > /dev/null; then
						echo "ERROR: Removing nonexistent device (1) from controller naa.$disk.${conf[0]} succeeded, but it shouldn't"
						false
					fi

					echo "INFO: Trying to remove existing device from a controller"
					$rpc_py remove_vhost_scsi_dev naa.$disk.${conf[0]} 0

					echo "INFO: Trying to remove a just-deleted device from a controller again"
					if $rpc_py remove_vhost_scsi_dev naa.$disk.${conf[0]} 0 > /dev/null; then
						echo "ERROR: Removing device 0 from controller naa.$disk.${conf[0]} succeeded, but it shouldn't"
						false
					fi

					echo "INFO: Re-adding device 0 to naa.$disk.${conf[0]}"
					$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $disk
				fi
			done

			echo "INFO: Trying to create scsi controller with incorrect cpumask"
			if $rpc_py construct_vhost_scsi_controller vhost.invalid.cpumask --cpumask 9; then
				echo "ERROR: Creating scsi controller with incorrect cpumask succeeded, but it shouldn't"
				false
			fi

			echo "INFO: Trying to remove device from nonexistent scsi controller"
			if $rpc_py remove_vhost_scsi_dev vhost.nonexistent.name 0; then
				echo "ERROR: Removing device from nonexistent scsi controller succeeded, but it shouldn't"
				false
			fi

			echo "INFO: Trying to add device to nonexistent scsi controller"
			if $rpc_py add_vhost_scsi_lun vhost.nonexistent.name 0 Malloc0; then
				echo "ERROR: Adding device to nonexistent scsi controller succeeded, but it shouldn't"
				false
			fi

			echo "INFO: Trying to create scsi controller with incorrect name"
			if $rpc_py construct_vhost_scsi_controller .; then
				echo "ERROR: Creating scsi controller with incorrect name succeeded, but it shouldn't"
				false
			fi

			echo "INFO: Trying to create block controller with incorrect cpumask"
			if $rpc_py construct_vhost_blk_controller vhost.invalid.cpumask  Malloc0 --cpumask 9; then
				echo "ERROR: Creating block controller with incorrect cpumask succeeded, but it shouldn't"
				false
			fi

			echo "INFO: Trying to remove nonexistent block controller"
			if $rpc_py remove_vhost_block_dev vhost.nonexistent.name 0; then
				echo "ERROR: Removing nonexistent block controller succeeded, but it shouldn't"
				false
			fi

			echo "INFO: Trying to create block controller with incorrect name"
			if $rpc_py construct_vhost_scsi_controller . Malloc0; then
				echo "ERROR: Creating block controller with incorrect name succeeded, but it shouldn't"
				false
			fi
		done <<< "${conf[2]}"
		unset IFS;
		$rpc_py get_vhost_controllers
	fi
	$setup_cmd
done

# Run everything
$BASE_DIR/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
vm_wait_for_boot 600 $used_vms

if [[ $test_type == "spdk_vhost_scsi" ]]; then
	for vm_conf in ${vms[@]}; do
		IFS=',' read -ra conf <<< "$vm_conf"
		while IFS=':' read -ra disks; do
			for disk in "${disks[@]}"; do
				echo "INFO: Hotdetach test. Trying to remove existing device from a controller naa.$disk.${conf[0]}"
				$rpc_py remove_vhost_scsi_dev naa.$disk.${conf[0]} 0

				sleep 0.1

				echo "INFO: Hotattach test. Re-adding device 0 to naa.$disk.${conf[0]}"
				$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $disk
			done
		done <<< "${conf[2]}"
		unset IFS;
	done
fi

sleep 0.1

echo "==============="
echo ""
echo "INFO: Testing..."

echo "INFO: Running fio jobs ..."
run_fio="python $BASE_DIR/run_fio.py "
run_fio+="$fio_bin "
run_fio+="--job-file="
for job in $fio_jobs; do
	run_fio+="$job,"
done
run_fio="${run_fio::-1}"
run_fio+=" "
run_fio+="--out=$TEST_DIR "

if [[ ! $disk_split == '' ]]; then
	run_fio+="--split-disks=$disk_split "
fi

# Check if all VM have disk in tha same location
DISK=""

for vm_num in $used_vms; do
	vm_dir=$VM_BASE_DIR/$vm_num

	qemu_mask_param="VM_${vm_num}_qemu_mask"

	host_name="VM-$vm_num-${!qemu_mask_param}"
	echo "INFO: Setting up hostname: $host_name"
	vm_ssh $vm_num "hostname $host_name"
	vm_start_fio_server $fio_bin $readonly $vm_num

	if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_num
		vm_reset_scsi_devices $vm_num $SCSI_DISK
	elif [[ "$test_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm_num
	fi

	run_fio+="127.0.0.1:$(cat $vm_dir/fio_socket):"
	for disk in $SCSI_DISK; do
		run_fio+="/dev/$disk:"
	done
	run_fio="${run_fio::-1}"
	run_fio+=","
done

run_fio="${run_fio%,}"
run_fio+=" "
run_fio="${run_fio::-1}"

echo -e "$run_fio"

if $dry_run; then
	read -p "Enter to kill evething" xx
	sleep 3
	at_app_exit
	exit 0
fi

$run_fio

if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
	for vm_num in $used_vms; do
	vm_reset_scsi_devices $vm_num $SCSI_DISK
	done
fi

if ! $no_shutdown; then
	echo "==============="
	echo "INFO: APP EXITING"
	echo "INFO: killing all VMs"
	vm_shutdown_all
	echo "INFO: waiting 2 seconds to let all VMs die"
	sleep 2
	if [[ $test_type =~ "spdk_vhost" ]]; then
		echo "INFO: Removing vhost devices & controllers via RPC ..."
		for vm_conf in ${vms[@]}; do
			IFS=',' read -ra conf <<< "$vm_conf"

			while IFS=':' read -ra disks; do
				for disk in "${disks[@]}"; do
					disk=${disk%%_*}
					echo "INFO: Removing all vhost devices from controller naa.$disk.${conf[0]}"
					if [[ "$test_type" == "spdk_vhost_blk" ]]; then
						$rpc_py remove_vhost_blk_controller naa.$disk.${conf[0]}
					else
						$rpc_py remove_vhost_scsi_dev naa.$disk.${conf[0]} 0
						$rpc_py remove_vhost_scsi_controller naa.$disk.${conf[0]}
					fi
				done
			done <<< "${conf[2]}"
		done
	fi
	echo "INFO: Testing done -> shutting down"
	echo "INFO: killing vhost app"
	spdk_vhost_kill

	echo "INFO: EXIT DONE"
	echo "==============="
else
	echo "==============="
	echo
	echo "INFO: Leaving environment working!"
	echo ""
	echo "==============="
fi
