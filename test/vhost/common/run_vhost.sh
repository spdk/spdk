#!/usr/bin/env bash

set -e
. $(readlink -f $(dirname $0))/common.sh

trap "trap - ERR; error 'Failed to run vhost'; exit 1" ERR

spdk_vhost_run "${VHOST_TEST_ARGS[@]}"
