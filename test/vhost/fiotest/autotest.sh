#!/usr/bin/env bash

. $(readlink -f $(dirname $0))/../common/common.sh || exit 1

set -e
no_shutdown=false
fio_bin="fio"
fio_job=""
test_type=spdk_vhost_scsi
force_build=false
vms=()
declare -A vms_os
declare -A vms_raw_disks
declare -A vms_ctrlrs
declare -A vms_ctrlrs_disks
disk_split=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "    --test-type=TYPE      Perform specified test:"
	echo "                          virtio - test host virtio-scsi-pci using file as disk image"
	echo "                          kernel_vhost - use kernel driver vhost-scsi"
	echo "                          spdk_vhost_scsi - use spdk vhost scsi"
	echo "                          spdk_vhost_blk - use spdk vhost block"
	echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
	echo "    --fio-job=            Fio config to use for test. All VMs will this fio job when FIO executes"
	echo "                          (no unique jobs for specific VMs)"
	echo "    --no-shutdown         Don't shutdown at the end but leave envirionment working"
	echo "    --vm=NUM,OS,DISKS     VM configuration. This parameter might be used more than once:"
	echo "                          NUM - VM number (mandatory)"
	echo "                          OS - VM os disk path"
	echo "                          DISKS - VM test disks/devices path separated by ':'"
	echo "                          If test-type=spdk_vhost_blk then each disk size is 20G e.g."
	echo "                          --vm=X,os.qcow,Malloc0:Nvme0n1:Malloc1"
	echo "    --disk-split          By default all test types execute fio jobs on all disks which are available on guest"
	echo "                          system. Use this option if only some of the disks should be used for testing."
	echo "                          Example: --disk-split=4,1-3 will result in VM 1 using it's first disk (ex. /dev/sda)"
	echo "                          and VM 2 using it's disks 1-3 (ex. /dev/sdb, /dev/sdc, /dev/sdd)"
	spdk_vhost_usage_common
	exit 0
}

function fail()
{
	error "$@"
	false		
}

#default raw file is NVMe drive

for arg in ${VHOST_TEST_ARGS[@]}; do
	case "$arg" in
	-h|--help) usage $0 ;;
	--fio-bin=*) fio_bin="--fio-bin=${arg#*=}" ;;
	--fio-job=*) fio_job="${arg#*=}" ;;
	--no-shutdown) no_shutdown=true ;;
	--test-type=*) test_type="${arg#*=}" ;;
	--vm=*) 
			vm_conf="${arg#*=}"
			# regexp to match syntach "(N),(os.qcow),(Malloc0:Nvme0n1:Malloc1:..)"
			if [[ ! $vm_conf =~ ^([^,]+)[,]([^,]+)[,](.+)$ ]]; then
				error "Invalid VM configuration '$vm_conf'"
				exit 1
			fi
			
			vm=${BASH_REMATCH[1]}
			os=${BASH_REMATCH[2]}
			raw_disks="${BASH_REMATCH[3]//:/ }"
			
			# Sanity check if VM is not defined twice
			for vm_num in ${vms[@]}; do
				if [[ $vm_num == $vm ]]; then
					error "VM$vm defined more than once ( $(printf "'%s' " "${vms[@]}"))!"
					exit 1
				fi
			done
			
			vms+=( $vm )
			vms_os[$vm]=$(readlink -f "$os")
			vms_raw_disks[$vm]="$raw_disks"
			unset vm os raw_disks		
			;;
	--disk-split=*) disk_split="--split-disks=${arg#*=}" ;;
	*) usage $0 "Invalid argument '$arg'" ;;
	esac
done

echo "AAAA"
echo "${vms[@]}"
echo "${vms_os[@]}"
echo "${vms_raw_disks[@]}"


if [[ -z "$fio_job" ]]; then
	fail "No '--fio-job' parameter"
fi

trap 'error_exit "${FUNCNAME}" "${LINENO}" > /dev/stderr' ERR

