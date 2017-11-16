#!/usr/bin/env bash

set -e
. $(readlink -f $(dirname $0))/common.sh

trap "trap - ERR; error_exit >&2" ERR

spdk_vhost_run "${VHOST_TEST_ARGS[@]}"

echo ""
echo "INFO: Started"
echo ""
