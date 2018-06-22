#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$(readlink -f $testdir/../..)
source $SPDK_BUILD_DIR/test/common/autotest_common.sh

timing_enter spdk_cli
function on_error_exit() {
	set +e
	killprocess $spdk_tgt_pid
	rm -f $testdir/spdkcli.test $testdir/spdkcli_details.test /tmp/sample_aio
	print_backtrace
	exit 1
}

timing_enter run_spdk_tgt
trap 'on_error_exit' ERR
spdkcli_job="python3 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py"
$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x3 -p 0 -s 1024 &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid
timing_exit run_spdk_tgt

timing_enter spdkcli_create_bdev_config
#create bdevs
$spdkcli_job "/bdevs/malloc create 32 512 Malloc0" "Malloc0" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc2" "Malloc2" True
$spdkcli_job "/bdevs/malloc create 32 4096 Malloc3" "Malloc3" True
$spdkcli_job "/bdevs/error create Malloc1" "EE_Malloc1" True
$spdkcli_job "/bdevs/null create null_bdev 32 512" "null_bdev" True
dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
$spdkcli_job "/bdevs/aio create sample /tmp/sample_aio 512" "sample" True
trtype=$(./scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").trtype')
traddr=$(./scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").traddr')
$spdkcli_job "/bdevs/nvme create Nvme0 $trtype $traddr" "Nvme0" True
$spdkcli_job "/bdevs/split_disk split_bdev Nvme0n1 4" "Nvme0n1p0" True

#create lvols
$spdkcli_job "/lvol_stores create lvs Malloc0" "lvs" True
$spdkcli_job "/bdevs/logical_volume create lvol 16 lvs" "lvs/lvol" True

#create vhosts
$spdkcli_job "vhost/block create vhost_blk1 Nvme0n1p0" "Nvme0n1p0" True
$spdkcli_job "vhost/block create vhost_blk2 Nvme0n1p1 0x2 readonly" "Nvme0n1p1" True
$spdkcli_job "vhost/scsi create vhost_scsi1" "vhost_scsi1" True
$spdkcli_job "vhost/scsi create vhost_scsi2" "vhost_scsi2" True
$spdkcli_job "vhost/scsi/vhost_scsi1 add_lun 0 Malloc2" "Malloc2" True
$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 0 Malloc3" "Malloc3" True
$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p2" "Nvme0n1p2" True
$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p3" "Nvme0n1p3" True
timing_exit spdkcli_create_bdev_config

#check match
timing_enter spdkcli_check_match
python3 $SPDK_BUILD_DIR/scripts/spdkcli.py ll > $testdir/spdkcli.test
python3 $SPDK_BUILD_DIR/scripts/spdkcli.py bdevs/split_disk/Nvme0n1p0 show_details | jq -r -S '.' > $testdir/spdkcli_details.test
$SPDK_BUILD_DIR/test/app/match/match -v $testdir/spdkcli.test.match
$SPDK_BUILD_DIR/test/app/match/match -v $testdir/spdkcli_details.test.match
timing_exit spdkcli_check_match

timing_enter spdkcli_delete_items
#delete items
$spdkcli_job "vhost/scsi/vhost_scsi2 remove_target 2" "Nvme0n1p3"
$spdkcli_job "vhost/scsi/vhost_scsi2 remove_target 1" "Nvme0n1p2"
$spdkcli_job "vhost/scsi/vhost_scsi2 remove_target 0" "Malloc3"
$spdkcli_job "vhost/scsi/vhost_scsi1 remove_target 0" "Malloc2"
$spdkcli_job "vhost/scsi delete vhost_scsi2" "vhost_scsi2"
$spdkcli_job "vhost/scsi delete vhost_scsi1" "vhost_scsi1"
$spdkcli_job "vhost/block delete vhost_blk2" "vhost_blk2"
$spdkcli_job "vhost/block delete vhost_blk1" "vhost_blk1"
$spdkcli_job "/bdevs/split_disk destruct_split_bdev Nvme0n1" "Nvme0n1p0"
$spdkcli_job "/bdevs/aio delete sample" "sample"
$spdkcli_job "/bdevs/nvme delete Nvme0n1" "Nvme0n1"
$spdkcli_job "/bdevs/null delete null_bdev" "null_bdev"
$spdkcli_job "/bdevs/logical_volume delete lvs/lvol" "lvs/lvol"
$spdkcli_job "/lvol_stores delete lvs" "lvs"
$spdkcli_job "/bdevs/malloc delete Malloc0" "Malloc0"
$spdkcli_job "/bdevs/malloc delete Malloc1" "Malloc1"
$spdkcli_job "/bdevs/malloc delete Malloc2" "Malloc2"
$spdkcli_job "/bdevs/malloc delete Malloc3" "Malloc3"
timing_exit spdkcli_delete_items

rm -f $testdir/spdkcli.test $testdir/spdkcli_details.test /tmp/sample_aio
killprocess $spdk_tgt_pid

timing_exit spdk_cli
report_test_completion spdk_cli
