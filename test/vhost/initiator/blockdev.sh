#!/usr/bin/env bash

set -x
testdir=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $testdir/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $testdir/../../../../ && pwd)"
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

guest_bdevs=""

function format_disk_512() {
        $rootdir/scripts/setup.sh reset
        sleep 4
        last_nvme_disk=$( sudo nvme list | tail -n 1 )
        last_nvme_disk="$( cut -d ' ' -f 1 <<< "$last_nvme_disk" )"
        sudo nvme format -l 0 $last_nvme_disk
        sudo NRHUGE=8 $rootdir/scripts/setup.sh
        sleep 4
}

function format_disk_4096() {
        sudo $rootdir/scripts/setup.sh reset
        sleep 4
        last_nvme_disk=$( sudo nvme list | tail -n 1 )
        last_nvme_disk="$( cut -d ' ' -f 1 <<< "$last_nvme_disk" )"
        sudo nvme format -l 3 $last_nvme_disk
        sudo NRHUGE=8 $rootdir/scripts/setup.sh
        sleep 4
}

function run_host_fio() {
        LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=128 --bs=192k --runtime=10 $testdir/bdev.fio "$@"
        fio_status=$?
        if [ $fio_status != 0 ]; then
                echo "Test $1 failed."
                spdk_vhost_kill
                exit 1
        fi
}

function prepare_fio_job() {
	rw="$1"
	bdevs="$2"
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                 echo "size=2m" >> $testdir/bdev.fio
                 echo "io_size=10m" >> $testdir/bdev.fio
        else
                 echo "size=1G" >> $testdir/bdev.fio
                 echo "io_size=4G" >> $testdir/bdev.fio
        fi
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "[job_write]" >> $testdir/bdev.fio
                echo "stonewall" >> $testdir/bdev.fio
                echo "rw=write" >> $testdir/bdev.fio
                echo "do_verify=0" >> $testdir/bdev.fio
                echo -n "filename=" >> $testdir/bdev.fio
                for b in $(echo $bdevs | jq -r '.name'); do
                        echo -n "$b:" >> $testdir/bdev.fio
                done
                echo "" >> $testdir/bdev.fio
        fi
        echo "[job_$rw]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=$rw" >> $testdir/bdev.fio
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $bdevs | jq -r '.name'); do
        	echo -n "$b:" >> $testdir/bdev.fio
        done
}

function start_and_prepare_vm() {
	os="/home/sys_sgsw/vhost_vm_image.qcow2"
	#os="/home/sys_sgsw/working_ubuntu_16_04.qcow2"
	test_type="spdk_vhost_scsi"
	disk="Nvme0n1"
	force_vm_num="0"
	vm_num="0"
	os_mode="original"
	setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
	setup_cmd+=" --os=$os --disk=$disk -f $force_vm_num --os-mode=$os_mode"
	$setup_cmd

	echo "used_vms: $vm_num"
	$COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $vm_num
	vm_wait_for_boot 600 $vm_num
        vm_ssh $vm_num "GRUB_CMDLINE_LINUX_DEFAULT=\"quiet splash default_hugepagesz=1G hugepagesz=1G hugepages=4\" >> /etc/default/grub"
        vm_ssh $vm_num "update-grub"
        vm_ssh $vm_num "reboot"
	vm_wait_for_boot 600 $vm_num
	init_fio="export http_proxy=http://proxy-mu.intel.com:911/ && git clone http://git.kernel.dk/fio.git fio_src &&"
        init_fio+=" cd fio_src && ./configure && make && make install"
        vm_ssh $vm_num "[ ! -d fio_src ] && $init_fio"
        init_spdk="export https_proxy=http://proxy-mu.intel.com:911/ ; "
	init_spdk+=" git clone https://github.com/spdk/spdk ; "
        init_spdk+=" cd spdk ; git submodule update --init ; "
	init_spdk+=" ./scripts/pkgdep.sh ;"
        pull_spdk="export https_proxy=http://proxy-mu.intel.com:911/ ; "
        pull_spdk+=" cd spdk ; git pull"
	vm_ssh $vm_num "[ -d spdk ] && $pull_spdk || $init_spdk"
        vm_ssh $vm_num "export https_proxy=http://proxy-mu.intel.com:911/ ; cd spdk ; git fetch https://review.gerrithub.io/spdk/spdk refs/changes/01/381201/2 && git checkout FETCH_HEAD"
	vm_ssh $vm_num "cd spdk; ./configure --with-fio=/root/fio_src ; make "
}

function run_guest_bdevio() {
        vm_num="0"
        conf_file="$testdir/bdevvm.conf"
        vm_scp $vm_num  $conf_file "127.0.0.1:/root/bdev.conf"
        timing_enter bounds
        vm_ssh $vm_num "./spdk/scripts/setup.sh ; /root/spdk/test/lib/bdev/bdevio/bdevio /root/bdev.conf"
        timing_exit bounds
}

