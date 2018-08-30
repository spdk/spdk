#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

function rpc_client_test() {
	if [ $(uname -s) = Linux ]; then
		local conf=$rootdir/test/bdev/bdev.conf.in

		if [ ! -e $conf ]; then
			return 1
		fi

		$rootdir/test/app/bdev_svc/bdev_svc -i 0 -c ${conf} &
		svc_pid=$!
		echo "Process bdev_svc pid: $svc_pid"
		waitforlisten $svc_pid

		$rootdir/test/rpc_client/rpc_client_test

		killprocess $svc_pid
	fi

	return 0
}

timing_enter rpc_client
rpc_client_test
timing_exit rpc_client
