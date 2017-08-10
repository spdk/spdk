#!/usr/bin/env bash

set -e

if ! hash ceph &> /dev/null; then
        exit 0
fi

echo "[Ceph]"
echo "  Ceph $RBD_NAME $RBD_POOL 512"
echo
