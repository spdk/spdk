#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

rpc_py="$rootdir/scripts/rpc.py"
conf_file="$testdir/bdev.conf"

function setup_bdev_conf() {
# Create a file to be used as an AIO backend
dd if=/dev/zero of=/tmp/aiofile bs=2048 count=5000

	cat >$conf_file <<-EOF
		[AIO]
		  AIO /tmp/aiofile AIO0 2048

		[Malloc]
		  NumberOfLuns 6
		  LunSizeInMB 32

		[Split]
		  Split Malloc1 2
		  Split Malloc2 8 4

		[Passthru]
		  PT Malloc3 TestPT

		[QoS]
		  Limit_IOPS Malloc0 20000
		  Limit_BPS Malloc3 100

		[RAID0]
		  Name raid0
		  StripSize 64
		  NumDevices 2
		  RaidLevel 0
		  Devices Malloc4 Malloc5
	EOF
}

function setup_nvme_conf() {
	$rootdir/scripts/gen_nvme.sh >> $conf_file
}

function setup_gpt_conf() {
	setup_nvme_conf
	if grep -q Nvme0 $conf_file; then
		part_dev_by_gpt $conf_file Nvme0n1 $rootdir
	else
		return 1
	fi
}

function setup_crypto_conf() {
	cat >$conf_file <<-EOF
		[Malloc]
		NumberOfLuns 3
		LunSizeInMB 16
	EOF
	$testdir/gen_crypto.sh Malloc0 Malloc1 Malloc2 >> $conf_file
}

function setup_pmem_conf() {
	if hash pmempool; then
		rm -f /tmp/spdk-pmem-pool
		pmempool create blk --size=32M 512 /tmp/spdk-pmem-pool
		echo "[Pmem]" >> $conf_file
		echo "  Blk /tmp/spdk-pmem-pool Pmem0" >> $conf_file
	else
		return 1
	fi
}

function setup_rbd_conf() {
	timing_enter rbd_setup
	rbd_setup 127.0.0.1
	timing_exit rbd_setup

	$rootdir/scripts/gen_rbd.sh >> $conf_file
}

function bdev_bounds() {
	$testdir/bdevio/bdevio -w -s $PRE_RESERVED_MEM -c $conf_file &
	bdevio_pid=$!
	trap 'killprocess $bdevio_pid; exit 1' SIGINT SIGTERM EXIT
	echo "Process bdevio pid: $bdevio_pid"
	waitforlisten $bdevio_pid
	$testdir/bdevio/tests.py perform_tests
	killprocess $bdevio_pid
	trap - SIGINT SIGTERM EXIT
}

