#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

HELLO_SOCK_APP="$TARGET_NS_CMD $rootdir/examples/sock/hello_world/hello_sock"
SOCAT_APP="$TARGET_NS_CMD socat"

# ----------------
# Test server path
# ----------------

timing_enter sock_server

$HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT -S & pid=$!
trap "killprocess $pid;exit 1" SIGINT SIGTERM EXIT
waitforlisten $pid
#while ! $TARGET_NS_CMD nc -z $TARGET_IP $ISCSI_PORT; do
#  sleep 1
#done

# send message
message="**MESSAGE:This is a test message to the server**"
response=$( echo $message | $SOCAT_APP - tcp:$TARGET_IP:$ISCSI_PORT 2>/dev/null )

if [ "$message" != "$response" ]; then
	exit 1
fi

killprocess $pid
timing_exit sock_server

# ----------------
# Test client path
# ----------------

timing_enter sock_client
echo "Testing client path"

# start echo server
$SOCAT_APP tcp-l:$ISCSI_PORT,fork,bind=$TARGET_IP exec:'/bin/cat' & pid=$!
trap "killprocess $pid;exit 1" SIGINT SIGTERM EXIT

#waitforlisten $pid $TARGET_IP:$ISCSI_PORT
while ! $TARGET_NS_CMD nc -z $TARGET_IP $ISCSI_PORT; do
  sleep 1
done

# send message
message="**MESSAGE:This is a test message from the client**"
response=$( echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT )

if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

killprocess $pid

report_test_completion "sock_client"
timing_exit sock_client