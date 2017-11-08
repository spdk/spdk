#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

timing_enter blobcli

function linux_iter_pci {
	lspci -mm -n -D | grep $1 | tr -d '"' | awk -F " " '{print $1}'
}

if [ "$NO_SPDK" = "1" ]
then
	[ -e /dev/nvme0n1 ] || (echo "No /dev/nvme0n1 device node found." && exit 1)
else
	[ -e /dev/nvme0n1 ] && (echo "/dev/nvme0n1 device found - need to run SPDK setup.sh script to bind to UIO." && exit 1)
fi

# Nvme0 target configuration
set -e

echo "[Nvme]" > $testdir/blobcli.conf
for bdf in $(linux_iter_pci 0108); do
	echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme0" >> $testdir/blobcli.conf
	break
done

cp $rootdir/examples/blob/cli/blobcli $testdir/
chmod a+x $testdir/blobcli

cd $testdir
$testdir/blobcli -b Nvme0n1 -T $testdir/test.bs ignore
cd $rootdir

rm -rf $testdir/blobcli.conf
rm -rf $testdir/blobcli
rm -rf $testdir/65.blob
rm -rf $testdir/M.blob

timing_exit blobcli
