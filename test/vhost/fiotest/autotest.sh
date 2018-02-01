#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

dry_run=false
no_shutdown=false
fio_bin=""
remote_fio_bin=""
fio_jobs=""
test_type=spdk_vhost_scsi
reuse_vms=false
vms=()
used_vms=""
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
	echo "    --fio-job=            Fio config to use for test."
	echo "                          All VMs will run the same fio job when FIO executes."
	echo "                          (no unique jobs for specific VMs)"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	echo "    --dry-run             Don't perform any tests, run only and wait for enter to terminate"
	echo "    --no-shutdown         Don't shutdown at the end but leave envirionment working"
	echo "    --vm=NUM[,OS][,DISKS] VM configuration. This parameter might be used more than once:"
	echo "                          NUM - VM number (mandatory)"
	echo "                          OS - VM os disk path (optional)"
	echo "                          DISKS - VM os test disks/devices path (virtio - optional, kernel_vhost - mandatory)"
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
			fio-job=*) fio_job="${OPTARG#*=}" ;;
			dry-run) dry_run=true ;;
			no-shutdown) no_shutdown=true ;;
			test-type=*) test_type="${OPTARG#*=}" ;;
			vm=*) vms+=("${OPTARG#*=}") ;;
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

if [[ ! -r "$fio_job" ]]; then
	fail "no fio job file specified"
fi

. $COMMON_DIR/common.sh

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

vm_kill_all

if [[ $test_type =~ "spdk_vhost" ]]; then
	notice "==============="
	notice ""
	notice "running SPDK"
	notice ""
	spdk_vhost_run --conf-path=$BASE_DIR
	notice ""
fi

notice "==============="
notice ""
notice "Setting up VM"
notice ""

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

for vm_conf in ${vms[@]}; do
	IFS=',' read -ra conf <<< "$vm_conf"
	if [[ x"${conf[0]}" == x"" ]] || ! assert_number ${conf[0]}; then
		fail "invalid VM configuration syntax $vm_conf"
	fi

	# Sanity check if VM is not defined twice
	for vm_num in $used_vms; do
		if [[ $vm_num -eq ${conf[0]} ]]; then
			fail "VM$vm_num defined more than twice ( $(printf "'%s' " "${vms[@]}"))!"
		fi
	done

	used_vms+=" ${conf[0]}"

	if [[ $test_type =~ "spdk_vhost" ]]; then

		notice "Adding device via RPC ..."

		while IFS=':' read -ra disks; do
			for disk in "${disks[@]}"; do
				if [[ "$test_type" == "spdk_vhost_blk" ]]; then
					disk=${disk%%_*}
					notice "Creating vhost block controller naa.$disk.${conf[0]} with device $disk"
					$rpc_py construct_vhost_blk_controller naa.$disk.${conf[0]} $disk
				else
					notice "Trying to remove nonexistent controller"
					if $rpc_py remove_vhost_controller unk0 > /dev/null; then
						error "Removing nonexistent controller succeeded, but it shouldn't"
					fi
					notice "Creating controller naa.$disk.${conf[0]}"
					$rpc_py construct_vhost_scsi_controller naa.$disk.${conf[0]}

					notice "Adding initial device (0) to naa.$disk.${conf[0]}"
					$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $disk

					notice "Trying to remove nonexistent device on existing controller"
					if $rpc_py remove_vhost_scsi_target naa.$disk.${conf[0]} 1 > /dev/null; then
						error "Removing nonexistent device (1) from controller naa.$disk.${conf[0]} succeeded, but it shouldn't"
					fi

					notice "Trying to remove existing device from a controller"
					$rpc_py remove_vhost_scsi_target naa.$disk.${conf[0]} 0

					notice "Trying to remove a just-deleted device from a controller again"
					if $rpc_py remove_vhost_scsi_target naa.$disk.${conf[0]} 0 > /dev/null; then
						error "Removing device 0 from controller naa.$disk.${conf[0]} succeeded, but it shouldn't"
					fi

					notice "Re-adding device 0 to naa.$disk.${conf[0]}"
					$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $disk
				fi
			done

			notice "Trying to create scsi controller with incorrect cpumask"
			if $rpc_py construct_vhost_scsi_controller vhost.invalid.cpumask --cpumask 0x2; then
				error "Creating scsi controller with incorrect cpumask succeeded, but it shouldn't"
			fi

			notice "Trying to remove device from nonexistent scsi controller"
			if $rpc_py remove_vhost_scsi_target vhost.nonexistent.name 0; then
				error "Removing device from nonexistent scsi controller succeeded, but it shouldn't"
			fi

			notice "Trying to add device to nonexistent scsi controller"
			if $rpc_py add_vhost_scsi_lun vhost.nonexistent.name 0 Malloc0; then
				error "Adding device to nonexistent scsi controller succeeded, but it shouldn't"
			fi

			notice "Trying to create scsi controller with incorrect name"
			if $rpc_py construct_vhost_scsi_controller .; then
				error "Creating scsi controller with incorrect name succeeded, but it shouldn't"
			fi

			notice "Trying to create block controller with incorrect cpumask"
			if $rpc_py construct_vhost_blk_controller vhost.invalid.cpumask  Malloc0 --cpumask 0x2; then
				error "Creating block controller with incorrect cpumask succeeded, but it shouldn't"
			fi

			notice "Trying to remove nonexistent block controller"
			if $rpc_py remove_vhost_controller vhost.nonexistent.name; then
				error "Removing nonexistent block controller succeeded, but it shouldn't"
			fi

			notice "Trying to create block controller with incorrect name"
			if $rpc_py construct_vhost_blk_controller . Malloc0; then
				error "Creating block controller with incorrect name succeeded, but it shouldn't"
			fi
		done <<< "${conf[2]}"
		unset IFS;
		$rpc_py get_vhost_controllers
	fi

	setup_cmd="vm_setup --force=${conf[0]} --disk-type=$test_type"
	[[ x"${conf[1]}" != x"" ]] && setup_cmd+=" --os=${conf[1]}"
	[[ x"${conf[2]}" != x"" ]] && setup_cmd+=" --disks=${conf[2]}"

	$setup_cmd
