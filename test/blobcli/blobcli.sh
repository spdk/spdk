#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

timing_enter blobcli

#rpc_py="python $rootdir/scripts/rpc.py"

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

echo "[Malloc]" > $testdir/blobcli.conf
echo "  NumberOfLuns 1" >> $testdir/blobcli.conf
echo "  LunSizeInMB 128" >> $testdir/blobcli.conf
echo "[Nvme]" >> $testdir/blobcli.conf
for bdf in $(linux_iter_pci 0108); do
	echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme0" >> $testdir/blobcli.conf
	break
done

cp $rootdir/examples/blob/cli/blobcli $testdir/
chmod a+x $testdir/blobcli

cd $testdir
$testdir/blobcli -b Nvme0n1 -T $testdir/test.bs ignore
cd $rootdir

echo "blobcli pid: $blobcli_pypid"

rm -rf $testdir/blobcli.conf
rm -rf $testdir/blobcli
rm -rf $testdir/65.blob
rm -rf $testdir/M.blob

trap "killprocess $blobcli_pypid; exit 1" SIGINT SIGTERM EXIT

trap - SIGINT SIGTERM EXIT

timing_exit blobcli
