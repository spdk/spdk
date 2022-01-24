#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

rpc_py=rpc_cmd
conf_file="$testdir/bdev.json"
# Make sure the configuration is clean
: > "$conf_file"

function cleanup() {
	rm -f "$SPDK_TEST_STORAGE/aiofile"
	rm -f "$SPDK_TEST_STORAGE/spdk-pmem-pool"
	rm -f "$conf_file"

	if [[ $test_type == rbd ]]; then
		rbd_cleanup
	fi

	if [[ "$test_type" = "gpt" ]]; then
		"$rootdir/scripts/setup.sh" reset
		if [[ -b $gpt_nvme ]]; then
			wipefs --all "$gpt_nvme"
		fi
	fi
}

function start_spdk_tgt() {
	"$SPDK_BIN_DIR/spdk_tgt" &
	spdk_tgt_pid=$!
	trap 'killprocess "$spdk_tgt_pid"; exit 1' SIGINT SIGTERM EXIT
	waitforlisten "$spdk_tgt_pid"
}

function setup_bdev_conf() {
	"$rpc_py" <<- RPC
		bdev_split_create Malloc1 2
		bdev_split_create -s 4 Malloc2 8
		bdev_malloc_create -b Malloc0 32 512
		bdev_malloc_create -b Malloc1 32 512
		bdev_malloc_create -b Malloc2 32 512
		bdev_malloc_create -b Malloc3 32 512
		bdev_malloc_create -b Malloc4 32 512
		bdev_malloc_create -b Malloc5 32 512
		bdev_passthru_create -p TestPT -b Malloc3
		bdev_raid_create -n raid0 -z 64 -r 0 -b "Malloc4 Malloc5"
		bdev_set_qos_limit --rw_mbytes_per_sec 100 Malloc3
		bdev_set_qos_limit --rw_ios_per_sec 20000 Malloc0
	RPC
	if [[ $(uname -s) != "FreeBSD" ]]; then
		dd if=/dev/zero of="$SPDK_TEST_STORAGE/aiofile" bs=2048 count=5000
		"$rpc_py" bdev_aio_create "$SPDK_TEST_STORAGE/aiofile" AIO0 2048
	fi
}

function setup_nvme_conf() {
	local json
	mapfile -t json < <("$rootdir/scripts/gen_nvme.sh")
	"$rpc_py" load_subsystem_config -j "'${json[*]}'"
}