done

# Run everything
vm_run $used_vms
vm_wait_for_boot 600 $used_vms

if [[ $test_type == "spdk_vhost_scsi" ]]; then
	for vm_conf in ${vms[@]}; do
		IFS=',' read -ra conf <<< "$vm_conf"
		while IFS=':' read -ra disks; do
			for disk in "${disks[@]}"; do
				notice "Hotdetach test. Trying to remove existing device from a controller naa.$disk.${conf[0]}"
				$rpc_py remove_vhost_scsi_target naa.$disk.${conf[0]} 0

				sleep 0.1

				notice "Hotattach test. Re-adding device 0 to naa.$disk.${conf[0]}"
				$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $disk
			done
		done <<< "${conf[2]}"
		unset IFS;
	done
fi

sleep 0.1

notice "==============="
notice ""
notice "Testing..."

notice "Running fio jobs ..."

# Check if all VM have disk in tha same location
DISK=""

fio_disks=""
for vm_num in $used_vms; do
	vm_dir=$VM_BASE_DIR/$vm_num

	qemu_mask_param="VM_${vm_num}_qemu_mask"

	host_name="VM-$vm_num-${!qemu_mask_param}"
	notice "Setting up hostname: $host_name"
	vm_ssh $vm_num "hostname $host_name"
	vm_start_fio_server $fio_bin $readonly $vm_num

	if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_num
		#vm_reset_scsi_devices $vm_num $SCSI_DISK
	elif [[ "$test_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm_num
	fi

	fio_disks+=" --vm=${vm_num}$(printf ':/dev/%s' $SCSI_DISK)"
done

if $dry_run; then
	read -p "Enter to kill evething" xx
	sleep 3
	at_app_exit
	exit 0
fi

run_fio $fio_bin --job-file="$fio_job" --out="$TEST_DIR/fio_results" $fio_disks

if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
	for vm_num in $used_vms; do
	vm_reset_scsi_devices $vm_num $SCSI_DISK
	done
fi

if ! $no_shutdown; then
	notice "==============="
	notice "APP EXITING"
	notice "killing all VMs"
	vm_shutdown_all
	notice "waiting 2 seconds to let all VMs die"
	sleep 2
	if [[ $test_type =~ "spdk_vhost" ]]; then
		notice "Removing vhost devices & controllers via RPC ..."
		for vm_conf in ${vms[@]}; do
			IFS=',' read -ra conf <<< "$vm_conf"

			while IFS=':' read -ra disks; do
				for disk in "${disks[@]}"; do
					disk=${disk%%_*}
					notice "Removing all vhost devices from controller naa.$disk.${conf[0]}"
					if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
						$rpc_py remove_vhost_scsi_target naa.$disk.${conf[0]} 0
					fi

					$rpc_py remove_vhost_controller naa.$disk.${conf[0]}
				done
			done <<< "${conf[2]}"
		done
	fi
	notice "Testing done -> shutting down"
	notice "killing vhost app"
	spdk_vhost_kill

	notice "EXIT DONE"
	notice "==============="
else
	notice "==============="
	notice ""
	notice "Leaving environment working!"
	notice ""
	notice "==============="
fi
