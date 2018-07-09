#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
SPDKCLI_BUILD_DIR=$(readlink -f $testdir/../..)
spdkcli_job="python3 $SPDKCLI_BUILD_DIR/test/spdkcli/spdkcli_job.py"
. $SPDKCLI_BUILD_DIR/test/common/autotest_common.sh

function on_error_exit() {
	set +e
	if [ ! -z $virtio_pid ]; then
		killprocess $virtio_pid
	fi
	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
	rm -f $testdir/spdkcli.test $testdir/spdkcli_details.test /tmp/sample_aio
	rm -f $testdir/spdkcli.test_pmem /tmp/sample_pmem
	print_backtrace
	exit 1
}

function run_spdk_tgt() {
	timing_enter run_spdk_tgt
	$SPDKCLI_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x3 -p 0 -s 1024 &
	spdk_tgt_pid=$!

	waitforlisten $spdk_tgt_pid
	timing_exit run_spdk_tgt
}

function run_virtio_initiator() {
	$SPDKCLI_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x2 -p 0 -g -u -s 1024 -r /var/tmp/virtio.sock &
	virtio_pid=$!

	waitforlisten $virtio_pid /var/tmp/virtio.sock
}

function create_bdevs() {
	$spdkcli_job "/bdevs/malloc create 32 512 Malloc0" "Malloc0" True
	$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
	$spdkcli_job "/bdevs/malloc create 32 512 Malloc2" "Malloc2" True
	$spdkcli_job "/bdevs/malloc create 32 4096 Malloc3" "Malloc3" True
	$spdkcli_job "/bdevs/error create Malloc1" "EE_Malloc1" True
	$spdkcli_job "/bdevs/null create null_bdev 32 512" "null_bdev" True
	dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
	$spdkcli_job "/bdevs/aio create sample /tmp/sample_aio 512" "sample" True
	trtype=$($SPDKCLI_BUILD_DIR/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").trtype')
	traddr=$($SPDKCLI_BUILD_DIR/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").traddr')
	$spdkcli_job "/bdevs/nvme create Nvme0 $trtype $traddr" "Nvme0" True
	$spdkcli_job "/bdevs/split_disk split_bdev Nvme0n1 4" "Nvme0n1p0" True
}

function create_lvols() {
	$spdkcli_job "/lvol_stores create lvs Malloc0" "lvs" True
	$spdkcli_job "/bdevs/logical_volume create lvol 16 lvs" "lvs/lvol" True
}

function create_vhosts() {
	$spdkcli_job "vhost/block create vhost_blk1 Nvme0n1p0" "Nvme0n1p0" True
	$spdkcli_job "vhost/block create vhost_blk2 Nvme0n1p1 0x2 readonly" "Nvme0n1p1" True
	$spdkcli_job "vhost/scsi create vhost_scsi1" "vhost_scsi1" True
	$spdkcli_job "vhost/scsi create vhost_scsi2" "vhost_scsi2" True
	$spdkcli_job "vhost/scsi/vhost_scsi1 add_lun 0 Malloc2" "Malloc2" True
	$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 0 Malloc3" "Malloc3" True
	$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p2" "Nvme0n1p2" True
	$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p3" "Nvme0n1p3" True
}

function clear_config() {
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
}

function run_vhost_test() {
	timing_enter spdkcli_create_bdevs_config
	create_bdevs
	timing_exit spdkcli_create_bdevs_config

	timing_enter spdkcli_create_lvols_config
	create_lvols
	timing_exit spdkcli_create_lvols_config

	timing_enter spdkcli_create_vhosts_config
	create_vhosts
	timing_exit spdkcli_create_vhosts_config

	#check match
	timing_enter spdkcli_check_match
	python3 $SPDKCLI_BUILD_DIR/scripts/spdkcli.py ll > $testdir/spdkcli.test
	python3 $SPDKCLI_BUILD_DIR/scripts/spdkcli.py bdevs/split_disk/Nvme0n1p0 show_details | jq -r -S '.' > $testdir/spdkcli_details.test
	$SPDKCLI_BUILD_DIR/test/app/match/match -v $testdir/spdkcli.test.match
	$SPDKCLI_BUILD_DIR/test/app/match/match -v $testdir/spdkcli_details.test.match
	timing_exit spdkcli_check_match

	timing_enter spdkcli_clear_config
	clear_config
	timing_exit spdkcli_clear_config
}

function run_pmem_test() {
	timing_enter spdkcli_pmem
	$spdkcli_job "/bdevs/pmemblk create_pmem_pool /tmp/sample_pmem 32 512" "" True
	$spdkcli_job "/bdevs/pmemblk create/tmp/sample_pmem pmem_bdev" "pmem_bdev" True

	$SPDKCLI_BUILD_DIR/scripts/spdkcli.py ll > $testdir/spdkcli.test_pmem
	$SPDKCLI_BUILD_DIR/test/app/match/match -v $testdir/spdkcli.test_pmem".match"

	$spdkcli_job "/bdevs/pmemblk delete pmem_bdev" "pmem_bdev"
	$spdkcli_job "/bdevs/pmemblk delete_pmem_pool /tmp/sample_pmem" ""
	timing_exit spdkcli_pmem
}

function run_virtio_test() {
	run_spdk_tgt
	run_virtio_initiator

	$spdkcli_job "/bdevs/malloc create 32 512 Malloc0" "Malloc0" True
	$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
	pci_blk=$(lspci -nn -D | grep '1af4:1001' | head -1 | awk '{print $1;}')
	if [ ! -z $pci_blk ]; then
		$spdkcli_job "/bdevs/virtioblk_disk create virtioblk_pci pci $pci_blk" "virtioblk_pci" True
	fi
	pci_scsi=$(lspci -nn -D | grep '1af4:1004' | head -1 | awk '{print $1;}')
	if [ ! -z $pci_scsi ]; then
		$spdkcli_job "/bdevs/virtioscsi_disk create virtioscsi_pci pci $pci_scsi" "virtioscsi_pci" True
	fi
	$spdkcli_job "/vhost/scsi create sample_scsi" "sample_scsi" True
	$spdkcli_job "/vhost/scsi/sample_scsi add_lun 0 Malloc0" "Malloc0" True
	$spdkcli_job "/vhost/block create sample_block Malloc1" "Malloc1" True
	$spdkcli_job "/bdevs/virtioblk_disk create virtioblk_user user $testdir/../../sample_block" "virtioblk_user" True "/var/tmp/virtio.sock"
	$spdkcli_job "/bdevs/virtioscsi_disk create virtioscsi_user user $testdir/../../sample_scsi" "virtioscsi_user" True "/var/tmp/virtio.sock"

	$spdkcli_job "/bdevs/virtioscsi_disk delete virtioscsi_user" "" False "/var/tmp/virtio.sock"
	$spdkcli_job "/bdevs/virtioblk_disk delete virtioblk_user" "" False "/var/tmp/virtio.sock"
	$spdkcli_job "/vhost/block delete sample_block" "sample_block"
	$spdkcli_job "/vhost/scsi/sample_scsi remove_target 0" "Malloc0"
	$spdkcli_job "/vhost/scsi delete sample_scsi" " sample_scsi"
	if [ ! -z $pci_blk ]; then
		$spdkcli_job "/bdevs/virtioblk_disk delete virtioblk_pci" "virtioblk_pci"
	fi
	if [ ! -z $pci_scsi ]; then
		$spdkcli_job "/bdevs/virtioscsi_disk delete virtioscsi_pci" "virtioscsi_pci"
	fi
	$spdkcli_job "/bdevs/malloc delete Malloc0" "Malloc0"
	$spdkcli_job "/bdevs/malloc delete Malloc1" "Malloc1"
}
timing_enter spdk_cli
trap 'on_error_exit' ERR

run_spdk_tgt

case $1 in
	-h|--help)
		echo "usage: $(basename $0) TEST_TYPE"
		echo "Test type can be:"
		echo "	--vhost"
		echo "	--pmem"
		echo "	--virtio"
	;;
	-v|--vhost)
		run_vhost_test
		;;
	-p|--pmem)
		run_pmem_test
                ;;
	-v|--virtio)
		run_virtio_test
		;;
	*)
		echo "Unknown test type: $1"
		exit 1
	;;
esac

if [ ! -z $virtio_pid ]; then
	killprocess $virtio_pid
fi
if [ ! -z $spdk_tgt_pid ]; then
	killprocess $spdk_tgt_pid
fi
rm -f $testdir/spdkcli.test $testdir/spdkcli_details.test /tmp/sample_aio
rm -f $testdir/spdkcli.test_pmem /tmp/sample_pmem
timing_exit spdk_cli
report_test_completion spdk_cli
