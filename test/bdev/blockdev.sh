#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

rpc_py="$rootdir/scripts/rpc.py"

function run_fio()
{
	if [ $RUN_NIGHTLY -eq 0 ]; then
		fio_bdev --ioengine=spdk_bdev --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio "$@"
	elif [ $RUN_NIGHTLY_FAILING -eq 1 ]; then
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		fio_bdev --ioengine=spdk_bdev --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio "$@"
	fi
}

function nbd_function_test() {
	if [ $(uname -s) = Linux ] && modprobe -n nbd; then
		local rpc_server=/var/tmp/spdk-nbd.sock
		local conf=$1
		local nbd_num=6
		local nbd_all=($(ls /dev/nbd* | grep -v p))
		local bdev_all=($bdevs_name)
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

		$rpc_py -s $rpc_server bdev_passthru_delete TestPT

		killprocess $nbd_pid
		trap - SIGINT SIGTERM EXIT
	fi

	return 0
}

QOS_DEV_1="Null_0"
QOS_DEV_2="Null_1"
QOS_RUN_TIME=5

function get_io_result() {
	local limit_type=$1
	local qos_dev=$2
	local iostat_result = $($rootdir/scripts/iostat.py -d -i 1 -t $QOS_RUN_TIME | grep $qos_dev | tail -1)
	if [ $limit_type = IOPS ]; then
		iostat_result=$(awk '{print $2}' <<< $iostat_result)
	elif [ $limit_type = BANDWIDTH ]
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
		run_qos_test $iops_limit IOPS $QOS_DEV_1

		# Run bdevperf with bandwidth rate limit on bdev 2
		# Set the bandwidth limit as 1/10 of the measure performance without QoS
		bw_limit=$(get_io_result BANDWIDTH $QOS_DEV_2)
		bw_limit=$((bw_limit/1024/10))
		if [ $bw_limit -lt $qos_lower_bw_limit ]; then
			bw_limit=$qos_lower_bw_limit
		fi
		$rpc_py bdev_set_qos_limit --rw_mbytes_per_sec $bw_limit $QOS_DEV_2
		run_qos_test $bw_limit BANDWIDTH $QOS_DEV_2

		# Run bdevperf with additional read only bandwidth rate limit on bdev 1
		$rpc_py bdev_set_qos_limit --r_mbytes_per_sec $qos_lower_bw_limit $QOS_DEV_1
		run_qos_test $qos_lower_bw_limit BANDWIDTH $QOS_DEV_1
	else
		echo "Actual IOPS without limiting is too low - exit testing"
	fi
}

timing_enter bdev

# Create a file to be used as an AIO backend
dd if=/dev/zero of=/tmp/aiofile bs=2048 count=5000

cp $testdir/bdev.conf.in $testdir/bdev.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/bdev.conf

if [ $SPDK_TEST_RBD -eq 1 ]; then
	timing_enter rbd_setup
	rbd_setup 127.0.0.1
	timing_exit rbd_setup

	$rootdir/scripts/gen_rbd.sh >> $testdir/bdev.conf
fi

if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
	$testdir/gen_crypto.sh Malloc6 Malloc7 >> $testdir/bdev.conf
fi

if hash pmempool; then
	rm -f /tmp/spdk-pmem-pool
	pmempool create blk --size=32M 512 /tmp/spdk-pmem-pool
	echo "[Pmem]" >> $testdir/bdev.conf
	echo "  Blk /tmp/spdk-pmem-pool Pmem0" >> $testdir/bdev.conf
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	timing_enter hello_bdev
	if grep -q Nvme0 $testdir/bdev.conf; then
		$rootdir/examples/bdev/hello_world/hello_bdev -c $testdir/bdev.conf -b Nvme0n1
	fi
	timing_exit hello_bdev
fi

timing_enter bounds
if [ $(uname -s) = Linux ]; then
	# Test dynamic memory management. All hugepages will be reserved at runtime
	PRE_RESERVED_MEM=0
else
	# Dynamic memory management is not supported on BSD
	PRE_RESERVED_MEM=2048
fi
$testdir/bdevio/bdevio -w -s $PRE_RESERVED_MEM -c $testdir/bdev.conf &
bdevio_pid=$!
trap 'killprocess $bdevio_pid; exit 1' SIGINT SIGTERM EXIT
echo "Process bdevio pid: $bdevio_pid"
waitforlisten $bdevio_pid
$testdir/bdevio/tests.py perform_tests
killprocess $bdevio_pid
trap - SIGINT SIGTERM EXIT
timing_exit bounds

timing_enter nbd_gpt
if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir
fi
timing_exit nbd_gpt

timing_enter bdev_svc
bdevs=$(discover_bdevs $rootdir $testdir/bdev.conf | jq -r '.[] | select(.claimed == false)')
timing_exit bdev_svc

timing_enter nbd
bdevs_name=$(echo $bdevs | jq -r '.name')
nbd_function_test $testdir/bdev.conf "$bdevs_name"
timing_exit nbd

if [ -d /usr/src/fio ]; then
	timing_enter fio

	timing_enter fio_rw_verify
	# Generate the fio config file given the list of all unclaimed bdevs
	fio_config_gen $testdir/bdev.fio verify AIO
	for b in $(echo $bdevs | jq -r '.name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	run_fio --spdk_conf=./test/bdev/bdev.conf --spdk_mem=$PRE_RESERVED_MEM --output=$output_dir/blockdev_fio_verify.txt

	rm -f ./*.state
	rm -f $testdir/bdev.fio
	timing_exit fio_rw_verify

	timing_enter fio_trim
	# Generate the fio config file given the list of all unclaimed bdevs that support unmap
	fio_config_gen $testdir/bdev.fio trim
	for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	run_fio --spdk_conf=./test/bdev/bdev.conf --output=$output_dir/blockdev_trim.txt

	rm -f ./*.state
	rm -f $testdir/bdev.fio
	timing_exit fio_trim
	report_test_completion "bdev_fio"
	timing_exit fio
else
	echo "FIO not available"
	exit 1
fi

# Create conf file for bdevperf with gpt
cat > $testdir/bdev_gpt.conf << EOL
[Gpt]
  Disable No
EOL

# Get Nvme info through filtering gen_nvme.sh's result
$rootdir/scripts/gen_nvme.sh >> $testdir/bdev_gpt.conf

# Run bdevperf with gpt
$testdir/bdevperf/bdevperf -c $testdir/bdev_gpt.conf -q 128 -o 4096 -w verify -t 5
$testdir/bdevperf/bdevperf -c $testdir/bdev_gpt.conf -q 128 -o 4096 -w write_zeroes -t 1
rm -f $testdir/bdev_gpt.conf

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Temporarily disabled - infinite loop
	timing_enter reset
	#$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -o 4096 -t 60
	timing_exit reset
	report_test_completion "nightly_bdev_reset"
fi

timing_enter qos

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

timing_exit qos

if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir reset
fi

rm -f /tmp/aiofile
rm -f /tmp/spdk-pmem-pool
rm -f $testdir/bdev.conf
rbd_cleanup
report_test_completion "bdev"
timing_exit bdev
