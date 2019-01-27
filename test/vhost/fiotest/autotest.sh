#!/usr/bin/env bash
set -e
AUTOTEST_BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $AUTOTEST_BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $AUTOTEST_BASE_DIR/../../../../ && pwd)"

dry_run=false
no_shutdown=false
fio_bin=""
remote_fio_bin=""
fio_jobs=""
test_type=spdk_vhost_scsi
reuse_vms=false
vms=()
used_vms=""
barmem=false
io_queues=1
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
	echo "    --barmem              Use memory-backend-file for vhost-user-nvme. Needed for vm kernel older than 4.12"
	echo "    --io_queues           Number of IO queues for the vhost nvme controller"
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
			barmem) barmem=true ;;
			io_queues=*) io_queues="${OPTARG#*=}" ;;
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
	spdk_vhost_run --json-path=$AUTOTEST_BASE_DIR
	notice ""
fi

notice "==============="
notice ""
notice "Setting up VM"
notice ""

rpc_py="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

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
				notice "Create a lvol store on RaidBdev2 and then a lvol bdev on the lvol store"
				if [[ $disk == "RaidBdev2" ]]; then
					ls_guid=$($rpc_py construct_lvol_store RaidBdev2 lvs_0 -c 4194304)
					free_mb=$(get_lvs_free_mb "$ls_guid")
					based_disk=$($rpc_py construct_lvol_bdev -u $ls_guid lbd_0 $free_mb)
				else
					based_disk="$disk"
				fi

				if [[ "$test_type" == "spdk_vhost_blk" ]]; then
					disk=${disk%%_*}
					notice "Creating vhost block controller naa.$disk.${conf[0]} with device $disk"
					$rpc_py construct_vhost_blk_controller naa.$disk.${conf[0]} $based_disk
				elif [[ "$test_type" == "spdk_vhost_nvme" ]]; then
					$rpc_py construct_vhost_nvme_controller naa.$disk.${conf[0]} $io_queues
					$rpc_py add_vhost_nvme_ns naa.$disk.${conf[0]} $disk
				else
					notice "Creating controller naa.$disk.${conf[0]}"
					$rpc_py construct_vhost_scsi_controller naa.$disk.${conf[0]}

					notice "Adding device (0) to naa.$disk.${conf[0]}"
					$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $based_disk
				fi
			done
		done <<< "${conf[2]}"
		unset IFS;
		$rpc_py get_vhost_controllers
	fi

	setup_cmd="vm_setup --force=${conf[0]} --disk-type=$test_type"
	[[ x"${conf[1]}" != x"" ]] && setup_cmd+=" --os=${conf[1]}"
	[[ x"${conf[2]}" != x"" ]] && setup_cmd+=" --disks=${conf[2]}"
	if $barmem; then
		setup_cmd+=" --barmem"
	fi

	$setup_cmd
done

# Run everything
vm_run $used_vms
vm_wait_for_boot 300 $used_vms

if [[ $test_type == "spdk_vhost_scsi" ]]; then
	for vm_conf in ${vms[@]}; do
		IFS=',' read -ra conf <<< "$vm_conf"
		while IFS=':' read -ra disks; do
			for disk in "${disks[@]}"; do
				# For RaidBdev2, the lvol bdev on RaidBdev2 is being used.
				if [[ $disk == "RaidBdev2" ]]; then
					based_disk="lvs_0/lbd_0"
				else
					based_disk="$disk"
				fi
				notice "Hotdetach test. Trying to remove existing device from a controller naa.$disk.${conf[0]}"
				$rpc_py remove_vhost_scsi_target naa.$disk.${conf[0]} 0

				sleep 0.1

				notice "Hotattach test. Re-adding device 0 to naa.$disk.${conf[0]}"
				$rpc_py add_vhost_scsi_lun naa.$disk.${conf[0]} 0 $based_disk
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

	host_name="VM-$vm_num"
	notice "Setting up hostname: $host_name"
	vm_ssh $vm_num "hostname $host_name"
	vm_start_fio_server $fio_bin $readonly $vm_num

	if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_num
		#vm_reset_scsi_devices $vm_num $SCSI_DISK
	elif [[ "$test_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm_num
	elif [[ "$test_type" == "spdk_vhost_nvme" ]]; then
		vm_check_nvme_location $vm_num
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
					if [[ $disk == "RaidBdev2" ]]; then
						notice "Removing lvol bdev and lvol store"
						$rpc_py destroy_lvol_bdev lvs_0/lbd_0
						$rpc_py destroy_lvol_store -l lvs_0
					fi
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
