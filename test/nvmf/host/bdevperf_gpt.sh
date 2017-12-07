#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

set -e

timing_enter bdevperf_gpt

# Create conf file for bdevperf_gpt
cat > $testdir/bdevperf_gpt.conf << EOL
[Gpt]
  Disable No
EOL

# Get Nvme0n1 info through filtering gen_nvme.sh's result
# Only need one nvme and discard the remaining lines from the 5th line
$rootdir/scripts/gen_nvme.sh >> $testdir/bdevperf_gpt.conf
sed -i '5,$d' $testdir/bdevperf_gpt.conf

# Set Gpt partition on disk Nvme0n1
$rootdir/scripts/setup.sh reset
sleep 2
parted -s /dev/nvme0n1 mklabel gpt mkpart first '0%' '50%' mkpart second '50%' '100%'
/usr/sbin/sgdisk -t 1:7c5222bd-8f5d-4087-9c00-bf9843c7b58c /dev/nvme0n1
/usr/sbin/sgdisk -t 2:7c5222bd-8f5d-4087-9c00-bf9843c7b58c /dev/nvme0n1
$rootdir/scripts/setup.sh
sleep 2

# Run bdevperf with bdevperf_gpt.conf
$rootdir/test/lib/bdev/bdevperf/bdevperf -c $testdir/bdevperf_gpt.conf -q 128 -s 4096 -w verify -t 5
sync

rm -rf $testdir/bdevperf_gpt.conf

# Delete gpt partition on the disk
$rootdir/scripts/setup.sh reset
sleep 1
parted -s /dev/nvme0n1 mklabel msdos
$rootdir/scripts/setup.sh
sleep 1

trap - SIGINT SIGTERM EXIT

timing_exit bdevperf_gpt
