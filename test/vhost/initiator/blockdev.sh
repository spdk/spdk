#!/usr/bin/env bash

set -x

testdir=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $testdir/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $testdir/../../../../ && pwd)"
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"

guest_bdevs=""

function run_host_fio() {
        LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev \
                --runtime=10 $testdir/bdev.fio "$2" --spdk_mem=1024
        fio_status=$?
        if [ $fio_status != $3 ]; then
                echo "Test $1 failed."
                spdk_vhost_kill
                vm_shutdown_all
                exit 1
        fi
}

function prepare_fio_job_for_guest() {
        rw="$1"
        fio_bdevs="$2"
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "size=100m" >> $testdir/bdev.fio
                echo "io_size=400m" >> $testdir/bdev.fio
        fi
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "[job_write]" >> $testdir/bdev.fio
                echo "stonewall" >> $testdir/bdev.fio
                echo "rw=write" >> $testdir/bdev.fio
                echo "do_verify=0" >> $testdir/bdev.fio
                echo -n "filename=" >> $testdir/bdev.fio
                for b in $(echo $fio_bdevs | jq -r '.name'); do
                        echo -n "$b:" >> $testdir/bdev.fio
                done
                echo "" >> $testdir/bdev.fio
        fi
        echo "[job_$rw]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=$rw" >> $testdir/bdev.fio
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
}

function prepare_fio_job_for_host() {
        rw="$1"
        fio_bdevs="$2"
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "size=100m" >> $testdir/bdev.fio
                echo "io_size=400m" >> $testdir/bdev.fio
        fi
        i=0
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                for b in $(echo $fio_bdevs | jq -r '.name'); do
                        echo "[job_write_$i]" >> $testdir/bdev.fio
                        echo "rw=write" >> $testdir/bdev.fio
                        echo "do_verify=0" >> $testdir/bdev.fio
                        echo "filename=$b" >> $testdir/bdev.fio
                done
                i+=1
        fi
        i=0
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo "[job_$rw_$i]" >> $testdir/bdev.fio
                if [ $i == 0 ]; then
                        echo "stonewall" >> $testdir/bdev.fio
                fi
                echo "rw=$rw" >> $testdir/bdev.fio
                echo "filename=$b" >> $testdir/bdev.fio
                i+=1
        done
}

function prepare_fio_job_for_unmap() {
        fio_bdevs="$1"
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
        echo "" >> $testdir/bdev.fio
        echo "size=100m" >> $testdir/bdev.fio
        echo "io_size=400m" >> $testdir/bdev.fio

        # Check that sequential TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_sequential]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=trim" >> $testdir/bdev.fio
        echo "trim_verify_zero=1" >> $testdir/bdev.fio

        # Check that random TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_random]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=randtrim" >> $testdir/bdev.fio
        echo "trim_verify_zero=1" >> $testdir/bdev.fio

        # Check that after TRIM/UNMAP operation disk space can be used for read
        # by using write with verify (which implies reads)
        echo "[write]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=write" >> $testdir/bdev.fio
}

function prepare_fio_job_4G() {
        rw="$1"
        fio_bdevs="$2"
        echo "size=1G" >> $testdir/bdev.fio
        echo "io_size=4G" >> $testdir/bdev.fio
        echo "offset=4G" >> $testdir/bdev.fio
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "[job_write]" >> $testdir/bdev.fio
                echo "stonewall" >> $testdir/bdev.fio
                echo "rw=write" >> $testdir/bdev.fio
                echo "do_verify=0" >> $testdir/bdev.fio
                echo -n "filename=" >> $testdir/bdev.fio
                for b in $(echo $fio_bdevs | jq -r '.name'); do
                    echo -n "$b:" >> $testdir/bdev.fio
                done
                echo "" >> $testdir/bdev.fio
        fi
        echo "[job_$rw]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=$rw" >> $testdir/bdev.fio
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
}

