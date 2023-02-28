#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function waitfortcp() {
	local addr="$2"

	if hash ip &> /dev/null; then
		local have_ip_cmd=true
	else
		local have_ip_cmd=false
	fi

	if hash ss &> /dev/null; then
		local have_ss_cmd=true
	else
		local have_ss_cmd=false
	fi

	echo "Waiting for process to start up and listen on address $addr..."
	# turn off trace for this loop
	xtrace_disable
	local ret=0
	local i
	for ((i = 40; i != 0; i--)); do
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
	if ((i == 0)); then
		echo "ERROR: timeout while waiting for process (pid: $1) to start listening on '$addr'"
		ret=1
	fi
	return $ret
}

iscsitestinit

HELLO_SOCK_APP="${TARGET_NS_CMD[*]} $SPDK_EXAMPLE_DIR/hello_sock"
SOCAT_APP="socat"
OPENSSL_APP="openssl"
PSK="-N ssl -E 1234567890ABCDEF -I psk.spdk.io"

# ----------------
# Test client path
# ----------------
timing_enter sock_client
echo "Testing client path"

# start echo server using socat
$SOCAT_APP tcp-l:$ISCSI_PORT,fork,bind=$INITIATOR_IP exec:'/bin/cat' &
server_pid=$!
trap 'killprocess $server_pid;iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitfortcp $server_pid $INITIATOR_IP:$ISCSI_PORT

# send message using hello_sock client
message="**MESSAGE:This is a test message from the client**"
response=$(echo $message | $HELLO_SOCK_APP -H $INITIATOR_IP -P $ISCSI_PORT -N "posix")

if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using hello_sock client with zero copy disabled
message="**MESSAGE:This is a test message from the client with zero copy disabled**"
response=$(echo $message | $HELLO_SOCK_APP -H $INITIATOR_IP -P $ISCSI_PORT -N "posix" -z)

if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using hello_sock client with zero copy enabled
message="**MESSAGE:This is a test message from the client with zero copy enabled**"
response=$(echo $message | $HELLO_SOCK_APP -H $INITIATOR_IP -P $ISCSI_PORT -N "posix" -Z)

if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

trap '-' SIGINT SIGTERM EXIT
# NOTE: socat returns code 143 on SIGINT
killprocess $server_pid || true

timing_exit sock_client

# ----------------
# Test SSL server path
# ----------------
timing_enter sock_ssl_server
echo "Testing SSL server path"

# start echo server using hello_sock echo server
$HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT -S $PSK -m 0x1 &
server_pid=$!
trap 'killprocess $server_pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $server_pid

# send message using hello_sock client
message="**MESSAGE:This is a test message from the hello_sock client with ssl**"
response=$(echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -m 0x2)
if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using hello_sock client using TLS 1.3
message="**MESSAGE:This is a test message from the hello_sock client with ssl using TLS 1.3**"
response=$(echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -T 13 -m 0x2)
if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using hello_sock client using TLS 1.2
message="**MESSAGE:This is a test message from the hello_sock client with ssl using TLS 1.2**"
response=$(echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -T 12 -m 0x2)
if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using hello_sock client using incorrect TLS 7
message="**MESSAGE:This is a test message from the hello_sock client with ssl using incorrect TLS 7**"
echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -T 7 -m 0x2 && exit 1

# send message using hello_sock client with KTLS disabled
message="**MESSAGE:This is a test message from the hello_sock client with KTLS disabled**"
response=$(echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -k -m 0x2)
if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using hello_sock client with KTLS enabled

# CI so far doesn't support new openssl-3 with this option.
# This section is commented out and will be changed back after the CI has systems that run with openssl-3
# See GH issue #2687

# message="**MESSAGE:This is a test message from the hello_sock client with KTLS enabled**"
# echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -K

# send message using openssl client using TLS 1.3
message="**MESSAGE:This is a test message from the openssl client using TLS 1.3**"
response=$( (
	echo -ne $message
	sleep 2
) | $OPENSSL_APP s_client -debug -state -tlsextdebug -tls1_3 -psk_identity psk.spdk.io -psk "1234567890ABCDEF" -connect $TARGET_IP:$ISCSI_PORT)
if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using openssl client using TLS 1.2
message="**MESSAGE:This is a test message from the openssl client using TLS 1.2**"
response=$( (
	echo -ne $message
	sleep 2
) | $OPENSSL_APP s_client -debug -state -tlsextdebug -tls1_2 -psk_identity psk.spdk.io -psk "1234567890ABCDEF" -connect $TARGET_IP:$ISCSI_PORT)
if ! echo "$response" | grep -q "$message"; then
	exit 1
fi

# send message using hello_sock client with unmatching PSK KEY, expect a failure
message="**MESSAGE:This is a test message from the hello_sock client with unmatching psk_key**"
echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -E 4321DEADBEEF1234 -m 0x2 && exit 1

# send message using hello_sock client with unmatching PSK IDENTITY, expect a failure
message="**MESSAGE:This is a test message from the hello_sock client with unmatching psk_key**"
echo $message | $HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT $PSK -I WRONG_PSK_ID -m 0x2 && exit 1

trap '-' SIGINT SIGTERM EXIT
# NOTE: socat returns code 143 on SIGINT
killprocess $server_pid || true

timing_exit sock_ssl_server

# ----------------
# Test server path
# ----------------

timing_enter sock_server

# start echo server using hello_sock echo server
$HELLO_SOCK_APP -H $TARGET_IP -P $ISCSI_PORT -S -N "posix" -m 0x1 &
server_pid=$!
trap 'killprocess $server_pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $server_pid

# send message to server using socat
message="**MESSAGE:This is a test message to the server**"
response=$(echo $message | $SOCAT_APP - tcp:$TARGET_IP:$ISCSI_PORT 2> /dev/null)

if [ "$message" != "$response" ]; then
	exit 1
fi

trap - SIGINT SIGTERM EXIT

killprocess $server_pid

iscsitestfini
timing_exit sock_server
