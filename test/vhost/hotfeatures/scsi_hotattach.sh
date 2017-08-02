
#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/cmdline_parser.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s 127.0.0.1 "

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

function pre_hotattach_test_case() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms
}

function reboot_all_and_prepare() {
        reboot_all_vms
        prepare_vms
}

function post_hotattach_test_case() {
        vm_shutdown_all
        spdk_vhost_kill
}

function prepare_fio_cmd_tc1() {
        print_test_fio_header
        run_fio="python $BASE_DIR/../fiotest/run_fio.py "
        run_fio+="--fio-bin=$fio_bin "
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
        fio_job=$BASE_DIR/fio_jobs/default_integrity_2discs.job
        print_test_fio_header

        # Check if all VM have disk in tha same location
        DISK=""
	for vm_num in "0"; do
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        gawk -i inplace -v iter="$j" -v new_disk="/dev/$disk" '/filename=.*/{c++;if(c==iter){sub("filename=.*","filename="new_disk);c=0}}1' $fio_job
                        j=$(( $j+1 ))
                done
        done

        scp -i "$SPDK_VHOST_SSH_KEY_FILE" -F "$VM_BASE_DIR/ssh_config" -P $(vm_ssh_socket 0) $fio_job root@127.0.0.1:/root/default_integrity_2discs.job
        run_fio="$fio_bin --eta=never --client=127.0.0.1,$(vm_fio_socket 0) --remote-config /root/default_integrity_2discs.job"
}

function hotattach_tc1() {
	pre_hotattach_test_case

	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

	sleep 0.1
	prepare_fio_cmd_tc1 "0"
	$run_fio
        check_fio_retcode "Hotattach test case 1: Iteration 1."

	reboot_all_and_prepare

	$run_fio
        check_fio_retcode "Hotattach test case 1: Iteration 2."

        post_hotattach_test_case
}

function hotattach_tc2() {
        pre_hotattach_test_case
        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

        sleep 0.1
        prepare_fio_cmd_tc1 "0"

        $run_fio &
        last_pid=$!
        sleep 3
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 1 Nvme0n1p1
	wait $last_pid
        check_fio_retcode "Hotattach test case 2: Iteration 1."

	prepare_fio_cmd_tc2
	$run_fio
        check_fio_retcode "Hotattach test case 2: Iteration 2."

        reboot_all_and_prepare

        prepare_fio_cmd_tc2
        $run_fio
        check_fio_retcode "Hotattach test case 2: Iteration 3."

        post_hotattach_test_case
}

function hotattach_tc3() {
        pre_hotattach_test_case

        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

        sleep 0.1
        prepare_fio_cmd_tc1 "0"

        $run_fio &
        last_pid=$!
        sleep 3
        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.0 0 Nvme0n1p1
        wait $last_pid
        check_fio_retcode "Hotattach test case 3: Iteration 1."

        prepare_fio_cmd_tc2
        $run_fio
        check_fio_retcode "Hotattach test case 3: Iteration 2."

        reboot_all_and_prepare

        prepare_fio_cmd_tc2
        $run_fio
        check_fio_retcode "Hotattach test case 3: Iteration 3."

        post_hotattach_test_case
}

function hotattach_tc4() {
        pre_hotattach_test_case

        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

        sleep 0.1
        prepare_fio_cmd_tc1 "0"

        $run_fio &
        last_pid=$!
        sleep 3
        $rpc_py add_vhost_scsi_lun naa.Nvme0n1p2.1 0 Nvme0n1p1
        wait $last_pid
        check_fio_retcode "Hotattach test case 4: Iteration 1."

        prepare_fio_cmd_tc1 "$used_vms"
        $run_fio
        check_fio_retcode "Hotattach test case 4: Iteration 2."

        reboot_all_and_prepare

        prepare_fio_cmd_tc1 "$used_vms"
        $run_fio
        check_fio_retcode "Hotattach test case 4: Iteration 3."

        post_hotattach_test_case
}

check_qemu
check_spdk
gen_hotattach_config
hotattach_tc1
hotattach_tc2
hotattach_tc3
hotattach_tc4
