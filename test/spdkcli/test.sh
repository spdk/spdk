#!/usr/bin/env bash

set -ex

CURRENT_DIR=$(readlink -f $(dirname $0))
. $CURRENT_DIR/../vhost/common/common.sh

function on_error_exit() {
        echo "Error on $1 - $2"
        print_backtrace
        set +e
        exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

fake_command
