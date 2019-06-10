testdir=$(readlink -f $(dirname $0))
SPDKCLI_BUILD_DIR=$(readlink -f $testdir/../..)
spdkcli_job="$SPDKCLI_BUILD_DIR/test/spdkcli/spdkcli_job.py"
spdk_clear_config_py="$SPDKCLI_BUILD_DIR/test/json_config/clear_config.py"
. $SPDKCLI_BUILD_DIR/test/common/autotest_common.sh

function on_error_exit() {
	set +e
	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
	if [ ! -z $nvmf_tgt_pid ]; then
		killprocess $nvmf_tgt_pid
	fi
	if [ ! -z $iscsi_tgt_pid ]; then
		killprocess $iscsi_tgt_pid
	fi
	if [ ! -z $vhost_tgt_pid ]; then
		killprocess $vhost_tgt_pid
	fi
	rm -f $testdir/${MATCH_FILE} $testdir/match_files/spdkcli_details_vhost.test /tmp/sample_aio /tmp/sample_pmem
	print_backtrace
	exit 1
}

function run_spdk_tgt() {
	$SPDKCLI_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x3 -p 0 -s 4096 &
	spdk_tgt_pid=$!
	waitforlisten $spdk_tgt_pid
}

function run_nvmf_tgt() {
	$SPDKCLI_BUILD_DIR/app/nvmf_tgt/nvmf_tgt -m 0x3 -p 0 -s 4096 &
	nvmf_tgt_pid=$!
	waitforlisten $nvmf_tgt_pid
}

function run_iscsi_tgt() {
	$SPDKCLI_BUILD_DIR/app/iscsi_tgt/iscsi_tgt -m 0x3 -p 0 -s 4096 &
	iscsi_tgt_pid=$!
	waitforlisten $iscsi_tgt_pid
}

function run_vhost_tgt() {
	$SPDKCLI_BUILD_DIR/app/vhost/vhost -m 0x3 -p 0 -s 4096 &
	vhost_tgt_pid=$!
	waitforlisten $vhost_tgt_pid
}

function check_match() {
	$SPDKCLI_BUILD_DIR/scripts/spdkcli.py ll $SPDKCLI_BRANCH > $testdir/match_files/${MATCH_FILE}
	$SPDKCLI_BUILD_DIR/test/app/match/match -v $testdir/match_files/${MATCH_FILE}.match
	rm -f $testdir/match_files/${MATCH_FILE}
}