function nbd_function_test() {
	if [ $(uname -s) = Linux ] && modprobe -n nbd; then
		local rpc_server=/var/tmp/spdk-nbd.sock
		local conf=$1
		local nbd_all=($(ls /dev/nbd* | grep -v p))
		local bdev_all=($bdevs_name)
		local nbd_num=${#bdevs_all[@]}
		if [ ${#nbd_all[@]} -le $nbd_num ]; then
			nbd_num=${#nbd_all[@]}
		fi
		local nbd_list=(${nbd_all[@]:0:$nbd_num})
		local bdev_list=(${bdev_all[@]:0:$nbd_num})

		if [ ! -e $conf ]; then
			return 1
		fi

		modprobe nbd
		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -c ${conf} &
		nbd_pid=$!
		trap 'killprocess $nbd_pid; exit 1' SIGINT SIGTERM EXIT
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid $rpc_server

		nbd_rpc_start_stop_verify $rpc_server "${bdev_list[*]}"
		nbd_rpc_data_verify $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"

		killprocess $nbd_pid
		trap - SIGINT SIGTERM EXIT
	fi

	return 0
}

function fio_test_suite() {
	# Generate the fio config file given the list of all unclaimed bdevs
	fio_config_gen $testdir/bdev.fio verify AIO
	for b in $(echo $bdevs | jq -r '.name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	if [ $RUN_NIGHTLY_FAILING -eq 0 ]; then
		local fio_params="--ioengine=spdk_bdev --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio --spdk_conf=./test/bdev/bdev.conf"
	else
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		local fio_params="--ioengine=spdk_bdev --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio --spdk_conf=./test/bdev/bdev.conf"
	fi

	run_test "bdev_fio_rw_verify" fio_bdev $fio_params --spdk_mem=$PRE_RESERVED_MEM \
	--output=$output_dir/blockdev_fio_verify.txt
	rm -f ./*.state
	rm -f $testdir/bdev.fio

	# Generate the fio config file given the list of all unclaimed bdevs that support unmap
	fio_config_gen $testdir/bdev.fio trim
	if [ "$(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name')" != "" ]; then
		for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
			fio_config_add_job $testdir/bdev.fio $b
		done
	else
		rm -f $testdir/bdev.fio
		return 0
	fi

	run_test "bdev_fio_trim" fio_bdev $fio_params --output=$output_dir/blockdev_trim.txt
	rm -f ./*.state
	rm -f $testdir/bdev.fio
}

function get_io_result() {
	local limit_type=$1
	local qos_dev=$2
	local iostat_result
	iostat_result=$($rootdir/scripts/iostat.py -d -i 1 -t $QOS_RUN_TIME | grep $qos_dev | tail -1)
	if [ $limit_type = IOPS ]; then
		iostat_result=$(awk '{print $2}' <<< $iostat_result)
	elif [ $limit_type = BANDWIDTH ]; then
		iostat_result=$(awk '{print $6}' <<< $iostat_result)
	fi

	echo ${iostat_result/.*}
}


function run_qos_test() {
	local qos_limit=$1
	local qos_result=0

	qos_result=$(get_io_result $2 $3)
	if [ $2 = BANDWIDTH ]; then
		qos_limit=$((qos_limit*1024))
	fi
	lower_limit=$((qos_limit*9/10))
	upper_limit=$((qos_limit*11/10))

	# QoS realization is related with bytes transfered. It currently has some variation.
	if [ $qos_result -lt $lower_limit ] || [ $qos_result -gt $upper_limit ]; then
		echo "Failed to limit the io read rate of NULL bdev by qos"
		$rpc_py bdev_null_delete $QOS_DEV_1
		$rpc_py bdev_null_delete $QOS_DEV_2
		killprocess $QOS_PID
		exit 1
	fi
}

function qos_function_test() {
	local qos_lower_iops_limit=1000
	local qos_lower_bw_limit=2
	local io_result=0
	local iops_limit=0
	local bw_limit=0

	io_result=$(get_io_result IOPS $QOS_DEV_1)
	# Set the IOPS limit as one quarter of the measured performance without QoS
	iops_limit=$(((io_result/4)/qos_lower_iops_limit*qos_lower_iops_limit))
	if [ $iops_limit -gt $qos_lower_iops_limit ]; then

		# Run bdevperf with IOPS rate limit on bdev 1
		$rpc_py bdev_set_qos_limit --rw_ios_per_sec $iops_limit $QOS_DEV_1
		run_test "bdev_qos_iops" run_qos_test $iops_limit IOPS $QOS_DEV_1

		# Run bdevperf with bandwidth rate limit on bdev 2
		# Set the bandwidth limit as 1/10 of the measure performance without QoS
		bw_limit=$(get_io_result BANDWIDTH $QOS_DEV_2)
		bw_limit=$((bw_limit/1024/10))
		if [ $bw_limit -lt $qos_lower_bw_limit ]; then
			bw_limit=$qos_lower_bw_limit
		fi
		$rpc_py bdev_set_qos_limit --rw_mbytes_per_sec $bw_limit $QOS_DEV_2
		run_test "bdev_qos_bw" run_qos_test $bw_limit BANDWIDTH $QOS_DEV_2

		# Run bdevperf with additional read only bandwidth rate limit on bdev 1
		$rpc_py bdev_set_qos_limit --r_mbytes_per_sec $qos_lower_bw_limit $QOS_DEV_1
		run_test "bdev_qos_ro_bw" run_qos_test $qos_lower_bw_limit BANDWIDTH $QOS_DEV_1
	else
		echo "Actual IOPS without limiting is too low - exit testing"
	fi
}

function qos_test_suite() {
	# Run bdevperf with QoS disabled first
	$testdir/bdevperf/bdevperf -z -m 0x2 -q 256 -o 4096 -w randread -t 60 &
	QOS_PID=$!
	echo "Process qos testing pid: $QOS_PID"
	trap 'killprocess $QOS_PID; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $QOS_PID

	$rpc_py bdev_null_create $QOS_DEV_1 128 512
	waitforbdev $QOS_DEV_1
	$rpc_py bdev_null_create $QOS_DEV_2 128 512
	waitforbdev $QOS_DEV_2

	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests &
	qos_function_test

	$rpc_py bdev_null_delete $QOS_DEV_1
	$rpc_py bdev_null_delete $QOS_DEV_2
	killprocess $QOS_PID
	trap - SIGINT SIGTERM EXIT
}

# Inital bdev creation and configuration
#-----------------------------------------------------
QOS_DEV_1="Null_0"
QOS_DEV_2="Null_1"
QOS_RUN_TIME=5

if [ $(uname -s) = Linux ]; then
	# Test dynamic memory management. All hugepages will be reserved at runtime
	PRE_RESERVED_MEM=0
else
	# Dynamic memory management is not supported on BSD
	PRE_RESERVED_MEM=2048
fi

test_type=$1
[ -z "$test_type" ] && setup_bdev_conf
if [ -n "$test_type" ]; then
	case "$test_type" in
	nvme )
		setup_nvme_conf;;
	gpt )
		setup_gpt_conf;;
	crypto )
		setup_crypto_conf;;
	pmem )
		setup_pmem_conf;;
	rbd )
		setup_rbd_conf;;
	* )
		echo "invalid test name"
		exit 1
		;;
	esac
fi

bdevs=$(discover_bdevs $rootdir $conf_file | jq -r '.[] | select(.claimed == false)')
bdevs_name=$(echo $bdevs | jq -r '.name')
bdev_list=($bdevs_name)
hello_world_bdev=${bdev_list[0]}
# End bdev configuration
#-----------------------------------------------------

run_test "bdev_hello_world" $rootdir/examples/bdev/hello_world/hello_bdev -c $conf_file -b $hello_world_bdev
run_test "bdev_bounds" bdev_bounds
run_test "bdev_nbd" nbd_function_test $conf_file "$bdevs_name"
if [ -d /usr/src/fio ]; then
        if [ "$test_type" = "nvme" ] || [ "$test_type" = "gpt" ]; then
                # TODO: once we get real multi-ns drives, re-enable this test for NVMe.
                echo "skipping fio tests on NVMe due to multi-ns failures."
        else
	        run_test "bdev_fio" fio_test_suite
        fi
else
	echo "FIO not available"
	exit 1
fi

run_test "bdev_verify" $testdir/bdevperf/bdevperf -c $conf_file -q 128 -o 4096 -w verify -t 5
run_test "bdev_write_zeroes" $testdir/bdevperf/bdevperf -c $conf_file -q 128 -o 4096 -w write_zeroes -t 1

if [ -z $test_type ]; then
	run_test "bdev_qos" qos_test_suite
fi

# Temporarily disabled - infinite loop
# if [ $RUN_NIGHTLY -eq 1 ]; then
	# run_test "bdev_reset" $testdir/bdevperf/bdevperf -c $conf_file -q 16 -w reset -o 4096 -t 60
# fi

# Bdev and configuration cleanup below this line
#-----------------------------------------------------
if [ "$test_type" = "gpt" ]; then
	part_dev_by_gpt $conf_file Nvme0n1 $rootdir reset
fi

rm -f /tmp/aiofile
rm -f /tmp/spdk-pmem-pool
rm -f $conf_file
rbd_cleanup
