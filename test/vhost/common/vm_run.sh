#!/usr/bin/env bash

. $(readlink -f $(dirname $0))/common.sh || exit 1

trap "trap - ERR; error_exit >&2" ERR

vm_run "${SPDK_VHOST_TEST_ARGS[@]}"
