#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source "$rootdir/test/common/autotest_common.sh"

TEST_TIMEOUT=1200

# The term transport is used a bit loosely for vhost tests.
allowed_nvme_transports=("rdma" "tcp")
allowed_vhost_transports=("scsi" "blk" "all")
bad_transport=true
config_params="--enable-asan --enable-ubsan --enable-debug --without-isal"

# These arguments are used in addition to the test arguments in autotest_common.sh
for i in "$@"; do
	case "$i" in
		--module=*)
			TEST_MODULE="${i#*=}"
			;;
		--timeout=*)
			TEST_TIMEOUT="${i#*=}"
			;;
	esac
done

timing_enter autofuzz
if [ "$TEST_MODULE" == "nvmf" ]; then
	allowed_transports=("${allowed_nvme_transports[@]}")
	if [ $TEST_TRANSPORT == "rdma" ]; then
		config_params="$config_params --with-rdma"
	fi
elif [ "$TEST_MODULE" == "vhost" ]; then
	allowed_transports=("${allowed_vhost_transports[@]}")
	config_params="$config_params --with-vhost --with-virtio"
else
	echo "Invalid module specified. Please specify either nvmf or vhost."
	exit 1
fi

for transport in "${allowed_transports[@]}"; do
	if [ $transport == "$TEST_TRANSPORT" ]; then
		bad_transport=false
	fi
done

if $bad_transport; then
	echo "invalid transport. Please supply one of the following for module: $TEST_MODULE."
	echo "${allowed_transports[@]}"
	exit 1
fi

timing_enter make
cd $rootdir
./configure $config_params
$MAKE $MAKEFLAGS
timing_exit make

# supply --iso to each test module so that it can run setup.sh.
timing_enter fuzz_module
if [ "$TEST_MODULE" == "nvmf" ]; then
	sudo $testdir/autofuzz_nvmf.sh --iso --transport=$TEST_TRANSPORT --timeout=$TEST_TIMEOUT
fi

if [ "$TEST_MODULE" == "vhost" ]; then
	sudo $testdir/autofuzz_vhost.sh --iso --transport=$TEST_TRANSPORT --timeout=$TEST_TIMEOUT
fi

if [ "$TEST_MODULE" == "iscsi" ]; then
	sudo $testdir/autofuzz_iscsi.sh --iso --transport=$TEST_TRANSPORT --timeout=$TEST_TIMEOUT
fi
timing_exit fuzz_module
timing_exit autofuzz