function run_guest_fio() {
	echo "INFO: Running fio jobs ..."
        vm_num="0"
	readonly=""
	fio_job="$testdir/bdev.fio"
        conf_file="$testdir/bdevvm.conf"
	run_fio="LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev --iodepth=16 --bs=192k --runtime=10 /root/bdev.fio $2"

	vm_dir=$VM_BASE_DIR/$vm_num
	qemu_mask_param="VM_${vm_num}_qemu_mask"
	host_name="VM-$vm_num-${!qemu_mask_param}"
	echo "INFO: Setting up hostname: $host_name"
	vm_ssh $vm_num "hostname $host_name"
        install_jq="SYSTEM=`uname -s` ; if [ -s /etc/redhat-release ]; then yum install -y jq ; "
        install_jq+=" elif [ -f /etc/debian_version ] ; then apt-get install -y jq ; elif [ $SYSTEM == \"FreeBSD\" ] ; then pkg install jq ; else exit 1 ; fi"
        vm_ssh $vm_num $install_jq
	vm_start_fio_server $fio_bin $readonly $vm_num
	vm_scp $vm_num $fio_job "127.0.0.1:/root/bdev.fio"
        disc_bdevs=" ./spdk/scripts/setup.sh ; . ./spdk/scripts/autotest_common.sh ; "
        disc_bdevs+='bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '"'"'.[] | select(.claimed == false)'"'"') ; '
        disc_bdevs+='for b in $(echo $bdevs | jq -r '"'"'.name'"'"') ; do echo -n "$b" >> /root/bdev.fio ; done'
        #disc_bdevs="echo 'Virtio0' >> /root/bdev.fio"
        vm_ssh $vm_num "$disc_bdevs"
        vm_ssh $vm_num "cat /root/bdev.fio"
	vm_ssh $vm_num $run_fio
	fio_status=$?
        if [ $fio_status != 0 ]; then
                vm_ssh $vm_num "ls"
                echo "Test $1 failed."
                #spdk_vhost_kill
		#vm_shutdown_all
                exit 1
        fi
}

source $COMMON_DIR/common.sh
set +e

for host_type in "guest"; do
	for block_size in 512; do
		cp $testdir/vhost.conf.malloc${block_size} $testdir/vhost.conf.in
                if [ $RUN_NIGHTLY -eq 1 ]; then
                        format_disk_${block_size}
                fi
		spdk_vhost_run $testdir
		if [ $host_type == "guest" ];then
			start_and_prepare_vm
			echo
		fi
		#for bdev_type in "nvme" "malloc"; do
		for bdev_type in "nvme"; do
			timing_enter bdev

			cp $testdir/bdev.conf.in $testdir/bdev.conf
			if [ $bdev_type == "malloc" ]; then
			        sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.1|g" $testdir/bdev.conf
			elif [ $bdev_type == "nvme" ]; then
			        sed -i "s|/tmp/vhost.0|$rootdir/../vhost/naa.Nvme0n1.0|g" $testdir/bdev.conf
			fi
			if [ $host_type == "guest" ]; then
				#run_guest_bdevio
                                echo
			else
				timing_enter bounds
				$rootdir/test/lib/bdev/bdevio/bdevio $testdir/bdev.conf
				timing_exit bounds

				timing_enter bdev_svc
				bdevs=$(discover_bdevs $rootdir $testdir/bdev.conf 5261 | jq -r '.[] | select(.bdev_opened_for_write == false)')
				timing_exit bdev_svc
                        fi

			if [ -d /usr/src/fio ]; then
				timing_enter fio
			        for rw in "write" "read" "randwrite" "randread" "rw" "randrw"; do
				        timing_enter fio_rw_verify
			                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
					prepare_fio_job "$rw" "$bdevs"
			                cat $testdir/bdev.fio
					echo "HOST_TYPE: $host_type"
					if [ $host_type == "guest" ]; then
						run_guest_fio "$host_type - $block_size - $bdev_type" --spdk_conf=/root/bdev.conf
					else
					        run_host_fio "$host_type - $block_size - $bdev_type" --spdk_conf=$testdir/bdev.conf
					fi

				        rm -f *.state
				        rm -f $testdir/bdev.fio
				        timing_exit fio_rw_verify
			        done
				timing_exit fio
			fi

			timing_enter reset
			#$rootdir/test/lib/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
			timing_exit reset

			rm -f $testdir/bdev.conf
			timing_exit bdev
		if [ $host_type == "guest" ]; then
			vm_shutdown_all
		fi
		done
		spdk_vhost_kill
	done
done
