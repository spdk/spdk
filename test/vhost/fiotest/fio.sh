#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

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
readonly=""
packed=false

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
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
	echo "    --dry-run             Don't perform any tests, run only and wait for enter to terminate"
	echo "    --no-shutdown         Don't shutdown at the end but leave envirionment working"
	echo "    --vm=NUM[,OS][,DISKS] VM configuration. This parameter might be used more than once:"
	echo "                          NUM - VM number (mandatory)"
	echo "                          OS - VM os disk path (optional)"
	echo "                          DISKS - VM os test disks/devices path (virtio - optional, kernel_vhost - mandatory)"
	echo "    --readonly            Use readonly for fio"
	echo "    --packed              Virtqueue format is packed"
	exit 0
}

#default raw file is NVMe drive

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage $0 ;;
				fio-bin=*) fio_bin="--fio-bin=${OPTARG#*=}" ;;
				fio-job=*) fio_job="${OPTARG#*=}" ;;
				dry-run) dry_run=true ;;
				no-shutdown) no_shutdown=true ;;
				test-type=*) test_type="${OPTARG#*=}" ;;
				vm=*) vms+=("${OPTARG#*=}") ;;
				readonly) readonly="--readonly" ;;
				packed) packed=true ;;
				*) usage $0 "Invalid argument '$OPTARG'" ;;
			esac
			;;
		h) usage $0 ;;
		x)
			set -x
			x="-x"
			;;
		*) usage $0 "Invalid argument '$OPTARG'" ;;
	esac
done
shift $((OPTIND - 1))

if [[ ! -r "$fio_job" ]]; then
	fail "no fio job file specified"
fi

vhosttestinit

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

vm_kill_all

if [[ $test_type =~ "spdk_vhost" ]]; then
	notice "==============="
	notice ""
	notice "running SPDK"
	notice ""
	vhost_run 0
	rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
	$rpc_py bdev_split_create Nvme0n1 4
	$rpc_py bdev_malloc_create -b Malloc0 128 4096
	$rpc_py bdev_malloc_create -b Malloc1 128 4096
	$rpc_py bdev_malloc_create -b Malloc2 64 512
	$rpc_py bdev_malloc_create -b Malloc3 64 512
	$rpc_py bdev_malloc_create -b Malloc4 64 512
	$rpc_py bdev_malloc_create -b Malloc5 64 512
	$rpc_py bdev_malloc_create -b Malloc6 64 512
	$rpc_py bdev_raid_create -n RaidBdev0 -z 128 -r 0 -b "Malloc2 Malloc3"
	$rpc_py bdev_raid_create -n RaidBdev1 -z 128 -r 0 -b "Nvme0n1p2 Malloc4"
	$rpc_py bdev_raid_create -n RaidBdev2 -z 128 -r 0 -b "Malloc5 Malloc6"
	$rpc_py vhost_create_scsi_controller --cpumask 0x1 vhost.0
	$rpc_py vhost_scsi_controller_add_target vhost.0 0 Malloc0
	$rpc_py vhost_create_blk_controller --cpumask 0x1 -r vhost.1 Malloc1
	notice ""
fi

notice "==============="
notice ""
notice "Setting up VM"
notice ""

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

for vm_conf in "${vms[@]}"; do
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
					ls_guid=$($rpc_py bdev_lvol_create_lvstore RaidBdev2 lvs_0 -c 4194304)
					free_mb=$(get_lvs_free_mb "$ls_guid")
					based_disk=$($rpc_py bdev_lvol_create -u $ls_guid lbd_0 $free_mb)
				else
					based_disk="$disk"
				fi

				if [[ "$test_type" == "spdk_vhost_blk" ]]; then
					disk=${disk%%_*}
					notice "Creating vhost block controller naa.$disk.${conf[0]} with device $disk"
					$rpc_py vhost_create_blk_controller naa.$disk.${conf[0]} $based_disk
				else
					notice "Creating controller naa.$disk.${conf[0]}"
					$rpc_py vhost_create_scsi_controller naa.$disk.${conf[0]}

					notice "Adding device (0) to naa.$disk.${conf[0]}"
					$rpc_py vhost_scsi_controller_add_target naa.$disk.${conf[0]} 0 $based_disk
				fi
			done
		done <<< "${conf[2]}"
		unset IFS
		$rpc_py vhost_get_controllers
	fi

	setup_cmd="vm_setup --force=${conf[0]} --disk-type=$test_type"
	[[ x"${conf[1]}" != x"" ]] && setup_cmd+=" --os=${conf[1]}"
	[[ x"${conf[2]}" != x"" ]] && setup_cmd+=" --disks=${conf[2]}"

	if [[ "$test_type" == "spdk_vhost_blk" ]] && $packed; then
		setup_cmd+=" --packed"
	fi

	$setup_cmd
done

# Run everything
vm_run $used_vms
vm_wait_for_boot 300 $used_vms

if [[ $test_type == "spdk_vhost_scsi" ]]; then
	for vm_conf in "${vms[@]}"; do
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
				$rpc_py vhost_scsi_controller_remove_target naa.$disk.${conf[0]} 0

				sleep 0.1

				notice "Hotattach test. Re-adding device 0 to naa.$disk.${conf[0]}"
				$rpc_py vhost_scsi_controller_add_target naa.$disk.${conf[0]} 0 $based_disk
			done
		done <<< "${conf[2]}"
		unset IFS
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
	qemu_mask_param="VM_${vm_num}_qemu_mask"

	host_name="VM-$vm_num"
	notice "Setting up hostname: $host_name"
	vm_exec $vm_num "hostname $host_name"
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
	read -r -p "Enter to kill evething" xx
	sleep 3
	at_app_exit
	exit 0
fi

run_fio $fio_bin --job-file="$fio_job" --out="$VHOST_DIR/fio_results" $fio_disks

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
		for vm_conf in "${vms[@]}"; do
			IFS=',' read -ra conf <<< "$vm_conf"

			while IFS=':' read -ra disks; do
				for disk in "${disks[@]}"; do
					disk=${disk%%_*}
					notice "Removing all vhost devices from controller naa.$disk.${conf[0]}"
					if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
						$rpc_py vhost_scsi_controller_remove_target naa.$disk.${conf[0]} 0
					fi

					$rpc_py vhost_delete_controller naa.$disk.${conf[0]}
					if [[ $disk == "RaidBdev2" ]]; then
						notice "Removing lvol bdev and lvol store"
						$rpc_py bdev_lvol_delete lvs_0/lbd_0
						$rpc_py bdev_lvol_delete_lvstore -l lvs_0
					fi
				done
			done <<< "${conf[2]}"
		done
	fi
	notice "Testing done -> shutting down"
	notice "killing vhost app"
	vhost_kill 0

	notice "EXIT DONE"
	notice "==============="
else
	notice "==============="
	notice ""
	notice "Leaving environment working!"
	notice ""
	notice "==============="
fi

vhosttestfini
