#!/usr/bin/env bash

set -xe

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

source "$1"

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/test/common/autotest_common.sh"

out=$PWD

MAKEFLAGS=${MAKEFLAGS:--j16}
cd $rootdir

timing_enter porcelain_check
$MAKE clean

if [ $(git status --porcelain --ignore-submodules | wc -l) -ne 0 ]; then
	echo make clean left the following files:
	git status --porcelain --ignore-submodules
	exit 1
fi
timing_exit porcelain_check

if [[ $RUN_NIGHTLY -eq 0 && $SPDK_TEST_RELEASE_BUILD -eq 0 ]]; then
	timing_finish
	exit 0
fi

timing_enter build_release

config_params="$(get_config_params | sed 's/--enable-debug//g')"
if [ $(uname -s) = Linux ]; then
	./configure $config_params --enable-lto
else
	# LTO needs a special compiler to work on BSD.
	./configure $config_params
fi
$MAKE ${MAKEFLAGS}
$MAKE ${MAKEFLAGS} clean

timing_exit build_release

timing_finish
