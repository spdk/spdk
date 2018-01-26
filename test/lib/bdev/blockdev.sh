#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

function run_fio()
{
	if [ $RUN_NIGHTLY -eq 0 ]; then
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio "$@"
	else
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio "$@"
	fi
}

source $rootdir/scripts/autotest_common.sh
source $testdir/nbd/nbd_common.sh

function nbd_function_test() {
	if [ $(uname -s) = Linux ] && modprobe -n nbd; then
		local rpc_server=/var/tmp/spdk-nbd.sock
		local conf=$1
		local nbd_num=6
		local nbd_all=(`ls /dev/nbd*`)
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

		killprocess $nbd_pid
	fi

	return 0
}

timing_enter bdev

timing_enter bdev_passthru

cp $testdir/passthru/pt.conf.in $testdir/passthru/pt.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/passthru/pt.conf
cd $testdir/passthru
$rootdir/examples/blob/cli/blobcli -c $testdir/passthru/pt.conf -b TestPT -T $testdir/passthru/test.bs > $testdir/passthru/pt_test.out
cd -

# the tool leaves some trailing whitespaces that we need to strip out
sed -i 's/[[:space:]]*$//' $testdir/passthru/pt_test.out
$rootdir/test/app/match/match -v $testdir/passthru/pt_test.out.match

rm -f $testdir/passthru/test.bs
rm -f $testdir/passthru/pt_test.out
rm -f $testdir/passthru/pt.conf

timing exit bdev_passthru

# Create a file to be used as an AIO backend
dd if=/dev/zero of=/tmp/aiofile bs=2048 count=5000

cp $testdir/bdev.conf.in $testdir/bdev.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/bdev.conf

timing_enter bounds
$testdir/bdevio/bdevio $testdir/bdev.conf
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

	run_fio --spdk_conf=./test/lib/bdev/bdev.conf

	rm -f *.state
	rm -f $testdir/bdev.fio
	timing_exit fio_rw_verify

	timing_enter fio_trim
	# Generate the fio config file given the list of all unclaimed bdevs that support unmap
	fio_config_gen $testdir/bdev.fio trim
	for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	run_fio --spdk_conf=./test/lib/bdev/bdev.conf

	rm -f *.state
	rm -f $testdir/bdev.fio
	timing_exit fio_trim

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
$testdir/bdevperf/bdevperf -c $testdir/bdev_gpt.conf -q 128 -s 4096 -w verify -t 5
rm -f $testdir/bdev_gpt.conf

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Temporarily disabled - infinite loop
	timing_enter reset
	#$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
	timing_exit reset
fi


if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir reset
fi

rm -f /tmp/aiofile
rm -f $testdir/bdev.conf
timing_exit bdev
