#!/usr/bin/env bash
set -ex
testdir=$(readlink -f $(dirname $0))
rootdir=$(realpath $testdir/../..)

source $rootdir/test/vhost/common/common.sh
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

# Commonly used variables for test
lvs_name="lvs_0"
lvol_name="lvol_0"
lvol_bs="512"
lvol_size="128"
lvol_alias="$lvs_name/$lvol_name"

trap "at_app_exit; error_exit" SIGINT ERR

spdk_vhost_run
echo "****************************************************"

# Create bdevs needed in later tests
malloc_bdev=$($rpc_py construct_malloc_bdev $lvol_size $lvol_bs)


# ----------- TEST 1 ----------- #
# Construct & delete a lvol store using generated UUID
lvs_uuid=$($rpc_py construct_lvol_store $malloc_bdev $lvs_name)
$rpc_py destroy_lvol_store -u $lvs_uuid

# Construct & delete a lvol store using lvol store alias
lvs_uuid=$($rpc_py construct_lvol_store $malloc_bdev $lvs_name)
$rpc_py destroy_lvol_store -l $lvs_name

# Construct & delete lvol bdevs from lvol store
# Use different combinations of using aliases and UUIDs
lvs_uuid=$($rpc_py construct_lvol_store $malloc_bdev $lvs_name)
lvs_size=$(jq ".[] | .cluster_size * .free_clusters" <<< $($rpc_py get_lvol_stores -u $lvs_uuid))
lvs_size=$(($lvs_size/1024/1024))

lvol_uuid=$($rpc_py construct_lvol_bdev $lvol_name $lvs_size -u $lvs_uuid)
$rpc_py destroy_lvol_bdev $lvol_uuid
lvol_uuid=$($rpc_py construct_lvol_bdev $lvol_name $lvs_size -l $lvs_name)
$rpc_py destroy_lvol_bdev $lvol_alias
$rpc_py destroy_lvol_store -l $lvs_name





echo "****************************************************"
spdk_vhost_kill

trap - SIGINT ERR


