#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

# nullglob will remove unmatched words containing '*', '?', '[' characters during word splitting.
# This means that empty alias arrays will be removed instead of printing "[]", which breaks
# consecutive "jq" calls, as the "aliases" key will have no value and the whole JSON will be
# invalid. Hence do not enable this option for the duration of the tests in this script.
shopt -s extglob

rpc_py=rpc_cmd
conf_file="$testdir/bdev.json"
nonenclosed_conf_file="$testdir/nonenclosed.json"
nonarray_conf_file="$testdir/nonarray.json"

# Make sure the configuration is clean
: > "$conf_file"

function cleanup() {
	rm -f "$SPDK_TEST_STORAGE/aiofile"
	rm -f "$SPDK_TEST_STORAGE/spdk-pmem-pool"
	rm -f "$conf_file"

	if [[ $test_type == rbd ]]; then
		rbd_cleanup
	fi

	if [[ $test_type == daos ]]; then
		daos_cleanup
	fi

	if [[ "$test_type" = "gpt" ]]; then
		"$rootdir/scripts/setup.sh" reset
		if [[ -b $gpt_nvme ]]; then
			wipefs --all "$gpt_nvme"
		fi
	fi
	if [[ $test_type == xnvme ]]; then
		"$rootdir/scripts/setup.sh"
	fi
}

