#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
test_root=$(readlink -f $(dirname $0))
rootdir="$test_root/../.."

source "$rootdir/test/common/autotest_common.sh"

set -e
SPDK_DIR=$1

if [ -z "$EXTERNAL_MAKE_HUGEMEM" ]; then
	EXTERNAL_MAKE_HUGEMEM=$HUGEMEM
fi

sudo HUGEMEM="$EXTERNAL_MAKE_HUGEMEM" $SPDK_DIR/scripts/setup.sh

if [ -n "$SPDK_RUN_EXTERNAL_DPDK" ]; then
	WITH_DPDK="--with-dpdk=$SPDK_RUN_EXTERNAL_DPDK"
fi
if [[ -e $SPDK_DIR/mk/config.mk ]]; then
	make -C $SPDK_DIR clean
fi
$SPDK_DIR/configure --with-shared --without-ocf --disable-asan $WITH_DPDK
make -C $SPDK_DIR -j$(nproc)

export SPDK_HEADER_DIR="$SPDK_DIR/include"
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:"$test_root/passthru"
_sudo="sudo -E --preserve-env=PATH LD_LIBRARY_PATH=$LD_LIBRARY_PATH"

# The default target is to make both the app and bdev and link them against the combined SPDK shared library libspdk.so.
run_test "external_make_hello_bdev_shared_combo" make -C $test_root hello_world_bdev_shared_combo
run_test "external_run_hello_bdev_shared_combo" $_sudo $test_root/hello_world/hello_bdev \
	--json $test_root/hello_world/bdev_external.json -b TestPT

make -C $test_root clean

# Make just the application linked against the combined SPDK shared library libspdk.so.
run_test "external_make_hello_no_bdev_shared_combo" make -C $test_root hello_world_no_bdev_shared_combo
run_test "external_run_hello_no_bdev_shared_combo" $_sudo $test_root/hello_world/hello_bdev \
	--json $test_root/hello_world/bdev.json -b Malloc0

make -C $test_root clean

# Make both the application and bdev against individual SPDK shared libraries.
run_test "external_make_hello_bdev_shared_iso" make -C $test_root hello_world_bdev_shared_iso
run_test "external_run_hello_bdev_shared_iso" $_sudo $test_root/hello_world/hello_bdev \
	--json $test_root/hello_world/bdev_external.json -b TestPT

make -C $test_root clean

# Make just the application linked against individual SPDK shared libraries.
run_test "external_make_hello_no_bdev_shared_iso" make -C $test_root hello_world_no_bdev_shared_iso
run_test "external_run_hello_no_bdev_shared_iso" $_sudo $test_root/hello_world/hello_bdev \
	--json $test_root/hello_world/bdev.json -b Malloc0

# Make the basic NVMe driver linked against individual shared SPDK libraries.
run_test "external_make_nvme_shared" make -C $test_root nvme_shared
run_test "external_run_nvme_shared" $_sudo $test_root/nvme/identify.sh

make -C $test_root clean

make -C $SPDK_DIR clean
$SPDK_DIR/configure --without-shared --without-ocf --disable-asan $WITH_DPDK
make -C $SPDK_DIR -j$(nproc)

# Make both the application and bdev against individual SPDK archives.
run_test "external_make_hello_bdev_static" make -C $test_root hello_world_bdev_static
run_test "external_run_hello_bdev_static" $_sudo $test_root/hello_world/hello_bdev \
	--json $test_root/hello_world/bdev_external.json -b TestPT

make -C $test_root clean

# Make just the application linked against individual SPDK archives.
run_test "external_make_hello_no_bdev_static" make -C $test_root hello_world_no_bdev_static
run_test "external_run_hello_no_bdev_static" $_sudo $test_root/hello_world/hello_bdev \
	--json $test_root/hello_world/bdev.json -b Malloc0

# Make the basic NVMe driver statically linked against individual SPDK archives.
run_test "external_make_nvme_static" make -C $test_root nvme_static
run_test "external_run_nvme_static" $_sudo $test_root/nvme/identify.sh

make -C $test_root clean
make -C $SPDK_DIR -j$(nproc) clean

sudo HUGEMEM="$HUGEMEM" $SPDK_DIR/scripts/setup.sh reset