function start_and_prepare_vm() {
        os="/home/sys_sgsw/vhost_vm_image.qcow2"
        test_type="spdk_vhost_scsi"
        disk="Nvme0n1p0:Nvme0n1p1"
        force_vm_num="0"
        vm_num="0"
        os_mode="original"
        setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
        setup_cmd+=" --os=$os --disk=$disk -f $force_vm_num --os-mode=$os_mode --memory=4096 --queue_num=20"
        $setup_cmd

        echo "used_vms: $vm_num"
        $COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $vm_num
        vm_wait_for_boot 600 $vm_num
        vm_scp $vm_num -r $rootdir "127.0.0.1:/root/spdk"
        vm_ssh $vm_num " cd spdk ; make clean ; ./configure --with-fio=/root/fio_src ; make -j3"
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
        disc_number=$4
        fio_job="$testdir/bdev.fio"
        conf_file="$testdir/bdevvm.conf"
        run_fio="LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio"
        run_fio+=" --ioengine=spdk_bdev --runtime=10 /root/bdev.fio $2 --spdk_mem=1024"

        vm_dir=$VM_BASE_DIR/$vm_num
        qemu_mask_param="VM_${vm_num}_qemu_mask"
        host_name="VM-$vm_num-${!qemu_mask_param}"
        echo "INFO: Setting up hostname: $host_name"
        vm_ssh $vm_num "hostname $host_name"
        vm_start_fio_server $fio_bin $readonly $vm_num
        vm_scp $vm_num $fio_job "127.0.0.1:/root/bdev.fio"
        disc_bdevs=" ./spdk/scripts/setup.sh ; . ./spdk/scripts/autotest_common.sh ; "
        if [ $disc_number == 1 ]; then
                disc_bdevs+='bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '"'"'.[] | select(.claimed == false)'"'"') ; '
                disc_bdevs+='virtio_bdevs="" ; for b in $(echo $bdevs | jq -r '"'"'.name'"'"') ; do if [ $b == "VirtioScsi0t0" ]; then virtio_bdevs+="$b:" ; fi ; done ; '
        elif [ $disc_number == 2 ]; then
                disc_bdevs+='bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '"'"'.[] | select(.claimed == false)'"'"') ; '
                disc_bdevs+='virtio_bdevs="" ; for b in $(echo $bdevs | jq -r '"'"'.name'"'"') ; do if [ $b == "VirtioScsi0t0" ] || [ $b == "VirtioScsi1t0" ]; then virtio_bdevs+="$b:" ; fi ; done ; '
        elif [ $disc_number == 3 ]; then
                disc_bdevs+='bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '"'"'.[] | select(.claimed == false)'"'"') ; '
                disc_bdevs+='virtio_bdevs="" ; for b in $(echo $bdevs | jq -r '"'"'.name'"'"') ; do if [ $b == "VirtioScsi1t0" ] || [ $b == "VirtioScsi1t1" ]; then virtio_bdevs+="$b:" ; fi ; done ; '
        else
                disc_bdevs+='bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '"'"'.[] | select(.claimed == false)'"'"') ; '
                disc_bdevs+='virtio_bdevs="" ; for b in $(echo $bdevs | jq -r '"'"'.name'"'"') ; do virtio_bdevs+="$b:" ; done ; '
        fi
        disc_bdevs+='sed -i "s|filename=|filename=$virtio_bdevs|g" /root/bdev.fio'
        vm_ssh $vm_num "$disc_bdevs"
        vm_ssh $vm_num "cat /root/bdev.fio"
        vm_ssh $vm_num $run_fio
        fio_status=$?
        if [ $fio_status != $3 ]; then
                vm_ssh $vm_num "ls"
                echo "Test $1 failed."
                spdk_vhost_kill
                vm_shutdown_al
                exit 1
        fi
}

source $rootdir/test/vhost/common/common.sh
$rootdir/scripts/gen_nvme.sh
spdk_vhost_run $testdir
$rpc_py construct_malloc_bdev 128 512
$rpc_py construct_malloc_bdev 128 4096
$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
$rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.0 0 Nvme0n1p1
$rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.0 1 Nvme0n1p2
$rpc_py add_vhost_scsi_lun naa.Nvme0n1.2 0 Nvme0n1p3
$rpc_py add_vhost_scsi_lun naa.Nvme0n1.3 0 Nvme0n1p4
$rpc_py add_vhost_scsi_lun naa.Nvme0n1.3 1 Nvme0n1p5
$rpc_py add_vhost_scsi_lun naa.Malloc0.4 0 Malloc0
$rpc_py add_vhost_scsi_lun naa.Malloc1.5 0 Malloc1
$rpc_py get_bdevs
bdevs=$($rpc_py get_bdevs | jq -r '.[] | .name')

timing_enter bdev

cp $testdir/bdev.conf.in $testdir/bdev.conf
sed -i "s|/tmp/vhost.0|$rootdir/../vhost/naa.Nvme0n1.2|g" $testdir/bdev.conf
sed -i "s|/tmp/vhost.1|$rootdir/../vhost/naa.Malloc0.4|g" $testdir/bdev.conf
sed -i "s|/tmp/vhost.2|$rootdir/../vhost/naa.Malloc1.5|g" $testdir/bdev.conf

cp $testdir/bdevms.conf.in $testdir/bdevms.conf
sed -i "s|/tmp/vhost.0|$rootdir/../vhost/naa.Nvme0n1.2|g" $testdir/bdevms.conf
sed -i "s|/tmp/vhost.1|$rootdir/../vhost/naa.Malloc0.4|g" $testdir/bdevms.conf

