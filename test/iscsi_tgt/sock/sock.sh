#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

HELLO_SOCK_APP="$TARGET_NS_CMD $rootdir/examples/sock/hello_world/hello_sock"
SOCAT_APP="socat"

# ----------------
# Test client path
# ----------------
timing_enter sock_client
echo "Testing client path"

# start echo server using socat
$SOCAT_APP tcp-l:$ISCSI_PORT,fork,bind=$INITIATOR_IP exec:'/bin/cat' & server_pid=$!
trap "killprocess $server_pid;exit 1" SIGINT SIGTERM EXIT

waitforlisten $server_pid $INITIATOR_IP:$ISCSI_PORT

# send message using hello_sock client
message="**MESSAGE:This is a test message from the client**"
response=$( echo $message | $HELLO_SOCK_APP -H $INITIATOR_IP -P $ISCSI_PORT )

if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

trap '-' SIGINT SIGTERM EXIT
# NOTE: socat returns code 143 on SIGINT
killprocess $server_pid || true

report_test_completion "sock_client"
timing_exit sock_client

# ----------------
# Test server path
# ----------------

timing_enter sock_server

# start echo server using hello_sock echo server
$HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT -S & server_pid=$!
trap "killprocess $server_pid;exit 1" SIGINT SIGTERM EXIT
waitforlisten $server_pid

sleep 1

# send message to server using socat
message="**MESSAGE:This is a test message to the server**"
response=$( echo $message | $SOCAT_APP - tcp:$TARGET_IP:$ISCSI_PORT 2>/dev/null )

if [ "$message" != "$response" ]; then
	exit 1
fi

trap - SIGINT SIGTERM EXIT

killprocess $server_pid

report_test_completion "sock_server"
timing_exit sock_server
