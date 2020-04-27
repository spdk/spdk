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

if [[ $RUN_NIGHTLY -eq 0 ]]; then
	timing_finish
	exit 0
fi

timing_enter build_release

if [ $(uname -s) = Linux ]; then
	./configure $(get_config_params) --disable-debug --enable-lto --disable-unit-tests
else
	# LTO needs a special compiler to work on BSD.
	./configure $(get_config_params) --disable-debug
fi
$MAKE ${MAKEFLAGS}
$MAKE ${MAKEFLAGS} clean

timing_exit build_release

timing_finish