function start_spdk_tgt() {
	"$SPDK_BIN_DIR/spdk_tgt" "$env_ctx" "$wait_for_rpc" &
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
		bdev_malloc_create -b Malloc6 32 512
		bdev_malloc_create -b Malloc7 32 512
		bdev_passthru_create -p TestPT -b Malloc3
		bdev_raid_create -n raid0 -z 64 -r 0 -b "Malloc4 Malloc5"
		bdev_raid_create -n concat0 -z 64 -r concat -b "Malloc6 Malloc7"
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

function setup_xnvme_conf() {
	# TODO: Switch to io_uring_cmd when proper CI support is in place
	local io_mechanism=io_uring
	local nvme nvmes

	"$rootdir/scripts/setup.sh" reset
	get_zoned_devs

	for nvme in /dev/nvme*n*; do
		[[ -b $nvme && -z ${zoned_devs["${nvme##*/}"]} ]] || continue
		nvmes+=("bdev_xnvme_create $nvme ${nvme##*/} $io_mechanism")
	done

	((${#nvmes[@]} > 0))
	"$rpc_py" < <(printf '%s\n' "${nvmes[@]}")
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
		SPDK_GPT_OLD_GUID=$(get_spdk_gpt_old)
		SPDK_GPT_GUID=$(get_spdk_gpt)
		sgdisk -t "1:$SPDK_GPT_GUID" "$gpt_nvme"
		sgdisk -t "2:$SPDK_GPT_OLD_GUID" "$gpt_nvme"
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
		dpdk_cryptodev_scan_accel_module
		dpdk_cryptodev_set_driver -d crypto_aesni_mb
		accel_assign_opc -o encrypt -m dpdk_cryptodev
		accel_assign_opc -o decrypt -m dpdk_cryptodev
		framework_start_init
		accel_crypto_key_create -c AES_CBC -k 01234567891234560123456789123456 -n test_dek_aesni_cbc_1
		accel_crypto_key_create -c AES_CBC -k 12345678912345601234567891234560 -n test_dek_aesni_cbc_2
		accel_crypto_key_create -c AES_CBC -k 23456789123456012345678912345601 -n test_dek_aesni_cbc_3
		accel_crypto_key_create -c AES_CBC -k 34567891234560123456789123456012 -n test_dek_aesni_cbc_4
		bdev_malloc_create -b Malloc0 32 512
		bdev_malloc_create -b Malloc1 32 512
		bdev_malloc_create -b Malloc2 32 4096
		bdev_malloc_create -b Malloc3 32 4096
		bdev_crypto_create Malloc0 crypto_ram -n test_dek_aesni_cbc_1
		bdev_crypto_create Malloc1 crypto_ram2 -n test_dek_aesni_cbc_2
		bdev_crypto_create Malloc2 crypto_ram3 -n test_dek_aesni_cbc_3
		bdev_crypto_create Malloc3 crypto_ram4 -n test_dek_aesni_cbc_4
	RPC
}

function setup_crypto_qat_conf() {
	# Malloc0 will use QAT AES_CBC
	# Malloc1 will use QAT AES_XTS
	"$rpc_py" <<- RPC
		dpdk_cryptodev_scan_accel_module
		dpdk_cryptodev_set_driver -d crypto_qat
		accel_assign_opc -o encrypt -m dpdk_cryptodev
		accel_assign_opc -o decrypt -m dpdk_cryptodev
		framework_start_init
		accel_crypto_key_create -c AES_CBC -k 01234567891234560123456789123456 -n test_dek_qat_cbc
		accel_crypto_key_create -c AES_XTS -k 00112233445566778899001122334455 -e 12345678912345601234567891234560 -n test_dek_qat_xts
		accel_crypto_key_create -c AES_CBC -k 23456789123456012345678912345601 -n test_dek_qat_cbc2
		accel_crypto_key_create -c AES_XTS -k 22334455667788990011223344550011 -e 34567891234560123456789123456012 -n test_dek_qat_xts2
		bdev_malloc_create -b Malloc0 32 512
		bdev_malloc_create -b Malloc1 32 512
		bdev_malloc_create -b Malloc2 32 4096
		bdev_malloc_create -b Malloc3 32 4096
		bdev_crypto_create Malloc0 crypto_ram -n test_dek_qat_cbc
		bdev_crypto_create Malloc1 crypto_ram1 -n test_dek_qat_xts
		bdev_crypto_create Malloc2 crypto_ram2 -n test_dek_qat_cbc2
		bdev_crypto_create Malloc3 crypto_ram3 -n test_dek_qat_xts2
		bdev_get_bdevs -b Malloc1
	RPC
}

function setup_crypto_sw_conf() {
	"$rpc_py" <<- RPC
		framework_start_init
		bdev_malloc_create -b Malloc0 16 512
		bdev_malloc_create -b Malloc1 16 4096
		accel_crypto_key_create -c AES_XTS -k 00112233445566778899001122334455 -e 11223344556677889900112233445500 -n test_dek_sw
		accel_crypto_key_create -c AES_XTS -k 22334455667788990011223344550011 -e 33445566778899001122334455001122 -n test_dek_sw2
		bdev_crypto_create Malloc0 crypto_ram -n test_dek_sw
		bdev_crypto_create Malloc1 crypto_ram2 -n test_dek_sw2
		bdev_get_bdevs -b Malloc1
	RPC
}

function setup_crypto_accel_mlx5_conf() {
	"$rpc_py" <<- RPC
		mlx5_scan_accel_module
		accel_assign_opc -o encrypt -m mlx5
		accel_assign_opc -o decrypt -m mlx5
		framework_start_init
		bdev_malloc_create -b Malloc0 32 512
		bdev_malloc_create -b Malloc1 32 512
		bdev_malloc_create -b Malloc2 32 4096
		bdev_malloc_create -b Malloc3 32 4096
		accel_crypto_key_create -c AES_XTS -k 00112233445566778899001122334455 -e 11223344556677889900112233445500 -n test_dek_accel_mlx5_1
		accel_crypto_key_create -c AES_XTS -k 11223344556677889900112233445500 -e 22334455667788990011223344550011 -n test_dek_accel_mlx5_2
		accel_crypto_key_create -c AES_XTS -k 22334455667788990011223344550011 -e 33445566778899001122334455002233 -n test_dek_accel_mlx5_3
		accel_crypto_key_create -c AES_XTS -k 33445566778899001122334455001122 -e 44556677889900112233445500112233 -n test_dek_accel_mlx5_4
		bdev_crypto_create Malloc0 crypto_ram_1 -n test_dek_accel_mlx5_1
		bdev_crypto_create Malloc1 crypto_ram_2 -n test_dek_accel_mlx5_2
		bdev_crypto_create Malloc2 crypto_ram_3 -n test_dek_accel_mlx5_3
		bdev_crypto_create Malloc3 crypto_ram_4 -n test_dek_accel_mlx5_4
		bdev_get_bdevs -b Malloc1
	RPC
}

function setup_crypto_mlx5_conf() {
	local key=$1
	local block_key
	local tweak_key
	if [ ${#key} == 96 ]; then
		# 96 bytes is 64 + 32 - AES_XTS_256 in hexlified format
		# Copy first 64 chars into the 'key'. This gives 32 in the
		# binary or 256 bit.
		block_key=${key:0:64}
		# Copy the the rest of the key and pass it as the 'key2'.
		tweak_key=${key:64:32}
	elif [ ${#key} == 160 ]; then
		# 160 bytes is 128 + 32 - AES_XTS_512 in hexlified format
		# Copy first 128 chars into the 'key'. This gives 64 in the
		# binary or 512 bit.
		block_key=${key:0:128}
		# Copy the the rest of the key and pass it as the 'key2'.
		tweak_key=${key:128:32}
	else
		echo "ERROR: Invalid DEK size for MLX5 crypto setup: ${#key}"
		echo "ERROR: Supported key sizes for MLX5: 96 bytes (AES_XTS_256) and 160 bytes (AES_XTS_512)."
		return 1
	fi

	# Malloc0 will use MLX5 AES_XTS
	"$rpc_py" <<- RPC
		dpdk_cryptodev_scan_accel_module
		dpdk_cryptodev_set_driver -d mlx5_pci
		accel_assign_opc -o encrypt -m dpdk_cryptodev
		accel_assign_opc -o decrypt -m dpdk_cryptodev
		framework_start_init
		bdev_malloc_create -b Malloc0 16 512
		bdev_crypto_create Malloc0 crypto_ram4 -k $block_key -c AES_XTS -k2 $tweak_key
		bdev_get_bdevs -b Malloc0
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

function setup_daos_conf() {
	local pool=testpool
	local cont=testcont

	timing_enter daos_setup
	daos_setup $pool $cont
	timing_exit daos_setup

	"$rpc_py" bdev_daos_create Daos0 $pool $cont 16 4096
}

function setup_raid5f_conf() {
	"$rpc_py" <<- RPC
		bdev_malloc_create -b Malloc0 32 512
		bdev_malloc_create -b Malloc1 32 512
		bdev_malloc_create -b Malloc2 32 512
		bdev_raid_create -n raid5f -z 2 -r 5f -b "Malloc0 Malloc1 Malloc2"
	RPC
}

function bdev_bounds() {
	$testdir/bdevio/bdevio -w -s $PRE_RESERVED_MEM --json "$conf_file" "$env_ctx" &
	bdevio_pid=$!
	trap 'cleanup; killprocess $bdevio_pid; exit 1' SIGINT SIGTERM EXIT
	echo "Process bdevio pid: $bdevio_pid"
	waitforlisten $bdevio_pid
	$testdir/bdevio/tests.py perform_tests
	killprocess $bdevio_pid
	trap - SIGINT SIGTERM EXIT
}

function nbd_function_test() {
	[[ $(uname -s) == Linux ]] || return 0

	local rpc_server=/var/tmp/spdk-nbd.sock
	local conf=$1
	local bdev_all=($2)
	local bdev_num=${#bdev_all[@]}

	# FIXME: Centos7 in the CI is not shipped with a kernel supporting BLK_DEV_NBD
	# so don't fail here for now.
	[[ -e /sys/module/nbd ]] || modprobe -q nbd nbds_max=$bdev_num || return 0

	local nbd_all=(/dev/nbd+([0-9]))
	bdev_num=$((${#nbd_all[@]} < bdev_num ? ${#nbd_all[@]} : bdev_num))

	local nbd_list=(${nbd_all[@]::bdev_num})
	local bdev_list=(${bdev_all[@]::bdev_num})

	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 --json "$conf" "$env_ctx" &
	nbd_pid=$!
	trap 'cleanup; killprocess $nbd_pid' SIGINT SIGTERM EXIT
	waitforlisten $nbd_pid $rpc_server

	nbd_rpc_start_stop_verify $rpc_server "${bdev_list[*]}"
	nbd_rpc_data_verify $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"
	nbd_with_lvol_verify $rpc_server "${nbd_list[*]}"

	killprocess $nbd_pid
	trap - SIGINT SIGTERM EXIT
}

function fio_test_suite() {
	local env_context

	# Make sure that state files and anything else produced by fio test will
	# stay at the testdir.
	pushd $testdir
	trap 'rm -f ./*.state; popd; exit 1' SIGINT SIGTERM EXIT

	# Generate the fio config file given the list of all unclaimed bdevs
	env_context=$(echo "$env_ctx" | sed 's/--env-context=//')
	fio_config_gen $testdir/bdev.fio verify AIO "$env_context"
	for b in $(echo $bdevs | jq -r '.name'); do
		echo "[job_$b]" >> $testdir/bdev.fio
		echo "filename=$b" >> $testdir/bdev.fio
	done

	local fio_params="--ioengine=spdk_bdev --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio \
			--verify_state_save=0 --spdk_json_conf=$conf_file"

	run_test "bdev_fio_rw_verify" fio_bdev $fio_params --spdk_mem=$PRE_RESERVED_MEM --aux-path=$output_dir
	rm -f ./*.state
	rm -f $testdir/bdev.fio

	# Generate the fio config file given the list of all unclaimed bdevs that support unmap
	fio_config_gen $testdir/bdev.fio trim "" "$env_context"
	if [ "$(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name')" != "" ]; then
		for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
			echo "[job_$b]" >> $testdir/bdev.fio
			echo "filename=$b" >> $testdir/bdev.fio
		done
	else
		rm -f $testdir/bdev.fio
		popd
		trap - SIGINT SIGTERM EXIT
		return 0
	fi

	run_test "bdev_fio_trim" fio_bdev $fio_params --verify_state_save=0 --aux-path=$output_dir
	rm -f ./*.state
	rm -f $testdir/bdev.fio
	popd
	trap - SIGINT SIGTERM EXIT
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
	"$rootdir/build/examples/bdevperf" -z -m 0x2 -q 256 -o 4096 -w randread -t 60 "$env_ctx" &
	QOS_PID=$!
	echo "Process qos testing pid: $QOS_PID"
	trap 'cleanup; killprocess $QOS_PID; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $QOS_PID

	$rpc_py bdev_malloc_create -b $QOS_DEV_1 128 512
	waitforbdev $QOS_DEV_1
	$rpc_py bdev_null_create $QOS_DEV_2 128 512
	waitforbdev $QOS_DEV_2

	$rootdir/examples/bdev/bdevperf/bdevperf.py perform_tests &
	qos_function_test

	$rpc_py bdev_malloc_delete $QOS_DEV_1
	$rpc_py bdev_null_delete $QOS_DEV_2
	killprocess $QOS_PID
	trap - SIGINT SIGTERM EXIT
}

function error_test_suite() {
	DEV_1="Dev_1"
	DEV_2="Dev_2"
	ERR_DEV="EE_Dev_1"

	# Run bdevperf with 1 normal bdev and 1 error bdev, also continue on error
	"$rootdir/build/examples/bdevperf" -z -m 0x2 -q 16 -o 4096 -w randread -t 5 -f "$env_ctx" &
	ERR_PID=$!
	echo "Process error testing pid: $ERR_PID"
	waitforlisten $ERR_PID

	$rpc_py bdev_malloc_create -b $DEV_1 128 512
	waitforbdev $DEV_1
	$rpc_py bdev_error_create $DEV_1
	$rpc_py bdev_malloc_create -b $DEV_2 128 512
	waitforbdev $DEV_2
	$rpc_py bdev_error_inject_error $ERR_DEV 'all' 'failure' -n 5

	$rootdir/examples/bdev/bdevperf/bdevperf.py -t 1 perform_tests &
	sleep 1

	# Bdevperf is expected to be there as the continue on error is set
	if kill -0 $ERR_PID; then
		echo "Process is existed as continue on error is set. Pid: $ERR_PID"
	else
		echo "Process exited unexpectedly. Pid: $ERR_PID"
		exit 1
	fi

	# Delete the error devices
	$rpc_py bdev_error_delete $ERR_DEV
	$rpc_py bdev_malloc_delete $DEV_1
	sleep 5
	# Expected to exit normally
	killprocess $ERR_PID

	# Run bdevperf with 1 normal bdev and 1 error bdev, and exit on error
	"$rootdir/build/examples/bdevperf" -z -m 0x2 -q 16 -o 4096 -w randread -t 5 "$env_ctx" &
	ERR_PID=$!
	echo "Process error testing pid: $ERR_PID"
	waitforlisten $ERR_PID

	$rpc_py bdev_malloc_create -b $DEV_1 128 512
	waitforbdev $DEV_1
	$rpc_py bdev_error_create $DEV_1
	$rpc_py bdev_malloc_create -b $DEV_2 128 512
	waitforbdev $DEV_2
	$rpc_py bdev_error_inject_error $ERR_DEV 'all' 'failure' -n 5

	$rootdir/examples/bdev/bdevperf/bdevperf.py -t 1 perform_tests &
	NOT wait $ERR_PID
}

function qd_sampling_function_test() {
	local bdev_name=$1
	local sampling_period=10
	local iostats

	$rpc_py bdev_set_qd_sampling_period $bdev_name $sampling_period

	iostats=$($rpc_py bdev_get_iostat -b $bdev_name)

	qd_sampling_period=$(jq -r '.bdevs[0].queue_depth_polling_period' <<< "$iostats")

	if [ $qd_sampling_period == null ] || [ $qd_sampling_period -ne $sampling_period ]; then
		echo "Qeueue depth polling period is not right"
		$rpc_py bdev_malloc_delete $QD_DEV
		killprocess $QD_PID
		exit 1
	fi
}

function qd_sampling_test_suite() {
	QD_DEV="Malloc_QD"

	"$rootdir/build/examples/bdevperf" -z -m 0x3 -q 256 -o 4096 -w randread -t 5 -C "$env_ctx" &
	QD_PID=$!
	echo "Process bdev QD sampling period testing pid: $QD_PID"
	trap 'cleanup; killprocess $QD_PID; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $QD_PID

	$rpc_py bdev_malloc_create -b $QD_DEV 128 512
	waitforbdev $QD_DEV

	$rootdir/examples/bdev/bdevperf/bdevperf.py perform_tests &
	sleep 2
	qd_sampling_function_test $QD_DEV

	$rpc_py bdev_malloc_delete $QD_DEV
	killprocess $QD_PID
	trap - SIGINT SIGTERM EXIT
}

function stat_function_test() {
	local bdev_name=$1
	local iostats
	local io_count1
	local io_count2
	local iostats_per_channel
	local io_count_per_channel1
	local io_count_per_channel2
	local io_count_per_channel_all=0

	iostats=$($rpc_py bdev_get_iostat -b $bdev_name)
	io_count1=$(jq -r '.bdevs[0].num_read_ops' <<< "$iostats")

	iostats_per_channel=$($rpc_py bdev_get_iostat -b $bdev_name -c)
	io_count_per_channel1=$(jq -r '.channels[0].num_read_ops' <<< "$iostats_per_channel")
	io_count_per_channel_all=$((io_count_per_channel_all + io_count_per_channel1))
	io_count_per_channel2=$(jq -r '.channels[1].num_read_ops' <<< "$iostats_per_channel")
	io_count_per_channel_all=$((io_count_per_channel_all + io_count_per_channel2))

	iostats=$($rpc_py bdev_get_iostat -b $bdev_name)
	io_count2=$(jq -r '.bdevs[0].num_read_ops' <<< "$iostats")

	# There is little time passed between the three iostats collected. So that
	# the accumulated statistics from per channel data shall be bigger than the
	# the first run and smaller than the third run in this short time of period.
	if [ $io_count_per_channel_all -lt $io_count1 ] || [ $io_count_per_channel_all -gt $io_count2 ]; then
		echo "Failed to collect the per Core IO statistics"
		$rpc_py bdev_malloc_delete $STAT_DEV
		killprocess $STAT_PID
		exit 1
	fi
}

function stat_test_suite() {
	STAT_DEV="Malloc_STAT"

	# Run bdevperf with 2 cores so as to collect per Core IO statistics
	"$rootdir/build/examples/bdevperf" -z -m 0x3 -q 256 -o 4096 -w randread -t 10 -C "$env_ctx" &
	STAT_PID=$!
	echo "Process Bdev IO statistics testing pid: $STAT_PID"
	trap 'cleanup; killprocess $STAT_PID; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $STAT_PID

	$rpc_py bdev_malloc_create -b $STAT_DEV 128 512
	waitforbdev $STAT_DEV

	$rootdir/examples/bdev/bdevperf/bdevperf.py perform_tests &
	sleep 2
	stat_function_test $STAT_DEV

	$rpc_py bdev_malloc_delete $STAT_DEV
	killprocess $STAT_PID
	trap - SIGINT SIGTERM EXIT
}

# Initial bdev creation and configuration
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
crypto_device=$2
wcs_file=$3
dek=$4
env_ctx=""
wait_for_rpc=""
if [ -n "$crypto_device" ] && [ -n "$wcs_file" ]; then
	# We need full path here since fio perf test does 'pushd' to the test dir
	# and crypto login of fio plugin test can fail.
	wcs_file=$(readlink -f $wcs_file)
	if [ -f $wcs_file ]; then
		env_ctx="--env-context=--allow=$crypto_device,class=crypto,wcs_file=$wcs_file"
	else
		echo "ERROR: Credentials file $3 is not found!"
		exit 1
	fi
fi
if [[ $test_type == crypto_* ]]; then
	wait_for_rpc="--wait-for-rpc"
fi
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
	crypto_sw)
		setup_crypto_sw_conf
		;;
	crypto_mlx5)
		setup_crypto_mlx5_conf $dek
		;;
	crypto_accel_mlx5)
		setup_crypto_accel_mlx5_conf
		;;
	pmem)
		setup_pmem_conf
		;;
	rbd)
		setup_rbd_conf
		;;
	daos)
		setup_daos_conf
		;;
	raid5f)
		setup_raid5f_conf
		;;
	xnvme)
		setup_xnvme_conf
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
	        $("$rpc_py" save_subsystem_config -n accel),
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

run_test "bdev_hello_world" $SPDK_EXAMPLE_DIR/hello_bdev --json "$conf_file" -b "$hello_world_bdev" "$env_ctx"
run_test "bdev_bounds" bdev_bounds "$env_ctx"
run_test "bdev_nbd" nbd_function_test $conf_file "$bdevs_name" "$env_ctx"
if [[ $CONFIG_FIO_PLUGIN == y ]]; then
	if [ "$test_type" = "nvme" ] || [ "$test_type" = "gpt" ]; then
		# TODO: once we get real multi-ns drives, re-enable this test for NVMe.
		echo "skipping fio tests on NVMe due to multi-ns failures."
	else
		run_test "bdev_fio" fio_test_suite "$env_ctx"
	fi
else
	echo "FIO not available"
	exit 1
fi

trap "cleanup" SIGINT SIGTERM EXIT

run_test "bdev_verify" $rootdir/build/examples/bdevperf --json "$conf_file" -q 128 -o 4096 -w verify -t 5 -C -m 0x3 "$env_ctx"
# TODO: increase queue depth to 128 once issue #2824 is fixed
run_test "bdev_verify_big_io" $rootdir/build/examples/bdevperf --json "$conf_file" -q 16 -o 65536 -w verify -t 5 -C -m 0x3 "$env_ctx"
run_test "bdev_write_zeroes" $rootdir/build/examples/bdevperf --json "$conf_file" -q 128 -o 4096 -w write_zeroes -t 1 "$env_ctx"

# test json config not enclosed with {}
run_test "bdev_json_nonenclosed" $rootdir/build/examples/bdevperf --json "$nonenclosed_conf_file" -q 128 -o 4096 -w write_zeroes -t 1 "$env_ctx" || true

# test json config "subsystems" not with array
run_test "bdev_json_nonarray" $rootdir/build/examples/bdevperf --json "$nonarray_conf_file" -q 128 -o 4096 -w write_zeroes -t 1 "$env_ctx" || true

if [[ $test_type == bdev ]]; then
	run_test "bdev_qos" qos_test_suite "$env_ctx"
	run_test "bdev_qd_sampling" qd_sampling_test_suite "$env_ctx"
	run_test "bdev_error" error_test_suite "$env_ctx"
	run_test "bdev_stat" stat_test_suite "$env_ctx"
fi

# Temporarily disabled - infinite loop
# if [ $RUN_NIGHTLY -eq 1 ]; then
# run_test "bdev_reset" $rootdir/build/examples/bdevperf --json "$conf_file" -q 16 -w reset -o 4096 -t 60 "$env_ctx"
# fi

# Bdev and configuration cleanup below this line
#-----------------------------------------------------

trap - SIGINT SIGTERM EXIT
cleanup
