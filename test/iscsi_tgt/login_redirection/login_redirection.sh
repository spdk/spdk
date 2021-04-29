#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

NULL_BDEV_SIZE=64
NULL_BLOCK_SIZE=512

rpc_py=$rootdir/scripts/rpc.py
fio_py=$rootdir/scripts/fio-wrapper

rpc_addr1="/var/tmp/spdk0.sock"
rpc_addr2="/var/tmp/spdk1.sock"

# This test case uses two iSCSI target applications.

timing_enter start_iscsi_tgts

"${ISCSI_APP[@]}" -r $rpc_addr1 -i 0 -m 0x1 --wait-for-rpc &
pid1=$!
echo "Process pid: $pid1"

"${ISCSI_APP[@]}" -r $rpc_addr2 -i 1 -m 0x2 --wait-for-rpc &
pid2=$!
echo "Process pid: $pid2"

trap 'killprocess $pid1; killprocess $pid2; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid1 $rpc_addr1
$rpc_py -s $rpc_addr1 iscsi_set_options -w 0 -o 30 -a 16
$rpc_py -s $rpc_addr1 framework_start_init
echo "iscsi_tgt_1 is listening."

waitforlisten $pid2 $rpc_addr2
$rpc_py -s $rpc_addr2 iscsi_set_options -w 0 -o 30 -a 16
$rpc_py -s $rpc_addr2 framework_start_init
echo "iscsi_tgt_2 is listening."

timing_exit start_iscsi_tgts

# iSCSI target application 1:
# - Portal group 1 which is public and has a portal
# - Null bdev "Null0" whose size is 64MB and block length is 512.
# - Target node "iqn.2016-06.io.spdk:Target1" which has portal group 1 and Null0.
$rpc_py -s $rpc_addr1 iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py -s $rpc_addr1 iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py -s $rpc_addr1 bdev_null_create Null0 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE
$rpc_py -s $rpc_addr1 iscsi_create_target_node Target1 Target1_alias 'Null0:0' "$PORTAL_TAG:$INITIATOR_TAG" 64 -d

# iSCSI target application 2:
# - Portal group 1 which is private and has a portal
# - A null bdev Null0 whose size is 64MB and block length is 512.
# - Target node "iqn.2016-06.io.spdk:Target1" which has portal group 1 and Null0.
$rpc_py -s $rpc_addr2 iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py -s $rpc_addr2 iscsi_create_portal_group $PORTAL_TAG $TARGET_IP2:$ISCSI_PORT -p
$rpc_py -s $rpc_addr2 bdev_null_create Null0 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE
$rpc_py -s $rpc_addr2 iscsi_create_target_node Target1 Target1_alias 'Null0:0' "$PORTAL_TAG:$INITIATOR_TAG" 64 -d

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1

$fio_py -p iscsi -i 512 -d 1 -t randrw -r 15 &
fiopid=$!
echo "FIO pid: $fiopid"

trap 'iscsicleanup; killprocess $pid1; killprocess $pid2; killprocess $fiopid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

[ "$($rpc_py -s $rpc_addr1 iscsi_get_connections | jq 'length')" = "1" ]
[ "$($rpc_py -s $rpc_addr2 iscsi_get_connections | jq 'length')" = "0" ]

# Move among two portals by login redirection while FIO runs.

$rpc_py -s $rpc_addr1 iscsi_target_node_set_redirect 'iqn.2016-06.io.spdk:Target1' $PORTAL_TAG -a $TARGET_IP2 -p $ISCSI_PORT
$rpc_py -s $rpc_addr1 iscsi_target_node_request_logout 'iqn.2016-06.io.spdk:Target1' -t $PORTAL_TAG

sleep 5

[ "$($rpc_py -s $rpc_addr1 iscsi_get_connections | jq 'length')" = "0" ]
[ "$($rpc_py -s $rpc_addr2 iscsi_get_connections | jq 'length')" = "1" ]

$rpc_py -s $rpc_addr1 iscsi_target_node_set_redirect 'iqn.2016-06.io.spdk:Target1' $PORTAL_TAG
$rpc_py -s $rpc_addr2 iscsi_target_node_request_logout 'iqn.2016-06.io.spdk:Target1' -t $PORTAL_TAG

sleep 5

[ "$($rpc_py -s $rpc_addr1 iscsi_get_connections | jq 'length')" = "1" ]
[ "$($rpc_py -s $rpc_addr2 iscsi_get_connections | jq 'length')" = "0" ]

wait $fiopid

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid1
killprocess $pid2
iscsitestfini
