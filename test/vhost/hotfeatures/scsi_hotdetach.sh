#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/scsi_hotattach.sh

function gen_hotdetach_config() {
        cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
        $SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in

        echo "[Split]" >> $BASE_DIR/vhost.conf.in
        echo "  Split Nvme0n1 8" >> $BASE_DIR/vhost.conf.in

        echo "[VhostScsi0]" >> $BASE_DIR/vhost.conf.in
        echo "  Name naa.Nvme0n1p0.0" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 0 Nvme0n1p0" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 1 Nvme0n1p1" >> $BASE_DIR/vhost.conf.in
        echo "[VhostScsi1]" >> $BASE_DIR/vhost.conf.in
        echo "  Name naa.Nvme0n1p1.0" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 0 Nvme0n1p2" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 1 Nvme0n1p3" >> $BASE_DIR/vhost.conf.in
        echo "[VhostScsi2]" >> $BASE_DIR/vhost.conf.in
        echo "  Name naa.Nvme0n1p2.1" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 0 Nvme0n1p4" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 1 Nvme0n1p5" >> $BASE_DIR/vhost.conf.in
        echo "[VhostScsi3]" >> $BASE_DIR/vhost.conf.in
        echo "  Name naa.Nvme0n1p3.1" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 0 Nvme0n1p6" >> $BASE_DIR/vhost.conf.in
        echo "  Dev 1 Nvme0n1p7" >> $BASE_DIR/vhost.conf.in
}

function prepare_fio_cmd_tc1_iter1() {
        fio_job=$BASE_DIR/fio_jobs/default_integrity.job1234
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."

        # Check if all VM have disk in tha same location
        DISK=""
        run_fio="$(echo $fio_bin | awk -F= '{print $NF}') --eta=never "
        for vm_num in $1; do
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        disk=$( echo "/dev/$disk" | sed 's#/#\\/#g' )
                        sed -i ''"$j"',/filename=.*/s/filename=.*/filename='"$disk/"''"$j"'' $fio_job
                        j=$(( $j+1 ))
                done
                scp -i "$SPDK_VHOST_SSH_KEY_FILE" -F "$VM_BASE_DIR/ssh_config" -P $(vm_ssh_socket $vm_num) $fio_job root@127.0.0.1:/root/default_integrity.job1234
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity.job1234 "
        done
}

function prepare_fio_cmd_tc1_iter2() {
        fio_job=$BASE_DIR/fio_jobs/default_integrity.job123
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."

        # Check if all VM have disk in tha same location
        DISK=""
        for vm_num in 0; do
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        disk=$( echo "/dev/$disk" | sed 's#/#\\/#g' )
                        sed -i ''"$j"',/filename=.*/s/filename=.*/filename='"$disk/"''"$j"'' $fio_job
                        j=$(( $j+1 ))
                done
                scp -i "$SPDK_VHOST_SSH_KEY_FILE" -F "$VM_BASE_DIR/ssh_config" -P $(vm_ssh_socket $vm_num) $fio_job root@127.0.0.1:/root/default_integrity.job123
        done
        run_fio="$(echo $fio_bin | awk -F= '{print $NF}') --eta=never "
        for vm_num in $used_vms; do
                 if test $vm_num == 0; then
                         run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity.job123 "
                         continue
                 fi
                 run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity.job1234 "
        done
}

function prepare_fio_cmd_tc2_iter1() {
        fio_job=$BASE_DIR/fio_jobs/default_integrity.job
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."

        # Check if all VM have disk in tha same location
        DISK=""
        run_fio="$(echo $fio_bin | awk -F= '{print $NF}') --eta=never "
        for vm_num in $1; do
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        if test $j -gt 1; then
                                break
                        fi
                        disk=$( echo "/dev/$disk" | sed 's#/#\\/#g' )
                        sed -i ''"$j"',/filename=.*/s/filename=.*/filename='"$disk/"''"$j"'' $fio_job
                        j=$(( $j+1 ))
                done
                scp -i "$SPDK_VHOST_SSH_KEY_FILE" -F "$VM_BASE_DIR/ssh_config" -P $(vm_ssh_socket $vm_num) $fio_job root@127.0.0.1:/root/default_integrity.job
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity.job "
        done
}

