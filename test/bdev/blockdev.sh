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
		trap "killprocess $nbd_pid; exit 1" SIGINT SIGTERM EXIT
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid $rpc_server

		nbd_rpc_start_stop_verify $rpc_server "${bdev_list[*]}"
		nbd_rpc_data_verify $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"

		killprocess $nbd_pid
		trap - SIGINT SIGTERM EXIT
	fi

	return 0
}

timing_enter bdev

cp $testdir/bdev.conf.in $testdir/bdev.conf


if [ $RUN_NIGHTLY -eq 1 ]; then
	timing_enter hello_bdev
	$rootdir/scripts/gen_nvme.sh > $testdir/hello.conf
	if grep -q Nvme0 $testdir/hello.conf; then
		$rootdir/examples/bdev/hello_world/hello_bdev -c $testdir/hello.conf -b Nvme0n1
	else
		echo "No NVMe present to test with"
		rm -f $testdir/hello.conf
		exit 1
	fi
	rm -f $testdir/hello.conf
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
trap "killprocess $bdevio_pid; exit 1" SIGINT SIGTERM EXIT
echo "Process bdevio pid: $bdevio_pid"
waitforlisten $bdevio_pid
$testdir/bdevio/tests.py perform_tests

# Test Malloc
timing_enter blockdev_malloc
$rpc_py	construct_malloc_bdev 32 512 -b Malloc_test
$testdir/bdevio/tests.py perform_tests -b Malloc_test
$rpc_py	delete_malloc_bdev Malloc_test
timing_exit blockdev_malloc

# Test Passthrough
timing_enter blockdev_passthrough
$rpc_py	construct_malloc_bdev 32 512 -b Malloc_PT
$rpc_py	construct_passthru_bdev -b Malloc_PT -p TestPT
$testdir/bdevio/tests.py perform_tests -b TestPT
$rpc_py	delete_passthru_bdev TestPT
$rpc_py	delete_malloc_bdev Malloc_PT
timing_exit blockdev_passthrough

# Test Split
timing_enter blockdev_split
$rpc_py	construct_malloc_bdev 32 512 -b Malloc_split

#Split Malloc into two auto-sized halves
$rpc_py	construct_split_vbdev Malloc_split 2
$testdir/bdevio/tests.py perform_tests
$rpc_py	destruct_split_vbdev Malloc_split

# Split Malloc2 into eight 4-megabyte pieces, leaving the rest of the device inaccessible
$rpc_py	construct_split_vbdev Malloc_split 8 -s 4
$testdir/bdevio/tests.py perform_tests
$rpc_py	destruct_split_vbdev Malloc_split

$rpc_py	delete_malloc_bdev Malloc_split
timing_exit blockdev_split

# Test Raid
timing_enter blockdev_raid
$rpc_py	construct_malloc_bdev 32 512 -b Malloc_raid1
$rpc_py	construct_malloc_bdev 32 512 -b Malloc_raid2
$rpc_py	construct_raid_bdev -n raid0 -b "Malloc_raid1 Malloc_raid2" -r 0 -z 64
$testdir/bdevio/tests.py perform_tests -b raid0
$rpc_py	destroy_raid_bdev raid0
$rpc_py	delete_malloc_bdev Malloc_raid1
$rpc_py	delete_malloc_bdev Malloc_raid2
timing_exit blockdev_raid

# Test NVMe
timing_enter blockdev_nvme
if [ "$(scripts/gen_nvme.sh --json | jq -r '.config[].params')" = "" ];then
	echo "No NVMe present to test with"
	exit 1
fi
$rootdir/scripts/gen_nvme.sh --json | $rpc_py load_subsystem_config
$testdir/bdevio/tests.py perform_tests -b Nvme0n1
$rpc_py delete_nvme_controller Nvme0
timing_exit blockdev_nvme

# Test AIO
timing_enter blockdev_aio
$rpc_py construct_aio_bdev /dev/ram0 AIO0
$rpc_py set_bdev_qos_limit AIO0 --r_mbytes_per_sec 50
$testdir/bdevio/tests.py perform_tests -b AIO0
$rpc_py delete_aio_bdev AIO0

# Create a file to be used as an AIO backend
dd if=/dev/zero of=/tmp/aiofile bs=2048 count=5000
$rpc_py construct_aio_bdev /tmp/aiofile AIO1 2048
$testdir/bdevio/tests.py perform_tests -b AIO1
$rpc_py delete_aio_bdev AIO1
timing_exit blockdev_aio

# Test PMDK
if [ $SPDK_TEST_PMDK -eq 1 ]; then
	timing_enter blockdev_pmdk
	pool_file="/tmp/spdk-pmem-pool"
	rm -f $pool_file
	$rpc_py create_pmem_pool $pool_file 32 512
	$rpc_py construct_pmem_bdev $pool_file -n Pmem0

	$testdir/bdevio/tests.py perform_tests -b Pmem0
	$rpc_py delete_pmem_bdev Pmem0
	$rpc_py delete_pmem_pool $pool_file
	timing_exit blockdev_pmdk
fi

# Test RBD
if [ $SPDK_TEST_RBD -eq 1 ]; then
	timing_enter blockdev_rbd
	rbd_setup 127.0.0.1

	rbd_bdev="$($rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 512)"
	$testdir/bdevio/tests.py perform_tests -b $rbd_bdev
	$rpc_py delete_rbd_bdev $rbd_bdev
	timing_exit blockdev_rbd
fi

# Test Crypto
if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
	timing_enter blockdev_crypto
	# Use QAT if available
	if [ $(lspci -d:37c8 | wc -l) -eq 1 ]; then
		$rpc_py	construct_malloc_bdev 32 4192 -b Malloc_QAT
		$rpc_py	-b Malloc_QAT -c bdev_qat -d crypto_qat -k 0123456789123456
		$testdir/bdevio/tests.py perform_tests -b bdev_qat
		$rpc_py	delete_malloc_bdev Malloc_QAT
	fi

	$rpc_py	construct_malloc_bdev 32 4192 -b Malloc_AES
	$rpc_py	-b Malloc_AES -c bdev_aes -d crypto_aesni_mb -k 9012345678912345
	$testdir/bdevio/tests.py perform_tests -b bdev_aes
	$rpc_py	delete_malloc_bdev Malloc_AES
	timing_exit blockdev_crypto
fi

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


if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir reset
fi

rm -f /tmp/aiofile
rm -f /tmp/spdk-pmem-pool
rm -f $testdir/bdev.conf
rbd_cleanup
report_test_completion "bdev"
timing_exit bdev