cp $testdir/bdevmq.conf.in $testdir/bdevmq.conf
sed -i "s|/tmp/vhost.0|$rootdir/../vhost/naa.Nvme0n1.3|g" $testdir/bdevmq.conf

timing_enter bounds
$rootdir/test/lib/bdev/bdevio/bdevio $testdir/bdev.conf
timing_exit bounds

timing_enter bdev_svc
vbdevs=$(discover_bdevs $rootdir $testdir/bdev.conf 5261 | jq -r '.[] | select(.claimed == false)')
msbdevs=$(discover_bdevs $rootdir $testdir/bdevms.conf 5261 | jq -r '.[] | select(.claimed == false)')
mqbdevs=$(discover_bdevs $rootdir $testdir/bdevmq.conf 5261 | jq -r '.[] | select(.claimed == false)')
timing_exit bdev_svc
if [ -d /usr/src/fio ]; then
        timing_enter fio
        if [ $RUN_NIGHTLY -eq 1 ]; then
                fio_rw=("read" "randwrite" "randread" "rw" "randrw")
        else
                fio_rw=("read")
        fi

	{
                #Test for guest
                start_and_prepare_vm
                run_guest_bdevio
                for rw in "${fio_rw[@]}"; do
                        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                        prepare_fio_job_for_guest "$rw" ""
                        run_guest_fio "Guest $bdev" --spdk_conf=/root/bdev.conf 0 1
                        rm -f *.state
                        rm -f $testdir/bdev.fio
                done

                #Guest test for unmap
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                prepare_fio_job_for_unmap ""
                run_guest_fio "Guest unmap" --spdk_conf=/root/bdev.conf 0 1
                rm -f *.state
                rm -f $testdir/bdev.fio

                #Guest test for +4G
                for rw in "${fio_rw[@]}"; do
                        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                        prepare_fio_job_4G "$rw" ""
                        #run_guest_fio "Guest +4G" --spdk_conf=/root/bdev.conf 0 1
                        rm -f *.state
                        rm -f $testdir/bdev.fio
                done

                #Guest test for multiqeueu
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                prepare_fio_job_for_guest "write" ""
                run_guest_fio "Guest multiqueue" --spdk_conf=/root/bdev.conf 0 3
                rm -f *.state
                rm -f $testdir/bdev.fio

                #Guest test for multiple socket
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                prepare_fio_job_for_guest "write" ""
                run_guest_fio "Guest multiple socket" --spdk_conf=/root/bdev.conf 0 2
                rm -f *.state
                rm -f $testdir/bdev.fio

                vm_shutdown_all
	} &
        guest_pid=$!

        #Test for host
        for rw in "${fio_rw[@]}"; do
                timing_enter fio_rw_verify
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                prepare_fio_job_for_host "$rw" "$vbdevs"
                run_host_fio "Host $vbdevs" --spdk_conf=$testdir/bdev.conf 0

                rm -f *.state
                rm -f $testdir/bdev.fio
                timing_exit fio_rw_verify
        done

        #Host test for unmap
        i=0
        test_bdevs=($bdevs)
        for vbdev in $(echo $vbdevs | jq -r '.name'); do
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                echo "asdas"$vbdev
                virtio_bdevs=$(echo $vbdevs | jq -r ". | select(.name == \"$vbdev\")")
                prepare_fio_job_for_unmap "$virtio_bdevs"
                run_host_fio "Host unmap ${test_bdevs[i]}" --spdk_conf=$testdir/bdev.conf 0
                rm -f *.state
                rm -f $testdir/bdev.fio
                i+=1
        done

        #Host test for multiple socket
        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
        prepare_fio_job_for_host "read" "$msbdevs"
        run_host_fio "Host multiple socket" --spdk_conf=$testdir/bdevms.conf 0
        rm -f *.state
        rm -f $testdir/bdev.fio

        #Host test for multiqueue
        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
        prepare_fio_job_for_host "read" "$mqbdevs"
        run_host_fio "Host multiqueue" --spdk_conf=$testdir/bdevmq.conf 0
        rm -f *.state
        rm -f $testdir/bdev.fio

        #Host test for +4G
        for rw in "${fio_rw[@]}"; do
                timing_enter fio_4G_rw_verify
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                virtio_nvme_bdev=$(echo $vbdevs | jq -r '. | select(.name == "VirtioScsi0t0")')
                prepare_fio_job_4G "$rw" "$virtio_nvme_bdev"
                #run_host_fio "Host +4G" --spdk_conf=$testdir/bdev.conf 0

                rm -f *.state
                rm -f $testdir/bdev.fio
                timing_exit fio_4G_rw_verify
        done

        timing_exit fio
fi
rm -f $testdir/bdev.conf
rm -f $testdir/bdevmq.conf
rm -f $testdir/bdevms.conf
wait $guest_pid
timing_exit bdev
spdk_vhost_kill
