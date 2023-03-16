#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
# (C) Copyright 2023 Hewlett Packard Enterprise Development LP
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

ctrlr_name="nvme0"
# Error injection timeout - 15 sec (in usec)
err_injection_timeout=15000000
# Test timeout - 5 sec
test_timeout=5

# SCT_GENERIC
err_injection_sct=0
# SC_INVALID_OPCODE
err_injection_sc=1

bdf=$(get_first_nvme_bdf)
if [ -z "${bdf}" ]; then
	echo "No NVMe drive found but test requires it. Failing the test."
	exit 1
fi

function base64_decode_bits() {
	python3 <<- EOF
		import base64
		bin_array = bytearray(base64.b64decode("$1"))
		array_length = len(bin_array)
		status = (bin_array[array_length-1] << 8) | bin_array[array_length-2]
		print("0x%x" % ((status >> $2) & $3))
	EOF
}

"$SPDK_BIN_DIR/spdk_tgt" -m 0xF &
spdk_target_pid=$!
trap 'killprocess "$spdk_target_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten "$spdk_target_pid"

$rpc_py bdev_nvme_attach_controller -b $ctrlr_name -t PCIe -a ${bdf}
tmp_file=$(mktemp "/tmp/err_inj_XXXXX.txt")

# Set error injection for SPDK_NVME_OPC_GET_FEATURES admin call
$rpc_py bdev_nvme_add_error_injection -n $ctrlr_name --cmd-type admin --opc 10 --timeout-in-us $err_injection_timeout --err-count 1 --sct $err_injection_sct --sc $err_injection_sc --do_not_submit
start_time=$(date +%s)

# The following RPC call generated based on C code taken from 'get_feature_test(...)'
#	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
#	cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_NUMBER_OF_QUEUES;
$rootdir/scripts/rpc.py bdev_nvme_send_cmd -n $ctrlr_name -t admin -r c2h -c "CgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAcAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==" > $tmp_file &
get_feat_pid=$!
trap 'killprocess "$get_feat_pid"; exit 1' SIGINT SIGTERM EXIT

# Making sure that 'get feat' process working for at least 1 sec
sleep 2

$rpc_py bdev_nvme_reset_controller $ctrlr_name

echo "Waiting for RPC error injection (bdev_nvme_send_cmd) process PID:" $get_feat_pid
wait $get_feat_pid
diff_time=$(($(date +%s) - start_time))
$rpc_py bdev_nvme_detach_controller $ctrlr_name

trap - SIGINT SIGTERM EXIT

# extracting 'sc' and 'sct' values from 'struct spdk_nvme_status'
spdk_nvme_status=$(jq -r '.cpl' $tmp_file)
nvme_status_sc=$(base64_decode_bits $spdk_nvme_status 1 255)
nvme_status_sct=$(base64_decode_bits $spdk_nvme_status 9 3)

rm -f $tmp_file

killprocess "$spdk_target_pid"

if ((err_injection_sc != nvme_status_sc || err_injection_sct != nvme_status_sct)); then
	echo "Error NVMe completion status. SC: $nvme_status_sc, SCT: $nvme_status_sct"
	echo " - expected status: SC: $err_injection_sc, SCT: $err_injection_sct"
	exit 1
elif ((diff_time > test_timeout)); then
	echo "Test failed. Error injection timeout $diff_time sec exceeded expected timeout $test_timeout sec."
	exit 1
fi
