
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
	exit 0
}

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

. $BASE_DIR/../fiotest/common.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s 127.0.0.1 "

function check_qemu() {
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
}

function check_spdk() {
	if [[ ! -x $SPDK_BUILD_DIR/app/vhost/vhost ]] || $force_build ; then
		echo "INFO: $SPDK_BUILD_DIR/app/vhost/vhost - building and installing"
		spdk_build_and_install
	fi
}

function gen_hotattach_config() {
	cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in

	echo "[Split]" >> $BASE_DIR/vhost.conf.in
	echo "  Split Nvme0n1 4" >> $BASE_DIR/vhost.conf.in

	echo "[VhostScsi0]" >> $BASE_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $BASE_DIR/vhost.conf.in
	echo "[VhostScsi1]" >> $BASE_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p1.0" >> $BASE_DIR/vhost.conf.in
	echo "[VhostScsi2]" >> $BASE_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p2.1" >> $BASE_DIR/vhost.conf.in
	echo "[VhostScsi3]" >> $BASE_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p3.1" >> $BASE_DIR/vhost.conf.in
}

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
		setup_cmd="$BASE_DIR/../fiotest/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
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
	setup_vms
	# Run everything
	$BASE_DIR/../fiotest/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
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
        run_fio="python $BASE_DIR/../fiotest/run_fio.py "
        run_fio+="$fio_bin "
        run_fio+="--job-file="
        for job in $fio_jobs; do
                run_fio+="./test/vhost/$job,"
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
        fio_job=$BASE_DIR/fio_jobs/default_integrity.job12
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

function hotattach_tc1() {
	run_vhost
        setup_and_run_vms
        prepare_vms

	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

	sleep 0.1
	prepare_fio_cmd_tc1 "0"
	$run_fio

	for vm_num in $used_vms; do
		nmap -p $(cat $VM_BASE_DIR/$vm_num/ssh_socket) 127.0.0.1
	done

	reboot_all_vms
	prepare_vms

	$run_fio

	vm_shutdown_all
	spdk_vhost_kill
}

function hotattach_tc2() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms

        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

        sleep 0.1
        prepare_fio_cmd_tc1 "0"

        $run_fio &
        LAST_PID=$!
        sleep 3
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 1 Nvme0n1p1
	wait $LAST_PID

        for vm_num in $used_vms; do
                nmap -p $(cat $VM_BASE_DIR/$vm_num/ssh_socket) 127.0.0.1
        done

	prepare_fio_cmd_tc2
	$run_fio

        reboot_all_vms
        prepare_vms

        prepare_fio_cmd_tc2
        $run_fio

        vm_shutdown_all
        spdk_vhost_kill
}

function hotattach_tc3() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms

        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

        sleep 0.1
        prepare_fio_cmd_tc1 "0"

        $run_fio &
        LAST_PID=$!
        sleep 3
        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.0 0 Nvme0n1p1
        wait $LAST_PID

        for vm_num in $used_vms; do
                nmap -p $(cat $VM_BASE_DIR/$vm_num/ssh_socket) 127.0.0.1
        done

        prepare_fio_cmd_tc2
        $run_fio

        reboot_all_vms
        prepare_vms

        prepare_fio_cmd_tc2
        $run_fio

        #vm_shutdown_all
        #spdk_vhost_kill

}

function hotattach_tc4() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms

        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

        sleep 0.1
        prepare_fio_cmd_tc1 "0"

        $run_fio &
        last_pid=$!
        sleep 3
        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p2.1 0 Nvme0n1p1
        wait $last_pid
        check_fio_retcode "Hotattach test case 4: Iteration 1."
        for vm_num in $used_vms; do
                nmap -p $(cat $VM_BASE_DIR/$vm_num/ssh_socket) 127.0.0.1
        done

        prepare_fio_cmd_tc1 "$used_vms"
        $run_fio
        check_fio_retcode "Hotattach test case 4: Iteration 2."

        reboot_all_vms
        prepare_vms

        prepare_fio_cmd_tc1 "$used_vms"
        $run_fio
        check_fio_retcode "Hotattach test case 4: Iteration 3."

        vm_shutdown_all
        spdk_vhost_kill
}

check_qemu
check_spdk
gen_hotattach_config
#hotattach_tc1
#hotattach_tc2
#hotattach_tc3
#hotattach_tc4