vm_kill_all

# FIXME: move $VHOST_DIR/vhost_rpc.socket to some common place, or whole rpc_py?
rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $VHOST_DIR/vhost_rpc.socket"

if [[ $test_type =~ "spdk_vhost" ]]; then
	spdk_vhost_run -c $BASE_DIR/vhost.conf.in
	
	notice "Trying to create scsi controller with incorrect cpumask"
	if $rpc_py construct_vhost_scsi_controller vhost.invalid.cpumask --cpumask 9; then
		fail "Creating scsi controller with incorrect cpumask succeeded, but it shouldn't"
	fi
else
	fail "Invalid test type '$test_type'"
fi

$rpc_py get_bdevs
$rpc_py get_vhost_controllers

if [[ $test_type == "spdk_vhost_scsi" ]]; then

	notice "Trying to remove device from nonexistent scsi controller"
	if $rpc_py remove_vhost_scsi_dev vhost.nonexistent.name 0; then
		fail "Removing device from nonexistent scsi controller succeeded, but it shouldn't"
	fi

	notice "Trying to add device to nonexistent scsi controller"
	if $rpc_py add_vhost_scsi_lun vhost.nonexistent.name 0 Malloc0; then
		fail "Adding device to nonexistent scsi controller succeeded, but it shouldn't"
	fi

	notice "Trying to create scsi controller with incorrect name"
	if $rpc_py construct_vhost_scsi_controller .; then
		error "Creating scsi controller with incorrect name succeeded, but it shouldn't"
	fi
elif [[ $test_typ == "spdk_vhost_blk" ]]; then
	notice "Trying to create block controller with incorrect cpumask"
	if $rpc_py construct_vhost_blk_controller vhost.invalid.cpumask  Malloc0 --cpumask 9; then
				fail "Creating block controller with incorrect cpumask succeeded, but it shouldn't"
	fi

	notice "Trying to remove nonexistent block controller"
	if $rpc_py remove_vhost_block_dev vhost.nonexistent.name 0; then
		fail "Removing nonexistent block controller succeeded, but it shouldn't"
	fi

	notice "Trying to create block controller with incorrect name"
	if $rpc_py construct_vhost_scsi_controller . Malloc0; then
		fail "Creating block controller with incorrect name succeeded, but it shouldn't"
	fi
fi

notice "==============="
notice ""
notice "Setting up VM"
notice ""

for vm in ${vms[@]}; do
	if [[ $test_type =~ "spdk_vhost" ]]; then

		notice "Adding device via RPC ..."
		notice ""

		for disk in ${vms_raw_disks[$vm]}; do
			if [[ "$test_type" == "spdk_vhost_blk" ]]; then
				ctrlr="naa.$disk.$vm"
				
				notice "Creating vhost block controller $ctrlr with device $disk"
				$rpc_py construct_vhost_blk_controller $ctrlr $disk
				
				disks+=" --disk=spdk-vhost-blk,lba=4096,size=128M,ctrlr=$ctrlr"
				
				vms_ctrlrs[$vm]+=" $ctrlr"
				vms_ctrlrs_disks[$ctrlr]+=" $disk"
			else
				notice "Trying to remove nonexistent controller"
				if $rpc_py remove_vhost_controller unk0 > /dev/null; then
					fail "Removing nonexistent controller succeeded, but it shouldn't"
				fi
				
				ctrlr="naa.$disk.$vm"
				notice "Creating controller $ctrlr"
				$rpc_py construct_vhost_scsi_controller $ctrlr
				 
				
				notice "Adding initial device (0) to n$ctrlr"
				$rpc_py add_vhost_scsi_lun $ctrlr 0 $disk

				notice "Trying to remove nonexistent device on existing controller"
				if $rpc_py remove_vhost_scsi_dev $ctrlr 1 > /dev/null; then
					fail "Removing nonexistent device (1) from controller $ctrlr succeeded, but it shouldn't"
				fi

				notice "Trying to remove existing device from a controller"
				$rpc_py remove_vhost_scsi_dev $ctrlr 0

				notice "Trying to remove a just-deleted device from a controller again"
				if $rpc_py remove_vhost_scsi_dev $ctrlr 0 > /dev/null; then
					fail "Removing device 0 from controller $ctrlr succeeded, but it shouldn't"
				fi

				echo "INFO: Re-adding device 0 to $ctrlr"
				$rpc_py add_vhost_scsi_lun $ctrlr 0 $disk
				
				vms_ctrlrs[$vm]+=" $ctrlr"
				vms_ctrlrs_disks[$ctrlr]+=" $disk"
				
				disks+=" --disk=spdk-vhost-scsi,ctrlr=$ctrlr"
			fi
		done
		$rpc_py get_vhost_controllers
	fi

	vm_setup --vm-num=$vm --os=${vms_os[$vm]} $disks
	unset disks