function setup_gpt_conf() {
	$rootdir/scripts/setup.sh reset
	get_zoned_devs
	# Get nvme devices by following drivers' links towards nvme class
	local nvme_devs=(/sys/bus/pci/drivers/nvme/*/nvme/nvme*/nvme*n*) nvme_dev
	gpt_nvme=""
	# Pick first device which doesn't have any valid partition table
	for nvme_dev in "${nvme_devs[@]}"; do
		[[ -z ${zoned_devs["${nvme_dev##*/}"]} ]] || continue
		dev=/dev/${nvme_dev##*/}
		if ! pt=$(parted "$dev" -ms print 2>&1); then
			[[ $pt == *"$dev: unrecognised disk label"* ]] || continue
			gpt_nvme=$dev
			break
		fi
	done
	if [[ -n $gpt_nvme ]]; then
		# Create gpt partition table
		parted -s "$gpt_nvme" mklabel gpt mkpart SPDK_TEST_first '0%' '50%' mkpart SPDK_TEST_second '50%' '100%'
		# change the GUID to SPDK GUID value
		SPDK_GPT_GUID=$(get_spdk_gpt)
		sgdisk -t "1:$SPDK_GPT_GUID" "$gpt_nvme"
		sgdisk -t "2:$SPDK_GPT_GUID" "$gpt_nvme"
		"$rootdir/scripts/setup.sh"
		"$rpc_py" bdev_get_bdevs
		setup_nvme_conf
	else
		printf 'Did not find any nvme block devices to work with, aborting the test\n' >&2
		"$rootdir/scripts/setup.sh"
		return 1
	fi
}

function setup_crypto_aesni_conf() {
	# Malloc0 and Malloc1 use AESNI
	"$rpc_py" <<- RPC
		bdev_malloc_create -b Malloc0 16 512
		bdev_malloc_create -b Malloc1 16 512
		bdev_crypto_create Malloc0 crypto_ram crypto_aesni_mb 0123456789123456
		bdev_crypto_create Malloc1 crypto_ram2 crypto_aesni_mb 9012345678912345
	RPC
}

function setup_crypto_qat_conf() {
	# Malloc0 will use QAT AES_CBC
	# Malloc1 will use QAT AES_XTS
	"$rpc_py" <<- RPC
		bdev_malloc_create -b Malloc0 16 512
		bdev_malloc_create -b Malloc1 16 512
		bdev_crypto_create Malloc0 crypto_ram crypto_qat 0123456789123456
		bdev_crypto_create -c AES_XTS -k2 0123456789123456 Malloc1 crypto_ram3 crypto_qat 0123456789123456
		bdev_get_bdevs -b Malloc1
	RPC
}

function setup_pmem_conf() {
	if hash pmempool; then
		rm -f "$SPDK_TEST_STORAGE/spdk-pmem-pool"
		pmempool create blk --size=32M 512 "$SPDK_TEST_STORAGE/spdk-pmem-pool"
		"$rpc_py" bdev_pmem_create -n Pmem0 "$SPDK_TEST_STORAGE/spdk-pmem-pool"
	else
		return 1
	fi
}

function setup_rbd_conf() {
	timing_enter rbd_setup
	rbd_setup 127.0.0.1
	timing_exit rbd_setup

	"$rpc_py" bdev_rbd_create -b Ceph0 rbd foo 512
}

function bdev_bounds() {
	$testdir/bdevio/bdevio -w -s $PRE_RESERVED_MEM --json "$conf_file" &
	bdevio_pid=$!
	trap 'cleanup; killprocess $bdevio_pid; exit 1' SIGINT SIGTERM EXIT
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
		local nbd_num=${#bdev_all[@]}
		if ((nbd_num < 1)); then
			# There should be at least one bdev and one valid nbd device
			return 1
		fi
		if [ ${#nbd_all[@]} -le $nbd_num ]; then
			nbd_num=${#nbd_all[@]}
		fi
		local nbd_list=(${nbd_all[@]:0:$nbd_num})
		local bdev_list=(${bdev_all[@]:0:$nbd_num})

		if [ ! -e $conf ]; then
			return 1
		fi

		modprobe nbd
		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 --json "$conf" &
		nbd_pid=$!
		trap 'cleanup; killprocess $nbd_pid; exit 1' SIGINT SIGTERM EXIT
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
		echo "[job_$b]" >> $testdir/bdev.fio
		echo "filename=$b" >> $testdir/bdev.fio
	done

	local fio_params="--ioengine=spdk_bdev --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio --spdk_json_conf=$conf_file"

	run_test "bdev_fio_rw_verify" fio_bdev $fio_params --spdk_mem=$PRE_RESERVED_MEM \
		--output=$output_dir/blockdev_fio_verify.txt
	rm -f ./*.state
	rm -f $testdir/bdev.fio

	# Generate the fio config file given the list of all unclaimed bdevs that support unmap
	fio_config_gen $testdir/bdev.fio trim
	if [ "$(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name')" != "" ]; then
		for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
			echo "[job_$b]" >> $testdir/bdev.fio
			echo "filename=$b" >> $testdir/bdev.fio
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

	echo ${iostat_result/.*/}
}

function run_qos_test() {
	local qos_limit=$1
	local qos_result=0

	qos_result=$(get_io_result $2 $3)
	if [ $2 = BANDWIDTH ]; then
		qos_limit=$((qos_limit * 1024))
	fi
	lower_limit=$((qos_limit * 9 / 10))
	upper_limit=$((qos_limit * 11 / 10))

	# QoS realization is related with bytes transferred. It currently has some variation.
	if [ $qos_result -lt $lower_limit ] || [ $qos_result -gt $upper_limit ]; then
		echo "Failed to limit the io read rate of NULL bdev by qos"
		$rpc_py bdev_malloc_delete $QOS_DEV_1
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
	iops_limit=$(((io_result / 4) / qos_lower_iops_limit * qos_lower_iops_limit))
	if [ $iops_limit -gt $qos_lower_iops_limit ]; then

		# Run bdevperf with IOPS rate limit on bdev 1
		$rpc_py bdev_set_qos_limit --rw_ios_per_sec $iops_limit $QOS_DEV_1
		run_test "bdev_qos_iops" run_qos_test $iops_limit IOPS $QOS_DEV_1

		# Run bdevperf with bandwidth rate limit on bdev 2
		# Set the bandwidth limit as 1/10 of the measure performance without QoS
		bw_limit=$(get_io_result BANDWIDTH $QOS_DEV_2)
		bw_limit=$((bw_limit / 1024 / 10))
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
	"$testdir/bdevperf/bdevperf" -z -m 0x2 -q 256 -o 4096 -w randread -t 60 &
	QOS_PID=$!
	echo "Process qos testing pid: $QOS_PID"
	trap 'cleanup; killprocess $QOS_PID; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $QOS_PID

	$rpc_py bdev_malloc_create -b $QOS_DEV_1 128 512
	waitforbdev $QOS_DEV_1
	$rpc_py bdev_null_create $QOS_DEV_2 128 512
	waitforbdev $QOS_DEV_2

	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests &
	qos_function_test

	$rpc_py bdev_malloc_delete $QOS_DEV_1
	$rpc_py bdev_null_delete $QOS_DEV_2
	killprocess $QOS_PID
	trap - SIGINT SIGTERM EXIT
}

# Inital bdev creation and configuration
#-----------------------------------------------------
QOS_DEV_1="Malloc_0"
QOS_DEV_2="Null_1"
QOS_RUN_TIME=5

if [ $(uname -s) = Linux ]; then
	# Test dynamic memory management. All hugepages will be reserved at runtime
	PRE_RESERVED_MEM=0
else
	# Dynamic memory management is not supported on BSD
	PRE_RESERVED_MEM=2048
fi

test_type=${1:-bdev}
start_spdk_tgt
case "$test_type" in
	bdev)
		setup_bdev_conf
		;;
	nvme)
		setup_nvme_conf
		;;
	gpt)
		setup_gpt_conf
		;;
	crypto_aesni)
		setup_crypto_aesni_conf
		;;
	crypto_qat)
		setup_crypto_qat_conf
		;;
	pmem)
		setup_pmem_conf
		;;
	rbd)
		setup_rbd_conf
		;;
	*)
		echo "invalid test name"
		exit 1
		;;
