#!/usr/bin/env bash

. $(readlink -f $(dirname $0))/common.sh || exit 1

# We don't know what command will do so don't trap errors here
trap - ERR

vm_ssh "${VHOST_TEST_ARGS[@]}"
