testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

dry_run=false
no_shutdown=false
fio_bin="fio"
fio_jobs="$testdir/fio_jobs/"
test_type=spdk_vhost_scsi
reuse_vms=false
vms=()
used_vms=""
disk_split=""
x=""
scsi_hot_remove_test=0
blk_hot_remove_test=0
readonly=""

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Shortcut script for doing automated hotattach/hotdetach test"
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
	echo "    --fio-jobs=           Fio configs to use for tests. Can point to a directory or"
	echo "    --vm=NUM[,OS][,DISKS] VM configuration. This parameter might be used more than once:"
	echo "                          NUM - VM number (mandatory)"
	echo "                          OS - VM os disk path (optional)"
	echo "                          DISKS - VM os test disks/devices path (virtio - optional, kernel_vhost - mandatory)"
	echo "    --scsi-hotremove-test Run scsi hotremove tests"
	echo "    --readonly            Use readonly for fio"
	exit 0
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage $0 ;;
				fio-bin=*) fio_bin="${OPTARG#*=}" ;;
				fio-jobs=*) fio_jobs="${OPTARG#*=}" ;;
				test-type=*) test_type="${OPTARG#*=}" ;;
				vm=*) vms+=("${OPTARG#*=}") ;;
				scsi-hotremove-test) scsi_hot_remove_test=1 ;;
				blk-hotremove-test) blk_hot_remove_test=1 ;;
				readonly) readonly="--readonly" ;;
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

fio_job=$testdir/fio_jobs/default_integrity.job
tmp_attach_job=$testdir/fio_jobs/fio_attach.job.tmp
tmp_detach_job=$testdir/fio_jobs/fio_detach.job.tmp

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

function print_test_fio_header() {
	notice "==============="
	notice ""
	notice "Testing..."

	notice "Running fio jobs ..."
	if [ $# -gt 0 ]; then
		echo $1
	fi
}

function vms_setup() {
	for vm_conf in "${vms[@]}"; do
		IFS=',' read -ra conf <<< "$vm_conf"
		if [[ -z ${conf[0]} ]] || ! assert_number ${conf[0]}; then
			fail "invalid VM configuration syntax $vm_conf"
		fi

		# Sanity check if VM is not defined twice
		for vm_num in $used_vms; do
			if [[ $vm_num -eq ${conf[0]} ]]; then
				fail "VM$vm_num defined more than twice ( $(printf "'%s' " "${vms[@]}"))!"
			fi
		done

		used_vms+=" ${conf[0]}"

		setup_cmd="vm_setup --disk-type=$test_type --force=${conf[0]}"
		[[ x"${conf[1]}" != x"" ]] && setup_cmd+=" --os=${conf[1]}"
		[[ x"${conf[2]}" != x"" ]] && setup_cmd+=" --disks=${conf[2]}"
		$setup_cmd
	done
}

function vm_run_with_arg() {
	local vms_to_run="$*"
	vm_run $vms_to_run
	vm_wait_for_boot 300 $vms_to_run
}

function vms_setup_and_run() {
	local vms_to_run="$*"
	vms_setup
	vm_run_with_arg $vms_to_run
}

function vms_prepare() {
	for vm_num in $1; do
		qemu_mask_param="VM_${vm_num}_qemu_mask"

		host_name="VM-${vm_num}-${!qemu_mask_param}"
		notice "Setting up hostname: $host_name"
		vm_exec $vm_num "hostname $host_name"
		vm_start_fio_server --fio-bin=$fio_bin $readonly $vm_num
	done
}

function vms_reboot_all() {
	notice "Rebooting all vms "
	for vm_num in $1; do
		vm_exec $vm_num "reboot" || true
		while vm_os_booted $vm_num; do
			sleep 0.5
		done
	done

	vm_wait_for_boot 300 $1
}

function check_fio_retcode() {
	local fio_retcode=$3
	echo $1
	local retcode_expected=$2
	if [ $retcode_expected == 0 ]; then
		if [ $fio_retcode != 0 ]; then
			error "    Fio test ended with error."
		else
			notice "    Fio test ended with success."
		fi
	else
		if [ $fio_retcode != 0 ]; then
			notice "    Fio test ended with expected error."
		else
			error "    Fio test ended with unexpected success."
		fi
	fi
}

function wait_for_finish() {
	local wait_for_pid=$1
	local sequence=${2:-30}
	for i in $(seq 1 $sequence); do
		if kill -0 $wait_for_pid; then
			sleep 0.5
			continue
		else
			break
		fi
	done
	if kill -0 $wait_for_pid; then
		error "Timeout for fio command"
	fi

	wait $wait_for_pid
}

function reboot_all_and_prepare() {
	vms_reboot_all "$1"
	vms_prepare "$1"
}

function post_test_case() {
	vm_shutdown_all
	vhost_kill 0
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	post_test_case
	print_backtrace
	exit 1
}

function check_disks() {
	if [ "$1" == "$2" ]; then
		echo "Disk has not been deleted"
		exit 1
	fi
}

function get_traddr() {
	local nvme_name=$1
	local nvme

	nvme="$($rootdir/scripts/gen_nvme.sh)"
	traddr=$(jq -r ".config[] | select(.params.name == \"$nvme_name\") | .params.traddr" <<< "$nvme")
	[[ -n $traddr ]] || return 1
}

function delete_nvme() {
	$rpc_py bdev_nvme_detach_controller $1
}

function add_nvme() {
	$rpc_py bdev_nvme_attach_controller -b $1 -t PCIe -a $2
}
