#!/usr/bin/env bash
set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py=$rootdir/scripts/rpc.py
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit

function error_exit {
	killprocess $rpc_proxy_pid
	killprocess $spdk_tgt_pid
	rm $rootdir/conf.json
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

timing_enter run_spdk_tgt
$rootdir/scripts/gen_nvme.sh >> $rootdir/conf.json
$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 -p 0 -s 1024 -c $rootdir/conf.json &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid
$rpc_py set_bdev_nvme_hotplug -e
timing_exit run_spdk_tgt

timing_enter run_rpc_proxy
$rootdir/scripts/rpc_http_proxy.py 127.0.0.1 3333 secret secret &
rpc_proxy_pid=$!
timing_exit run_rpc_proxy

timing_enter configure_spdk
$rpc_py get_bdevs
$rpc_py destroy_lvol_store -l lvs0 || true
$rpc_py construct_lvol_store Nvme0n1 lvs0
$rpc_py get_bdevs
timing_exit configure_spdk

sudo systemctl restart devstack@c-*
cd /opt/stack/tempest
#tox -e all -- tempest.api.compute.volumes.test_attach_volume.AttachVolumeTestJSON.test_attach_detach_volume
#tox -e all -- tempest.api.compute.volumes.test_attach_volume.AttachVolumeTestJSON.test_list_get_volume_attachments
tox -e all -- tempest.api.compute.volumes.test_volume_snapshots.VolumesSnapshotsTestJSON.test_volume_snapshot_create_get_list_delete
tox -e all -- tempest.api.compute.volumes.test_volumes_get.VolumesGetTestJSON.test_volume_create_get_delete
tox -e all -- tempest.api.compute.volumes.test_volumes_list.VolumesTestJSON.test_volume_list
tox -e all -- tempest.api.volume.test_versions.VersionsTest.test_list_versions
tox -e all -- tempest.api.volume.test_volumes_extend.VolumesExtendTest.test_volume_extend
tox -e all -- tempest.api.volume.test_volumes_extend.VolumesExtendTest.test_volume_extend_when_volume_has_snapshot
tox -e all -- tempest.api.volume.test_volumes_get.VolumesSummaryTest.test_show_volume_summary
tox -e all -- tempest.api.volume.test_volumes_list.VolumesListTestJSON.test_volume_list
#tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_snapshot_create_delete_with_volume_in_use
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_snapshot_create_get_list_update_delete
#tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_snapshot_create_offline_delete_online
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_volume_from_snapshot
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_volume_from_snapshot_no_size
tox -e all -- tempest.api.volume.test_volumes_snapshots_list.VolumesSnapshotListTestJSON.test_snapshot_list_param_limit
cd $rootdir

killprocess $rpc_proxy_pid
killprocess $spdk_tgt_pid
rm $rootdir/conf.json

