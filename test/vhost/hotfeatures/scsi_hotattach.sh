
#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s 127.0.0.1 "

function gen_hotattach_config() {
	cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in

        cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
        [Split]
          Split Nvme0n1 4
        [VhostScsi0]
          Name naa.Nvme0n1p0.0
        [VhostScsi1]
          Name naa.Nvme0n1p1.0
        [VhostScsi2]
          Name naa.Nvme0n1p2.1
        [VhostScsi3]
          Name naa.Nvme0n1p3.1
END_OF_CONFIG
}

function pre_hotattach_test_case() {
        used_vms=""
        run_vhost
        vm_setup_and_run_vms
        vm_prepare_vms
}

function reboot_all_and_prepare() {
        vm_reboot_all_vms
        vm_prepare_vms
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
        print_test_fio_header

        cp $fio_job $tmp_job
	for vm_num in "0"; do
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                for disk in $SCSI_DISK; do
                        echo "[nvme-host$disk]" >> $tmp_job
                        echo "filename=/dev/$disk" >> $tmp_job
                done
        done
        vm_scp "0" $tmp_job 127.0.0.1:/root/default_integrity_2discs.job
        run_fio="$fio_bin --eta=never --client=127.0.0.1,$(vm_fio_socket 0) --remote-config /root/default_integrity_2discs.job"
        rm $tmp_job
}

function hotattach_tc1() {
	pre_hotattach_test_case

	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0

	sleep 0.1
	prepare_fio_cmd_tc1 "0"
	$run_fio
        check_fio_retcode "Hotattach test case 1: Iteration 1." 0

	reboot_all_and_prepare

	$run_fio
        check_fio_retcode "Hotattach test case 1: Iteration 2." 0

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
        check_fio_retcode "Hotattach test case 2: Iteration 1." 0

	prepare_fio_cmd_tc2
	$run_fio
        check_fio_retcode "Hotattach test case 2: Iteration 2." 0

        reboot_all_and_prepare

        prepare_fio_cmd_tc2
        $run_fio
        check_fio_retcode "Hotattach test case 2: Iteration 3." 0

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
        check_fio_retcode "Hotattach test case 3: Iteration 1." 0

        prepare_fio_cmd_tc2
        $run_fio
        check_fio_retcode "Hotattach test case 3: Iteration 2." 0

        reboot_all_and_prepare

        prepare_fio_cmd_tc2
        $run_fio
        check_fio_retcode "Hotattach test case 3: Iteration 3." 0

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
        check_fio_retcode "Hotattach test case 4: Iteration 1." 0

        prepare_fio_cmd_tc1 "$used_vms"
        $run_fio
        check_fio_retcode "Hotattach test case 4: Iteration 2." 0

        reboot_all_and_prepare

        prepare_fio_cmd_tc1 "$used_vms"
        $run_fio
        check_fio_retcode "Hotattach test case 4: Iteration 3." 0

        post_hotattach_test_case
}

check_qemu
check_spdk
gen_hotattach_config
#hotattach_tc1
#hotattach_tc2
hotattach_tc3
#hotattach_tc4
