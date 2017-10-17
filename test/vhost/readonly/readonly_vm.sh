#!/usr/bin/env bash
set -x

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

lsblk -o NAME | grep 'vd.?'
echo "test"
sleep 2
echo "test"
