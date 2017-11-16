#!/usr/bin/env bash

. $(readlink -f $(dirname $0))/common.sh || exit 1

trap "trap - ERR; print_backtrace; error 'Failed to run VM'; exit 1" ERR

vm_run "${SPDK_VHOST_TEST_ARGS[@]}"
