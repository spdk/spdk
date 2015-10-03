#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/scripts/autotest_common.sh"

set -xe

if [ $EUID -ne 0 ]; then
	echo "$0 must be run as root"
	exit 1
fi

trap "process_core; $rootdir/scripts/cleanup.sh; exit 1" SIGINT SIGTERM EXIT

timing_enter autotest

src=$(readlink -f $(dirname $0))
out=$PWD
cd $src

# set up huge pages
timing_enter afterboot
./scripts/configure_hugepages.sh 3072
timing_exit afterboot

lsmod | grep -q ^nvme && rmmod nvme || true

#####################
# Unit Tests
#####################

timing_enter lib

time test/lib/nvme/nvme.sh
time test/lib/memory/memory.sh

timing_exit lib

./scripts/cleanup.sh

timing_exit autotest
chmod a+r $output_dir/timing.txt

trap - SIGINT SIGTERM EXIT

# catch any stray core files
process_core
