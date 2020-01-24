#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py=$rootdir/scripts/rpc.py
TEST_TRANSPORT='rdma'

function test_cleanup {
	$rpc_py bdev_lvol_delete_lvstore -l lvs0
	kill -9 $rpc_proxy_pid
}

function test_cleanup_trap {
	set +e
	trap - SIGINT SIGTERM EXIT
	test_cleanup
	set -e
}

nvmftestinit
nvmfappstart "-m 0x3 -p 0 -s 1024"
trap 'process_shm --id $NVMF_APP_SHM_ID; test_cleanup_trap; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
#$rootdir/scripts/gen_nvme.sh --json | $rpc_py load_subsystem_config
$rpc_py bdev_malloc_create -b Malloc0 6144 4096

timing_enter run_rpc_proxy
$rootdir/scripts/rpc_http_proxy.py 127.0.0.1 3333 secret secret &
rpc_proxy_pid=$!
timing_exit run_rpc_proxy

timing_enter configure_spdk
$rpc_py bdev_get_bdevs
$rpc_py bdev_lvol_delete_lvstore -l lvs0 || true
$rpc_py bdev_lvol_create_lvstore Malloc0 lvs0
$rpc_py bdev_get_bdevs
timing_exit configure_spdk

timing_enter restart_cinder
sudo systemctl restart devstack@c-*
sleep 10
timing_exit restart_cinder

# Start testing spdk with openstack using tempest (openstack tool that allow testing an openstack functionalities)
# In this tests is checked if spdk can correctly cooperate with openstack spdk driver
timing_enter tempest_tests
current_dir=$(pwd)
cd /opt/stack/tempest
tox -e all -- tempest.api.compute.volumes.test_attach_volume.AttachVolumeTestJSON.test_attach_detach_volume
tox -e all -- tempest.api.compute.volumes.test_attach_volume.AttachVolumeTestJSON.test_list_get_volume_attachments
tox -e all -- tempest.api.compute.volumes.test_volume_snapshots.VolumesSnapshotsTestJSON.test_volume_snapshot_create_get_list_delete
tox -e all -- tempest.api.compute.volumes.test_volumes_get.VolumesGetTestJSON.test_volume_create_get_delete
tox -e all -- tempest.api.compute.volumes.test_volumes_list.VolumesTestJSON.test_volume_list
tox -e all -- tempest.api.volume.test_versions.VersionsTest.test_list_versions
tox -e all -- tempest.api.volume.test_volumes_extend.VolumesExtendTest.test_volume_extend
tox -e all -- tempest.api.volume.test_volumes_extend.VolumesExtendTest.test_volume_extend_when_volume_has_snapshot
tox -e all -- tempest.api.volume.test_volumes_get.VolumesSummaryTest.test_show_volume_summary
tox -e all -- tempest.api.volume.test_volumes_list.VolumesListTestJSON.test_volume_list
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_snapshot_create_delete_with_volume_in_use
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_snapshot_create_get_list_update_delete
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_snapshot_create_offline_delete_online
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_volume_from_snapshot
tox -e all -- tempest.api.volume.test_volumes_snapshots.VolumesSnapshotTestJSON.test_volume_from_snapshot_no_size
tox -e all -- tempest.api.volume.test_volumes_snapshots_list.VolumesSnapshotListTestJSON.test_snapshot_list_param_limit
cd $current_dir
timing_exit tempest_tests

timing_enter test_cleanup
test_cleanup
trap - SIGINT SIGTERM EXIT
nvmftestfini
timing_exit test_cleanup
