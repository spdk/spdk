#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

function gen_hotdetach_config() {
        cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
        cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
        [Split]
          Split Nvme0n1 8
END_OF_CONFIG
}

function pre_hotdetach_test_case() {
        used_vms=""
        run_vhost
        $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p0.0
        $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p1.0
        $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p2.1
        $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p3.1
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p0.0 1 Nvme0n1p1
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p1.0 0 Nvme0n1p2
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p1.0 1 Nvme0n1p3
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p2.1 0 Nvme0n1p4
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p2.1 1 Nvme0n1p5
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p3.1 0 Nvme0n1p6
        $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p3.1 1 Nvme0n1p7
        vms_setup_and_run
        vms_prepare
}

function reboot_all_and_prepare() {
        vms_reboot_all
        vms_prepare
}

function post_hotdetach_test_case() {
        vm_shutdown_all
        spdk_vhost_kill
}

function get_first_disk() {
        vm_check_scsi_location $1
        disk_array=( $SCSI_DISK )
        eval "$2=${disk_array[0]}"
}

function check_disks() {
        if [ "$1" == "$2" ]; then 
                echo "Disk has not been deleted"
                exit 1
        fi
}

function prepare_fio_cmd_tc1_iter1() {
        print_test_fio_header

        run_fio="$fio_bin --eta=never "
        for vm_num in $1; do
                cp $fio_job $tmp_job
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                for disk in $SCSI_DISK; do
                        echo "[nvme-host$disk]" >> $tmp_job
                        echo "filename=/dev/$disk" >> $tmp_job
                done
                vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_4discs.job
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_4discs.job "
                rm $tmp_job
        done
}

function prepare_fio_cmd_tc1_iter2() {
        print_test_fio_header

        for vm_num in 0; do
                cp $fio_job $tmp_job
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                for disk in $SCSI_DISK; do
                        echo "[nvme-host$disk]" >> $tmp_job
                        echo "filename=/dev/$disk" >> $tmp_job
                done
                vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_3discs.job
                rm $tmp_job
        done
        run_fio="$fio_bin --eta=never "
        for vm_num in $used_vms; do
                 if test $vm_num == 0; then
                         run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_3discs.job "
                         continue
                 fi
                 run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_4discs.job "
        done
}

function prepare_fio_cmd_tc2_iter1() {
        print_test_fio_header

        run_fio="$fio_bin --eta=never "
        for vm_num in $1; do
                cp $fio_job $tmp_job
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        if test $j -gt 1; then
                                break
                        fi
                        echo "[nvme-host$disk]" >> $tmp_job
                        echo "filename=/dev/$disk" >> $tmp_job
                        (( j++ ))
                done
                vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity.job
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity.job "
                rm $tmp_job
        done
}

function prepare_fio_cmd_tc2_iter2() {
        print_test_fio_header

        run_fio="$fio_bin --eta=never "
        for vm_num in $1; do
                cp $fio_job $tmp_job
                if test $vm_num == 0; then
                        vm_job_name=default_integrity_3discs.job
                else
                        vm_job_name=default_integrity_4discs.job
                fi
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                for disk in $SCSI_DISK; do
                        echo "[nvme-host$disk]" >> $tmp_job
                        echo "filename=/dev/$disk" >> $tmp_job
                done
                vm_scp "$vm_num" $tmp_job  127.0.0.1:/root/$vm_job_name
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/${vm_job_name} "
                rm $tmp_job
        done
}

function prepare_fio_cmd_tc3_iter1() {
        print_test_fio_header

        run_fio="$fio_bin --eta=never "
        for vm_num in $1; do
                cp $fio_job $tmp_job
                if test $vm_num == 0; then
                        vm_job_name=default_integrity_3discs.job
                else
                        vm_job_name=default_integrity_4discs.job
                fi
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        if test $vm_num == 0; then
                              if test $j == 1; then
                                      (( j++ ))
                                      continue
                              fi
                        fi
                        echo "[nvme-host$disk]" >> $tmp_job
                        echo "filename=/dev/$disk" >> $tmp_job
                        (( j++ ))
                done
                vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/$vm_job_name
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/$vm_job_name "
                rm $tmp_job
        done

}

function hotdetach_tc1() {
        pre_hotdetach_test_case
        first_disk=""
        get_first_disk "0" first_disk
        prepare_fio_cmd_tc1_iter1 "$used_vms"
        $run_fio &
        last_pid=$!
        sleep 5
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        set +xe
        wait $last_pid
        check_fio_retcode "Hotdetach test case 1: Iteration 1." 1
        set -xe
        second_disk=""
        get_first_disk "0" second_disk
        check_disks $first_disk $second_disk
        reboot_all_and_prepare

        prepare_fio_cmd_tc1_iter2
        $run_fio
        check_fio_retcode "Hotdetach test case 1: Iteration 2." 0

        post_hotdetach_test_case
}

function hotdetach_tc2() {
        pre_hotdetach_test_case
        first_disk=""
        get_first_disk "0" first_disk
        prepare_fio_cmd_tc2_iter1 "0"
        $run_fio &
        last_pid=$!
        sleep 5
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        set +xe
        wait $last_pid
        check_fio_retcode "Hotdetach test case 2: Iteration 1." 1
        set -xe
        second_disk=""
        get_first_disk "0" second_disk
        check_disks $first_disk $second_disk
        reboot_all_and_prepare

        prepare_fio_cmd_tc2_iter2 "$used_vms"
        $run_fio
        check_fio_retcode "Hotdetach test case 2: Iteration 2." 0

        post_hotdetach_test_case
}

function hotdetach_tc3() {
        pre_hotdetach_test_case
        first_disk=""
        get_first_disk "0" first_disk
        prepare_fio_cmd_tc3_iter1 "$used_vms"
        $run_fio &
        last_pid=$!
        sleep 5
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        wait $last_pid
	check_fio_retcode "Hotdetach test case 3: Iteration 1." 0
        second_disk=""
        get_first_disk "0" second_disk 
        check_disks $first_disk $second_disk
        reboot_all_and_prepare

        prepare_fio_cmd_tc2_iter2 "$used_vms"
        $run_fio
        check_fio_retcode "Hotdetach test case 3: Iteration 2." 0

        post_hotdetach_test_case
}

function hotdetach_tc4() {
        pre_hotdetach_test_case
        first_disk=""
        get_first_disk "0" first_disk
        prepare_fio_cmd_tc2_iter1 "0"
        $run_fio &
        first_fio_pid=$!
        prepare_fio_cmd_tc3_iter1 "$used_vms"
        $run_fio &
        second_fio_pid=$!
        sleep 5
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        set +xe
        wait $first_fio_pid
        check_fio_retcode "Hotattach test case 4: Iteration 1." 1
        set -xe
        wait $second_fio_pid
	check_fio_retcode "Hotattach test case 4: Iteration 2." 0
        second_disk=""
        get_first_disk "0" second_disk
        check_disks $first_disk $second_disk
        reboot_all_and_prepare

        prepare_fio_cmd_tc2_iter2 "$used_vms"
        $run_fio
	check_fio_retcode "Hotdetach test case 4: Iteration 3." 0

        post_hotdetach_test_case
}

gen_hotdetach_config
hotdetach_tc1
hotdetach_tc2
hotdetach_tc3
hotdetach_tc4
