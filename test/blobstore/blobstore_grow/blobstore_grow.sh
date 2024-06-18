#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

SYSTEM=$(uname -s)
if [ $SYSTEM = "FreeBSD" ]; then
	echo "blob_io_wait.sh cannot run on FreeBSD currently."
	exit 0
fi

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
rpc_py="$rootdir/scripts/rpc.py"

$rootdir/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!

trap 'killprocess $bdev_svc_pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $bdev_svc_pid

$rpc_py bdev_malloc_create --name malloc0 128 4096
$rpc_py bdev_malloc_create --name malloc1 128 4096
$rpc_py bdev_malloc_create --name malloc2 128 4096
$rpc_py bdev_raid_create --name concat0 --raid-level concat --base-bdevs malloc0 --strip-size-kb 4
$rpc_py bdev_lvol_create_lvstore --cluster-sz 4194304 --clear-method unmap concat0 lvs0 --md-pages-per-cluster-ratio 300
free_clusters=$($rpc_py bdev_lvol_get_lvstores --lvs-name lvs0 | jq -rM '.[0].free_clusters')
test $free_clusters -eq 31
$rpc_py bdev_raid_delete concat0
$rpc_py bdev_raid_create --name concat0 --raid-level concat --base-bdevs "malloc0 malloc1" --strip-size-kb 4
$rpc_py bdev_lvol_grow_lvstore -l lvs0
free_clusters=$($rpc_py bdev_lvol_get_lvstores --lvs-name lvs0 | jq -rM '.[0].free_clusters')
test $free_clusters -eq 63
$rpc_py bdev_lvol_create --lvs-name lvs0 --thin-provision --clear-method unmap lv0 8192
$rpc_py bdev_raid_delete concat0
$rpc_py bdev_raid_create --name concat0 --raid-level concat --base-bdevs "malloc0 malloc1 malloc2" --strip-size-kb 4
$rpc_py bdev_lvol_grow_lvstore -l lvs0
free_clusters=$($rpc_py bdev_lvol_get_lvstores --lvs-name lvs0 | jq -rM '.[0].free_clusters')
test $free_clusters -eq 95
bdev_cnt=$($rpc_py bdev_get_bdevs --name lvs0/lv0 | jq -rM '. | length')
test $bdev_cnt -eq 1

killprocess $bdev_svc_pid

trap - SIGINT SIGTERM EXIT
