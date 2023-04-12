#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#

spdkcli_job="$rootdir/test/spdkcli/spdkcli_job.py"
spdk_clear_config_py="$rootdir/test/json_config/clear_config.py"

function on_error_exit() {
	set +e
	if [ -n "$spdk_tgt_pid" ]; then
		killprocess $spdk_tgt_pid
	fi
	if [ -n "$nvmf_tgt_pid" ]; then
		killprocess $nvmf_tgt_pid
	fi
	if [ -n "$iscsi_tgt_pid" ]; then
		killprocess $iscsi_tgt_pid
	fi
	if [ -n "$vhost_tgt_pid" ]; then
		killprocess $vhost_tgt_pid
	fi
	rm -f $testdir/${MATCH_FILE} $testdir/match_files/spdkcli_details_vhost.test /tmp/sample_aio /tmp/sample_pmem
	print_backtrace
	exit 1
}

function run_spdk_tgt() {
	$SPDK_BIN_DIR/spdk_tgt -m 0x3 -p 0 &
	spdk_tgt_pid=$!
	waitforlisten $spdk_tgt_pid
}

function run_nvmf_tgt() {
	$SPDK_BIN_DIR/nvmf_tgt -m 0x3 -p 0 &
	nvmf_tgt_pid=$!
	waitforlisten $nvmf_tgt_pid
}

function run_vhost_tgt() {
	$SPDK_BIN_DIR/vhost -m 0x3 -p 0 &
	vhost_tgt_pid=$!
	waitforlisten $vhost_tgt_pid
}

function check_match() {
	$rootdir/scripts/spdkcli.py ll $SPDKCLI_BRANCH > $testdir/match_files/${MATCH_FILE}
	$rootdir/test/app/match/match $testdir/match_files/${MATCH_FILE}.match
	rm -f $testdir/match_files/${MATCH_FILE}
}

function wait_for_all_nvme_ctrls_to_detach() {
	while (($(rpc_cmd bdev_nvme_get_controllers | jq '.|length') != 0)); do :; done
}
