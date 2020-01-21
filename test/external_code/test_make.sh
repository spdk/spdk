#!/usr/bin/env bash

test_root=$(readlink -f $(dirname $0))
rootdir="$test_root/../.."

source "$rootdir/test/common/autotest_common.sh"

set -e
SPDK_DIR=$1

$SPDK_DIR/configure --with-shared --without-isal --without-ocf --disable-asan
make -C $SPDK_DIR -j$(nproc)

export SPDK_HEADER_DIR="$SPDK_DIR/include"
export SPDK_LIB_DIR="$SPDK_DIR/build/lib"
export DPDK_LIB_DIR="$SPDK_DIR/dpdk/build/lib"

# The default target is to make both the app and bdev and link them against the combined SPDK shared library libspdk.so.
run_test "external_make_tc1" make -C $test_root hello_world_bdev_shared_combo

LD_LIBRARY_PATH=$SPDK_LIB_DIR:$DPDK_LIB_DIR:"$test_root/passthru" run_test "external_run_tc1" $test_root/hello_world/hello_bdev --json $test_root/hello_world/bdev_external.conf -b TestPT

make -C $test_root clean

# Make just the application linked against the combined SPDK shared library libspdk.so.
run_test "external_make_tc2" make -C $test_root hello_world_no_bdev_shared_combo

LD_LIBRARY_PATH=$SPDK_LIB_DIR:$DPDK_LIB_DIR run_test "external_run_tc2" $test_root/hello_world/hello_bdev --json $test_root/hello_world/bdev.conf -b Malloc0

make -C $test_root clean

# Make both the application and bdev against individual SPDK shared libraries.
run_test "external_make_tc3" make -C $test_root hello_world_bdev_shared_iso

LD_LIBRARY_PATH=$SPDK_LIB_DIR:$DPDK_LIB_DIR:"$test_root/passthru" run_test "external_run_tc3" $test_root/hello_world/hello_bdev --json $test_root/hello_world/bdev_external.conf -b TestPT

make -C $test_root clean

# Make just the application linked against individual SPDK shared libraries.
run_test "external_make_tc4" make -C $test_root hello_world_no_bdev_shared_iso

LD_LIBRARY_PATH=$SPDK_LIB_DIR:$DPDK_LIB_DIR run_test "external_run_tc4" $test_root/hello_world/hello_bdev --json $test_root/hello_world/bdev.conf -b Malloc0

make -C $test_root clean

make -C $SPDK_DIR clean
$SPDK_DIR/configure --without-shared --without-isal --without-ocf --disable-asan
make -C $SPDK_DIR -j$(nproc)

# Make both the application and bdev against individual SPDK archives.
run_test "external_make_tc5" make -C $test_root hello_world_bdev_static

LD_LIBRARY_PATH=$SPDK_LIB_DIR:$DPDK_LIB_DIR:"$test_root/passthru" run_test "external_run_tc5" $test_root/hello_world/hello_bdev --json $test_root/hello_world/bdev_external.conf -b TestPT

make -C $test_root clean

# Make just the application linked against individual SPDK archives.
run_test "external_make_tc6" make -C $test_root hello_world_no_bdev_static

LD_LIBRARY_PATH=$SPDK_LIB_DIR:$DPDK_LIB_DIR run_test "external_run_tc6" $test_root/hello_world/hello_bdev --json $test_root/hello_world/bdev.conf -b Malloc0

make -C $test_root clean
make -C $SPDK_DIR -j$(nproc) clean