function prepare_fio_cmd_tc2_iter2() {
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."

        # Check if all VM have disk in tha same location
        DISK=""
        run_fio="$(echo $fio_bin | awk -F= '{print $NF}') --eta=never "
        for vm_num in $1; do
                if test $vm_num == 0; then
                        fio_job=default_integrity.job123
                else
                        fio_job=default_integrity.job1234
                fi
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        disk=$( echo "/dev/$disk" | sed 's#/#\\/#g' )
                        sed -i ''"$j"',/filename=.*/s/filename=.*/filename='"$disk/"''"$j"'' $BASE_DIR/fio_jobs/$fio_job
                        j=$(( $j+1 ))
                done
                scp -i "$SPDK_VHOST_SSH_KEY_FILE" -F "$VM_BASE_DIR/ssh_config" -P $(vm_ssh_socket $vm_num) $BASE_DIR/fio_jobs/$fio_job root@127.0.0.1:/root/$fio_job
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/$fio_job "
        done
}

function prepare_fio_cmd_tc3_iter1() {
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."

        # Check if all VM have disk in tha same location
        DISK=""
        run_fio="$(echo $fio_bin | awk -F= '{print $NF}') --eta=never "
        for vm_num in $1; do
                if test $vm_num == 0; then
                        fio_job=default_integrity.job234
                else
                        fio_job=default_integrity.job1234
                fi
                vm_dir=$VM_BASE_DIR/$vm_num
                vm_check_scsi_location $vm_num
                j=1
                for disk in $SCSI_DISK; do
                        if test $vm_num == 0; then
                              if test $j == 1; then
                                      continue
                              fi
                        fi
                        disk=$( echo "/dev/$disk" | sed 's#/#\\/#g' )
                        sed -i ''"$j"',/filename=.*/s/filename=.*/filename='"$disk/"''"$j"'' $BASE_DIR/fio_jobs/$fio_job
                        j=$(( $j+1 ))
                done
                scp -i "$SPDK_VHOST_SSH_KEY_FILE" -F "$VM_BASE_DIR/ssh_config" -P $(vm_ssh_socket $vm_num) $BASE_DIR/fio_jobs/$fio_job root@127.0.0.1:/root/$fio_job
                run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/$fio_job "
        done

}

function hotdetach_tc1() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms
        prepare_fio_cmd_tc1_iter1 "$used_vms"
        $run_fio &
        last_pid=$!
        sleep 3
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        wait $last_pid || true

        reboot_all_vms
        prepare_vms

        prepare_fio_cmd_tc1_iter2
        $run_fio

        vm_shutdown_all
        spdk_vhost_kill
}

function hotdetach_tc2() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms
        prepare_fio_cmd_tc2_iter1 "0"
        $run_fio &
        last_pid=$!
        sleep 3
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        wait $last_pid || true

        reboot_all_vms
        prepare_vms

        prepare_fio_cmd_tc2_iter2 "$used_vms"
        $run_fio

        vm_shutdown_all
        spdk_vhost_kill
}

function hotdetach_tc3() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms
        prepare_fio_cmd_tc3_iter1 "$used_vms"
        $run_fio &
        last_pid=$!
        sleep 3
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        wait $last_pid

        reboot_all_vms
        prepare_vms

        prepare_fio_cmd_tc2_iter2 "$used_vms"
        $run_fio
        vm_shutdown_all
        spdk_vhost_kill
}

function hotdetach_tc4() {
        used_vms=""
        run_vhost
        setup_and_run_vms
        prepare_vms
        prepare_fio_cmd_tc2_iter1 "0"
        $run_fio &
        first_fio_pid=$!
        prepare_fio_cmd_tc3_iter1 "$used_vms"
        $run_fio &
        second_fio_pid=$!
        sleep 3
        $rpc_py remove_vhost_scsi_dev naa.Nvme0n1p0.0 0
        wait $first_fio_pid || true
        wait $second_fio_pid

        reboot_all_vms
        prepare_vms

        prepare_fio_cmd_tc2_iter2 "$used_vms"
        $run_fio

        vm_shutdown_all
        spdk_vhost_kill
}

check_qemu
check_spdk
gen_hotdetach_config
hotdetach_tc1
hotdetach_tc2
hotdetach_tc3
hotdetach_tc4
