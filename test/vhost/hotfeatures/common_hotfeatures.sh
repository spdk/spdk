#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                print help and exit"
	echo "    --test-type=TYPE      Perform specified test:"
	exit 0
}
function prepere_environment(){

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
. $COMMON_DIR/common.sh
}
function check_qemu() {
    echo "==============="
    echo "INFO: Start checking qemu"

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

    echo "INFO: Stop checking qemu"
    echo "==============="
}

function check_spdk() {
    echo "==============="
    echo ""
    echo "INFO: checking spdk"
    echo ""

    if [[ ! -x $SPDK_BUILD_DIR/app/vhost/vhost ]] ; then
        echo "INFO: $SPDK_BUILD_DIR/app/vhost/vhost - building and installing"
        spdk_build_and_install
    fi

    echo "INFO: Stop checking spdk"
    echo "==============="
}

function gen_tc_1_scsi_config_and_run_vhost() {
	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in

	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme0n1 1" >> $COMMON_DIR/vhost.conf.in

	echo "[VhostScsi0]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
}
function gen_tc_2_scsi_config_and_run_vhost() {
	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in

	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme0n1 2" >> $COMMON_DIR/vhost.conf.in

	echo "[VhostScsi0]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 0 Nvme0n1p0" >> $COMMON_DIR/vhost.conf.in
	echo "[VhostScsi1]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p1.0" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 0 Nvme0n1p1" >> $COMMON_DIR/vhost.conf.in
}

function gen_tc_1_blk_config_and_run_vhost() {
    NAME_DISK="NVMe0"
    rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py "
    rpc_py+="-s 127.0.0.1 "
    BDF_LIST="$(get_nvme_pci_addr $TEST_DIR/scripts/gen_nvme.sh $NAME_DISK)"
	for bdf in $BDF_LIST; do
	    $rpc_py construct_nvme_bdev -b Nvme0 -t pcie -a $bdf
	done
}
function gen_tc_2_blk_config_and_run_vhost() {
	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in

	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme0n1 2" >> $COMMON_DIR/vhost.conf.in

	echo "[VhostB0]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 0 Nvme0n1p0" >> $COMMON_DIR/vhost.conf.in
	echo "[VhostScsi1]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p1.0" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 0 Nvme0n1p1" >> $COMMON_DIR/vhost.conf.in
}
#function gen_tc_1_blk_config_and_run_vhost() {
#	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
#	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in
#
#	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
#	echo "  Split Nvme0n1 1" >> $COMMON_DIR/vhost.conf.in
#
#	echo "[VhostScsi0]" >> $COMMON_DIR/vhost.conf.in
#	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
#}
#function gen_tc_2_blk_config_and_run_vhost() {
#	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
#	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in
#
#	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
#	echo "  Split Nvme0n1 2" >> $COMMON_DIR/vhost.conf.in
#
#	echo "[VhostB0]" >> $COMMON_DIR/vhost.conf.in
#	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
#	echo "  Dev 0 Nvme0n1p0" >> $COMMON_DIR/vhost.conf.in
#	echo "[VhostScsi1]" >> $COMMON_DIR/vhost.conf.in
#	echo "  Name naa.Nvme0n1p1.0" >> $COMMON_DIR/vhost.conf.in
#	echo "  Dev 0 Nvme0n1p1" >> $COMMON_DIR/vhost.conf.in
#}

function run_vhost() {
	echo "==============="
	echo ""
	echo "INFO: running SPDK"
	echo ""
        spdk_vhost_run
	#$BASE_DIR/../fiotest/run_vhost.sh $x --work-dir=$TEST_DIR
	echo
}

function setup_vms() {
	for vm_conf in ${vms[@]}; do
		IFS=',' read -ra conf <<< "$vm_conf"
		setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR
		--test-type=$test_type"
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

		$setup_cmd
	done
}

function setup_and_run_vms() {
    echo "coss"
	setup_vms
	# Run everything
	$COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
	vm_wait_for_boot 600 $used_vms
}

function prepare_vms() {
        for vm_num in $used_vms; do
                vm_dir=$VM_BASE_DIR/$vm_num

                qemu_mask_param="VM_${vm_num}_qemu_mask"

                host_name="VM-$vm_num-${!qemu_mask_param}"
                echo "INFO: Setting up hostname: $host_name"
                vm_ssh $vm_num "hostname $host_name"
                vm_start_fio_server $fio_bin $readonly $vm_num
                #vm_check_scsi_location $vm_num
        done
}

function reboot_all_vms() {
        echo "Rebooting all vms"
        for vm_num in $used_vms; do
                vm_ssh $vm_num "reboot" || true
        done

        vm_wait_for_boot 600 $used_vms
}

function check_fio_retcode() {
        fio_retcode=$?
        echo $1
        if test "$fio_retcode" != "0"; then
                echo "    Fio test ended with error."
                exit 1
        else
                echo "    Fio test ended with success."
        fi
}


function prepare_fio_cmd_tc1() {
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."
        run_fio="python $COMMON_DIR/run_fio.py "
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
        for vm_num in $1; do
                vm_dir=$VM_BASE_DIR/$vm_num
                run_fio+="127.0.0.1:$(cat $vm_dir/fio_socket):"
                vm_check_scsi_location $vm_num
                for disk in $SCSI_DISK; do
                        run_fio+="/dev/$disk:"
                done
                run_fio="${run_fio::-1}"
                run_fio+=","
        done

        run_fio="${run_fio%,}"
        run_fio+=" "
        run_fio="${run_fio::-1}"

}

function prepare_fio_cmd_tc2() {
        fio_job=$COMMON_DIR/fio_jobs/default_integrity.job12
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."

        # Check if all VM have disk in tha same location
        DISK=""
	for vm_num in "0"; do
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        disk=$( echo "/dev/$disk" | sed 's#/#\\/#g' )
                        sed -i ''"$j"',/filename=.*/s/filename=.*/filename='"$disk/"''"$j"'' $fio_job
                        j=$(( $j+1 ))
                done
        done

        scp -i "$SPDK_VHOST_SSH_KEY_FILE" -F "$VM_BASE_DIR/ssh_config" -P $(vm_ssh_socket 0) $fio_job root@127.0.0.1:/root/default_integrity.job12
        run_fio="$(echo $fio_bin | awk -F= '{print $NF}') --eta=never --client=127.0.0.1,$(vm_fio_socket 0) --remote-config /root/default_integrity.job12"
}

function check_lsblk_in_vm() {

    for vm_num in $used_vms; do
        vm_check_scsi_location $vm_num
        for disk in $SCSI_DISK; do
            vm_ssh $vm_num "lsblk -d /dev/$disk"
        done
    done
}

function back_configuration(){
    vm_kill_all
    spdk_vhost_kill
    echo $bdf > /sys/bus/pci/drivers/uio_pci_generic/bind
}
