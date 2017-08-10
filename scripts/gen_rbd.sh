#!/usr/bin/env bash

set -e

if ! hash ceph &> /dev/null; then
        exit 0
fi

echo
echo "[Ceph]"
echo "  Ceph $RBD_POOL $RBD_NAME 512"
