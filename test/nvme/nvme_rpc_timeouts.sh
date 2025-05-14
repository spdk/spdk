#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

## Test goal: Confirm that rpc setting of bdev_nvme_set_options timeout values is working
## Test steps:
# 1. Run the target with default settings, and use "rpc.py save_config" to check them.
# 2. Use "rpc.py bdev_nvme_set_options" to set the timeout values. Capture the
#    modified settings using save_config.
# 3. Compare the default settings to the modified settings to make sure they got changed.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

tmpfile_default_settings=/tmp/settings_default_$$
tmpfile_modified_settings=/tmp/settings_modified_$$

$SPDK_BIN_DIR/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; rm -f ${tmpfile_default_settings} ${tmpfile_modified_settings} ; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_tgt_pid

echo Checking default timeout settings:
$rpc_py save_config > $tmpfile_default_settings

echo Making settings changes with rpc:
# Set timeouts to non-default values
$rpc_py bdev_nvme_set_options \
	--action-on-timeout=abort \
	--keep-alive-timeout-ms=4294967295 \
	--timeout-us=18446744073709551615 \
	--timeout-admin-us=18446744073709551615 \
	--ctrlr-loss-timeout-sec=2147483647 \
	--reconnect-delay-sec=2147483647 \
	--fast-io-fail-timeout-sec=2147483647 \
	--transport-ack-timeout=255 \
	--rdma-cm-event-timeout-ms=65535 \
	--tcp-connect-timeout-ms=2147483647

echo Check default vs. modified settings:
$rpc_py save_config > $tmpfile_modified_settings
settings_to_check="
	action_on_timeout
	keep_alive_timeout_ms
	timeout_us
	timeout_admin_us
	ctrlr_loss_timeout_sec
	reconnect_delay_sec
	fast_io_fail_timeout_sec
	transport_ack_timeout
	rdma_cm_event_timeout_ms
	tcp_connect_timeout_ms"
for setting in $settings_to_check; do
	setting_before=$(grep ${setting} ${tmpfile_default_settings} | awk '{print $2}' | sed 's/[^a-zA-Z0-9]//g')
	setting_modified=$(grep ${setting} ${tmpfile_modified_settings} | awk '{print $2}' | sed 's/[^a-zA-Z0-9]//g')
	if [ "$setting_before" == "$setting_modified" ]; then
		echo SETTING $setting NOT MODIFIED BY RPC.
		echo Default value = $setting_before, value after setting = $setting_modified. FAIL TEST.
		exit 1
	else
		echo Setting $setting is changed as expected.
	fi
done

# CLEANUP: kill target process and clean up temp files
trap - SIGINT SIGTERM EXIT
rm -f ${tmpfile_default_settings} ${tmpfile_modified_settings}
killprocess $spdk_tgt_pid

echo RPC TIMEOUT SETTING TEST PASSED.
