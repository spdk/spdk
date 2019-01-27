#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter vhost
timing_enter negative
run_test suite ./test/vhost/spdk_vhost.sh --negative
timing_exit negative

timing_enter vhost_boot
run_test suite ./test/vhost/spdk_vhost.sh --boot
timing_exit vhost_boot

if [ $RUN_NIGHTLY -eq 1 ]; then
	timing_enter integrity_blk
	run_test suite ./test/vhost/spdk_vhost.sh --integrity-blk
	timing_exit integrity_blk

	timing_enter integrity
	run_test suite ./test/vhost/spdk_vhost.sh --integrity
	timing_exit integrity

	timing_enter fs_integrity_scsi
	run_test suite ./test/vhost/spdk_vhost.sh --fs-integrity-scsi
	timing_exit fs_integrity_scsi

	timing_enter fs_integrity_blk
	run_test suite ./test/vhost/spdk_vhost.sh --fs-integrity-blk
	timing_exit fs_integrity_blk

	timing_enter integrity_lvol_scsi_nightly
	run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-scsi-nightly
	timing_exit integrity_lvol_scsi_nightly

	timing_enter integrity_lvol_blk_nightly
	run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-blk-nightly
	timing_exit integrity_lvol_blk_nightly

	# timing_enter readonly
	# run_test suite ./test/vhost/spdk_vhost.sh --readonly
	# timing_exit readonly
fi

timing_enter integrity_lvol_scsi
run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-scsi
timing_exit integrity_lvol_scsi

timing_enter integrity_lvol_blk
run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-blk
timing_exit integrity_lvol_blk

timing_enter vhost_nvme
run_test suite ./test/vhost/spdk_vhost.sh --vhost-nvme
timing_exit vhost_nvme

timing_enter spdk_cli
run_test suite ./test/spdkcli/vhost.sh
timing_exit spdk_cli

timing_exit vhost
