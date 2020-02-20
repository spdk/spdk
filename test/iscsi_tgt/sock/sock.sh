#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function waitfortcp() {
	local addr="$2"

	if hash ip &>/dev/null; then
		local have_ip_cmd=true
	else
		local have_ip_cmd=false
	fi

	if hash ss &>/dev/null; then
		local have_ss_cmd=true
	else
		local have_ss_cmd=false
	fi

	echo "Waiting for process to start up and listen on address $addr..."
	# turn off trace for this loop
	xtrace_disable
	local ret=0
	local i
	for (( i = 40; i != 0; i-- )); do
		# if the process is no longer running, then exit the script
		#  since it means the application crashed
		if ! kill -s 0 $1; then
			echo "ERROR: process (pid: $1) is no longer running"
			ret=1
			break
		fi

		if $have_ip_cmd; then
			namespace=$(ip netns identify $1)
			if [ -n "$namespace" ]; then
				ns_cmd="ip netns exec $namespace"
			fi
		fi

		if $have_ss_cmd; then
			if $ns_cmd ss -ln | grep -E -q "\s+$addr\s+"; then
				break
			fi
		elif [[ "$(uname -s)" == "Linux" ]]; then
			# For Linux, if system doesn't have ss, just assume it has netstat
			if $ns_cmd netstat -an | grep -iw LISTENING | grep -E -q "\s+$addr\$"; then
				break
			fi
		fi
		sleep 0.5
	done

	xtrace_restore
	if (( i == 0 )); then
		echo "ERROR: timeout while waiting for process (pid: $1) to start listening on '$addr'"
		ret=1
	fi
	return $ret
}

# $1 = "iso" - triggers isolation mode (setting up required environment).
# $2 = test type posix or vpp. defaults to posix.
iscsitestinit $1 $2

if [ "$1" == "iso" ]; then
	TEST_TYPE=$2
else
	TEST_TYPE=$1
fi

if [ -z "$TEST_TYPE" ]; then
	TEST_TYPE="posix"
fi

if [ "$TEST_TYPE" != "posix" ] && [ "$TEST_TYPE" != "vpp" ]; then
        echo "No correct sock implmentation specified"
        exit 1
fi

HELLO_SOCK_APP="${TARGET_NS_CMD[*]} $rootdir/examples/sock/hello_world/hello_sock"
if [ $SPDK_TEST_VPP -eq 1 ]; then
	HELLO_SOCK_APP+=" -L sock_vpp"
fi
SOCAT_APP="socat"

# ----------------
# Test client path
# ----------------
timing_enter sock_client
echo "Testing client path"

# start echo server using socat
$SOCAT_APP tcp-l:$ISCSI_PORT,fork,bind=$INITIATOR_IP exec:'/bin/cat' & server_pid=$!
trap 'killprocess $server_pid;iscsitestfini $1 $2; exit 1' SIGINT SIGTERM EXIT

waitfortcp $server_pid $INITIATOR_IP:$ISCSI_PORT

# send message using hello_sock client
message="**MESSAGE:This is a test message from the client**"
response=$( echo $message | $HELLO_SOCK_APP -H $INITIATOR_IP -P $ISCSI_PORT -N $TEST_TYPE)

if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

trap '-' SIGINT SIGTERM EXIT
# NOTE: socat returns code 143 on SIGINT
killprocess $server_pid || true

timing_exit sock_client

# ----------------
# Test server path
# ----------------

timing_enter sock_server

# start echo server using hello_sock echo server
$HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT -S -N $TEST_TYPE & server_pid=$!
trap 'killprocess $server_pid; iscsitestfini $1 $2; exit 1' SIGINT SIGTERM EXIT
waitforlisten $server_pid

# send message to server using socat
message="**MESSAGE:This is a test message to the server**"
response=$( echo $message | $SOCAT_APP - tcp:$TARGET_IP:$ISCSI_PORT 2>/dev/null )

if [ "$message" != "$response" ]; then
	exit 1
fi

trap - SIGINT SIGTERM EXIT

killprocess $server_pid

iscsitestfini $1 $2
timing_exit sock_server
