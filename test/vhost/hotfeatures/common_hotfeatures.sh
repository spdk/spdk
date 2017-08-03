#!/usr/bin/env bash

. $COMMON_DIR/common.sh

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

function setup_vm() {
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

function setup_and_run_vm() {
	setup_vm
	# Run everything
	$COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
	vm_wait_for_boot 600 $used_vms
}

function prepare_vm() {
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

function reboot_all_vm() {
        echo "Rebooting all vms"
        for vm_num in $used_vms; do
                vm_ssh $vm_num "reboot" || true
        done

        vm_wait_for_boot 600 $used_vms
}

function check_lsblk_in_vm() {
    for vm_num in $used_vms; do
        vm_ssh $vm_num "lsblk"
    done
}
function create_fio_cmd() {
    for vm_num in $used_vms; do
        vm_dir=$VM_BASE_DIR/$vm_num
        run_fio+="127.0.0.1:$(cat $vm_dir/fio_socket):"
        vm_check_scsi_location $vm_num
        for disk in $SCSI_DISK; do
            run_fio+="/dev/$disk:"
        done
        run_fio="${run_fio::-1}"
        run_fio+=","
    done
}

function gen_scsi_config_and_run_vhost() {

	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in

	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme0n1 4" >> $COMMON_DIR/vhost.conf.in

	echo "[VhostScsi0]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
	echo "[VhostScsi1]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p1.0" >> $COMMON_DIR/vhost.conf.in
	echo "[VhostScsi2]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p2.1" >> $COMMON_DIR/vhost.conf.in
	echo "[VhostScsi3]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p3.1" >> $COMMON_DIR/vhost.conf.in

	echo "==============="
	echo ""
	echo "INFO: running SPDK"
	echo ""
    spdk_vhost_run
    cp $COMMON_DIR/vhost.conf.tmp $COMMON_DIR/vhost.conf.in
    rm $COMMON_DIR/vhost.conf.tmp
	echo
}

function run_fio_in_vm(){
	echo "INFO: Running fio jobs ..."
	run_fio="python $COMMON_DIR/run_fio.py "
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
    create_fio_cmd

	run_fio="${run_fio%,}"
	run_fio+=" "
	run_fio="${run_fio::-1}"

    $run_fio
}
