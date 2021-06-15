#!/usr/bin/env bash

## Test goal: Confirm that rpc setting of bdev_nvme_set_options timeout values is working
## Test steps:
# 1. Run the target with default settings, and use "rpc.py save_config" to check them.
#    Defaults are:  action_on_timeout=none, timeout_us=0, and timeout_admin_us=0
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
# Set action_on_timeout, timeout_us, and timeout_admin_us to non-default values
$rpc_py bdev_nvme_set_options --timeout-us=12000000 --timeout-admin-us=24000000 --action-on-timeout=abort

echo Check default vs. modified settings:
$rpc_py save_config > $tmpfile_modified_settings
settings_to_check="action_on_timeout timeout_us timeout_admin_us"
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