esac

"$rpc_py" bdev_wait_for_examine

# Generate json config and use it throughout all the tests
cat <<- CONF > "$conf_file"
	        {"subsystems":[
	        $("$rpc_py" save_subsystem_config -n bdev)
	        ]}
CONF

bdevs=$("$rpc_py" bdev_get_bdevs | jq -r '.[] | select(.claimed == false)')
bdevs_name=$(echo $bdevs | jq -r '.name')
bdev_list=($bdevs_name)
hello_world_bdev=${bdev_list[0]}
trap - SIGINT SIGTERM EXIT
killprocess "$spdk_tgt_pid"
# End bdev configuration
#-----------------------------------------------------

trap "cleanup" SIGINT SIGTERM EXIT

run_test "bdev_hello_world" $SPDK_EXAMPLE_DIR/hello_bdev --json "$conf_file" -b "$hello_world_bdev"
run_test "bdev_bounds" bdev_bounds
run_test "bdev_nbd" nbd_function_test $conf_file "$bdevs_name"
if [[ $CONFIG_FIO_PLUGIN == y ]]; then
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

trap "cleanup" SIGINT SIGTERM EXIT

run_test "bdev_verify" $testdir/bdevperf/bdevperf --json "$conf_file" -q 128 -o 4096 -w verify -t 5 -C -m 0x3
run_test "bdev_write_zeroes" $testdir/bdevperf/bdevperf --json "$conf_file" -q 128 -o 4096 -w write_zeroes -t 1

if [[ $test_type == bdev ]]; then
	run_test "bdev_qos" qos_test_suite
fi

# Temporarily disabled - infinite loop
# if [ $RUN_NIGHTLY -eq 1 ]; then
# run_test "bdev_reset" $testdir/bdevperf/bdevperf --json "$conf_file" -q 16 -w reset -o 4096 -t 60
# fi

# Bdev and configuration cleanup below this line
#-----------------------------------------------------

trap - SIGINT SIGTERM EXIT
cleanup
