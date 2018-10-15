#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

base_bdev=$1

if [ -n $base_bdev ]; then
        echo
        echo "[crypto]"
        if [ $(lspci -d:37c8 | wc -l) -eq 0 ]; then
                echo "  CRY $base_bdev crypto_ram 0123456789123456 crypto_aesni_mb"
        else
                echo "  CRY $base_bdev crypto_ram 0123456789123456 crypto_qat"
        fi
fi