done


# Run everything
vm_run ${vms[@]}
vm_wait_for_boot 600 ${vms[@]}

if [[ $test_type == "spdk_vhost_scsi" ]]; then
	for vm in ${vms[@]}; do
		for ctrlr in ${vms_ctrlrs[$vm]}; do
			i=0
			for disk in ${vms_ctrlrs_disks[$ctrlr]}; do
				notice "Hotdetach test. Trying to remove target $i from a controller $ctrlr"
				$rpc_py remove_vhost_scsi_dev $ctrlr $i
				sleep 0.1
	
				notice "Hotattach test. Re-adding device $i to $ctrlr"
				$rpc_py add_vhost_scsi_lun $ctrlr $i $disk
				(( i += 1))
			done
		done
	done
fi

sleep 0.1

notice "==============="
notice ""
notice "Testing..."
notice ""
notice "Running fio jobs ..."

fio_disks=""
for vm in ${vms[@]}; do
	vm_dir=$VM_BASE_DIR/$vm

	qemu_mask_param="VM_${vm}_qemu_mask"
	host_name="VM-${vm}-${!qemu_mask_param}"
	
	notice "Setting up hostname: $host_name"
	vm_ssh $vm "hostname $host_name"
	vm_start_fio_server $fio_bin $vm

	if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm
		#vm_reset_scsi_devices $vm $VM_DISKS
	elif [[ "$test_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm
	else
		fail "VM$vm: No disks to test"
	fi

	fio_disks+=" --vm=${vm}$(printf ':/dev/%s' $VM_DISKS)"
done

run_fio --job-file=$fio_job --out=$TEST_DIR $disk_split $fio_disks


if false && [[ "$test_type" == "spdk_vhost_scsi" ]]; then
	for vm in ${vms[@]}; do
		vm_check_scsi_location $vm
		vm_reset_scsi_devices $vm $VM_DISKS[@]
	done
fi

if $no_shutdown; then
	notice "==============="
	notice ""
	notice "Leaving environment working!"
	notice ""
	notice "==============="
	return 0
fi
	
notice "==============="
notice " APP EXITING"
notice "shutting down all VMs"

vm_shutdown_all

if [[ $test_type =~ "spdk_vhost" ]]; then
	notice "Removing vhost devices & controllers via RPC ..."
	for vm in ${vms[@]}; do
		for ctrlr in ${vms_ctrlrs[$vm]}; do
			if [[ "$test_type" == "spdk_vhost_scsi" ]]; then
				i=0
				for disk in ${vms_ctrlrs_disks[$ctrlr]}; do
					notice "Removing vhost target $i from controller $ctrlr"
					$rpc_py remove_vhost_scsi_dev $ctrlr $i
				done
			fi

			notice "Removing vhost controllers $ctrlr"
			$rpc_py remove_vhost_controller $ctrlr
		done
	done
fi

notice "Testing done -> shutting down"
notice "killing vhost app"
spdk_vhost_kill

notice "EXIT DONE"
notice "==============="
