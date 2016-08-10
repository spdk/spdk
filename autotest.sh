#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/scripts/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

set -xe

if [ $EUID -ne 0 ]; then
	echo "$0 must be run as root"
	exit 1
fi

if [ $(uname -s) = Linux ]; then
	# set core_pattern to a known value to avoid ABRT, systemd-coredump, etc.
	echo "core" > /proc/sys/kernel/core_pattern
fi

trap "process_core; $rootdir/scripts/setup.sh reset; exit 1" SIGINT SIGTERM EXIT

timing_enter autotest

src=$(readlink -f $(dirname $0))
out=$PWD
cd $src

if hash lcov; then
	export LCOV_OPTS="
		--rc lcov_branch_coverage=1
		--rc lcov_function_coverage=1
		--rc genhtml_branch_coverage=1
		--rc genhtml_function_coverage=1
		--rc genhtml_legend=1
		--rc geninfo_all_blocks=1
		"
	export LCOV="lcov $LCOV_OPTS --no-external"
	export GENHTML="genhtml $LCOV_OPTS"
	# zero out coverage data
	$LCOV -q -c -i -t "Baseline" -d $src -o $out/cov_base.info
fi

# Make sure the disks are clean (no leftover partition tables)
timing_enter cleanup
if [ $(uname -s) = Linux ]; then
	# Load the kernel driver
	./scripts/setup.sh reset

	# Let the kernel discover any filesystems or partitions
	sleep 10

	# Delete all partitions on NVMe devices
	devs=`lsblk -l -o NAME | grep nvme | grep -v p`
	for dev in $devs; do
		parted -s /dev/$dev mklabel msdos
	done
fi
timing_exit cleanup

# set up huge pages
timing_enter afterboot
./scripts/setup.sh
timing_exit afterboot

timing_enter nvmf_setup
rdma_device_init
timing_exit nvmf_setup

#####################
# Unit Tests
#####################

timing_enter lib

time test/lib/bdev/blockdev.sh
time test/lib/event/event.sh
time test/lib/nvme/nvme.sh
time test/lib/nvmf/nvmf.sh
time test/lib/env/vtophys.sh
time test/lib/ioat/ioat.sh
time test/lib/json/json.sh
time test/lib/jsonrpc/jsonrpc.sh
time test/lib/log/log.sh
time test/lib/scsi/scsi.sh
time test/lib/iscsi/iscsi.sh
time test/lib/util/util.sh

timing_exit lib


if [ $(uname -s) = Linux ]; then
	export TARGET_IP=127.0.0.1
	export INITIATOR_IP=127.0.0.1

	timing_enter iscsi_tgt
	time ./test/iscsi_tgt/calsoft/calsoft.sh
	time ./test/iscsi_tgt/filesystem/filesystem.sh
	time ./test/iscsi_tgt/fio/fio.sh
	time ./test/iscsi_tgt/reset/reset.sh
	time ./test/iscsi_tgt/rpc_config/rpc_config.sh
	time ./test/iscsi_tgt/idle_migration/idle_migration.sh
	timing_exit iscsi_tgt
fi

timing_enter nvmf

time test/nvmf/fio/fio.sh
time test/nvmf/filesystem/filesystem.sh
time test/nvmf/discovery/discovery.sh
time test/nvmf/nvme_cli/nvme_cli.sh

timing_exit nvmf

timing_enter cleanup
./scripts/setup.sh reset
./scripts/build_kmod.sh clean
timing_exit cleanup

timing_exit autotest
chmod a+r $output_dir/timing.txt

trap - SIGINT SIGTERM EXIT

# catch any stray core files
process_core

if hash lcov; then
	# generate coverage data and combine with baseline
	$LCOV -q -c -d $src -t "$(hostname)" -o $out/cov_test.info
	$LCOV -q -a $out/cov_base.info -a $out/cov_test.info -o $out/cov_total.info
	$LCOV -q -r $out/cov_total.info 'test/*' -o $out/cov_total.info
	$GENHTML $out/cov_total.info -t "$(hostname)" -o $out/coverage
	chmod -R a+rX $out/coverage
	find . -name "*.gcda" -delete
fi
