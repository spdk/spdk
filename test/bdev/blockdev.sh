#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"

function run_fio()
{
	if [ $RUN_NIGHTLY -eq 0 ]; then
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio "$@"
	elif [ $RUN_NIGHTLY_FAILING -eq 1 ]; then
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio "$@"
	fi
}

source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

function nbd_function_test() {
	if [ $(uname -s) = Linux ] && modprobe -n nbd; then
		local rpc_server=/var/tmp/spdk-nbd.sock
		local conf=$1
		local nbd_num=6
		local nbd_all=(`ls /dev/nbd* | grep -v p`)
		local bdev_all=($bdevs_name)
		local nbd_list=(${nbd_all[@]:0:$nbd_num})
		local bdev_list=(${bdev_all[@]:0:$nbd_num})

		if [ ! -e $conf ]; then
			return 1
		fi

		modprobe nbd
		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -c ${conf} &
		nbd_pid=$!
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid $rpc_server

		nbd_rpc_data_verify $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"

		$rpc_py -s $rpc_server delete_passthru_bdev TestPT

		killprocess $nbd_pid
	fi

	return 0
}

function check_qos_works_well() {
	local enable_limit=$1
	local qos_limit=$2

	if [ $LIMIT_TYPE = IOPS ]; then
		io_result=`$BDEV_PERF | awk '/Malloc/ {print "%i",$4}'`
	else
		io_result=`$BDEV_PERF | awk '/Malloc/ {print "%i",$6}'`
	fi

	if [ $enable_limit = true ]; then
		down_limit=`echo "$qos_limit 0.9" | awk '{printf("%i",$1*$2)}'`
		up_limit=`echo "$qos_limit 1.05" | awk '{printf("%i",$1*$2)}'`
		#QoS realization is related with bytes transfered.It currently has some variation.
		if [ $io_result -lt $down_limit -o $io_result -gt $up_limit ]; then
			echo "Failed to limit the io read rate of malloc bdev by qos"
			exit 1
		fi
	else
		if [ $io_result -lt $qos_limit ]; then
			echo "$io_result by disabled QoS less than $qos_limit - exit QoS testing"
			$RUN_QOS=false
		fi
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
	$rootdir/scripts/gen_crypto.sh Malloc6 >> $testdir/bdev.conf
fi

if hash pmempool; then
	rm -f /tmp/spdk-pmem-pool
	pmempool create blk --size=32M 512 /tmp/spdk-pmem-pool
	echo "[Pmem]" >> $testdir/bdev.conf
	echo "  Blk /tmp/spdk-pmem-pool Pmem0" >> $testdir/bdev.conf
fi

timing_enter hello_bdev
if grep -q Nvme0 $testdir/bdev.conf; then
	$rootdir/examples/bdev/hello_world/hello_bdev -c $testdir/bdev.conf -b Nvme0n1
fi
timing_exit hello_bdev

timing_enter bounds
if [ $(uname -s) = Linux ]; then
	# Test dynamic memory management. All hugepages will be reserved at runtime
	PRE_RESERVED_MEM=0
else
	# Dynamic memory management is not supported on BSD
	PRE_RESERVED_MEM=2048
fi
$testdir/bdevio/bdevio -s $PRE_RESERVED_MEM -c $testdir/bdev.conf
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

if [ -d /usr/src/fio ] && [ $SPDK_RUN_ASAN -eq 0 ]; then
	timing_enter fio

	timing_enter fio_rw_verify
	# Generate the fio config file given the list of all unclaimed bdevs
	fio_config_gen $testdir/bdev.fio verify
	for b in $(echo $bdevs | jq -r '.name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	run_fio --spdk_conf=./test/bdev/bdev.conf --spdk_mem=$PRE_RESERVED_MEM

	rm -f *.state
	rm -f $testdir/bdev.fio
	timing_exit fio_rw_verify

	timing_enter fio_trim
	# Generate the fio config file given the list of all unclaimed bdevs that support unmap
	fio_config_gen $testdir/bdev.fio trim
	for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	run_fio --spdk_conf=./test/bdev/bdev.conf

	rm -f *.state
	rm -f $testdir/bdev.fio
	timing_exit fio_trim
	report_test_completion "bdev_fio"
	timing_exit fio
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

# Create conf file for bdevperf Malloc device for QoS testing
cat > $testdir/bdev_qos.conf << EOL
[Malloc]
  NumberOfLuns 1
  LunSizeInMB 128
EOL

IOPS_LIMIT=20000
BANDWIDTH_LIMIT=20
READ_BANDWIDTH_LIMIT=10
LIMIT_TYPE=IOPS
RUN_QOS=true
BDEV_PERF="$testdir/bdevperf/bdevperf -c $testdir/bdev_qos.conf -q 128 -o 4096 -w randread -t 5"

# Run bdevperf with QoS disabled
check_qos_works_well false $IOPS_LIMIT
if [ $RUN_QOS = true ]; then
	cat >> $testdir/bdev_qos.conf << EOL
[QoS]
  Limit_IOPS Malloc0 $IOPS_LIMIT
EOL

	check_qos_works_well true $IOPS_LIMIT
fi
rm -f $testdir/bdev_qos.conf

if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir reset
fi

rm -f /tmp/aiofile
rm -f /tmp/spdk-pmem-pool
rm -f $testdir/bdev.conf
trap - SIGINT SIGTERM EXIT
rbd_cleanup
report_test_completion "bdev"
timing_exit bdev
