#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$(readlink -f $testdir/../..)
source $SPDK_BUILD_DIR/test/common/autotest_common.sh
match_file=$testdir/spdkcli.test.match
out_file=$testdir/spdkcli.test
timing_enter spdk_cli

function on_error_exit() {
	set +e
	killprocess $spdk_tgt_pid
	rm -f $out_file
	print_backtrace
	exit 1
}

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"

trap 'on_error_exit' ERR

$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -s 1024 -r /var/tmp/spdk.sock &
spdk_tgt_pid=$!

waitforlisten $spdk_tgt_pid
#$SPDK_BUILD_DIR/scripts/gen_nvme.sh "--json" | $spdk_rpc_py load_subsystem_config

#$SPDK_BUILD_DIR/scripts/spdkcli.py bdevs/Malloc create 32 512 Malloc0
#$SPDK_BUILD_DIR/scripts/spdkcli.py bdevs/Malloc create 32 4096 Malloc1
#$SPDK_BUILD_DIR/scripts/spdkcli.py bdevs/Split_Disk split_bdev Nvme0n1 4
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/block create vhost_blk1 Nvme0n1p0
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/block create vhost_blk2 Nvme0n1p0 0x1 readonly
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/scsi create vhost_scsi1
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/scsi create vhost_scsi2
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/scsi/vhost_scsi1 add_lun 0 Malloc0
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/scsi/vhost_scsi2 add_lun 0 Malloc1
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p1
#$SPDK_BUILD_DIR/scripts/spdkcli.py vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p2
#$SPDK_BUILD_DIR/scripts/spdkcli.py ll / > $out_file

dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
$SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job load_spdk_tgt
#$SPDK_BUILD_DIR/test/app/match/match -v $match_file
$SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job clear_spdk_tgt

rm -f $out_file
killprocess $spdk_tgt_pid

timing_exit spdk_cli
report_test_completion spdk_cli
